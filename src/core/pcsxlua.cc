/***************************************************************************
 *   Copyright (C) 2021 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

#include "core/history.h"
#include "core/transactions.h"
#include "core/pcsxlua.h"

#include "core/debug.h"
#include "core/gpu.h"
#include "core/gpudump.h"
#include "core/gpulogger.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/sstate.h"
#include "lua/luafile.h"
#include "lua/luawrapper.h"

namespace {

struct LuaBreakpoint {
    PCSX::Debug::BreakpointUserListType wrapper;
};

struct ByteInput {
    const uint8_t* data;
    uint32_t size;
};

struct ByteRange {
    uint32_t offset;
    uint32_t length;
};

struct RangeCursor {
    ByteInput before;
    ByteInput after;
    uint32_t position = 0;
    uint32_t changedBytes = 0;
    uint32_t first = 0;
    bool open = false;

    bool next(ByteRange& range) noexcept {
        const uint32_t count = std::max(before.size, after.size);
        const uint32_t common = std::min(before.size, after.size);
        while (position < count) {
            if (position >= common) {
                if (!open) {
                    first = position;
                    open = true;
                }
                changedBytes += count - position;
                position = count;
                break;
            }
            const uint32_t finish = std::min(position + 4096 - position % 4096, common);
            if (std::memcmp(before.data + position, after.data + position, finish - position) == 0) {
                const uint32_t last = position;
                position = finish;
                if (open) {
                    range = {first, last - first};
                    open = false;
                    return true;
                }
                continue;
            }
            while (position < finish) {
                if (before.data[position] != after.data[position]) {
                    if (!open) {
                        first = position;
                        open = true;
                    }
                    ++changedBytes;
                    ++position;
                    continue;
                }
                const uint32_t last = position++;
                if (open) {
                    range = {first, last - first};
                    open = false;
                    return true;
                }
            }
        }
        if (!open) return false;
        range = {first, count - first};
        open = false;
        return true;
    }
};

struct ScanSummary {
    uint32_t changedBytes = 0;
    uint32_t ranges = 0;
    bool limitExceeded = false;
};

ScanSummary summarizeByteRanges(ByteInput before, ByteInput after, uint32_t allowance) noexcept {
    ScanSummary summary;
    RangeCursor cursor{before, after};
    ByteRange range;
    while (cursor.next(range)) {
        if (summary.ranges == allowance) {
            summary.limitExceeded = true;
            return summary;
        }
        ++summary.ranges;
    }
    summary.changedBytes = cursor.changedBytes;
    return summary;
}

int scanByteRanges(lua_State* state) {
    if (lua_gettop(state) != 3 || lua_type(state, 1) != LUA_TSTRING ||
        lua_type(state, 2) != LUA_TSTRING || lua_type(state, 3) != LUA_TNUMBER)
        return luaL_error(state, "invalid byte scanner arguments");
    size_t beforeSize;
    size_t afterSize;
    const auto* beforeData = reinterpret_cast<const uint8_t*>(lua_tolstring(state, 1, &beforeSize));
    const auto* afterData = reinterpret_cast<const uint8_t*>(lua_tolstring(state, 2, &afterSize));
    const lua_Number allowance = lua_tonumber(state, 3);
    if (beforeSize > 67108864 || afterSize > 67108864 || !std::isfinite(allowance) ||
        allowance < 0 || allowance > 65536 || std::floor(allowance) != allowance)
        return luaL_error(state, "invalid byte scanner arguments");
    const ByteInput before{beforeData, static_cast<uint32_t>(beforeSize)};
    const ByteInput after{afterData, static_cast<uint32_t>(afterSize)};
    const ScanSummary summary = summarizeByteRanges(before, after, static_cast<uint32_t>(allowance));
    if (summary.limitExceeded) {
        lua_pushnil(state);
        lua_pushliteral(state, "limit");
        return 2;
    }
    lua_createtable(state, 0, 2);
    lua_pushliteral(state, "changed_bytes");
    lua_pushnumber(state, summary.changedBytes);
    lua_rawset(state, -3);
    lua_pushliteral(state, "ranges");
    lua_createtable(state, summary.ranges, 0);
    if (summary.ranges != 0) {
        RangeCursor cursor{before, after};
        ByteRange range;
        uint32_t index = 1;
        while (cursor.next(range)) {
            lua_createtable(state, 0, 2);
            lua_pushliteral(state, "offset");
            lua_pushnumber(state, range.offset);
            lua_rawset(state, -3);
            lua_pushliteral(state, "length");
            lua_pushnumber(state, range.length);
            lua_rawset(state, -3);
            lua_rawseti(state, -2, index++);
        }
        if (index - 1 != summary.ranges || cursor.changedBytes != summary.changedBytes)
            return luaL_error(state, "invalid byte scanner result");
    }
    lua_rawset(state, -3);
    return 1;
}

struct VRAMRectangle {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t cx;
    uint32_t cy;
    size_t rowBytes;
    size_t rawSize;
    size_t pixels;
    size_t rgbSize;
    size_t outputSize;
};

bool vramProduct(size_t left, size_t right, size_t& output) noexcept {
    if (right != 0 && left > std::numeric_limits<size_t>::max() / right) return false;
    output = left * right;
    return true;
}

bool vramSum(size_t left, size_t right, size_t& output) noexcept {
    if (left > std::numeric_limits<size_t>::max() - right) return false;
    output = left + right;
    return true;
}

bool sizeVRAMRectangle(VRAMRectangle& rectangle) noexcept {
    size_t rowBits;
    size_t roundedBits;
    size_t rawAndRGB;
    if (!vramProduct(rectangle.width, rectangle.bpp, rowBits) || !vramSum(rowBits, 7, roundedBits))
        return false;
    rectangle.rowBytes = roundedBits / 8;
    if (!vramProduct(rectangle.rowBytes, rectangle.height, rectangle.rawSize) ||
        !vramProduct(rectangle.width, rectangle.height, rectangle.pixels) ||
        !vramProduct(rectangle.pixels, 3, rectangle.rgbSize) ||
        !vramSum(rectangle.rawSize, rectangle.rgbSize, rawAndRGB) ||
        !vramSum(rawAndRGB, rectangle.pixels, rectangle.outputSize))
        return false;
    return rectangle.outputSize <= 9437184;
}

void convertVRAMBytes(const uint8_t* input, const VRAMRectangle& rectangle, uint8_t* output) noexcept {
    uint8_t* rgb = output + rectangle.rawSize;
    uint8_t* masks = rgb + rectangle.rgbSize;
    const size_t palette = size_t(rectangle.cy) * 2048 + size_t(rectangle.cx) * 2;
    for (uint32_t row = 0; row < rectangle.height; ++row) {
        const size_t start = size_t(rectangle.y + row) * 2048 + size_t(rectangle.x) * 2;
        std::memcpy(output + size_t(row) * rectangle.rowBytes, input + start, rectangle.rowBytes);
        for (uint32_t column = 0; column < rectangle.width; ++column) {
            size_t offset = start + size_t(column) * 2;
            if (rectangle.bpp != 16) {
                const uint32_t bits = column * rectangle.bpp;
                const uint32_t packed = input[start + bits / 8];
                const uint32_t index = (packed >> (bits % 8)) & ((1u << rectangle.bpp) - 1);
                offset = palette + size_t(index) * 2;
            }
            const uint32_t pixel = uint32_t(input[offset]) | (uint32_t(input[offset + 1]) << 8);
            const size_t position = size_t(row) * rectangle.width + column;
            rgb[position * 3] = ((pixel & 31) * 255 + 15) / 31;
            rgb[position * 3 + 1] = (((pixel >> 5) & 31) * 255 + 15) / 31;
            rgb[position * 3 + 2] = (((pixel >> 10) & 31) * 255 + 15) / 31;
            masks[position] = pixel >> 15;
        }
    }
}

int convertVRAM(lua_State* state) {
    if (lua_gettop(state) != 8) return luaL_error(state, "convertVRAM: expected 8 arguments");
    if (lua_type(state, 1) != LUA_TSTRING) return luaL_error(state, "convertVRAM: bytes must be a string");
    size_t inputSize;
    const auto* input = reinterpret_cast<const uint8_t*>(lua_tolstring(state, 1, &inputSize));
    if (inputSize != 1048576) return luaL_error(state, "convertVRAM: bytes must contain 1048576 bytes");
    const char* names[] = {"x", "y", "width", "height", "bpp", "cx", "cy"};
    lua_Number numbers[7] = {};
    for (int index = 0; index < 5; ++index) {
        if (lua_type(state, index + 2) != LUA_TNUMBER)
            return luaL_error(state, "convertVRAM: %s must be a finite integer", names[index]);
        numbers[index] = lua_tonumber(state, index + 2);
        if (!std::isfinite(numbers[index]) || std::floor(numbers[index]) != numbers[index])
            return luaL_error(state, "convertVRAM: %s must be a finite integer", names[index]);
    }
    if (numbers[4] != 4 && numbers[4] != 8 && numbers[4] != 16)
        return luaL_error(state, "convertVRAM: bpp must be 4, 8, or 16");
    if (numbers[0] < 0 || numbers[0] > 1023) return luaL_error(state, "convertVRAM: x out of range");
    if (numbers[1] < 0 || numbers[1] > 511) return luaL_error(state, "convertVRAM: y out of range");
    if (numbers[2] < 1 || numbers[2] > (1024 - numbers[0]) * 16 / numbers[4])
        return luaL_error(state, "convertVRAM: width out of range");
    if (numbers[3] < 1 || numbers[3] > 512 - numbers[1])
        return luaL_error(state, "convertVRAM: height out of range");
    for (int index = 5; index < 7; ++index) {
        if (numbers[4] == 16) {
            if (lua_type(state, index + 2) != LUA_TNIL)
                return luaL_error(state, "convertVRAM: %s must be nil at 16 bpp", names[index]);
            continue;
        }
        if (lua_type(state, index + 2) != LUA_TNUMBER)
            return luaL_error(state, "convertVRAM: %s must be a finite integer", names[index]);
        numbers[index] = lua_tonumber(state, index + 2);
        if (!std::isfinite(numbers[index]) || std::floor(numbers[index]) != numbers[index])
            return luaL_error(state, "convertVRAM: %s must be a finite integer", names[index]);
    }
    if (numbers[4] != 16) {
        if (numbers[5] < 0 || numbers[5] > 1024 - (1u << static_cast<uint32_t>(numbers[4])))
            return luaL_error(state, "convertVRAM: cx out of range");
        if (numbers[6] < 0 || numbers[6] > 511) return luaL_error(state, "convertVRAM: cy out of range");
    }
    VRAMRectangle rectangle{static_cast<uint32_t>(numbers[0]), static_cast<uint32_t>(numbers[1]),
                            static_cast<uint32_t>(numbers[2]), static_cast<uint32_t>(numbers[3]),
                            static_cast<uint32_t>(numbers[4]), static_cast<uint32_t>(numbers[5]),
                            static_cast<uint32_t>(numbers[6]), 0, 0, 0, 0, 0};
    if (!sizeVRAMRectangle(rectangle)) return luaL_error(state, "convertVRAM: output size out of range");
    const size_t sourceEnd = size_t(rectangle.y + rectangle.height - 1) * 2048 + size_t(rectangle.x) * 2 +
                             rectangle.rowBytes;
    if (sourceEnd > inputSize) return luaL_error(state, "convertVRAM: rectangle out of range");
    if (rectangle.bpp != 16 &&
        size_t(rectangle.cy) * 2048 + size_t(rectangle.cx + (1u << rectangle.bpp)) * 2 > inputSize)
        return luaL_error(state, "convertVRAM: palette out of range");
    luaL_checkstack(state, 4, "convertVRAM: insufficient stack");
    auto* output = static_cast<uint8_t*>(lua_newuserdata(state, rectangle.outputSize));
    convertVRAMBytes(input, rectangle, output);
    lua_pushlstring(state, reinterpret_cast<const char*>(output), rectangle.rawSize);
    lua_pushlstring(state, reinterpret_cast<const char*>(output + rectangle.rawSize), rectangle.rgbSize);
    lua_pushlstring(state, reinterpret_cast<const char*>(output + rectangle.rawSize + rectangle.rgbSize),
                    rectangle.pixels);
    return 3;
}

uint64_t getCPUCycles() { return PCSX::g_emulator->m_cpu->m_regs.cycle; }
void* getMemPtr() { return PCSX::g_emulator->m_mem->m_wram; }
void* getParPtr() { return PCSX::g_emulator->m_mem->m_exp1; }
void* getRomPtr() { return PCSX::g_emulator->m_mem->m_bios; }
void* getScratchPtr() { return PCSX::g_emulator->m_mem->m_hard; }
void* getRegisters() { return &PCSX::g_emulator->m_cpu->m_regs; }
void* getReadLUT() { return PCSX::g_emulator->m_mem->m_readLUT; }
void* getWriteLUT() { return PCSX::g_emulator->m_mem->m_writeLUT; }

LuaBreakpoint* addBreakpoint(uint32_t address, PCSX::Debug::BreakpointType type, unsigned width, const char* cause,
                             bool (*invoker)(uint32_t address, unsigned width, const char* cause), const char* label) {
    LuaBreakpoint* ret = new LuaBreakpoint();
    auto* bp = PCSX::g_emulator->m_debug->addBreakpoint(
        address, type, width, std::string("Lua Breakpoint"), cause,
        [invoker](const PCSX::Debug::Breakpoint* self, uint32_t address, unsigned width, const char* cause) {
            try {
                return invoker(address, width, cause);
            } catch (...) {
                PCSX::g_system->luaMessage("Lua Breakpoint invoker threw an exception, deleting breakpoint", true);
                return false;
            }
        });

    ret->wrapper.push_back(bp);
    return ret;
}
void enableBreakpoint(LuaBreakpoint* wrapper) {
    if (wrapper->wrapper.size() == 0) return;
    wrapper->wrapper.begin()->enable();
}
void disableBreakpoint(LuaBreakpoint* wrapper) {
    if (wrapper->wrapper.size() == 0) return;
    wrapper->wrapper.begin()->disable();
}
bool breakpointEnabled(LuaBreakpoint* wrapper) {
    if (wrapper->wrapper.size() == 0) return false;
    return wrapper->wrapper.begin()->enabled();
}
void removeBreakpoint(LuaBreakpoint* wrapper) {
    if (!wrapper) return;
    wrapper->wrapper.destroyAll();
    delete wrapper;
}
void pauseEmulator() { PCSX::g_system->pause(); }
void resumeEmulator() { PCSX::g_system->resume(); }
bool debuggerStepInto() {
    if (PCSX::g_system->running() || PCSX::g_system->quitting()) return false;
    if (PCSX::g_emulator->m_cpu->isDynarec()) return false;
    if (!PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
             .get<PCSX::Emulator::DebugSettings::Debug>()) return false;
    PCSX::g_emulator->m_debug->stepIn();
    return true;
}
bool historyBegin(uint32_t events, uint32_t bytes) { return PCSX::History::begin(events, bytes); }
bool historyBeginCD(uint32_t events, uint32_t bytes, bool audio) { return PCSX::History::beginCD(events, bytes, audio); }
bool historyCDLimits(uint32_t flags, PCSX::History::CD::Limits* out) noexcept {
    try { return PCSX::History::CD::limits(flags, out); } catch (...) { return false; }
}
bool historyBeginCDV3(uint32_t events, uint32_t bytes, uint32_t workerEvents, uint32_t flags) noexcept {
    try { return PCSX::History::beginCDV3(events, bytes, workerEvents, flags); } catch (...) { return false; }
}
bool historyCDWorkerStatus(PCSX::History::CD::WorkerStatus* out) noexcept {
    try { return PCSX::History::CD::workerStatus(out); } catch (...) { return false; }
}
bool historyCDWorkerRecord(uint32_t index, PCSX::History::CD::WorkerRecord* out) noexcept {
    try { return PCSX::History::CD::workerRecord(index, out); } catch (...) { return false; }
}
bool historyStop() { return PCSX::History::stop(); }
bool historyClear() { return PCSX::History::clear(); }
bool historyStatus(PCSX::History::Status* status) { return PCSX::History::status(status); }
bool historyRecord(uint32_t index, PCSX::History::Record* record) { return PCSX::History::record(index, record); }
bool historyBytes(uint32_t offset, uint32_t length, void* output) { return PCSX::History::bytes(offset, length, output); }
void softResetEmulator() { PCSX::g_system->softReset(); }
void hardResetEmulator() { PCSX::g_system->hardReset(); }
void luaMessage(const char* msg, bool error) { PCSX::g_system->luaMessage(msg, error); }
void luaLog(const char* msg) { PCSX::g_system->log(PCSX::LogClass::LUA, msg); }
void jumpToPC(uint32_t pc) { PCSX::g_system->m_eventBus->signal(PCSX::Events::GUI::JumpToPC{pc}); }
void jumpToMemory(uint32_t address, unsigned width) {
    PCSX::g_system->m_eventBus->signal(PCSX::Events::GUI::JumpToMemory{address, width});
}
void invalidateCache() { PCSX::g_emulator->m_cpu->invalidateCache(); }

struct LuaScreenShot {
    PCSX::Slice* data;
    uint16_t width, height;
    decltype(PCSX::GPU::ScreenShot::bpp) bpp;
};

LuaScreenShot takeScreenShot() {
    LuaScreenShot ret;
    auto ss = PCSX::g_emulator->m_gpu->takeScreenShot();
    ret.data = new PCSX::Slice(std::move(ss.data));
    ret.width = ss.width;
    ret.height = ss.height;
    ret.bpp = ss.bpp;
    return ret;
}

double getGuestFPS() { return PCSX::g_emulator->m_gpuLogger->getGuestFPS(); }

void startGPUDump(PCSX::LuaFFI::LuaFile* file) { PCSX::g_emulator->m_gpuDumper->start(file->file); }
void stopGPUDump() { PCSX::g_emulator->m_gpuDumper->stop(); }
bool isGPUDumpArmed() { return PCSX::g_emulator->m_gpuDumper->armed(); }
bool isGPUDumpRecording() { return PCSX::g_emulator->m_gpuDumper->recording(); }
uint64_t getGPUDumpFrames() { return PCSX::g_emulator->m_gpuDumper->frames(); }

bool loadGPUDumpPlayer(PCSX::LuaFFI::LuaFile* file) { return PCSX::g_emulator->m_gpuDumpPlayer->load(file->file); }
void unloadGPUDumpPlayer() { PCSX::g_emulator->m_gpuDumpPlayer->unload(); }
bool stepGPUDumpPlayer() { return PCSX::g_emulator->m_gpuDumpPlayer->step(); }
void rewindGPUDumpPlayer() { PCSX::g_emulator->m_gpuDumpPlayer->rewind(); }
uint64_t getGPUDumpPlayerFrame() { return PCSX::g_emulator->m_gpuDumpPlayer->frame(); }
PCSX::Slice* getGPUDumpPlayerVRAM() {
    auto gpu = PCSX::g_emulator->m_gpuDumpPlayer->gpu();
    if (!gpu) return new PCSX::Slice();
    return new PCSX::Slice(gpu->getVRAM(PCSX::GPU::Ownership::ACQUIRE));
}
PCSX::Slice* getVRAM() { return new PCSX::Slice(PCSX::g_emulator->m_gpu->getVRAM(PCSX::GPU::Ownership::ACQUIRE)); }

PCSX::Slice* createSaveState() {
    auto ss = PCSX::SaveStates::save();
    return new PCSX::Slice(std::move(ss));
}

int captureCPUChecked(lua_State* state) {
    using namespace PCSX::SaveStates;
    CPUObservation observation{};
    const auto result = lua_gettop(state) == 0 ? captureCPUChecked(observation) : Result::InvalidState;
    lua_pushnumber(state, static_cast<uint32_t>(result));
    if (result != Result::Ok) return 1;
    // The checked state lease and SPU access have ended before any Lua allocation.
    const auto number = [&](const char* name, uint32_t value) {
        lua_pushnumber(state, value);
        lua_setfield(state, -2, name);
    };
    const auto boolean = [&](const char* name, bool value) {
        lua_pushboolean(state, value);
        lua_setfield(state, -2, name);
    };
    lua_createtable(state, 0, 14);
    number("pc", observation.pc);
    number("last_executed_word", observation.code);
    number("current_delayed_load", observation.currentDelayedLoad);
    boolean("in_isr", observation.inISR);
    boolean("next_is_delay_slot", observation.nextIsDelaySlot);
    char cycles[20];
    const auto decimal = std::to_chars(std::begin(cycles), std::end(cycles), observation.cycle);
    lua_pushlstring(state, cycles, decimal.ptr - cycles);
    lua_setfield(state, -2, "cycles");
    const auto bank = [&](const char* name, const auto& values) {
        lua_createtable(state, values.size(), 0);
        for (size_t i = 0; i < values.size(); ++i) {
            lua_pushnumber(state, values[i]);
            lua_rawseti(state, -2, i + 1);
        }
        lua_setfield(state, -2, name);
    };
    bank("gpr", observation.gpr);
    bank("cp0", observation.cp0);
    for (unsigned i = 0; i < observation.delays.size(); ++i) {
        const auto& delay = observation.delays[i];
        if (!delay.present) continue;
        lua_createtable(state, 0, 7);
        number("index", delay.index);
        number("value", delay.value);
        number("mask", delay.mask);
        number("pc_value", delay.pcValue);
        boolean("active", delay.active);
        boolean("pc_active", delay.pcActive);
        boolean("from_link", delay.fromLink);
        lua_setfield(state, -2, i ? "delay_slot_info_2" : "delay_slot_info_1");
    }
    if (observation.nextWord) number("next_memory_word", *observation.nextWord);
    lua_createtable(state, observation.nearbyCount, 0);
    for (size_t i = 0; i < observation.nearbyCount; ++i) {
        lua_createtable(state, 0, 2);
        number("address", observation.nearby[i].address);
        number("word", observation.nearby[i].value);
        lua_rawseti(state, -2, i + 1);
    }
    lua_setfield(state, -2, "nearby_memory");
    const uint32_t cause = observation.cp0[13];
    lua_createtable(state, 0, 6);
    number("cause", cause);
    number("epc", observation.cp0[14]);
    number("status", observation.cp0[12]);
    number("badvaddr", observation.cp0[8]);
    number("code", (cause >> 2) & 31);
    boolean("branch_delay", (cause & 0x80000000u) != 0);
    lua_setfield(state, -2, "exception");
    return 2;
}

void loadSaveStateFromSlice(PCSX::Slice* data) { PCSX::SaveStates::loadSafe(std::string(data->asStringView())); }

void loadSaveStateFromFile(PCSX::LuaFFI::LuaFile* file) {
    auto data = file->file->readAt(64 * 1024 * 1024, 0);
    PCSX::SaveStates::loadSafe(std::string(data.asStringView()));
}

uint32_t createSaveStateChecked(PCSX::Slice** output) noexcept {
    using namespace PCSX::SaveStates;
    const auto guard = context();
    if (guard != Result::Ok) return static_cast<uint32_t>(guard);
    if (!output) return static_cast<uint32_t>(Result::InvalidState);
    *output = nullptr;
    try {
        std::string bytes;
        const auto result = saveChecked(bytes);
        if (result != Result::Ok) return static_cast<uint32_t>(result);
        *output = new PCSX::Slice(std::move(bytes));
        return static_cast<uint32_t>(Result::Ok);
    } catch (const std::bad_alloc&) {
        return static_cast<uint32_t>(Result::AllocationFailed);
    } catch (...) {
        return static_cast<uint32_t>(Result::InvalidState);
    }
}

uint32_t loadSaveStateFromSliceChecked(PCSX::Slice* data) noexcept {
    using namespace PCSX::SaveStates;
    const auto guard = context();
    if (guard != Result::Ok) return static_cast<uint32_t>(guard);
    if (!data || (data->size() && !data->data<uint8_t>())) return static_cast<uint32_t>(Result::InvalidState);
    try { return static_cast<uint32_t>(loadChecked(data->asStringView())); }
    catch (...) { return static_cast<uint32_t>(Result::Malformed); }
}

uint32_t loadSaveStateFromFileChecked(PCSX::LuaFFI::LuaFile* wrapper) noexcept {
    using namespace PCSX::SaveStates;
    const auto guard = context();
    if (guard != Result::Ok) return static_cast<uint32_t>(guard);
    if (!wrapper || !wrapper->file) return static_cast<uint32_t>(Result::InvalidState);
    std::string bytes;
    PCSX::File::RawSnapshotStatus copied;
    {
        PCSX::IO<PCSX::File> owner = wrapper->file;
        copied = owner->snapshotRawOwned(MaxSize, bytes);
    }
    using Raw = PCSX::File::RawSnapshotStatus;
    switch (copied) {
        case Raw::Ok: return static_cast<uint32_t>(loadChecked(bytes));
        case Raw::Unsupported: return static_cast<uint32_t>(Result::InvalidState);
        case Raw::TooLarge: return static_cast<uint32_t>(Result::TooLarge);
        case Raw::Invalid: return static_cast<uint32_t>(Result::Malformed);
        case Raw::AllocationFailed: return static_cast<uint32_t>(Result::AllocationFailed);
    }
    return static_cast<uint32_t>(Result::InvalidState);
}

PCSX::LuaFFI::LuaFile* getMemoryAsFile() {
    return new PCSX::LuaFFI::LuaFile(PCSX::g_emulator->m_mem->getMemoryAsFile());
}

void quit(int code) { PCSX::g_system->quit(code); }

}  // namespace

template <typename T, size_t S>
static void registerSymbol(PCSX::Lua L, const char (&name)[S], const T ptr) {
    L.push<S>(name);
    L.push((void*)ptr);
    L.settable();
}

#define REGISTER(L, s) registerSymbol(L, #s, s)

static void registerAllSymbols(PCSX::Lua L) {
    L.getfieldtable("_CLIBS", LUA_REGISTRYINDEX);
    L.push("PCSX");
    L.newtable();
    REGISTER(L, getCPUCycles);
    REGISTER(L, getMemPtr);
    REGISTER(L, getParPtr);
    REGISTER(L, getRomPtr);
    REGISTER(L, getScratchPtr);
    REGISTER(L, getRegisters);
    REGISTER(L, getReadLUT);
    REGISTER(L, getWriteLUT);
    REGISTER(L, addBreakpoint);
    REGISTER(L, enableBreakpoint);
    REGISTER(L, disableBreakpoint);
    REGISTER(L, breakpointEnabled);
    REGISTER(L, removeBreakpoint);
    REGISTER(L, pauseEmulator);
    REGISTER(L, resumeEmulator);
    REGISTER(L, debuggerStepInto);
    REGISTER(L, historyBegin);
    REGISTER(L, historyBeginCD);
    REGISTER(L, historyCDLimits);
    REGISTER(L, historyBeginCDV3);
    REGISTER(L, historyCDWorkerStatus);
    REGISTER(L, historyCDWorkerRecord);
    REGISTER(L, historyStop);
    REGISTER(L, historyClear);
    REGISTER(L, historyStatus);
    REGISTER(L, historyRecord);
    REGISTER(L, historyBytes);

    REGISTER(L, softResetEmulator);
    REGISTER(L, hardResetEmulator);
    REGISTER(L, luaMessage);
    REGISTER(L, luaLog);
    REGISTER(L, jumpToPC);
    REGISTER(L, jumpToMemory);
    REGISTER(L, invalidateCache);
    REGISTER(L, takeScreenShot);
    REGISTER(L, getGuestFPS);
    REGISTER(L, startGPUDump);
    REGISTER(L, stopGPUDump);
    REGISTER(L, isGPUDumpArmed);
    REGISTER(L, isGPUDumpRecording);
    REGISTER(L, getGPUDumpFrames);
    REGISTER(L, loadGPUDumpPlayer);
    REGISTER(L, unloadGPUDumpPlayer);
    REGISTER(L, stepGPUDumpPlayer);
    REGISTER(L, rewindGPUDumpPlayer);
    REGISTER(L, getGPUDumpPlayerFrame);
    REGISTER(L, getGPUDumpPlayerVRAM);
    REGISTER(L, getVRAM);
    REGISTER(L, createSaveState);
    REGISTER(L, loadSaveStateFromSlice);
    REGISTER(L, loadSaveStateFromFile);
    REGISTER(L, createSaveStateChecked);
    REGISTER(L, loadSaveStateFromSliceChecked);
    REGISTER(L, loadSaveStateFromFileChecked);
    REGISTER(L, getMemoryAsFile);
    REGISTER(L, quit);
    L.settable();
    L.pop();
}

void PCSX::LuaFFI::open_pcsx(Lua L) {
    History::initialize();
    static int lualoader = 1;
    static const char* pcsxFFI = (
#include "core/pcsxffi.lua"
    );
    registerAllSymbols(L);
    L.load(pcsxFFI, "src:core/pcsxffi.lua", false);
    L.push(lua_CFunction(captureCPUChecked));
    L.pcall(1);
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.declareFunc("scanByteRanges", scanByteRanges, -1);
    L.declareFunc("convertVRAM", convertVRAM, -1);
    L.push("execSlots");
    L.newtable();
    L.settable();
    L.declareFunc("getSaveStateInfo", [](lua_State* state) -> int {
        lua_newtable(state);
        lua_pushnumber(state, SaveStates::Format::Version);
        lua_setfield(state, -2, "version");
        lua_pushstring(state, SaveStates::Format::VersionString);
        lua_setfield(state, -2, "versionString");
        lua_pushnumber(state, SaveStates::Format::MaxSize);
        lua_setfield(state, -2, "maxSize");
        return 1;
    }, -1);
    L.declareFunc(
        "getSaveStateProtoSchema",
        [](lua_State* L_) -> int {
            Lua L(L_);
            std::ostringstream os;
            SaveStates::ProtoFile::dumpSchema(os);
            L.push(os.str());
            return 1;
        },
        -1);
    L.declareFunc(
        "insertSymbol",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() != 2) {
                return L.error("Wrong number of arguments to insertSymbol");
            }
            uint32_t address = L.checknumber(1);
            auto name = L.tostring(2);
            g_emulator->m_cpu->m_symbols[address] = name;
            return 0;
        },
        -1);
    L.declareFunc(
        "removeSymbol",
        [](lua_State* L_) -> int {
            auto& symbols = g_emulator->m_cpu->m_symbols;
            Lua L(L_);
            if (L.gettop() != 1) {
                return L.error("Wrong number of arguments to insertSymbol");
            }
            auto i = symbols.begin();
            if (L.isnumber()) {
                i = symbols.find(L.tonumber());
            } else {
                auto name = L.tostring();
                while (i != symbols.end()) {
                    if (i->second == name) break;
                    i++;
                }
            }
            if (i != symbols.end()) symbols.erase(i);
            return 0;
        },
        -1);
    L.declareFunc(
        "iterateSymbols",
        [](lua_State* L_) -> int {
            Lua L(L_);
            L.push([](lua_State* L_) -> int {
                auto& symbols = g_emulator->m_cpu->m_symbols;
                Lua L(L_);
                if (L.gettop() != 2) {
                    return L.error("Wrong number of arguments");
                }
                auto iter = symbols.begin();
                if (L.isnumber(-1)) {
                    iter = symbols.find(L.tonumber(-1));
                    if (iter != symbols.end()) iter++;
                }
                if (iter != symbols.end()) {
                    L.push(lua_Number(iter->first));
                    L.push(iter->second);
                    return 2;
                }
                return 0;
            });
            L.push();
            L.push();
            return 3;
        },
        -1);
    L.pop();
}
