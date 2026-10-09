-- lualoader, R"EOF(--
-- Inspect controller state, read mounted media, or issue a bounded native controller command.
local m, s = require 'support', require 'snapshot'
if m.argument('action','inspect')=='capture' then require('transport').run();return end
m.begin('action,target,path,lba,sectors,mode,offset,length,command,params,frames,scratch,savestate', function()
    local action = m.argument('action', 'inspect')
    if action ~= 'command' then
        assert(m.integer('savestate', 0, 0, 1) == 0, 'savestate applies only to controller commands')
    end
    local function describe(state)
        local value, kind, repeated = s.select(state, 'cdrom')
        return s.describe(value, kind, repeated, 2)
    end
    local function finish(report)
        m.report('cdrom.json', report)
        m.finish()
    end
    if action == 'inspect' then
        s.observe(function(state, source)
            finish({schema = 'psx.runtime-cdrom/v2', action = action, source = source, state = describe(state)})
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
            local before = s.live()
            local function valueEmpty(queue) return not queue.has_value or queue.value_read end
            local function empty(queue) return valueEmpty(queue) and (queue.payload_size==0 or queue.payload_index==queue.payload_size) end
            assert(empty(before.cdrom.commandFifo) and valueEmpty(before.cdrom.responseFifo0) and empty(before.cdrom.responseFifo1),
                'native controller queues are not ready for command submission')
            local ops = {{0x1f801800, 1}, {0x1f801803, 0x40}, {0x1f801800, 0}}
            for pair in params:gmatch('..') do ops[#ops + 1] = {0x1f801802, tonumber(pair, 16)} end
            ops[#ops + 1] = {0x1f801801, command}
            require('bus').write(8, ops, function(probe)
                local frames = 0
                m.event('GPU::Vsync', function()
                    frames = frames + 1
                    PCSX.pauseEmulator()
                    PCSX.nextTick(m.guard(function()
                        local state = s.live()
                        if state.cdrom.responseFifo0.has_value and not state.cdrom.responseFifo0.value_read then
                            if save then m.captureState('state.pbuf') end
                            finish({schema = 'psx.runtime-cdrom/v2', action = action, source = 'live',
                                command = command, params_hex = params, probe = probe, vsyncs = frames,
                                interrupt = state.cdrom.responseFifo0.value, command_error = state.cdrom.responseFifo0.value == 5,
                                before = describe(before), after = describe(state),
                                stop = 'first controller IRQ observed at settled polling after GPU Vsync; FIFO not consumed or acknowledged'})
                        else
                            assert(frames < limit, 'no controller IRQ observed within frame bound')
                            PCSX.resumeEmulator()
                        end
                    end))
                end)
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
    local reader = iso:createReader()
    local path = m.argument('path', '')
    assert(not path:find('%z') and not path:find('\\', 1, true), 'invalid ISO path')
    if action == 'directory' then
        local current = ''
        for part in path:gmatch('[^/]+') do
            assert(part ~= '.' and part ~= '..', 'relative ISO path components unsupported')
            local found = false
            for _, entry in ipairs(reader:listDir(current)) do
                if entry.name == part and entry.isDir then found = true end
            end
            assert(found, 'ISO directory not found: ' .. part)
            current = current == '' and part or current .. '/' .. part
        end
        local entries = reader:listDir(current)
        assert(#entries <= 4096, 'directory exceeds 4096 entries')
        finish({schema = 'psx.runtime-cdrom/v2', action = action, path = current,
            entries = m.array(entries), source = 'mounted-media'})
    else
        assert(path ~= '', 'file path is required')
        local file = reader:open(path)
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
