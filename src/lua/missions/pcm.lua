-- lualoader, R"EOF(--
-- Export frozen native PCM, callback boundaries and already-named SPU records.
local m, bit = require 'support', require 'bit'
local M = {}
local block_flags = {mono=1, muted=2, submitted=4}
local faults = {'frames', 'blocks', 'events', 'thread', 'bounds', 'clock',
    'discontinuity', 'streaming-loss', 'output', 'workers', 'gains'}

local function integer(value, low, high, label)
    assert(type(value) == 'number' and value % 1 == 0 and value >= low and value <= high,
        'invalid native ' .. label)
    return value
end

local function decimal(value)
    assert(type(value) == 'string' and value:match('^%d+$') and
        (value == '0' or value:sub(1, 1) ~= '0') and
        (#value < 20 or (#value == 20 and value <= '18446744073709551615')),
        'native uint64 must be a canonical decimal string')
    return value
end

local function less(a, b)
    return #a < #b or (#a == #b and a < b)
end

local function plus(value, count)
    local result = m.exact(m.uint64(value) + m.uint64(tostring(count)))
    assert(not less(result, value), 'native voice coordinate overflow')
    return result
end

local function writeJournalBeforeValidation(name, count, read, byte_limit, validate)
    local file = assert(io.open(name, 'wb'))
    local bytes, rows = 0, 0
    local ok, err = pcall(function()
        for first = 0, count - 1, 1024 do
            local records = read(first, math.min(1024, count - first))
            for n, record in ipairs(records) do
                local line = m.json(record) .. '\n'
                assert(bytes + #line <= byte_limit, 'audio journal exceeds byte limit')
                assert(file:write(line)); bytes, rows = bytes + #line, rows + 1
                validate(record, first + n - 1)
            end
        end
    end)
    local closed, why = file:close()
    m.captures[#m.captures + 1] = {path = name, bytes = bytes}
    assert(closed, why)
    return {rows = rows, bytes = bytes, complete = ok, error = not ok and tostring(err) or nil}
end

function M.export(prefix)
    if prefix == nil then prefix = '' end
    assert(type(prefix) == 'string' and (#prefix == 0 or
        (#prefix <= 200 and prefix:match('^[%a%d][%w_.-]*$'))), 'invalid PCM capture prefix')
    local files = {pcm = prefix .. 'audio.f32', wav = prefix .. 'audio.wav',
        blocks = prefix .. 'blocks.ndjson', workers = prefix .. 'workers.ndjson', gains = prefix .. 'gains.ndjson',
        events = prefix .. 'spu.ndjson', report = prefix .. 'audio.json'}
    local h = assert(PCSX.Audio, 'native audio capture binding required')
    local status = h.status()
    local result = {schema = 'psx.native-audio/v7', native = status, complete = false, files = files,
        failures = m.array(), timing = {
            pcm = 'stereo frame offsets at the SDL input boundary, including underflow zero-fill',
            blocks = 'last CPU publication observed by callback; not per-sample cycles',
            events = 'CPU access cycles with observed capture cursor; not audible-effect positions',
            feed_waits = 'queue occupancy under its mutex; initial blocked predicate and host wait_for duration, excluding initial lock acquisition and copying; not CPU time or proof of OS sleep',
            workers = 'completed batches and acknowledged enqueue ranges; missing prefixes and unconfirmed tails remain unknown',
            stop_samples = 'first sample in each channel loop consuming a true ADSR stop input; -1 means none observed',
            pre_sample_steps = 'per-channel installed 16.16 steps before sample loops, not simultaneous or per-sample modulation observations',
            gains = 'first consumed dry stereo gain pair per voice and later changes at source frames; separate left/right reads, not an atomic snapshot or CPU-write positions; unconfirmed worker ranges remain unknown',
            voice_offsets = 'persistent stream-0 frame coordinates; underflow consumes none, fast-forward discards advance them',
            queues = 'begin/end snapshots; existing queued and partial mixer work is not flushed',
            stop = 'freezes output observations; does not drain queues or finish a DSP tail'}}
    local ok, err = pcall(function()
        assert(status.version == 7 and status.state == 2 and status.rate == 44100 and
            status.channels == 2 and status.format == 'f32le' and
            status.stage == 'post-mixer-mute-mono/pre-SDL-conversion', 'unsupported frozen audio ABI')
        integer(status.frame_limit, 1, 2646000, 'frame limit')
        integer(status.block_limit, 1, 65536, 'block limit')
        integer(status.event_limit, 1, 65536, 'event limit')
        integer(status.frames, 0, status.frame_limit, 'frame count')
        integer(status.blocks, 0, status.block_limit, 'block count')
        integer(status.events, 0, status.event_limit, 'event count')
        assert(status.worker_limit == status.event_limit, 'native worker capacity differs from event limit')
        integer(status.workers, 0, status.worker_limit, 'worker count')
        assert(status.gain_limit == status.event_limit, 'native gain capacity differs from event limit')
        integer(status.gains, 0, status.gain_limit, 'gain count')
        integer(status.failures, 0, 2047, 'failure bits')
        integer(status.first_failure, 0, 1024, 'first failure')
        integer(status.dropped_events, 0, 4294967295, 'dropped events')
        integer(status.dropped_workers, 0, 4294967295, 'dropped workers')
        integer(status.dropped_gains, 0, 4294967295, 'dropped gains')
        for _, key in ipairs({'begin_cycle', 'end_cycle', 'dropped_frames', 'streaming_dropped'}) do
            decimal(status[key])
        end
        for _, key in ipairs({'initial_voice_queue', 'initial_disc_queue', 'final_voice_queue', 'final_disc_queue'}) do
            integer(status[key], 0, 4294967295, key)
        end
        assert(type(status.settings) == 'table', 'missing native mixer settings')
        integer(status.settings.schema_version, 2, 2, 'setting schema version')
        integer(status.settings.reserved, 0, 0, 'reserved setting')
        for _, key in ipairs({'volume', 'interpolation', 'mute', 'mono', 'streaming',
            'null_sync', 'forced_irq', 'voice_mute', 'voice_solo', 'xa'}) do
            integer(status.settings[key], 0, 4294967295, 'setting ' .. key)
        end
        for _, key in ipairs({'key_on_delay', 'speed'}) do
            integer(status.settings[key], -2147483648, 2147483647, 'setting ' .. key)
        end
        for _, key in ipairs({'configured_backend', 'configured_device', 'active_driver'}) do
            assert(type(status.settings[key]) == 'string', 'missing native ' .. key)
        end
        for i, name in ipairs(faults) do
            if bit.band(status.failures, 2 ^ (i - 1)) ~= 0 then result.failures[#result.failures + 1] = name end
        end
        assert((status.failures == 0 and status.first_failure == 0) or
            (status.first_failure > 0 and bit.band(status.first_failure, status.first_failure - 1) == 0 and
            bit.band(status.failures, status.first_failure) ~= 0), 'inconsistent native first failure')
        assert(status.failures ~= 0 or (status.dropped_events == 0 and status.dropped_workers == 0 and status.dropped_gains == 0 and status.dropped_frames == '0' and
            status.streaming_dropped == '0'), 'native loss without failure')

        for _, capture in ipairs(m.captures) do
            assert(capture.path ~= files.pcm and capture.path ~= files.wav, 'duplicate capture')
        end
        local exported = h.exportPCM(files.pcm, files.wav)
        assert(exported.frames == status.frames and exported.pcm_bytes == status.frames * 8 and
            exported.wav_bytes == exported.pcm_bytes + 58, 'native PCM export counts differ')
        m.captures[#m.captures + 1] = {path = files.pcm, bytes = exported.pcm_bytes}
        m.captures[#m.captures + 1] = {path = files.wav, bytes = exported.wav_bytes}
        result.pcm_bytes, result.duration = exported.pcm_bytes, status.frames / 44100
        local offset, previous = 0, status.begin_cycle
        local voice_fill, disc_fill = 0, 0
        local voice_end = '0'
        result.blocks = writeJournalBeforeValidation(files.blocks, status.blocks, h.blocks, 16777216, function(b, index)
            assert(b.index == index and b.offset == offset, 'noncontiguous native PCM block')
            integer(b.frames, 1, 2048, 'block frames')
            integer(b.voice_frames, 0, b.frames, 'voice frames')
            integer(b.disc_frames, 0, b.frames, 'disc frames')
            local voice = decimal(b.voice_offset)
            assert(not less(voice, voice_end), 'native voice queue cursor regression')
            voice_end = plus(voice, b.voice_frames)
            integer(b.flags, 0, 7, 'block flags')
            local cycle = decimal(b.observed_cpu_cycle)
            assert(not less(cycle, previous), 'native block clock regression')
            -- Clock failure may freeze with an end cycle before the retained prefix.
            assert(bit.band(status.failures, 32) ~= 0 or not less(status.end_cycle, cycle),
                'native block after end cycle')
            assert(bit.band(b.flags, block_flags.submitted) ~= 0 or bit.band(status.failures, 256) ~= 0,
                'failed SDL submission without native failure')
            offset, previous = offset + b.frames, cycle
            assert(offset <= status.frames, 'block exceeds retained PCM')
            voice_fill, disc_fill = voice_fill + b.frames - b.voice_frames, disc_fill + b.frames - b.disc_frames
        end)
        result.zero_fill = {voice_frames = voice_fill, disc_frames = disc_fill}
        local worker_end, unconfirmed = nil, 0
        result.workers = writeJournalBeforeValidation(files.workers, status.workers, h.workers, 50331648, function(w, index)
            assert(w.index == index, 'noncontiguous native worker index')
            local voice, enqueue = decimal(w.voice_offset), decimal(w.enqueue_offset)
            integer(w.frames, 1, 2048, 'worker frames')
            integer(w.key_on_mask, 0, 16777215, 'worker key-on mask')
            assert(type(w.pre_sample_steps) == 'table' and #w.pre_sample_steps == 24, 'worker requires 24 installed steps')
            for _, step in ipairs(w.pre_sample_steps) do integer(step, -2147483648, 2147483647, 'worker installed step') end
            assert(type(w.stop_samples) == 'table' and #w.stop_samples == 24, 'worker requires 24 stop sample indices')
            for _, sample in ipairs(w.stop_samples) do integer(sample, -1, w.frames - 1, 'worker stop sample') end
            integer(w.enqueue_frames, 0, 2048, 'worker enqueue frames')
            assert(worker_end == nil or worker_end == voice, 'noncontiguous native worker source ranges')
            worker_end = plus(voice, w.frames)
            if w.enqueue_frames == 0 then
                assert(enqueue == '0', 'unconfirmed worker has enqueue position')
                unconfirmed = unconfirmed + 1
            else
                assert(not less(voice, enqueue) and not less(plus(enqueue, w.enqueue_frames), worker_end),
                    'native worker outside acknowledged enqueue')
            end
        end)
        result.unconfirmed_workers = unconfirmed
        local gain_previous = {}
        result.gains = writeJournalBeforeValidation(files.gains, status.gains, h.gains, 16777216, function(g, index)
            assert(g.index == index, 'noncontiguous native gain index')
            local voice = integer(g.voice, 0, 23, 'gain voice')
            local source = decimal(g.voice_offset)
            integer(g.left, -2147483648, 2147483647, 'left gain')
            integer(g.right, -2147483648, 2147483647, 'right gain')
            local before = gain_previous[voice]
            assert(not before or (less(before.voice_offset, source) and
                (before.left ~= g.left or before.right ~= g.right)), 'native gain source regression or unchanged pair')
            gain_previous[voice] = g
        end)
        previous = status.begin_cycle
        local observed = '0'
        local pending_feed
        result.events = writeJournalBeforeValidation(files.events, status.events, h.events, 16777216, function(e, index)
            assert(e.id == index + 1, 'noncontiguous native SPU event')
            integer(e.kind, 1, 7, 'event kind')
            local cycle, frame, parent = decimal(e.cycle), decimal(e.observed_frame), decimal(e.parent)
            assert(not less(cycle, previous) and not less(frame, observed) and
                not less(tostring(status.frames), frame), 'native event cursor regression or overflow')
            assert(bit.band(status.failures, 32) ~= 0 or not less(status.end_cycle, cycle),
                'native event after end cycle')
            assert(less(parent, tostring(e.id)), 'native event parent must be absent or earlier')
            previous, observed = cycle, frame
            assert(not pending_feed or e.kind == 7, 'native feed lacks adjacent wait observation')
            if e.kind == 3 then
                for _, key in ipairs({'source_rate', 'output_frames', 'source_frames'}) do
                    integer(e[key], 0, 4294967295, key)
                end
                for _, key in ipairs({'stereo', 'accepted', 'cdda'}) do integer(e[key], 0, 1, key) end
                pending_feed = e
            elseif e.kind == 7 then
                assert(pending_feed and parent == tostring(pending_feed.id) and cycle == pending_feed.cycle and
                    e.accepted == pending_feed.accepted, 'native feed wait parent or result mismatch')
                local before = integer(e.queued_before, 0, 16383, 'feed queue before')
                local after = integer(e.queued_after, 0, 16383, 'feed queue after')
                local batch = integer(pending_feed.output_frames, 1, 16383, 'feed batch')
                local blocked = integer(e.blocked, 0, 1, 'feed blocked')
                local elapsed = decimal(e.wait_nanoseconds)
                assert((blocked == 1) == (before + batch >= 16384), 'native feed initial predicate mismatch')
                assert((blocked == 1 and elapsed ~= '0') or (blocked == 0 and elapsed == '0'),
                    'native feed wait duration mismatch')
                assert(after <= before + e.accepted * batch and
                    ((e.accepted == 1 and after >= batch) or
                     (e.accepted == 0 and blocked == 1 and after + batch >= 16384)),
                    'native feed queue result mismatch')
                assert(blocked == 1 or after == before + batch, 'native immediate feed queue changed')
                pending_feed = nil
            else
                integer(e.address, 0, 4294967295, 'event address')
                integer(e.value, 0, 4294967295, 'event value')
                assert(e.width == 8 or e.width == 16 or e.width == 32, 'invalid native SPU access width')
            end
        end)
        assert(not pending_feed or status.failures ~= 0, 'unfinished native feed wait pair')
        assert(result.blocks.complete and result.workers.complete and result.gains.complete and result.events.complete, 'incomplete native journal export')
        assert(offset == status.frames, 'native PCM not covered by blocks')
        assert(bit.band(status.failures, 32) ~= 0 or not less(status.end_cycle, status.begin_cycle),
            'native end clock regression without failure')
        result.complete = status.failures == 0
    end)
    if not ok then result.export_error = tostring(err) end
    m.report(files.report, result)
    return result
end

return M
-- )EOF"
