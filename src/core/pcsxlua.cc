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
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "core/history.h"
#include "core/historylua.h"
#include "core/cdlua.h"
#include "core/memorycardlua.h"
#include "core/transactions.h"
#include "core/pcsxlua.h"

#include "core/debug.h"
#include "core/gpu.h"
#include "core/gpupackets.h"
#include "core/gpudump.h"
#include "core/gpulogger.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/sstate.h"
#include "core/stateprojection.h"
#include "lua/luafile.h"
#include "lua/luawrapper.h"
#include "support/protobuf-wire.h"

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

void writeBGR555RGB(uint32_t pixel, uint8_t* output) noexcept {
    output[0] = ((pixel & 31) * 255 + 15) / 31;
    output[1] = (((pixel >> 5) & 31) * 255 + 15) / 31;
    output[2] = (((pixel >> 10) & 31) * 255 + 15) / 31;
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
            writeBGR555RGB(pixel, rgb + position * 3);
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

int convertScreenshot(lua_State* state) {
    if (lua_gettop(state) != 4) return luaL_error(state, "convertScreenshot: expected 4 arguments");
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "convertScreenshot: bytes must be a string");
    size_t inputSize;
    const auto* input = reinterpret_cast<const uint8_t*>(lua_tolstring(state, 1, &inputSize));
    const char* names[] = {"width", "height", "bpp"};
    lua_Number numbers[3] = {};
    for (int index = 0; index < 3; ++index) {
        if (lua_type(state, index + 2) != LUA_TNUMBER)
            return luaL_error(state, "convertScreenshot: %s must be a finite integer", names[index]);
        numbers[index] = lua_tonumber(state, index + 2);
        if (!std::isfinite(numbers[index]) || std::floor(numbers[index]) != numbers[index])
            return luaL_error(state, "convertScreenshot: %s must be a finite integer", names[index]);
    }
    if (numbers[0] < 0 || numbers[0] > 1024)
        return luaL_error(state, "convertScreenshot: width out of range");
    if (numbers[1] < 0 || numbers[1] > 512)
        return luaL_error(state, "convertScreenshot: height out of range");
    if (numbers[2] != 0 && numbers[2] != 1)
        return luaL_error(state, "convertScreenshot: bpp must be 0 or 1");
    size_t pixels;
    size_t expectedSize;
    size_t rgbSize;
    if (!vramProduct(static_cast<size_t>(numbers[0]), static_cast<size_t>(numbers[1]), pixels) ||
        !vramProduct(pixels, numbers[2] == 0 ? 2 : 3, expectedSize) ||
        !vramProduct(pixels, 3, rgbSize))
        return luaL_error(state, "convertScreenshot: output size out of range");
    if (inputSize != expectedSize)
        return luaL_error(state, "convertScreenshot: unexpected screenshot storage");
    luaL_checkstack(state, 2, "convertScreenshot: insufficient stack");
    if (pixels == 0) {
        lua_pushliteral(state, "");
        return 1;
    }
    if (numbers[2] == 1) {
        lua_pushvalue(state, 1);
        return 1;
    }
    auto* output = static_cast<uint8_t*>(lua_newuserdata(state, rgbSize));
    for (size_t index = 0; index < pixels; ++index) {
        const size_t offset = index * 2;
        const uint32_t pixel = uint32_t(input[offset]) | (uint32_t(input[offset + 1]) << 8);
        writeBGR555RGB(pixel, output + index * 3);
    }
    lua_pushlstring(state, reinterpret_cast<const char*>(output), rgbSize);
    return 1;
}

uint32_t probeInteger(lua_State* state, int index, const char* label) {
    if (lua_type(state, index) != LUA_TNUMBER)
        luaL_error(state, "%s must be a uint32 integer", label);
    const lua_Number number = lua_tonumber(state, index);
    if (!std::isfinite(number) || number < 0 || number > UINT32_MAX || std::floor(number) != number)
        luaL_error(state, "%s must be a uint32 integer", label);
    return static_cast<uint32_t>(number);
}

