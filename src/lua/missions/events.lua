-- lualoader, R"EOF(--
-- Export frozen native history without converting uint64 identities or cycles to doubles.
local m, bit = require 'support', require 'bit'
local M = {}
local names = {'register-attempt', 'register-result', 'dma-start', 'dma-data',
    'dma-header', 'dma-complete', 'dma-irq', 'dma-deferred', 'dma-initial',
    'schedule', 'dispatch', 'irq-assert', 'irq-clear', 'irq-accept',
    'timer-before', 'timer-after', 'timer-read', 'timer-write', 'timer-gate',
    'discontinuity', 'dma-end', 'timer-rate', 'timer-initial', 'schedule-initial',
    'irq-initial', 'dma-device', 'dma-buffer', 'dma-cancel', 'dma-peek',
    'gpu-context', 'gpu-environment', 'gpu-input', 'gpu-result',
    'cd-context', 'cd-port', 'cd-command', 'cd-schedule', 'cd-response', 'cd-irq',
    'cd-media', 'cd-buffer', 'cd-data', 'cd-audio', 'cd-stream', 'cd-transition', 'cd-predicate', 'cd-image'}
local gpu_sources = {'cpu-gp0', 'cpu-gp1', 'direct-dma', 'chain-dma', 'read-dma',
    'cpu-read-gp0', 'host-gp0', 'host-gp1', 'host-read-gp0', 'host-replay'}
local failures = {'events', 'bytes', 'thread', 'unsupported', 'association',
    'discontinuity', 'bounds', 'clock-regression'}

M.exact = m.exact

