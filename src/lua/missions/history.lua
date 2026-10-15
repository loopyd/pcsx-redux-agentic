-- lualoader, R"EOF(--
-- Observe a bounded interval of guest DMA, IRQ and timer execution.
local m, events = require 'support', require 'events'
m.begin('target,identity,frames,stop,events,bytes,ram_start,ram_length', function()
    local h = assert(PCSX.History, 'native history binding required')
    local identity = assert(m.argument('identity'), 'identity is required')
    assert(#identity > 0 and #identity <= 160, 'identity must be 1..160 bytes')
    local frames = m.integer('frames', 1, 1, 36000)
    local event_limit = m.integer('events', 16384, 1, 65536)
    local byte_limit = m.integer('bytes', 1048576, 0, 8388608)
    local stop = m.argument('stop') and m.address('stop')
    local ram_start, ram_length
    if m.argument('ram_start') or m.argument('ram_length') then
        ram_start = m.integer('ram_start', nil, 0, 2097151)
        ram_length = m.integer('ram_length', nil, 1, 2097152 - ram_start)
        assert(byte_limit >= 2 * ram_length, 'RAM boundaries exceed the payload capacity')
    end
    local started, closing, count = false, false, 0
    local function finish(reason, success)
        if closing then return end
        closing = true
        PCSX.pauseEmulator()
        PCSX.nextPause(m.guard(function()
            local result
            if started then
                h.stop()
                result = events.export()
                success = success and result.complete
                m.captureState('final.pbuf')
            end
            m.report('history.json', {schema = 'psx.runtime-history/v2', native_envelope_version=1, build_identity=m.buildIdentity(), identity = identity,
                identity_authority = 'caller label; input and tool hashes in harness receipt',
                reason = reason, success = success, started = started, frames = count,
                frame_limit = frames, stop = stop,
                event_limit = event_limit, byte_limit = byte_limit,
                profile = ram_start and 'ram-interval-v1' or 'devices',
                ram_start = ram_start, ram_length = ram_length,
                native_complete = result and result.complete or false})
            if success then m.finish() else error('incomplete history: ' .. reason) end
        end))
    end
    m.event('GPU::Vsync', function()
        if not started or closing then return end
        count = count + 1
        if h.status().failures ~= 0 then finish('native recorder failure', false)
        elseif count >= frames then finish(stop and 'frame limit before stop' or 'frame limit', not stop) end
    end)
    m.event('ExecutionFlow::Pause', function()
        if started and not closing then finish('unexpected native pause', false) end
    end)
    if stop then m.breakpoint(stop, 'Exec', 4, function()
        if started and not closing then finish('stop reached', true); return false end
        return true
    end) end
    m.atTarget(function()
        PCSX.pauseEmulator()
        PCSX.nextPause(m.guard(function()
            if closing then return end
            m.captureState('initial.pbuf')
            if ram_start then h.beginRAM(event_limit, byte_limit, ram_start, ram_length)
            else h.begin(event_limit, byte_limit) end
            started = true
            if h.status().failures ~= 0 then finish('initial recorder failure', false)
            elseif stop and tonumber(PCSX.getRegisters().pc) == stop then finish('stop already reached', true)
            else PCSX.resumeEmulator() end
        end))
    end)
end)
-- )EOF"
