// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/gpupackets.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/gpu.h"
#include "lua.hpp"

namespace {

constexpr uint32_t ramBytes = 2097152;

uint32_t integer(lua_State* state, int index, uint32_t low, uint32_t high, const char* name) {
    if (lua_type(state, index) != LUA_TNUMBER) luaL_error(state, "invalid %s", name);
    const double value = lua_tonumber(state, index);
    if (!std::isfinite(value) || std::floor(value) != value || value < low || value > high)
        luaL_error(state, "invalid %s", name);
    return static_cast<uint32_t>(value);
}

void number(lua_State* state, int table, const char* key, double value) {
    lua_pushnumber(state, value);
    lua_setfield(state, table, key);
}

void boolean(lua_State* state, int table, const char* key, bool value) {
    lua_pushboolean(state, value);
    lua_setfield(state, table, key);
}

void text(lua_State* state, int table, const char* key, const char* value) {
    lua_pushstring(state, value);
    lua_setfield(state, table, key);
}

uint32_t field(uint32_t value, unsigned shift, unsigned bits) {
    return (value >> shift) & ((1u << bits) - 1);
}

int coordinate(uint32_t value) { return PCSX::GPU::signExtend<int, 11>(value & 0x7ff); }

void pushXY(lua_State* state, uint32_t value, bool signedCoordinates) {
    lua_createtable(state, 0, 2);
    const int table = lua_gettop(state);
    number(state, table, "x", signedCoordinates ? coordinate(value) : int(value & 0xffff));
    number(state, table, "y", signedCoordinates ? coordinate(value >> 16) : int(value >> 16));
}

void pushSize(lua_State* state, uint32_t width, uint32_t height) {
    lua_createtable(state, 0, 2);
    const int table = lua_gettop(state);
    number(state, table, "width", width);
    number(state, table, "height", height);
}

void pushSize(lua_State* state, uint32_t value) { pushSize(state, value & 0xffff, value >> 16); }

void pushTexture(lua_State* state, uint32_t value) {
    static constexpr unsigned depths[] = {4, 8, 16, 16};
    lua_createtable(state, 0, 11);
    const int table = lua_gettop(state);
    number(state, table, "raw", value);
    number(state, table, "x_words", field(value, 0, 4) * 64);
    number(state, table, "y", field(value, 4, 1) * 256);
    number(state, table, "blend", field(value, 5, 2));
    const uint32_t depth = field(value, 7, 2);
    number(state, table, "depth_bits", depth);
    number(state, table, "native_bpp", depths[depth]);
    number(state, table, "dither", field(value, 9, 1));
    number(state, table, "draw_to_display", field(value, 10, 1));
    number(state, table, "disable", field(value, 11, 1));
    number(state, table, "flip_x", field(value, 12, 1));
    number(state, table, "flip_y", field(value, 13, 1));
}

void pushUV(lua_State* state, uint32_t value) {
    lua_createtable(state, 0, 3);
    const int table = lua_gettop(state);
    number(state, table, "u", value & 0xff);
    number(state, table, "v", field(value, 8, 8));
    number(state, table, "upper", value >> 16);
}

void pushClut(lua_State* state, uint32_t value) {
    lua_createtable(state, 0, 3);
    const int table = lua_gettop(state);
    number(state, table, "raw", value);
    number(state, table, "x_words", (value & 63) * 16);
    number(state, table, "y", field(value, 6, 9));
}

uint32_t word(const uint8_t* bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 |
           uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}

struct Stream {
    lua_State* state;
    const uint8_t* bytes;
    size_t size;
    uint32_t limit;
    uint32_t offset = 0;
    uint32_t words = 0;
    int raw = 0;

    uint32_t peek() const {
        if (size - offset < 4) luaL_error(state, "truncated GPU word");
        return word(bytes, offset);
    }