-- Wire fields only. Whole-journal controller/stream/media/Audio relationships
-- belong to the CD capture consumer; a raw export cannot establish them.
local function cd_fields(record, data)
    local kind,phase,size=tonumber(record.kind),tonumber(record.device),tonumber(record.length)
    local labels,expected={},0
    if kind==34 then
        assert(data[1]==3,'historical or unknown CD wire version')
        if phase==2 then
            labels={'version','clock_hz','setting_count','scale_count','backend_bytes','device_bytes','initialized','stream_present'}
            assert(data[3]==14 and data[4]==11 and data[5]<=1024 and data[6]<=1024 and data[7]<=1 and data[8]<=1,'invalid native environment')
            expected=156+data[5]+data[6]
        else assert(phase<=1 and data[2]==384,'invalid typed context boundary');labels={'version','context_bytes'};expected=384 end
    elseif kind==35 or (kind==45 and phase<=1) then
        assert(phase<=1 and data[1]>=1 and data[1]<=15,'unknown native scope')
        labels={'operation','address','width','value','pc','returned','depth','reserved'};expected=384
    elseif kind==36 then
        assert(phase==1 or phase==2,'unknown native handler variant')
        labels=phase==1 and {'opcode','start','parameter_count'} or {'opcode','start','continuation'}
        assert(data[1]<=30 and data[2]<=1 and (phase==1 and data[3]<=16 or phase==2 and data[3]<=1),'invalid native handler fields')
        expected=phase==1 and 32 or 0
    elseif kind==37 then
        assert(phase>=1 and phase<=7,'unknown native scheduler variant')
        if phase==3 then labels={'snapshot_mask','live_mask'}
        elseif phase<=5 then labels={'slot',phase<=2 and 'cancel' or 'dispatch'};expected=48
        else labels={'slot','pending','target_low','target_high','scale_bits'} end
        if phase~=3 then assert(data[1]==2 or data[1]==3 or data[1]==4 or data[1]==10,'unknown CD scheduler slot') end
    elseif kind==38 then
        assert(phase<=3 and data[1]<=1 and data[2]<=255 and data[3]<=16,'invalid native response insertion')
        labels={'native_result','cause','parameter_count'};expected=64
    elseif kind==39 then
        assert(phase==1 or phase==4,'unknown native IRQ variant')
        if phase==1 then labels={'cause','mask','asserted'};assert(data[3]<=1,'invalid native IRQ predicate') else expected=32 end
    elseif kind==40 then
        assert(phase<=6,'unknown media variant')
        if phase==0 then labels={'lookup_kind','packed_msf','absolute_frame','prior_backing_low','prior_backing_high'}
        elseif phase==1 then labels={'lookup_kind','native_result','disposition'}
        elseif phase==2 then labels={'reason','count_low','count_high','track','file','sector_bits','flags','base_bytes'}
        elseif phase==6 then labels={'query','argument','native_result'};expected=(data[1]==6 and data[3]~=0) and 8 or 0
        else assert(phase==4 or phase==5,'unknown media branch');labels={'id_bytes','label_bytes'};expected=64 end
    elseif kind==41 then
        assert(phase<=2,'unknown native buffer variant')
        if phase<=1 then labels={'buffer','fifo_index','fifo_size','fifo_pending','storage_bytes'};expected=2352
        else labels={'buffer','offset','byte_count'};expected=data[3];assert(data[1]>=1 and data[1]<=8 and data[3]>0 and data[3]<=16384,'invalid native buffer mutation') end
    elseif kind==42 then
        assert(phase==0 or phase==1 or phase==3 or phase==4,'unknown native data variant')
        if phase<=1 then
            labels={'fifo_offset','byte_count','address','origin_runs'};expected=size
            assert(data[1]<=2352 and data[2]<=2352-data[1] and data[4]<=data[2] and size==32+data[4]*16+data[2],'invalid native consumed span')
        elseif phase==3 then labels={'address','fifo_offset','requested_bytes','copied_bytes','dropped_bytes','destination','destination_offset'}
            assert(data[3]==data[4]+data[5] and data[6]<=5,'invalid native DMA receipt')
        else labels={'address','fifo_offset','requested_bytes','returned_bytes','receipted_bytes'} end
    elseif kind==43 then
        assert(phase<=1,'unknown native Audio handoff')
        if phase==0 then labels={'cdda','source_rate','source_frames','stereo'};expected=24
        else labels={'reason','source_rate','source_frames','stereo','output_frames','audio_state','event_disposition','cdda'};expected=16 end
    elseif kind==44 then error('undeclared wire-3 stream producer')
    elseif kind==45 then assert(phase==16 or phase==17,'unknown worker boundary');expected=48
    elseif kind==46 then
        assert(phase>=1 and phase<=15,'unknown native predicate');expected=size
        labels={'native_1','native_2','native_3','native_4','native_5','native_6','native_7','native_8'}
    elseif kind==47 then
        assert(phase<=1 and data[1]==3 and data[2]==716 and data[3]<=202 and data[4]==100 and size>=2864+data[3]*32 and size<=836720,'invalid native image v3')
        labels={'version','word_count','file_count','slot_count'};expected=size
    else error('unknown CD record') end
    assert(size==expected,'wire-3 record payload mismatch')
    local result={wire_version=3,variant=phase}
    for i,label in ipairs(labels) do result[label]=data[i] end
    for i=#labels+1,8 do assert(data[i]==0,'nonzero unused native field') end
    return result
end

