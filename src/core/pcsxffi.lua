-- lualoader, R"EOF(--
--   Copyright (C) 2022 PCSX-Redux authors
--
--   This program is free software; you can redistribute it and/or modify
--   it under the terms of the GNU General Public License as published by
--   the Free Software Foundation; either version 2 of the License, or
--   (at your option) any later version.
--
--   This program is distributed in the hope that it will be useful,
--   but WITHOUT ANY WARRANTY; without even the implied warranty of
--   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--   GNU General Public License for more details.
--
--   You should have received a copy of the GNU General Public License
--   along with this program; if not, write to the
--   Free Software Foundation, Inc.,
--   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
local nativeCPU, nativeNextPause = ...
ffi.cdef [[
typedef union {
    struct {
        uint32_t r0, at, v0, v1, a0, a1, a2, a3;
        uint32_t t0, t1, t2, t3, t4, t5, t6, t7;
        uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
        uint32_t t8, t9, k0, k1, gp, sp, s8, ra;
        uint32_t lo, hi;
    } n;
    uint32_t r[34];
} psxGPRRegs;

typedef union {
    uint32_t r[32];
} psxCP0Regs;

typedef union {
    uint32_t r[32];
} psxCP2Data;

typedef union {
    uint32_t r[32];
} psxCP2Ctrl;

typedef struct {
    psxGPRRegs GPR;
    psxCP0Regs CP0;
    psxCP2Data CP2D;
    psxCP2Ctrl CP2C;
    uint32_t pc;
} psxRegisters;

enum BreakpointType { Exec, Read, Write };
typedef struct { uint8_t opaque[?]; } Breakpoint;

uint64_t getCPUCycles();
uint8_t* getMemPtr();
uint8_t* getParPtr();
uint8_t* getRomPtr();
uint8_t* getScratchPtr();
psxRegisters* getRegisters();
uint8_t** getReadLUT();
uint8_t** getWriteLUT();
Breakpoint* addBreakpoint(uint32_t address, enum BreakpointType type, unsigned width, const char* cause, bool (*invoker)(uint32_t address, unsigned width, const char* cause), const char* label);
void enableBreakpoint(Breakpoint*);
void disableBreakpoint(Breakpoint*);
bool breakpointEnabled(Breakpoint*);
void removeBreakpoint(Breakpoint*);
void pauseEmulator();
void resumeEmulator();
bool debuggerStepInto();
typedef struct {
    uint64_t sequence, cycle, request, related;
    uint32_t kind, device, data[8], offset, length;
} HistoryRecord;
typedef struct {
    uint64_t beginCycle, endCycle;
    uint32_t version, state, events, bytes, failures, firstFailure, dropped;
} HistoryStatus;
typedef struct {
    uint32_t structureBytes, wireVersion, contextBytes, queueBytes, schedulerCount, cdSchedulerMask;
    uint32_t eventCeiling, payloadCeiling, minimumBoundaryEvents, minimumBoundaryBytes, workerRecordBytes, workerEventCeiling;
} HistoryCDLimits;
typedef struct {
    uint64_t sequence, epoch, batch, sampleOrdinal, cpuPublicationCycle, relatedFeed;
    uint32_t kind, flags, data[6];
} CDWorkerRecord;
typedef struct {
    uint64_t epoch, beginBatch, endBatch;
    uint32_t version, state, events, limit, failures, dropped;
} CDWorkerStatus;
bool historyCDLimits(uint32_t flags, HistoryCDLimits* out);
bool historyBeginCDV3(uint32_t events, uint32_t bytes, uint32_t workerEvents, uint32_t flags);
bool historyCDWorkerStatus(CDWorkerStatus* out);
bool historyCDWorkerRecords(uint32_t first, uint32_t count, CDWorkerRecord* out);
bool historyBegin(uint32_t events, uint32_t bytes);
bool historyBeginCD(uint32_t events, uint32_t bytes, bool audio);
bool historyStop();
bool historyClear();
bool historyStatus(HistoryStatus* out);
bool historyRecords(uint32_t first, uint32_t count, HistoryRecord* out);
bool historyBytes(uint32_t offset, uint32_t length, void* out);

void softResetEmulator();
void hardResetEmulator();
void luaMessage(const char* msg, bool error);
void luaLog(const char* msg);
void jumpToPC(uint32_t address);
void jumpToMemory(uint32_t address, unsigned width);
void invalidateCache();

typedef enum { BPP_16, BPP_24 } ScreenShotBPP;

typedef struct {
    LuaSlice* data;
    uint16_t width, height;
    ScreenShotBPP bpp;
} LuaScreenShot;

LuaScreenShot takeScreenShot();
double getGuestFPS();
void startGPUDump(LuaFile*);
void stopGPUDump();
bool isGPUDumpArmed();
bool isGPUDumpRecording();
uint64_t getGPUDumpFrames();
bool loadGPUDumpPlayer(LuaFile*);
void unloadGPUDumpPlayer();
bool stepGPUDumpPlayer();
void rewindGPUDumpPlayer();
uint64_t getGPUDumpPlayerFrame();
LuaSlice* getGPUDumpPlayerVRAM();
LuaSlice* getVRAM();

LuaSlice* createSaveState();
void loadSaveStateFromSlice(LuaSlice*);
void loadSaveStateFromFile(LuaFile*);
uint32_t createSaveStateChecked(LuaSlice**);
uint32_t loadSaveStateFromSliceChecked(LuaSlice*);
uint32_t loadSaveStateFromFileChecked(LuaFile*);

LuaFile* getMemoryAsFile();

void quit(int code);
]]

