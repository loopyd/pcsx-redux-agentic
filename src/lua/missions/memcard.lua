-- lualoader, R"EOF(--
local m, cards = require 'support', PCSX.MemoryCards
local function import(slot)
    local file = assert(io.open(m.input('card', true), 'rb'))
    local bytes = assert(file:read(134977)); assert(file:close())
    return cards.importChecked(slot, bytes, m.argument('format', 'auto'))
end
local function capture(slot)
    local ffi = require 'ffi'
    local frames = m.integer('frames', 180, 1, 36000)
    local stop = m.address('stop')
    local flush = m.integer('flush', 0, 0, 1) == 1
    local block = m.argument('block') and m.integer('block', nil, 1, 15)
    local address = m.argument('address') and m.integer('address', nil, 0, 4294967295)
    local offset = address and address % 536870912
    local length = m.integer('length', address and 256 or 0, address and 1 or 0, 2097152)
    assert(not address or (math.floor(address / 536870912) == 0 or
        math.floor(address / 536870912) == 4 or math.floor(address / 536870912) == 5) and
        offset + length <= 2097152, 'transcript must fit physical RAM')
    assert(address or length == 0, 'transcript length requires address')
    local resumed = m.input('state', false) ~= nil
    if resumed then
        assert(os.getenv('PSX_RUNTIME_ORIGIN_VALIDATED') == '1', 'card continuation requires --origin')
        assert(not os.getenv('PSX_RUNTIME_ENTRY'), 'card continuation cannot load an EXE')
        assert(not m.input('card', false), 'card continuation cannot reimport an image')
        assert(not m.argument('target'), 'card continuation starts at the saved PC')
    end
    local report = {schema = 'psx.runtime-memcard-capture/v1', slot = slot, continued = resumed,
        frames_requested = frames, stop = stop, transcript = address and {address = address, bytes = length},
        backing = 'fresh portable memcard1.mcd and memcard2.mcd; staged input is imported into memory',
        qualification = 'Guest protocol bytes require the selected RAM transcript; host extraction alone is not guest save success.'}
    local function boundary(name)
        local result = {registers = m.registers(), cards = m.array()}
        for card = 1, 2 do
            local value = cards.captureChecked(card)
            local path = name .. '-card' .. card .. '.mcd'
            m.write(path, value.bytes)
            result.cards[card] = {path = path, status = value.status}
        end
        if address then m.write(name .. '-transcript.bin', ffi.string(ffi.cast('uint8_t*', PCSX.getMemPtr()) + offset, length)) end
        m.captureState(name .. '.pbuf')
        return result
    end
    m.atTarget(function()
        if m.input('card', false) then report.import = import(slot) end
        report.initial = boundary('initial')
        local observed, ending, stopCycles = 0, false
        local function finish(reason)
            if ending then return end
            ending = true
            PCSX.pauseEmulator()
            PCSX.nextPause(m.guard(function()
                report.reason, report.frames_observed = reason, observed
                report.final = boundary('final')
                report.complete = reason == 'stop PC' and report.final.registers.pc == stop and
                    m.exact(PCSX.getCPUCycles()) == stopCycles
                if report.complete then
                    local ok, problem = pcall(function()
                        if block then
                            local bytes, metadata = cards.readFile(slot, block)
                            metadata.chain, metadata.diagnostics = m.array(metadata.chain), m.array(metadata.diagnostics)
                            m.write('save.bin', bytes)
                            report.file = metadata
                        end
                        if flush then
                            report.persistence = cards.flushChecked(slot)
                        end
                    end)
                    if flush then report.persistence_status = cards.captureChecked(slot, 0, 0).status end
                    if not ok then report.complete, report.error = false, tostring(problem) end
                else report.error = reason == 'frame deadline' and
                    'card stop PC unmet at frame deadline' or 'card stop changed before settled capture' end
                m.report('memcard.json', report)
                assert(report.complete, report.error)
                m.finish()
            end))
        end
        m.breakpoint(stop, 'Exec', 4, function(address)
            assert(address == stop and tonumber(PCSX.getRegisters().pc) == stop, 'card stop raw PC differs')
            stopCycles = m.exact(PCSX.getCPUCycles())
            finish('stop PC'); return false
        end)
        m.event('GPU::Vsync', function()
            observed = observed + 1
            if observed >= frames then finish('frame deadline') end
        end)
        PCSX.resumeEmulator()
    end)
end
m.begin('action,target,slot,offset,length,block,destination,icons,format,savestate,frames,stop,address,flush', function()
    local action = m.argument('action', 'directory')
    local slot = m.integer('slot', 1, 1, 2)
    local save = m.integer('savestate', 0, 0, 1) == 1
    if action == 'capture' then capture(slot); return end
    m.atTarget(function()
        local report = {schema = 'psx.runtime-memcard/v1', action = action, slot = slot, source = 'live'}
        if action == 'directory' then
            report.directory = cards.directory(slot, {icons = m.integer('icons', 0, 0, 1) == 1})
            report.directory.blocks = m.array(report.directory.blocks)
            report.directory.diagnostics = m.array(report.directory.diagnostics)
            for _, block in ipairs(report.directory.blocks) do
                block.chain = m.array(block.chain)
                block.diagnostics = m.array(block.diagnostics)
            end
        elseif action == 'read' then
            local offset = m.integer('offset', 0, 0, 131072)
            local length = m.integer('length', 131072 - offset, 0, 131072 - offset)
            local capture = cards.captureChecked(slot, offset, length)
            m.write('card.bin', capture.bytes)
            report.offset, report.bytes, report.status = offset, length, capture.status
        elseif action == 'file' then
            local data, metadata = cards.readFile(slot, m.integer('block', nil, 1, 15))
            metadata.chain, metadata.diagnostics = m.array(metadata.chain), m.array(metadata.diagnostics)
            m.write('save.bin', data)
            report.bytes, report.file = #data, metadata
        elseif action == 'copy' or action == 'move' or action == 'erase' then
            local options = {operation = action, source = slot, block = m.integer('block', nil, 1, 15)}
            if action ~= 'erase' then options.destination = m.integer('destination', nil, 1, 2) end
            report.edit = cards.editChecked(options)
        elseif action == 'import' then
            report.edit = import(slot)
        elseif action == 'export' then
            report.export = cards.export(slot, 'card.mcd')
            m.captures[#m.captures + 1] = {path = 'card.mcd', bytes = report.export.size}
        elseif action == 'flush' then
            report.persistence = cards.flushChecked(slot)
        else error('unknown memory card action') end
        if save then m.captureState('state.pbuf') end
        m.report('memcard.json', report)
        m.finish()
    end)
end)
-- )EOF"