local function fields(record, data)
    local k, result = tonumber(record.kind), {}
    local function assign(labels)
        for i, label in ipairs(labels) do result[label] = data[i] end
    end
    if k >= 34 and k <= 47 then return cd_fields(record, data)
    elseif k == 1 or k == 2 then
        assign({'address', 'requested_value', 'width', 'aligned_mirror', 'read'})
    elseif k == 3 then
        assign({'original_madr', 'normalized_madr', 'bcr', 'chcr', 'previous_chcr', 'dpcr'})
        result.previous_request = M.exact(record.related)
    elseif k == 4 or k == 29 then
        assign({'ram_address', 'device_address', 'byte_count', 'direction'})
        if record.device == 3 then result.destination_offset = data[5]; result.destination_class = data[6] end
        result.direction = assert(({[0]='device-to-ram', [1]='ram-to-device'})[data[4]],
            'unknown DMA direction')
        result.device_address_scope = record.device == 3 and 'CD transfer-buffer index' or
            (record.device == 4 and 'SPU byte address' or 'unspecified')
        if record.device == 0 then result.input_request = M.exact(record.request)
        elseif record.device == 1 then result.input_request = M.exact(record.related) end
    elseif k == 5 then assign({'address', 'header', 'words'})
    elseif k == 6 then
        assign({'chcr_before', 'chcr_after'})
        result.touched_channel_request = M.exact(record.related)
    elseif k == 7 then
        assign({'stage', 'dicr'})
        result.touched_channel_request = M.exact(record.related)
    elseif k == 8 or k == 9 then assign({'madr', 'bcr', 'chcr'})
    elseif k == 10 then
        assign({'delay', 'prior_pending_mask', 'old_target_low', 'old_target_high',
            'old_request_low', 'old_request_high'})
        result.target_cycle = M.exact(record.related)
    elseif k == 11 then result.target_cycle = M.exact(record.related)
    elseif k == 12 or k == 13 then assign({'mask', 'istat_before', 'istat_after'})
    elseif k == 14 then assign({'istat', 'imask', 'cp0_status', 'pc'})
    elseif k == 17 then
        assign({'selector', 'value', 'mode_after', 'guest_address', 'guest_width'})
        if data[1] == 8 then result.mode_after = nil end
    elseif k == 15 or k == 16 or k == 18 or k == 19 or k == 22 or k == 23 then
        assign({'mode', 'target', 'rate', 'irq', 'counter_state', 'irq_state', 'gate_started', 'reason'})
        result.cycle_start = M.exact(record.related)
    elseif k == 21 then assign({'terminal_address'})
    elseif k == 24 then
        if record.device == 4294967295 then
            result.pending_mask, result.lowest_target = data[1], M.exact(record.related)
        else result.pending, result.target_cycle = data[1], M.exact(record.related) end
    elseif k == 25 then assign({'istat', 'imask', 'cp0_status', 'cp0_cause'})
    elseif k == 26 then
        if record.device == 0 then
            assign({'reason', 'reg0', 'reg1', 'pending_madr', 'pending_bcr', 'pending_chcr', 'buffer_offset'})
            result.buffer_input_request = M.exact(record.related)
        elseif record.device == 1 then
            assign({'reason', 'source_offset', 'source_end', 'buffer_offset'})
            result.retained_input_request = M.exact(record.related)
        else error('unknown DMA device-state variant') end
    elseif k == 27 then
        assign({'byte_count'})
        result.input_request = M.exact(record.related)
    elseif k == 28 then result.discarded_buffer_request = M.exact(record.related)
    elseif k == 30 or k == 31 then
        assert(record.length == 0 and (record.device == 0 or record.device == 1), 'invalid GPU context')
        result.phase = record.device == 0 and 'begin' or 'end'
        if k == 30 then
            assign({'processor', 'read_fifo_bytes', 'data_return', 'ram_mask', 'known_environment',
                'ready', 'reserved_1', 'reserved_2'})
            assert(data[5] <= 7 and data[6] <= 1 and data[7] == 0 and data[8] == 0,
                'invalid GPU context flags')
        else
            assign({'last_page', 'last_window', 'last_offset', 'window_mirror', 'area_start_mirror',
                'area_end_mirror', 'offset_mirror', 'reserved'})
            assert(data[8] == 0, 'invalid GPU environment flags')
            result.known_scope = 'last_* availability comes from paired gpu-context bitmask; zero is not proof of initialization'
        end
    elseif k == 32 or k == 33 then
        assert(record.device == 2 and record.length == 0 and data[8] <= 1, 'invalid GPU input boundary')
        assign({'source_index', 'origin', 'words', 'value', 'processor', 'read_fifo_bytes', 'pc', 'ready'})
        result.source = assert(gpu_sources[data[1] + 1], 'unknown GPU input source')
        result.buffer = M.exact(record.related)
        assert(result.buffer ~= '0', 'missing GPU buffer identity')
        result.value_scope = (data[1] == 5 or data[1] == 8)
            and 'read result only at gpu-result; input value is a placeholder'
            or 'write operand for CPU/host writes; DMA bytes reside in enclosed dma-data'
    end
    return result
end