local C = ffi.load 'PCSX'

local stateResults = {
    [0] = 'ok', 'unavailable_context', 'running', 'quitting', 'recorder_active', 'busy',
    'too_large', 'malformed', 'unsupported_version', 'invalid_state', 'invalid_memory_card',
    'topology_mismatch', 'allocation_failed', 'commit_failed',
}
local function stateResult(result)
    result = tonumber(result)
    if result ~= 0 then error('savestate: ' .. (stateResults[result] or 'unknown_result'), 3) end
end

local function historyInteger(value, maximum, label)
    assert(type(value) == 'number' and value >= 0 and value <= maximum and value == math.floor(value),
           'invalid history ' .. label)
    return value
end

local history = {
    cdLimits = function(flags)
        historyInteger(flags, 3, 'CD flags')
        local result = ffi.new('HistoryCDLimits[1]')
        assert(C.historyCDLimits(flags, result), 'CD limits require a paused owner and supported native environment')
        return ffi.new('HistoryCDLimits', result[0])
    end,
    beginCDV3 = function(events, bytes, workers, flags)
        historyInteger(events, 65536, 'event limit'); historyInteger(bytes, 8388608, 'byte limit')
        historyInteger(workers, 65536, 'worker event limit'); historyInteger(flags, 3, 'CD flags')
        assert(C.historyBeginCDV3(events, bytes, workers, flags), 'CD v3 begin refused by native ownership, capacity or environment guards')
    end,
    cdWorkerStatus = function()
        local result = ffi.new('CDWorkerStatus[1]')
        assert(C.historyCDWorkerStatus(result), 'CD worker status requires a paused owner')
        return ffi.new('CDWorkerStatus', result[0])
    end,
    cdWorkerRecords = function(first, count)
        historyInteger(first, 65536, 'worker record first index')
        historyInteger(count, 65536, 'worker record count')
        local result = ffi.new('CDWorkerRecord[?]', count)
        assert(C.historyCDWorkerRecords(first, count, result), 'CD worker records require a frozen lane and valid range')
        return result
    end,
    beginCD = function(events, bytes, audio)
        historyInteger(events, 65536, 'event limit')
        historyInteger(bytes, 8388608, 'byte limit')
        assert(type(audio) == 'boolean', 'CD capture requires explicit joined-audio boolean')
        assert(events > 0 and C.historyBeginCDV3(events, bytes, 0, audio and 1 or 0),
               'CD begin requires empty recorder, context capacity, idle paused interpreter and active joined Audio')
    end,
    begin = function(events, bytes)
        historyInteger(events, 65536, 'event limit')
        historyInteger(bytes, 8388608, 'byte limit')
        assert(events > 0 and C.historyBegin(events, bytes),
               'history begin requires an empty recorder and paused owner-thread interpreter without MSAN')
    end,
    stop = function() assert(C.historyStop(), 'history stop requires a paused active recorder') end,
    clear = function() assert(C.historyClear(), 'history clear requires a paused inactive recorder') end,
    status = function()
        local result = ffi.new('HistoryStatus[1]')
        assert(C.historyStatus(result), 'history status requires the owner thread')
        return ffi.new("HistoryStatus", result[0])
    end,
    records = function(first, count)
        historyInteger(first, 65536, 'record first index')
        historyInteger(count, 65536, 'record count')
        local result = ffi.new('HistoryRecord[?]', count)
        assert(C.historyRecords(first, count, result), 'history records require paused frozen capture and a valid range')
        return result
    end,
    bytes = function(offset, length)
        historyInteger(offset, 8388608, 'payload offset')
        historyInteger(length, 8388608, 'payload length')
        local result = ffi.new('uint8_t[?]', math.max(1, length))
        assert(C.historyBytes(offset, length, result), 'history bytes require paused frozen capture and a valid range')
        return ffi.string(result, length)
    end,
}

