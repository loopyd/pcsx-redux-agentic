-- lualoader, R"EOF(--
-- Observe bounded debugger accesses; these are pre-instruction observations.
local m = require 'support'

local function bounded_watch(address, width, save, limit)
    assert(width == 1 and address % 4 == 0, 'bounded watch needs width 1 and aligned PC')
    assert(address < 0x200000 or
        (address >= 0x80000000 and address < 0x80200000) or
        (address >= 0xa0000000 and address < 0xa0200000), 'bounded watch needs canonical RAM')
    local mission = {
        phase = 'acquiring', count = 0, limit = limit,
        verdict = {kind = 'waiting'}, hooks = {},
        initial = nil, last = nil, selected = nil, stop = nil,
        expected_flow = nil, pause = nil, pending_range_count = nil,
    }
    local priority = {waiting = 0, bound = 1, hit = 2, failed = 3}
    local finalize

    local function active()
        return mission.phase == 'initializing' or mission.phase == 'armed' or
            mission.phase == 'settling'
    end

    local function owned_flow(kind, callback)
        mission.expected_flow = kind
        local ok, err = pcall(callback)
        mission.expected_flow = nil
        assert(ok, err)
    end

    local function same_boundary(selected, boundary)
        return boundary and selected.address == boundary.address and
            selected.width == boundary.width and selected.cause == boundary.cause and
            selected.registers.pc == boundary.raw_pc and
            selected.registers.cycles == boundary.cycles
    end

    local function stop(kind, reason, source)
        if mission.phase == 'closed' then return end
        if priority[kind] > priority[mission.verdict.kind] then
            mission.verdict = {kind = kind, reason = reason}
        end
        local previous = mission.phase
        if previous ~= 'settling' then
            if source == 'range' or
                (source == 'selected' and same_boundary(mission.selected, mission.last)) then
                mission.pending_range_count = mission.count
            end
            mission.phase = 'settling'
        end
        local paused, err = pcall(owned_flow, 'pause', PCSX.pauseEmulator)
        if not paused and mission.verdict.kind ~= 'failed' then
            mission.verdict = {kind = 'failed', reason = 'pause error: ' .. tostring(err):sub(1, 4096)}
        end
        if previous == 'armed' or previous == 'acquiring' then
            PCSX.nextTick(m.guard(finalize))
        end
    end

    local function protected(callback)
        return function(...)
            local ok, result = pcall(callback, ...)
            if not ok then
                stop('failed', 'callback error: ' .. tostring(result):sub(1, 4096), 'error')
                return false
            end
            return result
        end
    end

    finalize = function()
        if mission.phase == 'closed' then return end
        local ok, registers = pcall(m.registers)
        if ok then
            mission.stop = {step = mission.count, raw_pc = registers.pc, registers = registers}
        else
            mission.verdict = {kind = 'failed', reason =
                'stop observation error: ' .. tostring(registers):sub(1, 4096)}
        end
        if mission.verdict.kind == 'hit' then
            local reached = mission.selected and mission.selected.registers or mission.initial.registers
            if not mission.stop or mission.stop.raw_pc ~= address or
                mission.stop.registers.cycles ~= reached.cycles or
                (mission.selected and (mission.count == 0 or
                    not same_boundary(mission.selected, mission.last))) then
                mission.verdict = {kind = 'failed', reason = 'selected/range stop observations disagree'}
            end
        end
        for _, hook in ipairs(mission.hooks) do
            local disabled, err = pcall(function() hook:disable() end)
            if not disabled then
                mission.verdict = {kind = 'failed', reason =
                    'hook disable error: ' .. tostring(err):sub(1, 4096)}
            end
        end
        mission.phase = 'closed'
        local first_reach
        if mission.verdict.kind == 'hit' then
            if mission.selected then
                first_reach = {step = mission.count, raw_pc = mission.selected.registers.pc,
                    actual = mission.selected.address, width = mission.selected.width,
                    cause = mission.selected.cause, registers = mission.selected.registers}
            else
                first_reach = mission.initial
            end
            local captured, err = pcall(function() m.capture(save) end)
            if not captured then
                mission.verdict = {kind = 'failed', reason =
                    'capture error: ' .. tostring(err):sub(1, 4096)}
                first_reach = nil
            end
        end
        local events = m.array()
        if mission.selected then events[1] = mission.selected end
        local success = mission.verdict.kind == 'hit'
        m.report('watch.json', {
            schema = 'psx.runtime-watch/v1', kind = 'Exec', address = address, width = width,
            boundary = 'paused step0; subsequent pre-instruction Exec debugger boundaries',
            counter_semantics = 'observed_exec_boundaries; not retired instructions; updatedPC may overcount',
            instruction_limit = mission.limit, observed_exec_boundaries = mission.count,
            initial = mission.initial, stop = mission.stop, first_reach = first_reach,
            last_boundary = mission.last, pause = mission.pause, events = events,
            success = success, reason = mission.verdict.reason,
            word_source = success and {capture = 'ram.bin', bytes = 2097152,
                raw_pc = first_reach.raw_pc,
                addressing = 'physical RAM; verify original loading/address mapping'} or nil,
        })
        if success then m.finish()
        else error(mission.verdict.reason or 'bounded watch failed') end
    end

    m.event('ExecutionFlow::Pause', protected(function(event)
        if not active() then return end
        local expected = mission.expected_flow == 'pause'
        mission.pause = {exception = event.exception, raw_pc = tonumber(PCSX.getRegisters().pc),
            owner = expected and 'mission' or 'external'}
        mission.expected_flow = nil
        if not expected or event.exception then
            stop('failed', event.exception and 'exception pause' or 'unexpected pause', 'event')
        end
    end))
    m.event('ExecutionFlow::Run', protected(function()
        if not active() then return end
        local expected = mission.phase == 'initializing' and mission.expected_flow == 'run'
        mission.expected_flow = nil
        if not expected then stop('failed', 'unexpected run', 'event') end
    end))
    for _, name in ipairs({'ExecutionFlow::Reset', 'ExecutionFlow::SaveStateLoaded', 'Memory::SetLuts'}) do
        local event_name = name
        m.event(event_name, protected(function()
            if active() then stop('failed', 'forbidden event: ' .. event_name, 'event') end
        end))
    end

    protected(function()
        m.atTarget(protected(function()
            if mission.phase ~= 'acquiring' then return end
            owned_flow('pause', PCSX.pauseEmulator)
            mission.phase = 'initial-pending'
            PCSX.nextTick(m.guard(function()
                mission.phase = 'initializing'
                local ok, err = pcall(function()
                    local registers = m.registers()
                    mission.initial = {step = 0, raw_pc = registers.pc, registers = registers}
                    assert(registers.pc % 4 == 0, 'unaligned initial PC')
                    local raw_target = m.argument('target', os.getenv('PSX_RUNTIME_ENTRY'))
                    if raw_target then
                        assert(registers.pc == m.address('target', tonumber(raw_target)),
                            'initial raw PC differs from requested acquisition target')
                    end
                    if registers.pc == address then
                        stop('hit', 'initial PC reached', 'initial')
                        return
                    end
                    mission.hooks[#mission.hooks + 1] = m.breakpoint(0, 'Exec', 0x20000000,
                        protected(function(actual, size, cause)
                            if mission.phase ~= 'armed' and mission.phase ~= 'settling' then return false end
                            local duplicate = mission.phase == 'settling' and mission.pending_range_count ~= nil
                            mission.count = mission.count + 1
                            mission.last = {address = actual, width = size, cause = cause,
                                observed_exec_boundaries = mission.count}
                            mission.last.raw_pc = tonumber(PCSX.getRegisters().pc)
                            mission.last.cycles = tonumber(PCSX.getCPUCycles())
                            if mission.phase == 'settling' then mission.pending_range_count = mission.count end
                            if duplicate then
                                stop('failed', 'new boundary after terminal pause', 'range')
                            elseif mission.count > mission.limit then
                                stop('failed', 'boundary observed after limit', 'range')
                            elseif mission.count == mission.limit then
                                stop('bound', 'execution boundary limit reached', 'range')
                            end
                            return true
                        end))
                    mission.hooks[#mission.hooks + 1] = m.breakpoint(address, 'Exec', width,
                        protected(function(actual, size, cause)
                            if mission.phase ~= 'armed' and mission.phase ~= 'settling' then return false end
                            mission.selected = {address = actual, width = size, cause = cause}
                            mission.selected.registers = m.registers()
                            if actual ~= address or mission.selected.registers.pc ~= address then
                                stop('failed', 'native match has nonmatching raw PC', 'selected')
                            else stop('hit', 'selected PC reached', 'selected') end
                            return false
                        end))
                    owned_flow('run', PCSX.resumeEmulator)
                    if mission.phase == 'initializing' then mission.phase = 'armed' end
                end)
                if not ok then stop('failed', 'initial observation error: ' .. tostring(err):sub(1, 4096), 'error') end
                -- Nested nextTick work is discarded by the native UI poller.
                if mission.phase == 'settling' then finalize() end
            end))
        end))
    end)()
