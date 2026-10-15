-- lualoader, R"EOF(--
-- Measure repeated native continuation; differences are evidence, never masked.
local m = require 'support'
m.begin('runs,depth,limit,audio,samples,blocks,events', function()
    PCSX.pauseEmulator()
    assert(os.getenv('PSX_RUNTIME_ORIGIN_VALIDATED') == '1', 'replay requires --origin capture receipt')
    assert(not os.getenv('PSX_RUNTIME_ENTRY'), 'replay restores a state without loading an EXE')
    local snapshot, difference, timeline = require 'snapshot', require 'difference', require 'timeline'
    local original = snapshot.read(m.input('state', true), true)
    snapshot.validate(original, "execution")
    local schedule = timeline.read(m.input('schedule', true))
    local alternatePath = m.input('alternate', false)
    local alternate = alternatePath and timeline.read(alternatePath) or schedule
    timeline.compatible(schedule, alternate)
    for _, event in ipairs(schedule.samples) do
        if event.action == 'sample' then snapshot.select(original, event.path, true)
        else require('display') end
    end
    original = nil
    local runs = m.integer('runs', 2, 2, 8)
    local audio = m.integer('audio', 0, 0, 1) == 1
    local audioLimits
    if audio then
        assert(PCSX.Audio and PCSX.Audio.status().state == 0, 'replay requires an empty native Audio recorder')
        audioLimits = {samples = m.integer('samples', 441000, 1, 2646000),
            blocks = m.integer('blocks', 16384, 1, 65536), events = m.integer('events', 16384, 1, 65536)}
        local boundaries = {[schedule.frames] = true}
        for _, event in ipairs(schedule.samples) do
            if event.frame > 0 then boundaries[event.frame] = true end
        end
        local segments = 0
        for _ in pairs(boundaries) do segments = segments + 1 end
        assert(runs * segments * 5 + #schedule.samples + 7 <= 128, 'replay audio exceeds capture file limit')
    else
        for _, name in ipairs({'samples', 'blocks', 'events'}) do
            assert(not m.argument(name), name .. ' requires audio=1')
        end
    end
    local options = {depth = m.integer('depth', 16, 0, 64), limit = m.integer('limit', 1024, 1, 65536)}
    local result = {schema = 'psx.runtime-replay/v1', runs_requested = runs,
        identical_schedule = not alternatePath, runs = m.array(), checkpoints = m.array(),
        audio_requested = audio, audio_limits = audioLimits, audio_equal = audio and true or nil,
        audio_complete = false,
        equal = true, timing = 'GPU Vsync boundaries; cycles from native state',
        settings = 'receipt.json settings records fixed launch flags, fresh portable defaults and unpinned host behavior',
        limitations = {'Finite checkpoint comparison, not proof of determinism between observations.',
            'Host audio queues are not isolated; physical pad input is isolated only while an analog override is active.',
            'Native serialization/restoration and capture gaps may affect host scheduling; audio comparison is optional.',
            'Display pixels are compared only at explicit screen checkpoints; no continuous GPU history.'}}
    local baselines, baselineButtons, baselineCycles, current, frame, index = {}, {}, {}, 0, 0, 1
    local held = {{}, {}}
    local active, run
    local artifactBytes = 0
    local audioActive, audioStart, audioFirstFrame, freezeAudio = false, nil, nil, nil
    local audioBaselines = {}
    local replayPublished = false
    local function capture(name, bytes)
        artifactBytes = artifactBytes + #bytes
        assert(artifactBytes <= 120 * 1024 * 1024, 'replay capture budget exceeded')
        if name:match("%.pbuf$") then m.captureState(name, bytes)
        else m.write(name, bytes) end
    end
    local function clear()
        for slot = 1, 2 do
            for _, button in pairs(PCSX.CONSTS.PAD.BUTTON) do
                PCSX.SIO0.slots[slot].pads[1].clearOverride(button)
            end
            timeline.applyAnalog(slot, nil)
            held[slot] = {}
        end
    end
    m.onCleanup(function()
        clear()
        m.report('input-cleanup.json', {schema = 'psx.runtime-input-cleanup/v1',
            slots = {timeline.inputState(1), timeline.inputState(2)}})
    end)
    local function buttons()
        local observed = {{}, {}}
        for slot = 1, 2 do
            for name, button in pairs(PCSX.CONSTS.PAD.BUTTON) do
                observed[slot][name] = PCSX.SIO0.slots[slot].pads[1].getButton(button)
            end
        end
        local inputs = {}
        for slot = 1, 2 do
            local input = timeline.inputState(slot)
            if input.override_active then
                -- The native poll counter is cumulative host observation, outside saved state.
                input.poll_count = nil
                inputs[slot] = input
            else inputs[slot] = {override_active = false} end
        end
        return m.json({buttons = observed, slots = inputs})
    end
    local function divergence(boundary, detail, bytes)
        freezeAudio('divergence')
        result.equal = false
        if result.first_divergence then return end
        capture('divergence.pbuf', bytes or m.stateBytes())
        detail.run, detail.frame, detail.boundary = current, frame, boundary
        detail.capture = 'divergence.pbuf'
        result.first_divergence = detail
    end
    local function compareFile(beforePath, afterPath)
        local before = assert(io.open(beforePath, 'rb'))
        local after, problem = io.open(afterPath, 'rb')
        if not after then before:close(); error(problem) end
        local report = {equal = true, before_bytes = assert(before:seek('end')), after_bytes = assert(after:seek('end'))}
        assert(before:seek('set')); assert(after:seek('set'))
        local ok, err = pcall(function()
            local offset = 0
            while true do
                local a, aError = before:read(65536)
                local b, bError = after:read(65536)
                assert(not aError and not bError, aError or bError)
                a, b = a or '', b or ''
                if a ~= b then
                    report.equal = false
                    for index = 1, math.max(#a, #b) do
                        if a:byte(index) ~= b:byte(index) then
                            report.first_byte = offset + index - 1
                            report.before, report.after = a:byte(index), b:byte(index)
                            return
                        end
                    end
                end
                if #a == 0 then return end
                offset = offset + #a
            end
        end)
        local closedBefore, closedAfter = before:close(), after:close()
        assert(ok, err)
        assert(closedBefore and closedAfter, 'replay comparison file close failed')
        return report
    end
    freezeAudio = function(reason, compare)
        if not audioActive then return end
        local h = PCSX.Audio
        h.stop()
        audioActive = false
        local ordinal = #run.audio + 1
        local segment = {index = ordinal, first_frame = audioFirstFrame, last_frame = frame,
            begin_cycles = audioStart, end_cycles = m.exact(PCSX.getCPUCycles()), reason = reason}
        run.audio[ordinal] = segment
        local firstCapture = #m.captures
        local exported = require('pcm').export(string.format('run-%02d-segment-%02d-', current, ordinal))
        segment.files, segment.native, segment.complete = exported.files, exported.native, exported.complete
        for index = firstCapture + 1, #m.captures do artifactBytes = artifactBytes + m.captures[index].bytes end
        assert(artifactBytes <= 120 * 1024 * 1024, 'replay capture budget exceeded')
        assert(exported.complete, 'incomplete replay audio; inspect retained native prefix')
        assert(exported.native.frames > 0, 'replay sound interval produced no PCM frames')
        h.clear()
        if compare == false then return end
        if current == 1 then audioBaselines[ordinal] = segment; return end
        local baseline = audioBaselines[ordinal]
        local comparison = {segment = ordinal, available = baseline ~= nil, equal = false}
        segment.comparison = comparison
        if baseline then
            comparison.boundary_equal = segment.first_frame == baseline.first_frame and segment.last_frame == baseline.last_frame
            comparison.cycles_equal = segment.begin_cycles == baseline.begin_cycles and segment.end_cycles == baseline.end_cycles
            comparison.settings_equal = m.json(segment.native.settings) == m.json(baseline.native.settings)
            comparison.counts_equal = segment.native.frames == baseline.native.frames and
                segment.native.blocks == baseline.native.blocks and segment.native.events == baseline.native.events
            comparison.metadata_equal = m.json(segment.native) == m.json(baseline.native)
            comparison.pcm = compareFile(baseline.files.pcm, segment.files.pcm)
            comparison.blocks = compareFile(baseline.files.blocks, segment.files.blocks)
            comparison.events = compareFile(baseline.files.events, segment.files.events)
            comparison.equal = comparison.boundary_equal and comparison.cycles_equal and comparison.settings_equal and
                comparison.counts_equal and comparison.metadata_equal and comparison.pcm.equal and
                comparison.blocks.equal and comparison.events.equal
        end
        if not comparison.equal then
            result.audio_equal = false
            divergence('audio', {segment = ordinal, audio = comparison})
        end
    end
    local function beginAudio()
        if not audio or audioActive then return end
        m.requireIdleCapture()
        assert(PCSX.Audio.status().state == 0, 'replay Audio recorder was not cleared')
        audioStart, audioFirstFrame = m.exact(PCSX.getCPUCycles()), frame
        PCSX.Audio.begin(audioLimits.samples, audioLimits.blocks, audioLimits.events)
        audioActive = true
    end
    if audio then
        result.audio_scope = 'Native PCM/settings/counts and callback/access journals between checked checkpoints; capture gaps are unobserved, host queues are not restored or drained.'
        m.onCleanup(function()
            local ok, err = pcall(freezeAudio, 'mission cleanup', false)
            if not replayPublished then
                result.failure = 'mission failed; inspect logs and retained captures'
                m.report('replay.json', result)
                replayPublished = true
            end
            assert(ok, err)
        end)
    end
    local function sample(event)
        freezeAudio('checkpoint')
        local bytes = m.stateBytes()
        local value, kind, repeated = snapshot.decodeSelected(bytes, event.path, true, true)
        local cycles = snapshot.decodeSelected(bytes, 'registers.cycle', true, true)
        local observed = buttons()
        if current == 1 then
            local name = string.format('baseline-%03d.pbuf', event.index)
            capture(name, bytes)
            baselines[event.index], baselineButtons[event.index] = name, observed
            baselineCycles[event.index] = cycles
            result.checkpoints[#result.checkpoints + 1] = {index = event.index, path = event.path,
                frame = frame, capture = name, cycles = cycles, buttons = observed}
        else
            local before = snapshot.readSelected(baselines[event.index], event.path, true, true)
            local report = difference.compare(before, value, kind, repeated, event.path, options)
            local buttonsEqual = observed == baselineButtons[event.index]
            local cyclesEqual = cycles == baselineCycles[event.index]
            run.comparisons[#run.comparisons + 1] = {index = event.index, frame = frame,
                cycles = cycles, cycles_equal = cyclesEqual, state = report,
                buttons_equal = buttonsEqual, buttons = observed}
            if not report.equal or not buttonsEqual or not cyclesEqual then
                divergence('checkpoint', {index = event.index, path = event.path,
                    state_equal = report.equal, buttons_equal = buttonsEqual, cycles_equal = cyclesEqual}, bytes)
            end
        end
    end
    local screens = {}
    local function screen(event)
        freezeAudio('screen checkpoint')
        local display = require 'display'
        local bytes, metadata = display.capture()
        local observed, cycles = buttons(), tostring(PCSX.getCPUCycles())
        if current == 1 then
            local name = string.format('baseline-%03d.bin', event.index)
            capture(name, bytes)
            screens[event.index] = {name = name, metadata = metadata, cycles = cycles, buttons = observed}
            result.checkpoints[#result.checkpoints + 1] = {index = event.index, kind = 'display',
                frame = frame, capture = name, cycles = cycles, buttons = observed, metadata = metadata}
        else
            local baseline = screens[event.index]
            local file = assert(io.open(baseline.name, 'rb'))
            local before, problem = file:read(1572865); file:close()
            assert(not problem, problem)
            before = before or '' -- Empty native displays have an empty capture.
            local report = display.compare(before, bytes, baseline.metadata, metadata, options)
            local buttonsEqual, cyclesEqual = observed == baseline.buttons, cycles == baseline.cycles
            run.comparisons[#run.comparisons + 1] = {index = event.index, frame = frame, cycles = cycles,
                cycles_equal = cyclesEqual, display = report, buttons_equal = buttonsEqual, buttons = observed}
            if not report.equal or not buttonsEqual or not cyclesEqual then
                if not result.first_divergence then
                    capture('divergence.bin', bytes)
                    m.report('divergence.json', metadata)
                end
                divergence('checkpoint', {index = event.index, kind = 'display',
                    display_equal = report.equal, buttons_equal = buttonsEqual, cycles_equal = cyclesEqual})
            end
        end
    end
    local advance, begin
    begin = function()
        freezeAudio('before restore')
        clear()
        current, frame, index = current + 1, 0, 1
        active = current == 1 and schedule or alternate
        assert(m.restore())
        run = {number = current, comparisons = m.array(), inputs = m.array(), audio = audio and m.array() or nil, start_cycles = tostring(PCSX.getCPUCycles())}
        result.runs[#result.runs + 1] = run
        if current > 1 then
            run.start_cycles_equal = run.start_cycles == result.runs[1].start_cycles
            if not run.start_cycles_equal then divergence('start', {cycles_equal = false}) end
        end
        advance()
    end
    local function stop(event, observation)
        freezeAudio('stop')
        if audio and current > 1 and #run.audio ~= #audioBaselines then
            result.audio_equal = false
            divergence('audio', {segment_count_equal = false, baseline_segments = #audioBaselines, segments = #run.audio})
        end
        run.stop_frame, run.stop_cycles = frame, tostring(PCSX.getCPUCycles())
        run.reason = observation and 'condition matched' or 'scheduled stop'
        run.condition = observation
        if current > 1 then
            run.stop_cycles_equal = run.stop_cycles == result.runs[1].stop_cycles
            run.stop_frame_equal = frame == result.runs[1].stop_frame
            if not run.stop_cycles_equal or not run.stop_frame_equal then
                divergence('stop', {cycles_equal = run.stop_cycles_equal, frame_equal = run.stop_frame_equal})
            end
        end
        clear()
        if current < runs then begin()
        else
            result.runs_completed = current
            if audio then result.audio_complete = true end
            m.write('state.proto', snapshot.schema)
            m.report('replay.json', result)
            replayPublished = true
            m.finish()
        end
    end
    local function condition(event)
        if not event.condition then return nil end
        return require('condition').observe(event.condition)
    end
    advance = function()
        if audioActive and PCSX.Audio.status().failures ~= 0 then
            freezeAudio('native recorder failure')
            error('replay Audio recorder failed')
        end
        while index <= #active.events and active.events[index].frame == frame do
            local event = active.events[index]
            index = index + 1
            if event.action == 'sample' then sample(event)
            elseif event.action == 'screen' then screen(event)
            elseif event.action == 'analog' then
                timeline.applyAnalog(event.slot, event.input)
                run.inputs[#run.inputs + 1] = {frame = frame, order = index - 1, slot = event.slot,
                    input = timeline.inputState(event.slot)}
            elseif event.action == 'buttons' then
                local pad = PCSX.SIO0.slots[event.slot].pads[1]
                for _, button in ipairs(held[event.slot]) do pad.clearOverride(button) end
                held[event.slot] = event.buttons
                for _, button in ipairs(event.buttons) do pad.setOverride(button) end
            else
                local observation = condition(event)
                if observation and not observation.matched then
                    clear()
                    m.report('condition.json', {schema = 'psx.runtime-condition/v1', frame = frame,
                        run = current, observation = observation, registers = m.registers()})
                    error('stop condition unmet at frame deadline')
                end
                stop(event, observation)
                return
            end
        end
        local nextEvent = active.events[index]
        if nextEvent and nextEvent.action == 'stop' and nextEvent.condition and frame >= 1 then
            local observation = condition(nextEvent)
            if observation.matched then stop(nextEvent, observation); return end
        end
        beginAudio()
        PCSX.resumeEmulator()
    end
    m.event('GPU::Vsync', function()
        PCSX.pauseEmulator()
        frame = frame + 1
        assert(frame <= active.frames, 'replay frame bound exceeded')
        PCSX.nextPause(m.guard(advance))
    end)
    begin()
end)
-- )EOF"