uint32_t probeScratch(lua_State* state) {
    const uint32_t start = probeInteger(state, 1, "scratch");
    const uint32_t segment = start >> 29;
    if (segment != 0 && segment != 4 && segment != 5)
        luaL_error(state, "scratch must use a RAM address in KUSEG, KSEG0 or KSEG1");
    if (start & 3) luaL_error(state, "scratch must be 4-byte aligned");
    return start;
}

void probeSequence(lua_State* state, int index, size_t length, const char* message) {
    if (index < 0) index = lua_gettop(state) + index + 1;
    size_t entries = 0;
    lua_pushnil(state);
    while (lua_next(state, index)) {
        if (lua_type(state, -2) != LUA_TNUMBER) luaL_error(state, "%s", message);
        const lua_Number key = lua_tonumber(state, -2);
        if (!std::isfinite(key) || key < 1 || key > length || std::floor(key) != key)
            luaL_error(state, "%s", message);
        ++entries;
        lua_pop(state, 1);
    }
    if (entries != length) luaL_error(state, "%s", message);
}

void writeProbeWord(uint8_t* output, size_t offset, uint32_t word) noexcept {
    for (unsigned byte = 0; byte < 4; ++byte) output[offset + byte] = word >> (byte * 8);
}

uint8_t* newProbe(lua_State* state, uint32_t start, uint32_t width, size_t operations) {
    if (operations == 0 || operations > 262000) luaL_error(state, "invalid MMIO operation count");
    uint32_t store = 0;
    switch (width) {
        case 8: store = 0xa14b0000; break;
        case 16: store = 0xa54b0000; break;
        case 32: store = 0xad4b0000; break;
        default: luaL_error(state, "MMIO store width must be 8, 16, or 32");
    }
    const size_t bytes = 52 + operations * 8;
    const uint32_t physical = start & 0x1fffffff;
    if (physical > 2097152 || bytes > 2097152 - physical)
        luaL_error(state, "scratch region does not fit RAM");
    const uint32_t data = start + 52;
    const uint32_t words[] = {
        0x3c080000 | (data >> 16), 0x35080000 | (data & 65535),
        0x3c090000 | (uint32_t(operations) >> 16), 0x35290000 | (uint32_t(operations) & 65535),
        0x8d0a0000, 0x8d0b0004, 0, store, 0x25080008, 0x2529ffff, 0x1520fff9, 0, 0,
    };
    auto* output = static_cast<uint8_t*>(lua_newuserdata(state, bytes));
    for (size_t index = 0; index < std::size(words); ++index) writeProbeWord(output, index * 4, words[index]);
    return output;
}

int packMMIOProbe(lua_State* state) {
    if (lua_gettop(state) != 3 || lua_type(state, 3) != LUA_TTABLE)
        return luaL_error(state, "packMMIOProbe: expected scratch, store width and operations table");
    const uint32_t start = probeScratch(state);
    const uint32_t width = probeInteger(state, 2, "MMIO store width");
    const size_t count = lua_objlen(state, 3);
    luaL_checkstack(state, 4, "packMMIOProbe: insufficient stack");
    auto* output = newProbe(state, start, width, count);
    probeSequence(state, 3, count, "packMMIOProbe: operations must be a contiguous array");
    for (size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, 3, index + 1);
        if (lua_type(state, -1) != LUA_TTABLE || lua_objlen(state, -1) != 2)
            return luaL_error(state, "packMMIOProbe: operations must be address/value pairs");
        probeSequence(state, -1, 2, "packMMIOProbe: operations must be address/value pairs");
        lua_rawgeti(state, -1, 1);
        const uint32_t address = probeInteger(state, -1, "MMIO address");
        lua_pop(state, 1);
        lua_rawgeti(state, -1, 2);
        const uint32_t value = probeInteger(state, -1, "MMIO value");
        lua_pop(state, 2);
        writeProbeWord(output, 52 + index * 8, address);
        writeProbeWord(output, 56 + index * 8, value);
    }
    lua_pushlstring(state, reinterpret_cast<const char*>(output), 52 + count * 8);
    return 1;
}