end

m.begin('target,address,kind,width,hits,savestate,instructions', function()
    local address = m.integer('address', nil, 0, 4294967295)
    local width = m.integer('width', 1, 1, 65536)
    assert(address + width <= 4294967296, 'watch range overflows address space')
    local kind = m.argument('kind', 'Write')
    assert(kind == 'Exec' or kind == 'Read' or kind == 'Write', 'kind must be Exec, Read, or Write')
    local hits = m.integer('hits', 1, 1, 4096)
    local save = m.integer('savestate', 0, 0, 1) == 1
    if m.argument('instructions') ~= nil then
        assert(kind == 'Exec' and hits == 1, 'bounded watch needs Exec and hits 1')
        bounded_watch(address, width, save, m.integer('instructions', nil, 1, 100000000))
        return
    end
    local events = {}
    m.atTarget(function()
        m.breakpoint(address, kind, width, function(actual, size, cause)
            events[#events + 1] = {address = actual, width = size, cause = cause,
                registers = m.registers()}
            if #events == hits then
                PCSX.pauseEmulator()
                PCSX.nextTick(m.guard(function()
                    m.report('watch.json', {schema = 'psx.runtime-watch/v1', kind = kind,
                        address = address, width = width, boundary = 'before instruction', events = events})
                    m.capture(save)
                    m.finish()
                end))
                return false
            end
            return true
        end)
        PCSX.resumeEmulator()
    end)
end)
-- )EOF"