-- Shared wire decoding for raw export and device correlation. This validates
-- the row layout only; callers still own ordering, payload bounds and ancestry.
function M.describe(record)
    local kind = assert(names[tonumber(record.kind)], 'unknown native history kind')
    local data = m.array()
    for i = 0, 7 do data[i + 1] = tonumber(record.data[i]) end
    local decoded = fields(record, data)
    if record.kind == 4 then
        assert(record.length == data[3], 'DMA payload length differs from native byte count')
    end
    return kind, data, decoded
end

function M.export()
    local h = assert(PCSX.History, 'native history binding required')
    local status = h.status()
    assert(status.version == 1 and status.state == 2, 'unsupported or unfrozen native history')
    local count, size = tonumber(status.events), tonumber(status.bytes)
    assert(count <= 65536 and size <= 8388608, 'native history exceeds ABI limits')
    m.write('payload.bin', h.bytes(0, size))
    local file = assert(io.open('events.ndjson', 'wb'))
    local total, offset, previous = 0, 0, status.beginCycle
    local kinds, channels = {}, {}
    local ok, err = pcall(function()
        for index = 0, count - 1 do
            local record = h.record(index)
            local kind, data, decoded = M.describe(record)
            assert(record.sequence == index + 1, 'noncontiguous native event sequence')
            assert(record.cycle >= previous and record.cycle <= status.endCycle,
                'native event outside monotonic capture interval')
            previous = record.cycle
            assert(record.offset == offset and offset + record.length <= size,
                'native event payload bounds are inconsistent')
            local value = {sequence = M.exact(record.sequence), cycle = M.exact(record.cycle),
                request = M.exact(record.request), related = M.exact(record.related), kind = kind,
                device = tonumber(record.device), raw = data, fields = decoded,
                payload = {offset = offset, bytes = tonumber(record.length)}}
            local line = m.json(value) .. '\n'
            assert(total + #line <= 67108864, 'event export exceeds 64 MiB')
            assert(file:write(line)); total = total + #line
            offset = offset + tonumber(record.length)
            kinds[kind] = (kinds[kind] or 0) + 1
            if record.kind == 4 then
                assert(record.length == data[3], 'DMA byte count differs from retained payload')
                local channel = tostring(tonumber(record.device))
                channels[channel] = (channels[channel] or 0) + tonumber(record.length)
            end
        end
        assert(offset == size, 'unreferenced native payload bytes')
    end)
    assert(file:close())
    m.captures[#m.captures + 1] = {path = 'events.ndjson', bytes = total}
    local faults = m.array()
    for i, name in ipairs(failures) do
        if bit.band(status.failures, 2 ^ (i - 1)) ~= 0 then faults[#faults + 1] = name end
    end
    if bit.band(status.failures, 0xffffff00) ~= 0 then faults[#faults + 1] = 'unknown-native-failure' end
    local result = {schema = 'psx.native-history/v2', native_envelope_version = 1, cd_observation_version = 3, build_identity = m.buildIdentity(),
        begin_cycle = M.exact(status.beginCycle), end_cycle = M.exact(status.endCycle),
        events = count, payload_bytes = size, exported_bytes = total, kinds = kinds,
        transferred_bytes = channels, failures = faults, failure_bits = tonumber(status.failures),
        first_failure = tonumber(status.firstFailure), dropped_attempts = tonumber(status.dropped),
        complete = ok and status.failures == 0, export_error = not ok and tostring(err) or nil,
        ordering = 'native CPU-thread hook order; bulk records do not imply byte-cycle timing',
        request_zero = 'absent or pre-capture owner; inspect initial state and pending events',
        payload_scope = 'device context and copied buffers; only dma-data counts as transferred bytes',
        cd_scope = kinds['cd-context'] and 'wire export only; CD lifecycle, ownership and Audio joins require the CD consumer' or nil,
        irq_scope = 'CPU-visible latch/delivery; SPU worker requests may coalesce',
        host_edits = 'audited mission required; arbitrary Lua pointer writes are not intercepted'}
    m.report('events.json', result)
    return result
end
return M
-- )EOF"