int packVRAMUploadProbe(lua_State* state) {
    if (lua_gettop(state) != 6 || lua_type(state, 6) != LUA_TSTRING)
        return luaL_error(state, "packVRAMUploadProbe: expected scratch, x, y, width, height and pixels");
    const uint32_t start = probeScratch(state);
    const uint32_t x = probeInteger(state, 2, "VRAM x");
    const uint32_t y = probeInteger(state, 3, "VRAM y");
    const uint32_t width = probeInteger(state, 4, "VRAM width");
    const uint32_t height = probeInteger(state, 5, "VRAM height");
    if (x > 1023 || y > 511 || width == 0 || width > 1024 - x || height == 0 || height > 512 - y)
        return luaL_error(state, "packVRAMUploadProbe: rectangle out of range");
    size_t size;
    const auto* pixels = reinterpret_cast<const uint8_t*>(lua_tolstring(state, 6, &size));
    if (size != size_t(width) * height * 2)
        return luaL_error(state, "pixels input must exactly fit rectangle");
    const size_t count = 4 + (size + 3) / 4;
    luaL_checkstack(state, 2, "packVRAMUploadProbe: insufficient stack");
    auto* output = newProbe(state, start, 32, count);
    const uint32_t commands[] = {0x01000000, 0xa0000000, (y << 16) | x, (height << 16) | width};
    for (size_t index = 0; index < std::size(commands); ++index) {
        writeProbeWord(output, 52 + index * 8, index == 0 ? 0x1f801814 : 0x1f801810);
        writeProbeWord(output, 56 + index * 8, commands[index]);
    }
    for (size_t offset = 0; offset < size; offset += 4) {
        uint32_t word = 0;
        for (size_t byte = 0; byte < std::min(size_t(4), size - offset); ++byte)
            word |= uint32_t(pixels[offset + byte]) << (byte * 8);
        writeProbeWord(output, 84 + offset * 2, 0x1f801810);
        writeProbeWord(output, 88 + offset * 2, word);
    }
    lua_pushlstring(state, reinterpret_cast<const char*>(output), 52 + count * 8);
    return 1;
}

char jsonArrayMetatableMarker;
char jsonWriterMetatableMarker;

struct JSONKey {
    const char* bytes;
    size_t size;
    int slot;
};

struct JSONWriter {
    static constexpr size_t maxDepth = 64;
    static constexpr size_t maxValues = 1000000;
    static constexpr size_t maxOutput = 67108864;
    std::string output;
    std::array<std::vector<JSONKey>, maxDepth> keys;
    std::array<const void*, maxDepth> ancestors = {};
    size_t values = 0;
    const char* error = nullptr;

    bool fail(const char* message) {
        error = message;
        return false;
    }

    bool visit() {
        if (values == maxValues) return fail("value budget exceeded");
        ++values;
        return true;
    }

    bool append(const char* bytes, size_t size) {
        if (size > maxOutput - output.size()) return fail("output exceeds 64 MiB");
        output.append(bytes, size);
        return true;
    }

    template <size_t Size>
    bool append(const char (&bytes)[Size]) {
        return append(bytes, Size - 1);
    }

    bool string(const char* bytes, size_t size) {
        if (maxOutput - output.size() < 2 || size > maxOutput - output.size() - 2)
            return fail("output exceeds 64 MiB");
        if (!append("\"")) return false;
        size_t start = 0;
        constexpr char hex[] = "0123456789abcdef";
        for (size_t index = 0; index < size; ++index) {
            const auto byte = static_cast<uint8_t>(bytes[index]);
            if (byte >= 32 && byte != '\\' && byte != '"') continue;
            const char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
            if (!append(bytes + start, index - start) || !append(escaped, sizeof(escaped))) return false;
            start = index + 1;
        }
        return append(bytes + start, size - start) && append("\"");
    }
};

int destroyJSONWriter(lua_State* state) {
    static_cast<JSONWriter*>(lua_touserdata(state, 1))->~JSONWriter();
    return 0;
}