    uint32_t take() {
        if (words >= limit) luaL_error(state, "GPU packet word limit exceeded");
        const uint32_t value = peek();
        offset += 4;
        lua_pushnumber(state, value);
        lua_rawseti(state, raw, ++words);
        return value;
    }
};

uint32_t pushVertex(Stream& stream, uint32_t color, bool textured) {
    pushXY(stream.state, stream.take(), true);
    const int vertex = lua_gettop(stream.state);
    number(stream.state, vertex, "color", color);
    if (!textured) return 0;
    const uint32_t texture = stream.take();
    pushUV(stream.state, texture);
    lua_setfield(stream.state, vertex, "texture");
    return texture >> 16;
}

bool terminator(uint32_t value) { return (value & 0xf000f000) == 0x50005000; }

void unsupported(lua_State* state, const char* port, uint32_t opcode, uint32_t offset, bool withOffset) {
    char message[96];
    if (withOffset)
        std::snprintf(message, sizeof(message), "unsupported native %s opcode 0x%02x at byte %u", port, opcode, offset);
    else
        std::snprintf(message, sizeof(message), "unsupported native %s opcode 0x%02x", port, opcode);
    luaL_error(state, "%s", message);
}

void polygon(Stream& stream, int packet, uint32_t first, uint32_t opcode) {
    auto* state = stream.state;
    const bool shaded = opcode & 16, textured = opcode & 4;
    text(state, packet, "kind", "polygon");
    boolean(state, packet, "shaded", shaded);
    boolean(state, packet, "textured", textured);
    boolean(state, packet, "semitransparent", opcode & 2);
    boolean(state, packet, "raw_texture", opcode & 1);
    const unsigned count = opcode & 8 ? 4 : 3;
    lua_createtable(state, count, 0);
    const int vertices = lua_gettop(state);
    uint32_t color = first & 0xffffff, clut = 0, page = 0;
    for (unsigned index = 1; index <= count; ++index) {
        if (index > 1 && shaded) color = stream.take() & 0xffffff;
        const uint32_t upper = pushVertex(stream, color, textured);
        if (index == 1) clut = upper;
        if (index == 2) page = upper;
        lua_rawseti(state, vertices, index);
    }
    lua_setfield(state, packet, "vertices");
    if (!textured) return;
    pushClut(state, clut);
    lua_setfield(state, packet, "clut");
    pushTexture(state, page & 0x9ff);
    lua_setfield(state, packet, "page");
}

void line(Stream& stream, int packet, uint32_t first, uint32_t opcode) {
    auto* state = stream.state;
    const bool shaded = opcode & 16, polyline = opcode & 8;
    text(state, packet, "kind", "line");
    boolean(state, packet, "shaded", shaded);
    boolean(state, packet, "semitransparent", opcode & 2);
    boolean(state, packet, "polyline", polyline);
    if (polyline && terminator(first))
        luaL_error(state, "polyline command matches the native terminator pattern");
    if (polyline && terminator(stream.peek()))
        luaL_error(state, "polyline first coordinate matches the native terminator pattern");
    lua_createtable(state, polyline ? 0 : 2, 0);
    const int vertices = lua_gettop(state);
    uint32_t color = first & 0xffffff, count = 1;
    pushVertex(stream, color, false);
    lua_rawseti(state, vertices, count);
    if (!polyline) {
        if (shaded) color = stream.take() & 0xffffff;
        pushVertex(stream, color, false);
        lua_rawseti(state, vertices, 2);
        lua_setfield(state, packet, "vertices");
        return;
    }
    while (!terminator(stream.peek())) {
        if (shaded) {
            color = stream.take() & 0xffffff;
            if (terminator(stream.peek()))
                luaL_error(state, "polyline terminator in coordinate slot is unsupported");
        }
        pushVertex(stream, color, false);
        lua_rawseti(state, vertices, ++count);
    }
    if (count < 2) luaL_error(state, "polyline has fewer than two vertices");
    number(state, packet, "terminator", stream.take());
    lua_setfield(state, packet, "vertices");
}

void rectangle(Stream& stream, int packet, uint32_t first, uint32_t opcode) {
    auto* state = stream.state;
    const bool textured = opcode & 4;
    text(state, packet, "kind", "rectangle");
    boolean(state, packet, "textured", textured);
    boolean(state, packet, "semitransparent", opcode & 2);
    boolean(state, packet, "raw_texture", opcode & 1);
    const uint32_t upper = pushVertex(stream, first & 0xffffff, textured);
    lua_setfield(state, packet, "vertex");
    if (textured) {
        pushClut(state, upper);
        lua_setfield(state, packet, "clut");
    }
    const uint32_t kind = field(opcode, 3, 2);
    if (!kind) pushSize(state, stream.take());
    else {
        static constexpr unsigned sizes[] = {0, 1, 8, 16};
        pushSize(state, sizes[kind], sizes[kind]);
    }
    lua_setfield(state, packet, "size");
}

void transfer(Stream& stream, int packet, unsigned group) {
    auto* state = stream.state;
    text(state, packet, "kind", group == 4 ? "copy" : group == 5 ? "upload" : "readback");
    pushXY(state, stream.take(), group == 4);
    lua_setfield(state, packet, group == 4 ? "source" : "position");
    if (group == 4) {
        pushXY(state, stream.take(), true);
        lua_setfield(state, packet, "destination");
    }
    const uint32_t dimensions = stream.take();
    pushSize(state, dimensions);
    lua_setfield(state, packet, "size");
    if (group != 5) return;
    const uint64_t pixels = uint64_t(dimensions & 0xffff) * (dimensions >> 16);
    const uint64_t count = (pixels + 1) / 2;
    if (!count) luaL_error(state, "zero-area native upload has a deferred boundary and is unsupported");
    if (count + 3 > stream.limit || count * 4 > stream.size - stream.offset)
        luaL_error(state, "truncated or oversized GPU upload");
    number(state, packet, "pixel_offset", stream.offset);
    number(state, packet, "pixel_words", count);
    number(state, packet, "pixels", pixels);
    for (uint32_t index = 0; index < count; ++index) stream.take();
}

void environment(Stream& stream, int packet, uint32_t first, uint32_t opcode) {
    auto* state = stream.state;
    static constexpr const char* names[] = {"page", "window", "area-start", "area-end", "offset", "mask"};
    if (opcode < 0xe1 || opcode > 0xe6) {
        unsupported(state, "GP0", opcode, 0, false);
        return;
    }
    text(state, packet, "kind", names[opcode - 0xe1]);
    if (opcode == 0xe1) {
        pushTexture(state, first & 0xffffff);
        lua_setfield(state, packet, "page");
        return;
    }
    if (opcode == 0xe6) {
        number(state, packet, "set_mask", first & 1);
        number(state, packet, "check_mask", field(first, 1, 1));
        return;
    }
    lua_createtable(state, 0, opcode == 0xe2 ? 4 : 2);
    const int operand = lua_gettop(state);
    if (opcode == 0xe2) {
        number(state, operand, "mask_x", field(first, 0, 5));
        number(state, operand, "mask_y", field(first, 5, 5));
        number(state, operand, "offset_x", field(first, 10, 5));
        number(state, operand, "offset_y", field(first, 15, 5));
        lua_setfield(state, packet, "window");
        return;
    }
    number(state, operand, "x", opcode == 0xe5 ? coordinate(first) : int(field(first, 0, 10)));
    number(state, operand, "y", opcode == 0xe5 ? coordinate(first >> 11) : int(field(first, 10, 9)));
    lua_setfield(state, packet, "position");
}

void gp0(Stream& stream, int packet, uint32_t first, uint32_t opcode, uint32_t start) {
    const unsigned group = opcode / 32;
    if (group == 0) {
        if (first == 0) text(stream.state, packet, "kind", "nop");
        else if (opcode == 1) text(stream.state, packet, "kind", "clear-cache");
        else if (opcode == 2) {
            text(stream.state, packet, "kind", "fill");
            number(stream.state, packet, "color", first & 0xffffff);
            pushXY(stream.state, stream.take(), false);
            lua_setfield(stream.state, packet, "position");
            pushSize(stream.state, stream.take());
            lua_setfield(stream.state, packet, "size");
        } else unsupported(stream.state, "GP0", opcode, start, true);
        return;
    }
    if (group == 1) polygon(stream, packet, first, opcode);
    else if (group == 2) line(stream, packet, first, opcode);
    else if (group == 3) rectangle(stream, packet, first, opcode);
    else if (group <= 6) transfer(stream, packet, group);
    else environment(stream, packet, first, opcode);
}

void gp1(lua_State* state, int packet, uint32_t value, uint32_t opcode) {
    static constexpr const char* names[] = {"reset", "clear-fifo", "irq-ack", "display-enable", "dma",
                                            "display-start", "horizontal-range", "vertical-range", "display-mode"};
    if (opcode > 8 && opcode != 16) {
        unsupported(state, "GP1", opcode, 0, false);
        return;
    }
    text(state, packet, "kind", opcode == 16 ? "query" : names[opcode]);
    if (opcode == 3) boolean(state, packet, "enabled", !(value & 1));
    if (opcode == 4) number(state, packet, "direction", value & 3);
    if (opcode == 16) number(state, packet, "query", value & 7);
    if (opcode < 5 || opcode == 16) return;
    lua_createtable(state, 0, opcode == 8 ? 8 : 2);
    const int operand = lua_gettop(state);
    if (opcode == 5) {
        number(state, operand, "x", field(value, 0, 10));
        number(state, operand, "y", field(value, 10, 9));
        lua_setfield(state, packet, "position");
        return;
    }
    if (opcode == 6 || opcode == 7) {
        const unsigned bits = opcode == 6 ? 12 : 10;
        number(state, operand, "first", field(value, 0, bits));
        number(state, operand, "last", field(value, bits, bits));
        lua_setfield(state, packet, "range");
        return;
    }
    number(state, operand, "horizontal_bits", field(value, 0, 2));
    static constexpr const char* bits[] = {"vertical_bit", "pal", "depth24", "interlace", "alternate_horizontal", "reverse"};
    for (unsigned index = 0; index < 6; ++index) number(state, operand, bits[index], field(value, index + 2, 1));
    lua_setfield(state, packet, "mode");
}

}

