-- lualoader, R"EOF(--
-- Inspect serialized MDEC state or qualify a bounded guest DMA decode interval.
local bit = require 'bit'
local m, s, events = require 'support', require 'snapshot', require 'events'
local function summary(state)
    local value = assert(state.mdec, 'state lacks MDEC')
    assert(#value.block_buffer == 768, 'unexpected MDEC block buffer')
    local result = {}
    for _, name in ipairs({'reg0', 'reg1', 'rl', 'rl_end', 'block_buffer_pos', 'adr', 'bcr', 'chcr',
        'iq_y', 'iq_uv', 'qt_y', 'qt_uv', 'scaletable', 'custom_scale'}) do
        result[name] = value[name]
    end
    result.busy = bit.band(value.reg1, 0x20000000) ~= 0
    result.pointer_encoding = 'zero is null; nonzero is backing byte offset plus one'
    return result
end

local function decode(h, report, limits)
    local status = h.status()
    local rows = h.records(0, tonumber(status.events))
    local requests, command, input, output, consumed = {}, nil, {}, {}, {}
    local in_size, out_size, words = 0, 0, 0
    local ok, err = pcall(function()
        for index = 0, tonumber(status.events) - 1 do
            local row = rows[index]
            local kind, channel, data = tonumber(row.kind), tonumber(row.device), row.data
            local sequence, request = m.exact(row.sequence), m.exact(row.request)
            if kind == 26 and channel == 0 and data[0] == 2 then
                assert(#report.commands < limits.commands, 'MDEC command limit exceeded')
                command = {sequence = sequence, cycle = m.exact(row.cycle), value = tonumber(data[1])}
                report.commands[#report.commands + 1] = command
            elseif kind == 3 and channel <= 1 then
                assert(request ~= '0' and not requests[request], 'missing or repeated MDEC DMA identity')
                local bcr = tonumber(data[2])
                local count = math.floor(bcr / 65536) * (bcr % 65536)
                assert(count > 0 and count <= limits.words - words, 'MDEC DMA word limit exceeded')
                words = words + count
                local transfer = {request = request, sequence = sequence, channel = channel,
                    original_address = tonumber(data[0]), address = tonumber(data[1]), bcr = bcr,
                    chcr = tonumber(data[3]), requested_bytes = count * 4, observed_bytes = 0,
                    spans = m.array(), completed = false}
                requests[request] = transfer
                consumed[request] = {}
                report.transfers[#report.transfers + 1] = transfer
                assert(transfer.address % 4 == 0 and transfer.address < 2097152 and
                    transfer.requested_bytes <= 2097152 - transfer.address, 'MDEC DMA exceeds standard RAM')
                if channel == 0 then
                    assert(command, 'MDEC input DMA has no observed guest command')
                    transfer.command_sequence, transfer.command = command.sequence, command.value
                    local opcode = math.floor(command.value / 268435456)
                    if opcode == 3 then
                        assert(bit.band(command.value, 0x05ff0000) == 0, 'unsupported MDEC signed/depth/reserved flags')
                        local rgb15 = bit.band(command.value, 0x08000000) ~= 0
                        assert(rgb15 or bit.band(command.value, 0x02000000) == 0, 'STP requires RGB15 output')
                        assert(command.value % 65536 == count, 'MDEC command count differs from DMA input words')
                        transfer.format = rgb15 and 'rgb555-le' or 'rgb24'
                        transfer.block_bytes = rgb15 and 512 or 768
                        transfer.width, transfer.height = 16, 16
                        transfer.stp = bit.band(command.value, 0x02000000) ~= 0
                    else
                        assert((opcode == 4 or opcode == 6) and count == 32, 'unsupported MDEC table command/length')
                        transfer.table = opcode == 4 and 'quantization-y-uv' or 'scale'
                    end
                end
            elseif kind == 4 and channel <= 1 then
                local transfer = assert(requests[request], 'MDEC bytes have no captured DMA start')
                local size, offset = tonumber(row.length), tonumber(row.offset)
                assert(size == tonumber(data[2]), 'MDEC payload length mismatch')
                local bytes = h.bytes(offset, size)
                local span = {sequence = sequence, address = tonumber(data[0]), bytes = size,
                    payload_offset = offset, stream_offset = channel == 0 and in_size or out_size}
                transfer.spans[#transfer.spans + 1] = span
                transfer.observed_bytes = transfer.observed_bytes + size
                assert(transfer.observed_bytes <= transfer.requested_bytes, 'MDEC transfer exceeds requested count')
                if channel == 0 then
                    assert(data[3] == 1, 'MDEC input direction mismatch')
                    input[#input + 1], in_size = bytes, in_size + size
                    local parts = consumed[request]
                    parts[#parts + 1] = bytes
                else
                    assert(data[3] == 0, 'MDEC output direction mismatch')
                    local owner = assert(requests[m.exact(row.related)], 'MDEC output lacks captured input owner')
                    assert(owner.channel == 0 and owner.format, 'MDEC output owner is not decode input')
                    assert(not transfer.input_request or transfer.input_request == owner.request,
                        'one MDEC output DMA crosses decode input owners')
                    transfer.input_request, transfer.command_sequence = owner.request, owner.command_sequence
                    transfer.format, transfer.block_bytes = owner.format, owner.block_bytes
                    transfer.width, transfer.height, transfer.stp = 16, 16, owner.stp
                    output[#output + 1], out_size = bytes, out_size + size
                end
            elseif kind == 6 and channel <= 1 then
                local transfer = assert(requests[request], 'MDEC completion lacks captured DMA start')
                assert(m.exact(row.related) == request, 'MDEC completion cleared a different channel owner')
                assert(bit.band(tonumber(data[1]), 0x01000000) == 0, 'MDEC completion leaves channel busy')
                transfer.completed, transfer.completion_sequence = true, sequence
            end
        end
        assert(#report.commands > 0 and out_size > 0, 'no decoded MDEC output')
        for _, transfer in ipairs(report.transfers) do
            assert(transfer.completed, 'MDEC DMA has no observed completion')
            if transfer.format and transfer.channel == 0 then
                local bytes, position, blocks = table.concat(consumed[transfer.request]), 1, 0
                local function word()
                    assert(position + 1 <= #bytes, 'truncated MDEC run-length block')
                    local a, b = bytes:byte(position, position + 1)
                    position = position + 2
                    return a + b * 256
                end
                while position <= #bytes do
                    word()
                    local coefficient = 0
                    while true do
                        local value = word()
                        if value == 0xfe00 then break end
                        coefficient = coefficient + math.floor(value / 1024) + 1
                        assert(coefficient <= 63, 'unsupported MDEC run-length overflow')
                    end
                    blocks = blocks + 1
                end
                assert(blocks > 0 and blocks % 6 == 0, 'incomplete MDEC macroblock input')
                transfer.macroblocks = blocks / 6
            end
            if transfer.channel == 1 or transfer.table then
                assert(transfer.observed_bytes == transfer.requested_bytes, 'truncated MDEC input/output transfer')
            end
        end
    end)
    m.write('mdec-input.bin', table.concat(input))
    m.write('mdec-output.bin', table.concat(output))
    report.input_bytes, report.output_bytes, report.dma_words = in_size, out_size, words
    report.decode_error = not ok and tostring(err) or nil
    return ok
end

m.begin('action,target,identity,frames,stop,events,bytes,commands,words', function()
    local action = m.argument('action', 'inspect')
    assert(action == 'inspect' or action == 'export' or action == 'decode', 'unknown MDEC action')
    if action ~= 'decode' then
        for _, name in ipairs({'identity', 'frames', 'stop', 'events', 'bytes', 'commands', 'words'}) do
            assert(not m.argument(name), name .. ' applies only to decode')
        end
        s.observe(function(state, source)
            local value = summary(state)
            if action == 'export' then m.write('mdec-buffer.bin', state.mdec.block_buffer) end
            m.report('mdec.json', {schema = 'psx.runtime-mdec/v1', action = action, source = source,
                state = value, buffer_bytes = 768, scope = 'serialized state; no guest decode execution'})
            m.finish()
        end)
        return
    end
    local identity = assert(m.argument('identity'), 'identity is required')
    assert(#identity > 0 and #identity <= 160, 'identity must be 1..160 bytes')
    if m.input('state', false) then
        assert(os.getenv('PSX_RUNTIME_ORIGIN_VALIDATED') == '1', 'MDEC continuation requires a matching origin receipt')
    end
    local stop = m.address('stop')
    local frames = m.integer('frames', 1, 1, 36000)
    local event_limit = m.integer('events', 16384, 1, 65536)
    local byte_limit = m.integer('bytes', 1048576, 0, 8388608)
    local limits = {commands = m.integer('commands', 64, 1, 4096), words = m.integer('words', 262144, 1, 2097152)}
    local h = assert(PCSX.History, 'native History binding required')
    local started, closing, count = false, false, 0
    local report = {schema = 'psx.runtime-mdec/v1', action = action, identity = identity,
        identity_authority = 'caller label; input/tool hashes in receipt', commands = m.array(), transfers = m.array(),
        frame_limit = frames, stop = stop, event_limit = event_limit, byte_limit = byte_limit, limits = limits,
        source = 'guest execution through existing MDEC command/DMA handlers',
        layout = '16x16 macroblocks in DMA order; each block has row-major RGB24 or little-endian RGB555 pixels',
        scope = 'complete selected stream; no image tiling, hardware rounding or latency qualification'}
    local function finish(reason, success)
        if closing then return end
        closing = true
        PCSX.pauseEmulator()
        PCSX.nextPause(m.guard(function()
            local native, qualified
            local ok, err = pcall(function()
                if started then
                    h.stop()
                    native = events.export()
                    report.after = summary(s.decode(m.captureState('final.pbuf')))
                    qualified = decode(h, report, limits)
                    assert(not report.after.busy and report.after.adr == 0 and report.after.block_buffer_pos == 0,
                        'MDEC stream or partial output buffer remains pending')
                end
            end)
            report.reason, report.frames, report.started = reason, count, started
            report.native_complete = native and native.complete or false
            report.capture_error = not ok and tostring(err) or nil
            report.success = success and ok and qualified and report.native_complete or false
            m.report('mdec.json', report)
            if report.success then m.finish() else error('incomplete MDEC decode: ' .. reason) end
        end))
    end
    m.event('GPU::Vsync', function()
        if not started or closing then return end
        count = count + 1
        if h.status().failures ~= 0 then finish('native recorder failure', false)
        elseif count >= frames then finish('frame limit before stop', false) end
    end)
    m.event('ExecutionFlow::Pause', function()
        if started and not closing then finish('unexpected native pause', false) end
    end)
    m.breakpoint(stop, 'Exec', 4, function()
        if started and not closing then finish('stop reached', true); return false end
        return true
    end)
    m.atTarget(function()
        PCSX.pauseEmulator()
        PCSX.nextPause(m.guard(function()
            if closing then return end
            m.requireIdleCapture()
            report.before = summary(s.decode(m.captureState('initial.pbuf')))
            assert(not report.before.busy and report.before.adr == 0 and report.before.block_buffer_pos == 0,
                'MDEC decode requires no pre-existing stream or partial output')
            h.begin(event_limit, byte_limit)
            started = true
            if h.status().failures ~= 0 then finish('initial recorder failure', false)
            elseif tonumber(PCSX.getRegisters().pc) == stop then finish('stop already reached', false)
            else PCSX.resumeEmulator() end
        end))
    end)
end)
-- )EOF"