void pushJSONMetatable(lua_State* state, void* marker, lua_CFunction finalizer = nullptr) {
    lua_pushlightuserdata(state, marker);
    lua_rawget(state, LUA_REGISTRYINDEX);
    if (!lua_isnil(state, -1)) return;
    lua_pop(state, 1);
    lua_newtable(state);
    if (finalizer) {
        lua_pushcfunction(state, finalizer);
        lua_setfield(state, -2, "__gc");
    }
    lua_pushliteral(state, "native JSON metatable");
    lua_setfield(state, -2, "__metatable");
    lua_pushlightuserdata(state, marker);
    lua_pushvalue(state, -2);
    lua_rawset(state, LUA_REGISTRYINDEX);
}

bool isJSONArray(lua_State* state, int index) {
    if (!lua_getmetatable(state, index)) return false;
    lua_pushlightuserdata(state, &jsonArrayMetatableMarker);
    lua_rawget(state, LUA_REGISTRYINDEX);
    const bool tagged = lua_rawequal(state, -1, -2);
    lua_pop(state, 2);
    return tagged;
}

const char* validateJSONArray(lua_State* state, int index, size_t length) {
    if (length > JSONWriter::maxValues) return "array exceeds value budget";
    const int base = lua_gettop(state);
    size_t entries = 0;
    lua_pushnil(state);
    while (lua_next(state, index)) {
        if (lua_type(state, -2) != LUA_TNUMBER) {
            lua_settop(state, base);
            return "array keys must be contiguous positive integers";
        }
        const lua_Number key = lua_tonumber(state, -2);
        if (!std::isfinite(key) || std::floor(key) != key || key < 1 || key > length ||
            ++entries > JSONWriter::maxValues) {
            lua_settop(state, base);
            return "array keys must be contiguous positive integers";
        }
        lua_pop(state, 1);
    }
    return entries == length ? nullptr : "array keys must be contiguous positive integers";
}

bool writeJSONValue(lua_State* state, int index, JSONWriter& writer, size_t depth) {
    if (depth >= JSONWriter::maxDepth) return writer.fail("depth exceeds 64 levels");
    if (!writer.visit()) return false;
    switch (lua_type(state, index)) {
        case LUA_TNIL:
            return writer.append("null");
        case LUA_TBOOLEAN:
            return lua_toboolean(state, index) ? writer.append("true") : writer.append("false");
        case LUA_TNUMBER: {
            const lua_Number value = lua_tonumber(state, index);
            if (!std::isfinite(value)) return writer.fail("nonfinite JSON number");
            char number[64];
            const auto formatted = std::to_chars(std::begin(number), std::end(number), value,
                                                 std::chars_format::general, 17);
            if (formatted.ec != std::errc{})
                return writer.fail("invalid JSON number format");
            return writer.append(number, formatted.ptr - number);
        }
        case LUA_TSTRING: {
            size_t size;
            const char* bytes = lua_tolstring(state, index, &size);
            return writer.string(bytes, size);
        }
        case LUA_TTABLE:
            break;
        default:
            return writer.fail("unsupported JSON value");
    }
    if (!lua_checkstack(state, 8)) return writer.fail("insufficient Lua stack");
    const void* identity = lua_topointer(state, index);
    for (size_t ancestor = 0; ancestor < depth; ++ancestor) {
        if (writer.ancestors[ancestor] == identity) return writer.fail("cyclic JSON table");
    }
    writer.ancestors[depth] = identity;
    const size_t length = lua_objlen(state, index);
    if (length || isJSONArray(state, index)) {
        if (const char* error = validateJSONArray(state, index, length)) return writer.fail(error);
        if (!writer.append("[")) return false;
        for (size_t item = 1; item <= length; ++item) {
            if (item > 1 && !writer.append(",")) return false;
            lua_rawgeti(state, index, static_cast<int>(item));
            if (!writeJSONValue(state, lua_gettop(state), writer, depth + 1)) return false;
            lua_pop(state, 1);
        }
        writer.ancestors[depth] = nullptr;
        return writer.append("]");
    }
    lua_newtable(state);
    const int keyTable = lua_gettop(state);
    auto& keys = writer.keys[depth];
    keys.clear();
    lua_pushnil(state);
    while (lua_next(state, index)) {
        if (lua_type(state, -2) != LUA_TSTRING) return writer.fail("object keys must be strings");
        if (!writer.visit()) return false;
        const int slot = static_cast<int>(keys.size() + 1);
        lua_pushvalue(state, -2);
        lua_rawseti(state, keyTable, slot);
        size_t size;
        const char* bytes = lua_tolstring(state, -2, &size);
        keys.push_back({bytes, size, slot});
        lua_pop(state, 1);
    }
    std::sort(keys.begin(), keys.end(), [](const JSONKey& left, const JSONKey& right) {
        const int order = std::memcmp(left.bytes, right.bytes, std::min(left.size, right.size));
        return order < 0 || (order == 0 && left.size < right.size);
    });
    if (!writer.append("{")) return false;
    for (size_t item = 0; item < keys.size(); ++item) {
        const auto& key = keys[item];
        if ((item && !writer.append(",")) || !writer.string(key.bytes, key.size) || !writer.append(":"))
            return false;
        lua_rawgeti(state, keyTable, key.slot);
        lua_rawget(state, index);
        if (!writeJSONValue(state, lua_gettop(state), writer, depth + 1)) return false;
        lua_pop(state, 1);
    }
    keys.clear();
    lua_pop(state, 1);
    writer.ancestors[depth] = nullptr;
    return writer.append("}");
}

