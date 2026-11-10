-- lualoader, R"EOF(--
-- Export frozen native PCM, callback boundaries and already-named SPU records.
local m, bit = require 'support', require 'bit'
local M = {}
local block_flags = {mono=1, muted=2, submitted=4}
local faults = {'frames', 'blocks', 'events', 'thread', 'bounds', 'clock',
    'discontinuity', 'streaming-loss', 'output', 'workers', 'gains', 'envelope-inputs', 'scheduling'}

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

local function writeJournalBeforeValidation(name, count, read, byte_limit, validate, compressed)
    return m.writeJournal(name, compressed, byte_limit, function(write)
        for first = 0, count - 1, 1024 do
            local records = read(first, math.min(1024, count - first))
            for n, record in ipairs(records) do
                write(m.json(record) .. '\n')
                validate(record, first + n - 1)
            end
        end
    end)
end

function M.export(prefix)
    if prefix == nil then prefix = '' end
    assert(type(prefix) == 'string' and (#prefix == 0 or
        (#prefix <= 200 and prefix:match('^[%a%d][%w_.-]*$'))), 'invalid PCM capture prefix')
    local files = {pcm = prefix .. 'audio.f32', wav = prefix .. 'audio.wav',
        blocks = prefix .. 'blocks.ndjson', workers = prefix .. 'workers.ndjson', gains = prefix .. 'gains.ndjson',
        envelope_inputs = prefix .. 'envelope_inputs.ndjson', scheduling = prefix .. 'scheduling.ndjson',
        events = prefix .. 'spu.ndjson', report = prefix .. 'audio.json'}
    local h = assert(PCSX.Audio, 'native audio capture binding required')
    local status = h.status()
    local result = {schema = 'psx.native-audio/v11', native = status, complete = false, files = files,
        failures = m.array(), timing = {
            pcm = 'stereo frame offsets at the SDL input boundary, including underflow zero-fill',
            blocks = 'last CPU publication observed by callback; not per-sample cycles',
            main_gain = 'signed Q15 pair consumed by the output chunk after summing the two streams, before mono',
            events = 'CPU access cycles with observed capture cursor; not audible-effect positions',
            feed_waits = 'queue occupancy under its mutex; initial blocked predicate and host wait_for duration, excluding initial lock acquisition and copying; not CPU time or proof of OS sleep',
            workers = 'completed batches and acknowledged enqueue ranges; missing prefixes and unconfirmed tails remain unknown',
            stop_samples = 'first sample in each channel loop consuming a true ADSR stop input; -1 means none observed',
            pre_sample_steps = 'per-channel installed 16.16 steps before sample loops, not simultaneous or per-sample modulation observations',
            first_reverb_sample = 'first sample whose existing reverb base check admits active work; -1 means none observed, not master write enable or an activation edge',
            gains = 'first consumed dry stereo gain pair per voice and later changes at source frames; separate left/right reads, not an atomic snapshot or CPU-write positions; unconfirmed worker ranges remain unknown',
            envelope_inputs = 'first consumed phase controls per voice and later changes at source frames; existing phase-specific reads, not an atomic register snapshot, envelope state import or CPU-write positions; unconfirmed worker ranges remain unknown',
            scheduling = 'recorder observation order across CPU, producer, callback and null clock; enqueue/dequeue hooks hold the existing queue mutex, other anchors are not atomic effect positions; initial producer staging is observed at its next batch boundary',
            voice_offsets = 'persistent stream-0 frame coordinates; underflow consumes none, fast-forward discards advance them',
            queues = 'begin/end snapshots; existing queued and partial mixer work is not flushed',
            stop = 'freezes output observations; does not drain queues or finish a DSP tail'}}
    local ok, err = pcall(function()
        assert(status.version == 11 and status.state == 2 and status.rate == 44100 and
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
        assert(status.envelope_input_limit == status.event_limit, 'native envelope input capacity differs from event limit')
        integer(status.envelope_inputs, 0, status.envelope_input_limit, 'envelope input count')
        integer(status.scheduling_limit, 1, 1048576, 'scheduling limit')
        integer(status.scheduling, 0, status.scheduling_limit, 'scheduling count')
        integer(status.failures, 0, 8191, 'failure bits')
        integer(status.first_failure, 0, 4096, 'first failure')
        integer(status.dropped_events, 0, 4294967295, 'dropped events')
        integer(status.dropped_workers, 0, 4294967295, 'dropped workers')
        integer(status.dropped_gains, 0, 4294967295, 'dropped gains')
        integer(status.dropped_envelope_inputs, 0, 4294967295, 'dropped envelope inputs')
        integer(status.dropped_scheduling, 0, 4294967295, 'dropped scheduling observations')
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
        assert(status.failures ~= 0 or (status.dropped_events == 0 and status.dropped_workers == 0 and
            status.dropped_gains == 0 and status.dropped_envelope_inputs == 0 and status.dropped_scheduling == 0 and status.dropped_frames == '0' and
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
            integer(b.main_gain_left, -32768, 32767, 'main left gain')
            integer(b.main_gain_right, -32768, 32767, 'main right gain')
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
            integer(w.first_reverb_sample, -1, w.frames - 1, 'worker first active reverb sample')
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
        local envelope_previous = {}
        result.envelope_inputs = writeJournalBeforeValidation(files.envelope_inputs, status.envelope_inputs,
            h.envelopeInputs, 16777216, function(e, index)
                assert(e.index == index, 'noncontiguous native envelope input index')
                local voice = integer(e.voice, 0, 23, 'envelope input voice')
                local source = decimal(e.voice_offset)
                local phase = integer(e.phase, 0, 3, 'envelope input phase')
                integer(e.rate, 0, ({127, 15, 127, 31})[phase + 1], 'envelope input rate')
                integer(e.exponential, 0, 1, 'envelope input exponential')
                integer(e.increasing, 0, 1, 'envelope input increasing')
                integer(e.sustain_level, -1, 15, 'envelope input sustain level')
                assert((phase ~= 0 or e.increasing == 1) and
                    (phase ~= 1 or (e.exponential == 1 and e.increasing == 0)) and
                    (phase ~= 3 or e.increasing == 0) and
                    ((phase == 1 and e.sustain_level >= 0) or (phase ~= 1 and e.sustain_level == -1)),
                    'inconsistent native phase controls')
                local before = envelope_previous[voice]
                assert(not before or (less(before.voice_offset, source) and
                    (before.phase ~= e.phase or before.rate ~= e.rate or before.exponential ~= e.exponential or
                    before.increasing ~= e.increasing or before.sustain_level ~= e.sustain_level)),
                    'native envelope input source regression or unchanged controls')
                envelope_previous[voice] = e
            end)
        local enqueue, waiting
        local large_schedule = status.scheduling > 65536
        result.scheduling = writeJournalBeforeValidation(files.scheduling, status.scheduling, h.scheduling,
            large_schedule and 67108864 or 33554432, function(s, index)
                assert(s.index == index, 'noncontiguous native scheduling index')
                local kind = integer(s.kind, 1, 8, 'scheduling kind')
                if kind == 1 then
                    local staged = integer(s.staged_frames, 0, 765, 'producer staged frames')
                    integer(s.batch_phase, 0, 17, 'producer batch phase')
                    assert(s.frames == 45 and staged == s.batch_phase * 45,
                        'native producer batch position mismatch')
                    assert(plus(decimal(s.enqueue_offset), staged) == decimal(s.voice_offset),
                        'native producer source position mismatch')
                elseif kind == 2 then
                    decimal(s.voice_offset)
                    integer(s.frames, 1, 2048, 'voice enqueue frames')
                    integer(s.queued_before, 0, 2048, 'voice enqueue queue before')
                    integer(s.blocked, 0, 1, 'voice enqueue blocked')
                    assert(not enqueue and (s.blocked == 1) == (s.queued_before + s.frames >= 2048),
                        'native voice enqueue initial predicate mismatch')
                    enqueue = {voice = s.voice_offset, frames = s.frames, queued = s.queued_before,
                        blocked = s.blocked}
                elseif kind == 3 then
                    decimal(s.voice_offset)
                    integer(s.frames, 1, 2048, 'voice enqueue frames')
                    integer(s.queued_after, 0, 2048, 'voice enqueue queue after')
                    integer(s.accepted, 0, 1, 'voice enqueue accepted')
                    local elapsed = decimal(s.wait_nanoseconds)
                    assert(enqueue and enqueue.voice == s.voice_offset and enqueue.frames == s.frames,
                        'native voice enqueue result lacks matching entry')
                    assert(s.queued_after == enqueue.queued + s.accepted * s.frames and
                        ((s.accepted == 1 and s.queued_after < 2048) or
                        (s.accepted == 0 and enqueue.queued + s.frames >= 2048)),
                        'native voice enqueue result mismatch')
                    assert((enqueue.blocked == 1 and elapsed ~= '0') or
                        (enqueue.blocked == 0 and elapsed == '0'), 'native voice enqueue duration mismatch')
                    enqueue = nil
                elseif kind == 4 then
                    decimal(s.voice_offset)
                    integer(s.requested_frames, 1, 2048, 'voice dequeue request')
                    integer(s.frames, 0, s.requested_frames, 'voice dequeue frames')
                    integer(s.queued_before, 0, 2048, 'voice dequeue queue before')
                    integer(s.queued_after, 0, 2048, 'voice dequeue queue after')
                    integer(s.discard, 0, 1, 'voice dequeue discard')
                    assert(s.frames == math.min(s.requested_frames, s.queued_before) and
                        s.queued_after == s.queued_before - s.frames, 'native voice dequeue count mismatch')
                    if enqueue then
                        assert(enqueue.queued == s.queued_before, 'native voice wait queue changed without observation')
                        enqueue.queued = s.queued_after
                    end
                elseif kind == 5 then
                    integer(s.frames, 1, 4294967295, 'master advance frames')
                    integer(s.speed, 1, 1024, 'master effective speed')
                    integer(s.old_frames, 0, 4294967295, 'master previous frames')
                    integer(s.new_frames, 0, 4294967295, 'master resulting frames')
                    integer(s.goal, 0, 4294967295, 'master goal')
                    integer(s.action, 0, 2, 'master action')
                    integer(s.source, 0, 1, 'master source')
                    assert(s.new_frames == (s.old_frames + s.frames * s.speed) % 4294967296,
                        'native master advancement mismatch')
                    local diff = bit.tobit(s.goal - s.old_frames)
                    assert(s.action == 0 or (s.action == 1 and diff > 0) or
                        (s.action == 2 and diff <= 0), 'native master wake predicate mismatch')
                elseif kind == 6 then
                    local cycle, previous_cycle = decimal(s.cycle), decimal(s.previous_cycle)
                    assert(not less(cycle, previous_cycle), 'native CPU scheduling cycle regression')
                    for _, key in ipairs({'previous_audio_frames', 'target', 'master_frames'}) do
                        integer(s[key], 0, 4294967295, 'CPU scheduling ' .. key)
                    end
                    integer(s.action, 0, 2, 'CPU scheduling action')
                    local diff = bit.tobit(s.target - s.master_frames)
                    local action = diff > 0 and 1 or (diff < -11025 and 2 or 0)
                    assert(s.action == action, 'native CPU throttle predicate mismatch')
                else
                    decimal(s.cycle)
                    integer(s.goal, 0, 4294967295, 'CPU wait goal')
                    integer(s.initial_trigger, 0, 4294967295, 'CPU initial wait trigger')
                    if kind == 7 then
                        assert(not waiting, 'nested native CPU audio wait')
                        waiting = s
                    else
                        assert(waiting and waiting.cycle == s.cycle and waiting.goal == s.goal and
                            waiting.initial_trigger == s.initial_trigger, 'native CPU wait return lacks matching entry')
                        waiting = nil
                    end
                end
            end, large_schedule)
        files.scheduling = result.scheduling.file
        result.pending_scheduling = {enqueue = enqueue ~= nil, cpu_wait = waiting ~= nil}
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
        assert(result.blocks.complete and result.workers.complete and result.gains.complete and
            result.envelope_inputs.complete and result.scheduling.complete and result.events.complete,
            'incomplete native journal export')
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