local function removeBreakpoint(bp)
    C.removeBreakpoint(ffi.gc(bp._wrapper, nil))
    bp._wrapper = ffi.cast('Breakpoint*', 0)
    if bp._invokercb ~= nil then
        bp._invokercb:free()
        bp._invokercb = nil
    end
end

local function defaultInvoker(address, width, cause)
    C.pauseEmulator()
    return true
end

local validBpTypes = { Exec = true, Read = true, Write = true }

local function addBreakpoint(address, bptype, width, cause, invoker, label)
    if type(address) ~= 'number' then error 'PCSX.addBreakpoint needs an address' end
    if bptype == nil then bptype = 'Exec' end
    if not validBpTypes[bptype] then error 'PCSX.addBreakpoint needs a valid breakpoint type' end
    if width == nil then width = 4 end
    if type(width) ~= 'number' then error 'PCSX.addBreakpoint needs a width that is a number' end
    if cause == nil then cause = '' end
    if type(cause) == 'function' and invoker == nil then
        invoker = cause
        cause = ''
    end
    if type(cause) ~= 'string' then error 'PCSX.addBreakpoint needs a cause that is a string' end
    local invokercb = defaultInvoker
    if invoker ~= nil then
        if type(invoker) ~= 'function' then error 'PCSX.addBreakpoint needs an invoker that is a function' end
        invokercb = function(address, width, cause)
            cause = ffi.string(cause)
            local ret = invoker(address, width, cause)
            if ret == false then
                return false
            else
                return true
            end
        end
    end
    if label == nil then label = '' end
    if type(label) ~= 'string' then error 'PCSX.addBreakpoint needs a label that is a string' end
    invokercb = ffi.cast('bool (*)(uint32_t address, unsigned width, const char* cause)', invokercb)
    local wrapper = C.addBreakpoint(address, bptype, width, cause, invokercb, label)
    local bp = {
        _wrapper = wrapper,
        _proxy = newproxy(),
        _invokercb = invokercb,
        enable = function(bp) C.enableBreakpoint(bp._wrapper) end,
        disable = function(bp) C.disableBreakpoint(bp._wrapper) end,
        isEnabled = function(bp) return C.breakpointEnabled(bp._wrapper) end,
        remove = function(bp) removeBreakpoint(bp) end,
    }
    -- Use a proxy instead of doing this on the wrapper directly using ffi.gc, because of a bug in LuaJIT,
    -- where circular references on finalizers using ffi.gc won't actually collect anything.
    debug.setmetatable(bp._proxy, { __gc = function() removeBreakpoint(bp) end })
    return bp