int validateSaveStateWire(lua_State* state) {
    if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "validateSaveStateWire: expected one byte string");
    size_t size;
    const char* bytes = lua_tolstring(state, 1, &size);
    const auto result = PCSX::Protobuf::StrictWireValidator<PCSX::SaveStates::SaveState>::validate(bytes, size);
    if (!result) {
        if (result.fieldName)
            return luaL_error(state, "%s: %s.%s", result.message(), result.messageType, result.fieldName);
        if (result.messageType)
            return luaL_error(state, "%s: %s#%d", result.message(), result.messageType, result.fieldNumber);
        return luaL_error(state, "%s", result.message());
    }
    lua_pushboolean(state, true);
    return 1;
}

int encodeJSON(lua_State* state) {
    if (lua_gettop(state) != 1) return luaL_error(state, "encodeJSON: expected 1 argument");
    luaL_checkstack(state, 4, "encodeJSON: insufficient Lua stack");
    pushJSONMetatable(state, &jsonWriterMetatableMarker, destroyJSONWriter);
    auto* writer = new (lua_newuserdata(state, sizeof(JSONWriter))) JSONWriter;
    lua_pushvalue(state, -2);
    lua_setmetatable(state, -2);
    lua_remove(state, -2);
    bool success = false;
    try {
        success = writeJSONValue(state, 1, *writer, 0);
    } catch (const std::bad_alloc&) {
        writer->error = "native allocation failed";
    }
    lua_settop(state, 2);
    if (!success) return luaL_error(state, "encodeJSON: %s", writer->error);
    lua_pushlstring(state, writer->output.data(), writer->output.size());
    return 1;
}