int PCSX::LuaFFI::decodeGPU(lua_State* state) {
    if (lua_gettop(state) != 4) return luaL_error(state, "decodeGPU: expected 4 arguments");
    if (lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "invalid GPU port");
    size_t portSize = 0;
    const char* port = lua_tolstring(state, 2, &portSize);
    const bool control = portSize == 3 && std::memcmp(port, "gp1", 3) == 0;
    if (!control && !(portSize == 3 && std::memcmp(port, "gp0", 3) == 0))
        return luaL_error(state, "invalid GPU port");
    const uint32_t maxWords = integer(state, 3, 1, 1048576, "word limit");
    const uint32_t maxPackets = integer(state, 4, 1, 65536, "packet limit");
    const char* sizeError = control ? "invalid GPU control stream size" : "invalid GPU stream size";
    if (lua_type(state, 1) != LUA_TSTRING) return luaL_error(state, "%s", sizeError);
    size_t size = 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(lua_tolstring(state, 1, &size));
    if (size % 4 || size / 4 > maxWords || (control && size / 4 > maxPackets))
        return luaL_error(state, "%s", sizeError);
    luaL_checkstack(state, 16, "decodeGPU: insufficient stack");
    lua_createtable(state, control ? size / 4 : 0, 0);
    const int result = lua_gettop(state);
    Stream stream{state, bytes, size, maxWords};
    unsigned count = 0;
    while (stream.offset < size) {
        if (count >= maxPackets) return luaL_error(state, "GPU packet limit exceeded");
        const uint32_t start = stream.offset;
        lua_createtable(state, 0, 10);
        const int packet = lua_gettop(state);
        lua_createtable(state, control ? 1 : 0, 0);
        stream.raw = lua_gettop(state);
        stream.words = 0;
        lua_pushvalue(state, stream.raw);
        lua_setfield(state, packet, "raw");
        const uint32_t first = stream.take(), opcode = first >> 24;
        number(state, packet, "offset", start);
        number(state, packet, "opcode", opcode);
        if (control) gp1(state, packet, first, opcode);
        else gp0(stream, packet, first, opcode, start);
        number(state, packet, "words", stream.words);
        lua_pop(state, 1);
        lua_rawseti(state, result, ++count);
    }
    return 1;
}