end

local function printLike(callback, ...)
    local s = ''
    for i, v in ipairs({ ... }) do s = s .. tostring(v) .. ' ' end
    callback(s)
end

local function jumpToPC(pc)
    if type(pc) ~= 'number' then error 'PCSX.GUI.jumpToPC requires a numeric address' end
    C.jumpToPC(pc)
end

local function jumpToMemory(address, width)
    if type(address) ~= 'number' then error 'PCSX.GUI.jumpToMemory requires a numeric address' end
    if width == nil then width = 1 end
    if type(width) ~= 'number' then error 'PCSX.GUI.jumpToMemory requires a numeric width' end
    C.jumpToMemory(address, width)
end

PCSX = {
    nextPause = nativeNextPause,
    captureCPUChecked = function()
        local result, observation = nativeCPU()
        stateResult(result)
        return observation
    end,
    getCPUCycles = function() return C.getCPUCycles() end,
    getMemPtr = function() return C.getMemPtr() end,
    getParPtr = function() return C.getParPtr() end,
    getRomPtr = function() return C.getRomPtr() end,
    getScratchPtr = function() return C.getScratchPtr() end,
    getRegisters = function() return C.getRegisters() end,
    getReadLUT = function() return C.getReadLUT() end,
    getWriteLUT = function() return C.getWriteLUT() end,
    addBreakpoint = addBreakpoint,
    pauseEmulator = function() C.pauseEmulator() end,
    resumeEmulator = function() C.resumeEmulator() end,
    History = history,
    Debugger = {
        stepInto = function()
            assert(C.debuggerStepInto(), 'step requires a paused, non-quitting interpreter with debugger enabled')
        end,
    },
    softResetEmulator = function() C.softResetEmulator() end,
    hardResetEmulator = function() C.hardResetEmulator() end,
    invalidateCache = function() C.invalidateCache() end,
    log = function(...) printLike(function(msg) C.luaLog(msg .. '\n') end, ...) end,
    GUI = { jumpToPC = jumpToPC, jumpToMemory = jumpToMemory },
    nextTick = function(f)
        local oldCleanup = AfterPollingCleanup
        AfterPollingCleanup = function()
            if oldCleanup then oldCleanup() end
            f()
        end
    end,
    GPU = {
        takeScreenShot = function()
            local ss = C.takeScreenShot()
            return {
                data = Support.File._createSliceWrapper(ss.data),
                width = ss.width,
                height = ss.height,
                bpp = ss.bpp,
            }
        end,
        getGuestFPS = function() return C.getGuestFPS() end,
        getVRAM = function() return Support.File._createSliceWrapper(C.getVRAM()) end,
        startDump = function(file)
            if type(file) == 'string' then file = Support.File.open(file, 'TRUNCATE') end
            if type(file) ~= 'table' or file._type ~= 'File' then
                error('startDump: requires a File or a filename')
            end
            C.startGPUDump(file._wrapper)
        end,
        stopDump = function() C.stopGPUDump() end,
        isDumpArmed = function() return C.isGPUDumpArmed() end,
        isDumping = function() return C.isGPUDumpRecording() end,
        getDumpFrames = function() return tonumber(C.getGPUDumpFrames()) end,
        DumpPlayer = {
            load = function(file)
                if type(file) == 'string' then file = Support.File.open(file) end
                if type(file) ~= 'table' or file._type ~= 'File' then
                    error('load: requires a File or a filename')
                end
                return C.loadGPUDumpPlayer(file._wrapper)
            end,
            unload = function() C.unloadGPUDumpPlayer() end,
            step = function() return C.stepGPUDumpPlayer() end,
            rewind = function() C.rewindGPUDumpPlayer() end,
            getFrame = function() return tonumber(C.getGPUDumpPlayerFrame()) end,
            getVRAM = function() return Support.File._createSliceWrapper(C.getGPUDumpPlayerVRAM()) end,
        },
    },
    createSaveState = function()
        local slice = C.createSaveState()
        return Support.File._createSliceWrapper(slice)
    end,
    loadSaveState = function(obj)
        if type(obj) ~= 'table' then error('loadSaveState: requires an object as input') end
        if obj._type == 'Slice' then
            assert(rawget(obj, '_wrapper') ~= nil, 'loadSaveState: closed Slice')
            C.loadSaveStateFromSlice(obj._wrapper)
        elseif obj._type == 'File' then
            C.loadSaveStateFromFile(obj._wrapper)
        else
            error('loadSaveState: requires a Slice or File as input')
        end
    end,
    createSaveStateChecked = function()
        local output = ffi.new("LuaSlice*[1]")
        stateResult(C.createSaveStateChecked(output))
        return Support.File._createSliceWrapper(output[0])
    end,
    loadSaveStateChecked = function(obj)
        if type(obj) ~= 'table' then error('loadSaveState: requires an object as input') end
        if obj._type == 'Slice' then
            assert(rawget(obj, "_wrapper") ~= nil, "loadSaveState: closed Slice")
            stateResult(C.loadSaveStateFromSliceChecked(obj._wrapper))
        elseif obj._type == 'File' then
            stateResult(C.loadSaveStateFromFileChecked(obj._wrapper))
        else
            error('loadSaveState: requires a Slice or File as input')
        end
        return true
    end,
    getMemoryAsFile = function() return Support.File._createFileWrapper(C.getMemoryAsFile()) end,
    quit = function(code) C.quit(code or 0) end,
}

