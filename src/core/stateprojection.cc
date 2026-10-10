// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/stateprojection.h"

#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "core/sstate.h"
#include "lua.hpp"
#include "support/protobuf-wire.h"

namespace {

using namespace PCSX::Protobuf::WireDetail;
using RootSchema = Schema<PCSX::SaveStates::SaveState>;

struct PathPart {
    const SchemaView* parent;
    const Descriptor* field;
    size_t index;
};

struct Occurrence {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    unsigned wire = 0;
};

struct Projection {
    std::vector<PathPart> path;
    std::string wire;
    std::string versionWire;
    bool parentPresent = true;
};

int destroyProjection(lua_State* state) {
    static_cast<Projection*>(lua_touserdata(state, 1))->~Projection();
    return 0;
}

bool firstCharacter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool nameCharacter(char value) { return firstCharacter(value) || (value >= '0' && value <= '9'); }

void parsePath(lua_State* state, std::string_view path, Projection& projection) {
    const SchemaView* parent = &RootSchema::view;
    size_t position = 0;
    while (position < path.size()) {
        const size_t start = position;
        if (!parent) luaL_error(state, "unknown schema field: %s", path.data() + start);
        if (!firstCharacter(path[position])) luaL_error(state, "invalid query path: %s", path.data());
        while (position < path.size() && nameCharacter(path[position])) ++position;
        const auto* descriptor = parent->find(path.substr(start, position - start));
        if (!descriptor) luaL_error(state, "unknown schema field: %s", path.data() + start);
        size_t index = 0;
        if (position < path.size() && path[position] == '[') {
            if (!descriptor->repeated) luaL_error(state, "index requires a repeated field");
            const size_t firstDigit = ++position;
            while (position < path.size() && path[position] >= '0' && path[position] <= '9') {
                index = index * 10 + size_t(path[position++] - '0');
                if (index > 1000000) luaL_error(state, "invalid state index");
            }
            if (position == firstDigit || position == path.size() || path[position++] != ']')
                luaL_error(state, "invalid query path: %s", path.data());
            if (!index) luaL_error(state, "invalid state index");
        }
        if (projection.path.size() >= 64) luaL_error(state, "state schema nesting exceeds 64");
        projection.path.push_back({parent, descriptor, index});
        if (position == path.size()) return;
        if (path[position++] != '.' || position == path.size() || (descriptor->repeated && !index))
            luaL_error(state, "invalid query path: %s", path.data());
        parent = descriptor->child();
    }
}

Occurrence next(Slice& input, uint32_t& number) {
    const size_t start = input.position;
    Context context;
    context.strict = false;
    uint64_t tag;
    context.varint(input, tag);
    number = uint32_t(tag) >> 3;
    const unsigned wire = uint32_t(tag) & 7;
    const uint8_t* payload = input.bytes + input.position;
    size_t payloadSize = 0;
    if (wire == 2) {
        uint64_t length;
        context.varint(input, length);
        payload = input.bytes + input.position;
        payloadSize = size_t(length);
        input.position += payloadSize;
    } else {
        const size_t payloadStart = input.position;
        context.skip(input, wire, number, 0);
        payloadSize = input.position - payloadStart;
    }
    return {input.bytes + start, input.position - start, payload, payloadSize, wire};
}

void appendVarint(std::string& output, uint64_t value) {
    while (value > 127) {
        output.push_back(char((value & 127) | 128));
        value >>= 7;
    }
    output.push_back(char(value));
}

void append(std::string& output, const Occurrence& occurrence) {
    output.append(reinterpret_cast<const char*>(occurrence.bytes), occurrence.size);
}

Occurrence select(Slice input, const Descriptor& descriptor, size_t index, std::string* output) {
    Occurrence selected;
    size_t count = 0;
    while (input.remaining()) {
        uint32_t number;
        const auto occurrence = next(input, number);
        if (number != descriptor.number) continue;
        if (!index) {
            selected = occurrence;
            if (output) {
                if (!descriptor.repeated) output->clear();
                append(*output, occurrence);
            }
            continue;
        }
        if (occurrence.wire == 2 && descriptor.wire != 2) {
            Slice packed{occurrence.payload, occurrence.payloadSize};
            Context context;
            context.strict = false;
            while (packed.remaining()) {
                const size_t start = packed.position;
                descriptor.read(context, packed, descriptor.wire, 0);
                if (++count != index) continue;
                selected = {packed.bytes + start, packed.position - start, packed.bytes + start,
                            packed.position - start, descriptor.wire};
                if (output) {
                    appendVarint(*output, uint64_t(descriptor.number) << 3 | descriptor.wire);
                    append(*output, selected);
                }
                return selected;
            }
            continue;
        }
        if (++count != index) continue;
        if (output) append(*output, occurrence);
        return occurrence;
    }
    return index ? Occurrence{} : selected;
}

void project(Slice input, Projection& projection) {
    const auto& infoField = *RootSchema::view.find("save_state_info");
    const auto info = select(input, infoField, 0, nullptr);
    if (info.bytes) {
        const auto& versionField = *infoField.child()->find("version");
        const auto version = select({info.payload, info.payloadSize}, versionField, 0, nullptr);
        appendVarint(projection.versionWire, uint64_t(infoField.number) << 3 | 2);
        appendVarint(projection.versionWire, version.size);
        if (version.bytes) append(projection.versionWire, version);
    }
    for (size_t index = 0; index < projection.path.size(); ++index) {
        const auto& part = projection.path[index];
        if (index + 1 == projection.path.size()) {
            select(input, *part.field, part.index, &projection.wire);
            return;
        }
        const auto occurrence = select(input, *part.field, part.index, nullptr);
        if (!occurrence.bytes) {
            projection.parentPresent = false;
            return;
        }
        input = {occurrence.payload, occurrence.payloadSize};
    }
}

void text(lua_State* state, const char* key, const char* value) {
    lua_pushstring(state, value);
    lua_setfield(state, -2, key);
}

void boolean(lua_State* state, const char* key, bool value) {
    lua_pushboolean(state, value);
    lua_setfield(state, -2, key);
}

}

