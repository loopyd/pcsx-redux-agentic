-- lualoader, R"EOF(--
local m, cards = require 'support', PCSX.MemoryCards
m.begin('action,target,slot,offset,length,block,destination,icons,format,savestate', function()
    local action = m.argument('action', 'directory')
    local slot = m.integer('slot', 1, 1, 2)
    local save = m.integer('savestate', 0, 0, 1) == 1
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
            local file = assert(io.open(m.input('card', true), 'rb'))
            local bytes = assert(file:read(134977)); assert(file:close())
            report.edit = cards.importChecked(slot, bytes, m.argument('format', 'auto'))
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
