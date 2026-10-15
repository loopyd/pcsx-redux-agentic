-- lualoader, R"EOF(--
local m, events, transactions = require 'support', require 'events', require 'transactions'
local M = {}
function M.run()
    m.begin('action,target,identity,events,bytes', function()
        local identity = assert(m.argument('identity'), 'identity is required')
        assert(#identity > 0 and #identity <= 160, 'identity must be 1..160 bytes')
        if m.input('state', false) then
            assert(os.getenv('PSX_RUNTIME_ORIGIN_VALIDATED') == '1', 'disc schedule requires matching state origin')
        end
        local file = assert(io.open(m.input('schedule', true), 'rb'))
        local text = assert(file:read(8193)); assert(file:close())
        assert(#text <= 8192 and not text:find('%z'), 'disc schedule exceeds 8 KiB or contains NUL')
        local schedule, previous, stopped = {}, 0, false
        local lineNumber = 0
        for line in (text..'\n'):gmatch('(.-)\n') do
            lineNumber = lineNumber + 1; line = line:gsub('\r$', '')
            if lineNumber == 1 then assert(line == 'psx.disc-schedule/v1', 'unsupported disc schedule')
            elseif line ~= '' then
                assert(not stopped and #schedule < 32, 'disc schedule exceeds 32 operations or continues after stop')
                local frame, action, name = line:match('^(%d+) ([a-z]+) ?([a-z0-9-]*)$')
                frame = tonumber(frame)
                assert(frame and frame >= previous and frame <= 36000, 'disc schedule frames must increase in 0..36000')
                assert(({open=true,eject=true,mount=true,close=true,stop=true})[action], 'unknown disc schedule operation')
                assert((action == 'mount') == (name ~= ''), 'mount requires a declared replacement name; other operations take none')
                if action == 'mount' then require('mount').replacement(name) end
                if action == 'stop' then assert(frame >= 1, 'stop requires a positive frame'); stopped = true end
                schedule[#schedule+1] = {frame=frame,action=action,name=name~='' and name or nil}
                previous = frame
            end
        end
        assert(stopped, 'disc schedule requires final stop')
        local h = PCSX.History
        local eventLimit = m.integer('events',8192,1,65536)
        local byteLimit = m.integer('bytes',1048576,1,8388608)
        local limits = {mutations=4096,bytes=8388608}
        local bindings = require('mount').read()
        local report = {schema='psx.runtime-disc-schedule/v1',identity=identity,build_identity=m.buildIdentity(),
            operations=m.array(),segments=m.array(),frame_limit=previous,success=false,
            scope='ordered host operations at settled GPU Vsync boundaries; guest execution only between boundaries',
            limitations=m.array({'Recorder segments exclude host replacement operations.',
                'Preparation failure retains the old image; postcommit notification failure has no rollback.',
                'Guest commands, disc conformance, physical media and hardware timing require separate acceptance.'})}
        local frame,index,active,closing,boundaryPending = 0,1,false,false,false
        local function observe()
            return {lid_open=PCSX.CD.peekLid(),controller=require('snapshot').describe(PCSX.CD.captureChecked(),'CDRom',false,2),
                cycle=tostring(PCSX.getCPUCycles())}
        end
        local function freeze()
            if not active then return end
            h.stop(); active=false
            local prefix=string.format('segment-%02d-',#report.segments+1)
            local capture=h.snapshot()
            local native=events.export(capture,prefix)
            local correlation=transactions.export(capture,limits,native,nil,bindings,nil,prefix)
            report.segments[#report.segments+1]={prefix=prefix,frame=frame,complete=native.complete and correlation.complete,
                begin_cycle=native.begin_cycle,end_cycle=native.end_cycle}
            assert(native.complete and correlation.complete,'incomplete disc schedule segment')
            local total=0
            for _,artifact in ipairs(m.captures) do total=total+artifact.bytes end
            assert(total<=100663296,'disc schedule capture budget exceeded')
            h.clear()
        end
        local function begin()
            assert(#report.segments<8,'disc schedule exceeds eight recorder segments')
            local capacity=h.cdLimits(0)
            assert(eventLimit>=capacity.minimumBoundaryEvents and byteLimit>=capacity.minimumBoundaryBytes,
                'native CD profile capacity floor not met')
            local ok,err=pcall(function() h.beginCDV3(eventLimit,byteLimit,0,0) end)
            active=h.status().state~=0
            assert(ok,err)
        end
        local function finish(ok, reason)
            if closing then return end
            closing=true
            local frozen, failure=pcall(freeze)
            if not frozen then ok=false;reason=tostring(failure) end
            report.success,report.reason,report.frames=ok,reason,frame
            local saved, saveError=pcall(function() m.captureState('final.pbuf') end)
            if not saved then report.success=false;report.state_error=tostring(saveError) end
            m.report('disc-schedule.json',report)
            if report.success then m.finish() else error('disc schedule failed: '..tostring(reason)) end
        end
        local function boundary()
            local ok, err=pcall(function()
                freeze()
                while schedule[index] and schedule[index].frame==frame do
                    local event=schedule[index];index=index+1
                    if event.action=='stop' then finish(true,'scheduled stop');return end
                    local record={frame=frame,action=event.action,name=event.name,before=observe()}
                    report.operations[#report.operations+1]=record
                    local committed,problem=pcall(function()
                        if event.action=='open' then PCSX.CD.openLid()
                        elseif event.action=='eject' then PCSX.CD.eject()
                        elseif event.action=='mount' then PCSX.CD.mount(require('mount').replacement(event.name))
                        else PCSX.CD.closeLid() end
                    end)
                    local observed,value=pcall(observe)
                    if observed then record.after=value else record.observation_error=tostring(value) end
                    record.success,record.error=committed,not committed and tostring(problem) or nil
                    assert(committed,problem)
                end
                if not closing then begin();PCSX.resumeEmulator() end
            end)
            if not ok then finish(false,tostring(err)) end
        end
        m.event('GPU::Vsync',function()
            if not active or closing then return end
            frame=frame+1
            if h.status().failures~=0 or schedule[index].frame==frame then
                boundaryPending=true
                PCSX.pauseEmulator()
                PCSX.nextPause(m.guard(function()
                    boundaryPending=false
                    if h.status().failures~=0 then finish(false,'native recorder failure') else boundary() end
                end))
            end
        end)
        m.event('ExecutionFlow::Pause',function()
            if not active or closing or boundaryPending then return end
            boundaryPending=true
            PCSX.nextPause(m.guard(function()
                boundaryPending=false
                finish(false,'unexpected native pause')
            end))
        end)
        m.atTarget(function()
            m.captureState('initial.pbuf')
            boundary()
        end)
    end)
end
return M
-- )EOF"
