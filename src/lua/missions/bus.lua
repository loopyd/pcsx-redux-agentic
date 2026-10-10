-- lualoader, R"EOF(--
-- Issue MMIO stores through the native CPU, not the debugger's side-effect-free memory file.
local ffi, bit = require 'ffi', require 'bit'
local m, s = require 'support', require 'snapshot'
local M = {}
local function execute(width, construct, callback)
    m.requireIdleCapture()
    local start = m.address('scratch')
    local program = construct(start)
    local physical = bit.band(start, 0x1fffffff)
    local bytes, operations = #program, (#program - 52) / 8
    local state, regs = s.live(), PCSX.getRegisters()
    assert(not state.registers.next_is_delay_slot, 'MMIO probe cannot start in a delay slot')
    for _, name in ipairs({'delay_slot_info_1', 'delay_slot_info_2'}) do
        local delay = state.registers[name]
        assert(not delay or (not delay.pc_active and not delay.active), 'MMIO probe requires no pending delayed load/branch')
    end
    local saved, pc = {}, tonumber(regs.pc)
    for i = 0, 33 do saved[i] = tonumber(regs.GPR.r[i]) end
    local pointer = ffi.cast('uint8_t*', PCSX.getMemPtr()) + physical
    local original = ffi.string(pointer, bytes)
    ffi.copy(pointer, program, bytes)
    PCSX.invalidateCache()
    m.breakpoint(start + 48, 'Exec', 4, function()
        PCSX.pauseEmulator()
        m.requireIdleCapture()
        ffi.copy(pointer, original, bytes)
        PCSX.invalidateCache()
        for i = 0, 33 do regs.GPR.r[i] = saved[i] end
        regs.pc = pc
        local probe = {scratch = start, scratch_bytes = bytes, operations = operations,
            store_bits = width, before_cycles = state.registers.cycle,
            after_cycles = tonumber(PCSX.getCPUCycles())}
        PCSX.nextPause(m.guard(function() callback(probe) end))
        return false
    end)
    regs.pc = start
    PCSX.resumeEmulator()
end
function M.write(width, operations, callback)
    execute(width, function(start) return PCSX.packMMIOProbe(start, width, operations) end, callback)
end
function M.upload(x, y, width, height, pixels, callback)
    execute(32, function(start) return PCSX.packVRAMUploadProbe(start, x, y, width, height, pixels) end, callback)
end
return M
-- )EOF"