int PCSX::LuaFFI::readGPUChain(lua_State* state) {
    if (lua_gettop(state) != 4) return luaL_error(state, "readGPUChain: expected 4 arguments");
    const uint32_t maxWords = integer(state, 3, 1, 1048576, "word limit");
    const uint32_t maxNodes = integer(state, 4, 1, 65536, "packet limit");
    size_t size = 0;
    if (lua_type(state, 1) != LUA_TSTRING || lua_objlen(state, 1) != ramBytes)
        return luaL_error(state, "ordering table requires exact physical 2 MiB RAM");
    const auto* ram = reinterpret_cast<const uint8_t*>(lua_tolstring(state, 1, &size));
    const uint32_t first = integer(state, 2, 0, UINT32_MAX, "ordering-table address");
    const uint32_t segment = first / 0x20000000;
    if ((segment != 0 && segment != 4 && segment != 5) || first % 0x20000000 >= 0x800000)
        return luaL_error(state, "ordering-table address is not a supported RAM alias");
    luaL_checkstack(state, 16, "readGPUChain: insufficient stack");
    auto* seen = static_cast<uint8_t*>(lua_newuserdata(state, ramBytes / 32));
    std::memset(seen, 0, ramBytes / 32);
    uint32_t address = first, nodes = 0, total = 0, terminal = 0;
    while (true) {
        if (address % 4) return luaL_error(state, "unaligned ordering-table pointer");
        const uint32_t physical = address % ramBytes, bit = physical / 4;
        if (seen[bit / 8] & (1u << (bit % 8))) return luaL_error(state, "cyclic ordering table");
        if (nodes >= maxNodes) return luaL_error(state, "ordering-table node limit exceeded");
        seen[bit / 8] |= 1u << (bit % 8);
        const uint32_t header = word(ram, physical), count = header >> 24, next = header & 0xffffff;
        if (total + count > maxWords) return luaL_error(state, "ordering-table word limit exceeded");
        if (physical + 4 + count * 4 > ramBytes)
            return luaL_error(state, "ordering-table payload crosses physical RAM boundary");
        ++nodes;
        total += count;
        if (next & 0x800000) {
            terminal = next;
            break;
        }
        address = next;
    }
    auto* payload = static_cast<uint8_t*>(lua_newuserdata(state, size_t(total) * 4));
    lua_createtable(state, 0, 4);
    const int result = lua_gettop(state);
    lua_createtable(state, nodes, 0);
    const int list = lua_gettop(state);
    address = first;
    uint32_t offset = 0;
    for (uint32_t index = 1; index <= nodes; ++index) {
        const uint32_t physical = address % ramBytes;
        const uint32_t header = word(ram, physical), count = header >> 24, next = header & 0xffffff;
        lua_createtable(state, 0, 6);
        const int node = lua_gettop(state);
        number(state, node, "address", address);
        number(state, node, "physical", physical);
        number(state, node, "header", header);
        number(state, node, "next", next);
        number(state, node, "words", count);
        number(state, node, "stream_offset", offset);
        lua_rawseti(state, list, index);
        std::memcpy(payload + offset, ram + physical + 4, count * 4);
        offset += count * 4;
        address = next;
    }
    lua_setfield(state, result, "nodes");
    number(state, result, "words", total);
    number(state, result, "terminal", terminal);
    lua_pushlstring(state, reinterpret_cast<const char*>(payload), offset);
    lua_setfield(state, result, "bytes");
    return 1;
}
