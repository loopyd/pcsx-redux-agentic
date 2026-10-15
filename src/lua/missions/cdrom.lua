-- lualoader, R"EOF(--
-- Inspect controller state, read mounted media, or issue a bounded native controller command.
local m, s = require 'support', require 'snapshot'
if m.argument('action','inspect')=='capture' then require('transport').run();return end
if m.argument('action','inspect')=='schedule' then require('exchange').run();return end
m.begin('action,target,path,lba,sectors,mode,offset,length,command,params,frames,scratch,savestate', function()
    local action = m.argument('action', 'inspect')
    if action ~= 'command' then
        assert(m.integer('savestate', 0, 0, 1) == 0, 'savestate applies only to controller commands')
    end
    local function describe(cd)
        return s.describe(cd, 'CDRom', false, 2)
    end
    local function finish(report)
        m.report('cdrom.json', report)
        m.finish()
    end
    if action == 'inspect' then
        local path = m.input('state', false)
        if path and not m.argument('target') and not os.getenv('PSX_RUNTIME_ENTRY') then
            s.observe(function(state, source)
                finish({schema = 'psx.runtime-cdrom/v2', action = action, source = source, state = describe(state.cdrom)})
            end)
        else
            m.atTarget(function()
                finish({schema = 'psx.runtime-cdrom/v2', action = action, source = 'live',
                    state = describe(PCSX.CD.captureChecked())})
            end)
        end
        return
    end
    local hostActions = {['open-lid'] = PCSX.CD.openLid, ['close-lid'] = PCSX.CD.closeLid,
        ['schedule-close-lid'] = PCSX.CD.scheduleCloseLid, eject = PCSX.CD.eject}
    if action == 'mount' or hostActions[action] then
        local path
        if action == 'mount' then
            path = m.input('disc', false) or m.argument('path')
            assert(path and path ~= '', 'mount requires an explicit disc input or host path')
        end
        m.atTarget(function()
            if action == 'mount' then PCSX.CD.mount(path) else hostActions[action]() end
            finish({schema = 'psx.runtime-cdrom/v2', action = action, source = 'live', path = path,
                lid_open = PCSX.CD.peekLid(), state = describe(PCSX.CD.captureChecked())})
        end)
        return
    end
    if action == 'command' then
        local command = m.integer('command', nil, 0, 255)
        local params = m.argument('params', '')
        assert(#params <= 32 and #params % 2 == 0 and (params == '' or params:match('^%x+$')),
            'params requires up to sixteen hexadecimal byte pairs')
        local limit = m.integer('frames', 60, 1, 120)
        local save = m.integer('savestate', 0, 0, 1) == 1
        m.atTarget(function()
            local before = PCSX.CD.captureChecked()
            local function valueEmpty(queue) return not queue.has_value or queue.value_read end
            local function empty(queue) return valueEmpty(queue) and (queue.payload_size==0 or queue.payload_index==queue.payload_size) end
            assert(empty(before.commandFifo) and valueEmpty(before.responseFifo0) and empty(before.responseFifo1),
                'native controller queues are not ready for command submission')
            local ops = {{0x1f801800, 1}, {0x1f801803, 0x40}, {0x1f801800, 0}}
            for pair in params:gmatch('..') do ops[#ops + 1] = {0x1f801802, tonumber(pair, 16)} end
            ops[#ops + 1] = {0x1f801801, command}
            require('bus').write(8, ops, function(probe)
                local wait = PCSX.CD.waitResponse({maxVsyncs = limit}, m.guard(function(response, error)
                    assert(response, error)
                    if save then m.captureState('state.pbuf') end
                    finish({schema = 'psx.runtime-cdrom/v2', action = action, source = 'live',
                        command = command, params_hex = params, probe = probe, vsyncs = response.vsyncs,
                        interrupt = response.interrupt, command_error = response.command_error,
                        before = describe(before), after = describe(response.state),
                        stop = 'first controller IRQ observed at settled polling after GPU Vsync; FIFO not consumed or acknowledged'})
                end))
                m.retained[#m.retained + 1] = wait
                PCSX.resumeEmulator()
            end)
        end)
        return
    end
    assert(action == 'directory' or action == 'file' or action == 'sectors', 'unknown CD-ROM action')
    PCSX.pauseEmulator()
    local iso = PCSX.getCurrentIso()
    assert(not iso:failed(), 'no valid mounted disc; supply --disc CUE')
    if action == 'sectors' then
        local modes = {RAW = 2352, M1 = 2048, M2_RAW = 2336, M2_FORM1 = 2048, M2_FORM2 = 2324}
        local mode = m.argument('mode', 'RAW')
        local bytes = assert(modes[mode], 'unknown sector mode')
        local lba = m.integer('lba', 0, 0, 449849)
        local count = m.integer('sectors', 1, 1, 1024)
        local file = iso:open(lba, bytes * count, mode)
        assert(not file:failed(), 'cannot open sector range')
        local data = tostring(file:read(bytes * count)); file:close()
        assert(#data == bytes * count, 'short sector read')
        m.write('sectors.bin', data)
        finish({schema = 'psx.runtime-cdrom/v2', action = action, lba = lba,
            sectors = count, mode = mode, bytes = #data, source = 'mounted-media'})
        return
    end
    local volume = iso:open(16, 2048, 'GUESS')
    local descriptor = tostring(volume:read(2048)); volume:close()
    assert(#descriptor == 2048 and descriptor:sub(1, 7) == '\1CD001\1', 'invalid ISO9660 primary volume descriptor')
    local path = m.argument('path', '')
    assert(not path:find('%z') and not path:find('\\', 1, true), 'invalid ISO path')
    if action == 'directory' then
        local entries, current = iso:listDirectory(path, {maxEntries = 4096})
        finish({schema = 'psx.runtime-cdrom/v2', action = action, path = current,
            entries = m.array(entries), source = 'mounted-media'})
    else
        assert(path ~= '', 'file path is required')
        local file = iso:createReader():open(path)
        assert(not file:failed(), 'ISO file not found')
        local size = tonumber(file:size())
        local offset = m.integer('offset', 0, 0, size)
        local length = m.integer('length', size - offset, 0, math.min(8388608, size - offset))
        local data = tostring(file:readAt(length, offset)); file:close()
        assert(#data == length, 'short ISO file read')
        m.write('file.bin', data)
        finish({schema = 'psx.runtime-cdrom/v2', action = action, path = path,
            offset = offset, bytes = #data, source_bytes = size, source = 'mounted-media'})
    end
end)
-- )EOF"
