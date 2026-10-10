-- lualoader, R"EOF(--
local M = {}
local function limits(options)
    options = options or {}
    return options.words or 65536, options.packets or 4096
end

function M.gp0(bytes, options)
    local words, packets = limits(options)
    return PCSX.decodeGPU(bytes, 'gp0', words, packets)
end

function M.gp1(bytes, options)
    local words, packets = limits(options)
    return PCSX.decodeGPU(bytes, 'gp1', words, packets)
end

function M.chain(ram, address, options)
    local words, nodes = limits(options)
    return PCSX.readGPUChain(ram, address, words, nodes)
end

return M
-- )EOF"