print = function(...) printLike(function(s) C.luaMessage(s, false) end, ...) end
printError = function(...) printLike(function(s) C.luaMessage(s, true) end, ...) end
bit.extract = function(x, n, w) return bit.rshift(bit.band(x, bit.lshift(1, w) - 1), n) end

return function()
    local history, nativeSnapshot = PCSX.History, PCSX.History.snapshot
    history.correlateCD = function(capture)
        return (capture or history.snapshot()).correlateCD()
    end
    history.snapshot = function()
        local owner = nativeSnapshot()
        local statusBytes, recordBytes, payload = owner:statusBytes(), owner:recordBytes(), owner:payload()
        assert(#statusBytes == ffi.sizeof('HistoryStatus'), 'invalid native history status size')
        local status = ffi.new('HistoryStatus')
        ffi.copy(status, statusBytes, #statusBytes)
        local count = tonumber(status.events)
        assert(#recordBytes == count * ffi.sizeof('HistoryRecord'), 'invalid native history record size')
        local records = ffi.new('HistoryRecord[?]', math.max(1, count))
        ffi.copy(records, recordBytes, #recordBytes)
        return {
            status = function() return ffi.new('HistoryStatus', status) end,
            records = function(first, length)
                historyInteger(first, count, 'record first index')
                historyInteger(length, count - first, 'record count')
                if first == 0 and length == count then return records end
                local result = ffi.new('HistoryRecord[?]', math.max(1, length))
                ffi.copy(result, records + first, length * ffi.sizeof('HistoryRecord'))
                return result
            end,
            bytes = function(offset, length)
                historyInteger(offset, #payload, 'payload offset')
                historyInteger(length, #payload - offset, 'payload length')
                return payload:sub(offset + 1, offset + length)
            end,
            correlateCD = function() return owner:correlateCD() end,
        }
    end
    local function checked(namespace, name)
        local native = assert(namespace[name])
        namespace[name] = function(...)
            local result, observation = native(...)
            stateResult(result)
            return observation
        end
    end
    for _, name in ipairs({'captureChecked', 'waitResponse', 'openLid', 'closeLid', 'scheduleCloseLid', 'mount', 'eject'}) do
        checked(PCSX.CD, name)
    end
    for _, name in ipairs({'captureChecked', 'editChecked', 'importChecked', 'flushChecked'}) do
        checked(PCSX.MemoryCards, name)
    end
end

-- )EOF"