int jsonArray(lua_State* state) {
    if (lua_gettop(state) > 1) return luaL_error(state, "jsonArray: expected at most 1 argument");
    luaL_checkstack(state, 8, "jsonArray: insufficient Lua stack");
    if (lua_gettop(state) == 0 || lua_isnil(state, 1)) {
        lua_settop(state, 0);
        lua_newtable(state);
    } else if (lua_type(state, 1) != LUA_TTABLE) {
        return luaL_error(state, "jsonArray: expected a table or nil");
    }
    if (const char* error = validateJSONArray(state, 1, lua_objlen(state, 1)))
        return luaL_error(state, "jsonArray: %s", error);
    if (isJSONArray(state, 1)) return 1;
    if (lua_getmetatable(state, 1)) {
        lua_pushliteral(state, "__metatable");
        lua_rawget(state, -2);
        if (!lua_isnil(state, -1)) return luaL_error(state, "jsonArray: protected metatable");
        lua_pop(state, 2);
    }
    pushJSONMetatable(state, &jsonArrayMetatableMarker);
    lua_setmetatable(state, 1);
    return 1;
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
bool historyCDWorkerRecords(uint32_t first, uint32_t count, PCSX::History::CD::WorkerRecord* out) noexcept {
    try { return PCSX::History::CD::workerRecords(first, count, out); } catch (...) { return false; }
}
bool historyStop() { return PCSX::History::stop(); }
bool historyClear() { return PCSX::History::clear(); }
bool historyStatus(PCSX::History::Status* status) { return PCSX::History::status(status); }
bool historyRecords(uint32_t first, uint32_t count, PCSX::History::Record* out) {
    return PCSX::History::records(first, count, out);
}
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

constexpr char pauseCallbacks[] = "PCSX.PauseCallbacks";
constexpr size_t maxPauseCallbacks = 64;

int queuePausedCallback(lua_State* state) {
    if (lua_gettop(state) != 1 || !lua_isfunction(state, 1))
        return luaL_error(state, "PCSX.nextPause requires one function");
    lua_getfield(state, LUA_REGISTRYINDEX, pauseCallbacks);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_createtable(state, maxPauseCallbacks, 0);
        lua_pushvalue(state, -1);
        lua_setfield(state, LUA_REGISTRYINDEX, pauseCallbacks);
    }
    const auto count = lua_objlen(state, -1);
    if (count >= maxPauseCallbacks) return luaL_error(state, "PCSX.nextPause queue exceeds 64 callbacks");
    lua_pushvalue(state, 1);
    lua_rawseti(state, -2, count + 1);
    return 0;
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
    REGISTER(L, historyCDWorkerRecords);
    REGISTER(L, historyStop);
    REGISTER(L, historyClear);
    REGISTER(L, historyStatus);
    REGISTER(L, historyRecords);
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

void PCSX::LuaFFI::runPausedCallbacks(Lua L) {
    pollCD(L);
    auto* state = L.getState();
    const int base = lua_gettop(state);
    lua_getfield(state, LUA_REGISTRYINDEX, pauseCallbacks);
    if (!lua_istable(state, -1)) { lua_settop(state, base); return; }
    const int queue = base + 1;
    const size_t limit = lua_objlen(state, queue);
    for (size_t i = 0; i < std::min(limit, maxPauseCallbacks); ++i) {
        if (g_system->running() || g_system->inExecute() || g_system->quitting() ||
            g_system->hasPendingSaveStateLoad() || g_system->m_eventBus->dispatching()) break;
        const size_t count = lua_objlen(state, queue);
        if (!count) break;
        lua_rawgeti(state, queue, 1);
        for (size_t j = 1; j < count; ++j) {
            lua_rawgeti(state, queue, j + 1);
            lua_rawseti(state, queue, j);
        }
        lua_pushnil(state);
        lua_rawseti(state, queue, count);
        try { L.pcall(); } catch (...) { return; }
        lua_settop(state, queue);
    }
    lua_settop(state, base);
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
    L.push(lua_CFunction(queuePausedCallback));
    L.pcall(2);
    openHistoryCapture(L);
    openCD(L);
    openMemoryCards(L);
    L.pcall();
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.declareFunc("scanByteRanges", scanByteRanges, -1);
    L.declareFunc("convertVRAM", convertVRAM, -1);
    L.declareFunc("convertScreenshot", convertScreenshot, -1);
    L.declareFunc("packMMIOProbe", packMMIOProbe, -1);
    L.declareFunc("packVRAMUploadProbe", packVRAMUploadProbe, -1);
    L.declareFunc("encodeJSON", encodeJSON, -1);
    L.declareFunc("jsonArray", jsonArray, -1);
    L.declareFunc("validateSaveStateWire", validateSaveStateWire, -1);
    L.declareFunc("selectSaveStateWire", selectSaveStateWire, -1);
    L.declareFunc("decodeGPU", PCSX::LuaFFI::decodeGPU, -1);
    L.declareFunc("readGPUChain", PCSX::LuaFFI::readGPUChain, -1);
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
