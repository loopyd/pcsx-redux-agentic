-- lualoader, R"EOF(--
-- Shared bounded mission transport; Lua runs as trusted host code.
-- Native FFI calls can synchronously reenter Lua through emulator events.
-- Keep the host VM out of the trace compiler, including preexisting traces.
local jit = require 'jit'
jit.off()
jit.flush()
local ffi = require 'ffi'
local M = {captures = {}, retained = {}, done = false}
local cleanups, cleaned = {}, false
function M.onCleanup(callback)
    assert(not cleaned and type(callback) == 'function', 'cleanup registration is closed or invalid')
    cleanups[#cleanups + 1] = callback
end
function M.cleanup()
    if cleaned then return end
    cleaned = true
    local pending, failures = cleanups, {}
    cleanups = {}
    for index = #pending, 1, -1 do
        local ok, problem = pcall(pending[index])
        if not ok then failures[#failures + 1] = tostring(problem) end
    end
    assert(#failures == 0, table.concat(failures, "; "))
end
function M.requireIdleCapture()
    assert(PCSX.History and PCSX.History.status().state ~= 1,
        'host edits are forbidden during native history capture')
    -- Builds without Audio cannot have an active audio recorder. Capture itself
    -- requires the binding; this check supplies no alternative implementation.
    assert(not PCSX.Audio or PCSX.Audio.status().state ~= 1,
        'host edits are forbidden during native audio capture')
end
M.array = PCSX.jsonArray
function M.uint64(value)
    if type(value)=='number' then
        assert(value>=0 and value<=9007199254740991 and value%1==0,'inexact uint64 input')
        value=string.format('%.0f',value)
    end
    assert(type(value)=='string' and value:match('^%d+$') and (#value==1 or value:sub(1,1)~='0') and
        (#value<20 or (#value==20 and value<='18446744073709551615')),'invalid uint64 decimal')
    local result=ffi.new('uint64_t',0)
    for n=1,#value do result=result*10+value:byte(n)-48 end
    return result
end

function M.buildIdentity()
    local binary=os.getenv('PSX_RUNTIME_EMULATOR_SHA256')
    local source=os.getenv('PSX_RUNTIME_SOURCE_MANIFEST_SHA256')
    assert(binary and #binary==64 and binary:match('^[0-9a-f]+$') and source and #source==64 and source:match('^[0-9a-f]+$'),
        'exact emulator binary/source identity requires the native harness receipt')
    return {binary_sha256=binary,source_manifest_sha256=source,authority='verified native harness receipt'}
end
function M.exact(value)
    local text=tostring(value):gsub('ULL$',''):gsub('^#','')
    assert(text:match('^%d+$'),'invalid unsigned native integer');return text
end
function M.u32le(bytes,offset)
    assert(offset>=0 and offset%1==0 and #bytes>=offset+4,'short little-endian field')
    local a,b,c,d=bytes:byte(offset+1,offset+4);return a+b*256+c*65536+d*16777216
end
function M.u64le(bytes,offset)
    return ffi.new('uint64_t',M.u32le(bytes,offset+4))*4294967296+M.u32le(bytes,offset)
end
function M.hex(bytes) return (bytes:gsub('.',function(c) return string.format('%02x',c:byte()) end)) end


function M.argument(name, default)
    return os.getenv('PSX_RUNTIME_ARG_' .. name:upper()) or default
end

function M.integer(name, default, minimum, maximum)
    local value = tonumber(M.argument(name, default and tostring(default)))
    assert(value and value % 1 == 0 and value >= minimum and value <= maximum,
        name .. ' must be an integer in ' .. minimum .. '..' .. maximum)
    return value
end

function M.address(name, default)
    local value = M.integer(name, default, 0, 4294967295)
    assert(value % 4 == 0, name .. ' must be word aligned')
    return value
end

function M.input(name, required)
    local path = os.getenv('PSX_RUNTIME_INPUT_' .. name:upper())
    assert(path or not required, 'missing --input ' .. name .. '=PATH')
    return path
end

M.json = PCSX.encodeJSON

function M.write(name, bytes)
    assert(name:match('^[%w][%w_.-]*$') and #bytes <= 67108864, 'invalid capture name/size')
    for _, capture in ipairs(M.captures) do assert(capture.path ~= name, 'duplicate capture') end
    local file = assert(io.open(name, 'wb'))
    assert(file:write(bytes)); assert(file:close())
    M.captures[#M.captures + 1] = {path = name, bytes = #bytes}
end

function M.stateBytes()
    local slice = PCSX.createSaveStateChecked()
    local ok, bytes = pcall(tostring, slice)
    slice:close()
    assert(ok, bytes)
    return bytes
end

function M.captureState(name, bytes)
    bytes = bytes or M.stateBytes()
    M.write(name, bytes)
    local file = assert(io.open(name, 'rb'))
    local readback = assert(file:read(67108865))
    assert(file:close())
    assert(readback == bytes, 'saved state readback differs')
    local snapshot = require 'snapshot'
    snapshot.validate(snapshot.decode(readback, true), 'execution')
    return bytes
end

function M.report(name, value) M.write(name, M.json(value)) end

function M.finish()
    assert(not M.done, 'mission already finished')
    M.cleanup()
    M.done = true
    PCSX.pauseEmulator()
    local captures = #M.captures == 0 and '[]' or M.json(M.captures)
    local file = assert(io.open('completion.json', 'wb'))
    assert(file:write('{"schema":"psx.runtime-completion/v1","captures":' .. captures .. '}'))
    assert(file:close())
    PCSX.quit(0)
end

function M.guard(callback)
    return function(...)
        if M.done then return false end
        local ok, result = pcall(callback, ...)
        if not ok then
            M.done = true
            -- Refuse an enabled-JIT entry without reentering native code in it.
            jit.off()
            jit.flush()
            PCSX.pauseEmulator()
            local cleaned, cleanupProblem = pcall(M.cleanup)
            if not cleaned then io.stderr:write('mission cleanup failed: ' .. tostring(cleanupProblem) .. '\n') end
            io.stderr:write('mission failed: ' .. tostring(result) .. '\n')
            PCSX.quit(1)
            return false
        end
        return result
    end
end

function M.begin(allowed, callback)
    M.guard(function()
        assert(not jit.status(), 'maintained missions require host Lua JIT disabled')
        for name in (os.getenv('PSX_RUNTIME_ARGUMENT_NAMES') or ''):gmatch('[^,]+') do
            assert((',' .. allowed:upper() .. ','):find(',' .. name .. ',', 1, true),
                'unknown mission argument: ' .. name)
        end
        callback()
    end)()
end

function M.breakpoint(address, kind, width, callback)
    local bp = PCSX.addBreakpoint(address, kind, width, 'skill mission', M.guard(callback))
    M.retained[#M.retained + 1] = bp
    return bp
end

function M.event(name, callback)
    local event = PCSX.Events.createEventListener(name, M.guard(callback))
    M.retained[#M.retained + 1] = event
    return event
end

function M.registers()
    local regs, values = PCSX.getRegisters(), {}
    for i = 0, 31 do values[i + 1] = tonumber(regs.GPR.r[i]) end
    return {schema = 'psx.runtime-registers/v1', pc = tonumber(regs.pc),
        registers = values, cycles = tonumber(PCSX.getCPUCycles())}
end

function M.capture(save)
    M.write('ram.bin', ffi.string(PCSX.getMemPtr(), 2097152))
    M.report('registers.json', M.registers())
    if save then M.captureState('state.pbuf') end
end

function M.restore()
    local path = M.input('state', false)
    if not path then return false end
    M.requireIdleCapture()
    local snapshot = require 'snapshot'
    snapshot.validate(snapshot.read(path, true), "execution")
    local file = Support.File.open(path)
    PCSX.pauseEmulator()
    local ok, result = pcall(PCSX.loadSaveStateChecked, file)
    file:close()
    assert(ok and result == true, result)
    return true
end

function M.atTarget(callback, beforeResume)
    local explicit = M.argument('target')
    local raw = explicit or os.getenv('PSX_RUNTIME_ENTRY')
    local restored = M.restore()
    if raw then
        local target = M.address('target', tonumber(raw))
        local function reached(address)
            local selected = M.registers()
            local cycles = M.exact(PCSX.getCPUCycles())
            assert(address == target and selected.pc == target, 'target raw PC differs')
            PCSX.pauseEmulator()
            PCSX.nextPause(M.guard(function()
                local settled = M.registers()
                assert(settled.pc == selected.pc and M.exact(PCSX.getCPUCycles()) == cycles,
                    'target changed before settled capture')
                callback()
            end))
        end
        if (restored or explicit ~= nil) and tonumber(PCSX.getRegisters().pc) == target then
            reached(target)
            return
        end
        M.breakpoint(target, 'Exec', 4, function(address)
            reached(address)
            return false
        end)
        if beforeResume then beforeResume() end
        PCSX.resumeEmulator()
    else
        assert(restored, 'supply target, executable, or a saved state')
        callback()
    end
end

return M
-- )EOF"