int PCSX::LuaFFI::selectSaveStateWire(lua_State* state) {
    if ((lua_gettop(state) != 2 && lua_gettop(state) != 3) || lua_type(state, 1) != LUA_TSTRING ||
        lua_type(state, 2) != LUA_TSTRING ||
        (lua_gettop(state) == 3 && !lua_isnil(state, 3) && lua_type(state, 3) != LUA_TBOOLEAN))
        return luaL_error(state, "selectSaveStateWire: expected bytes, path, optional strict boolean");
    size_t size, pathSize;
    const char* bytes = lua_tolstring(state, 1, &size);
    const char* path = lua_tolstring(state, 2, &pathSize);
    if (size > PCSX::Protobuf::StrictWireValidator<PCSX::SaveStates::SaveState>::MaxSize)
        return luaL_error(state, "state exceeds 64 MiB");
    Context context;
    context.strict = lua_toboolean(state, 3);
    PCSX::Protobuf::WireDetail::Slice input{reinterpret_cast<const uint8_t*>(bytes), size};
    RootSchema::read(context, input, 0);
    const auto& result = context.result;
    if (!result) {
        if (result.fieldName)
            return luaL_error(state, "%s: %s.%s", result.message(), result.messageType, result.fieldName);
        if (result.messageType)
            return luaL_error(state, "%s: %s#%d", result.message(), result.messageType, result.fieldNumber);
        return luaL_error(state, "%s", result.message());
    }
    if (luaL_newmetatable(state, "PCSX.StateProjection")) {
        lua_pushcfunction(state, destroyProjection);
        lua_setfield(state, -2, "__gc");
    }
    auto* projection = new (lua_newuserdata(state, sizeof(Projection))) Projection;
    lua_pushvalue(state, -2);
    lua_setmetatable(state, -2);
    lua_remove(state, -2);
    try {
        parsePath(state, {path, pathSize}, *projection);
        project({reinterpret_cast<const uint8_t*>(bytes), size}, *projection);
    } catch (const std::bad_alloc&) {
        return luaL_error(state, "selectSaveStateWire: allocation failed");
    }
    lua_createtable(state, 0, 9);
    const bool root = projection->path.empty();
    const PathPart* selected = root ? nullptr : &projection->path.back();
    if (root) lua_pushvalue(state, 1);
    else lua_pushlstring(state, projection->wire.data(), projection->wire.size());
    lua_setfield(state, -2, "wire");
    lua_pushlstring(state, projection->versionWire.data(), projection->versionWire.size());
    lua_setfield(state, -2, "version_wire");
    text(state, "parent_type", root ? RootSchema::view.name : selected->parent->name);
    text(state, "kind", root ? RootSchema::view.name : selected->field->kind);
    if (!root) text(state, "field_name", selected->field->name);
    boolean(state, "repeated", !root && selected->field->repeated && !selected->index);
    boolean(state, "indexed", !root && selected->index);
    boolean(state, "parent_present", projection->parentPresent);
    return 1;
}
