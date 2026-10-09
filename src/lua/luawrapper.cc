/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include "lua/luawrapper.h"

#include <assert.h>
#include <cstdio>

static int callwrap(lua_State* raw, lua_CFunction func) {
    PCSX::Lua L(raw);

    try {
        return func(raw);
    } catch (std::exception& e) {
        return L.error("LuaException: %s", e.what());
    }
}

std::function<void(std::string_view)> PCSX::Lua::normalPrinter = nullptr;
std::function<void(std::string_view)> PCSX::Lua::errorPrinter = nullptr;

PCSX::Lua::Lua() : L(lua_open()) {
    static_assert(sizeof(Lua) == sizeof(lua_State*));
    assert(("Couldn't create Lua VM", L));
    lua_atpanic(L, [](lua_State* L) -> int { throw std::runtime_error(lua_tostring(L, 1)); });
    setCallWrap(callwrap);
    push("_THREADS");
    newtable();
    settable(LUA_REGISTRYINDEX);
}

PCSX::Lua& PCSX::Lua::operator=(Lua&& oL) noexcept {
    if (this == &oL) return *this;

    assert(("Can't assign a Lua VM to another one.", !L));

    L = oL.L;
    oL.L = nullptr;

    return *this;
}

void PCSX::Lua::close() {
    assert(("Can't close an already closed VM", L));

    lua_close(L);
    L = nullptr;
}

void PCSX::Lua::openlibs() { luaL_openlibs(L); }

void PCSX::Lua::open_base() {
    int n = gettop();
    luaopen_base(L);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_bit() {
    int n = gettop();
    luaopen_bit(L);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_debug() {
    int n = gettop();
    luaopen_debug(L);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_ffi() {
    int n = gettop();
    luaopen_ffi(L);
    push("ffi");
    copy(-2);
    settable(LUA_GLOBALSINDEX);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_math() {
    int n = gettop();
    luaopen_math(L);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_jit() {
    int n = gettop();
    luaopen_jit(L);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_table() {
    int n = gettop();
    luaopen_table(L);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_package() {
    int n = gettop();
    luaopen_package(L);
    while (n < gettop()) pop();
}

void PCSX::Lua::open_string() {
    int n = gettop();
    luaopen_string(L);
    while (n < gettop()) pop();
}

PCSX::Lua PCSX::Lua::thread(bool saveit) {
    checkstack();
    lua_State* L1 = lua_newthread(L);
    if (saveit) {                     // -1 = thread
        push("_THREADS");             // -2 = thread, -1 = "_THREADS"
        gettable(LUA_REGISTRYINDEX);  // -2 = thread, -1 = _THREADS
        push(L1);                     // -3 = thread, -2 = _THREADS, -1 = key-Lt
        copy(-3);                     // -4 = thread, -3 = _THREADS, -2 = key-Lt, -1 = thread
        settable();                   // -2 = thread, -1 = _THREADS
        pop();                        // -1 = thread
    }
    return L1;
}

void PCSX::Lua::weaken() {
    push("_THREADS");             // -1 = "_THREADS"
    gettable(LUA_REGISTRYINDEX);  // -1 = _THREADS
    push(L);                      // -2 = _THREADS, -1 = key-Lt
    push();                       // -3 = _THREADS, -2 = key-Lt, -1 = nil
    settable();                   // -1 = _THREADS
    pop();
}

void PCSX::Lua::setCallWrap(lua_CallWrapper wrapper) {
    push((void*)wrapper);
    luaJIT_setmode(L, -1, LUAJIT_MODE_WRAPCFUNC | LUAJIT_MODE_ON);
    pop();
}

void PCSX::Lua::declareFunc(std::string_view name, lua_CFunction f, int i, int upvalues) {
    i = getabsolute(i);
    checkstack(2);
    lua_pushlstring(L, name.data(), name.size());
    if (upvalues != 0) {
        lua_insert(L, -upvalues - 1);
    }
    lua_pushcclosure(L, f, upvalues);
    lua_settable(L, i);
}

typedef std::function<int(PCSX::Lua)> LuaLambda;

void PCSX::Lua::declareFunc(std::string_view name, LuaLambda&& f, int i) {
    i = getabsolute(i);
    checkstack(5);
    lua_pushlstring(L, name.data(), name.size());
    push(std::move(f));
    lua_settable(L, i);
}

void PCSX::Lua::push(std::function<int(Lua)>&& f) {
    new (lua_newuserdata(L, sizeof(LuaLambda))) LuaLambda(std::move(f));
    newtable();
    push("__gc");
    push([](lua_State* L_) -> int {
        PCSX::Lua L(L_);
        auto* lambda = reinterpret_cast<LuaLambda*>(L.touserdata());
        if (lambda) lambda->~LuaLambda();
        return 0;
    });
    settable();
    setmetatable();
    lua_pushcclosure(
        L,
        [](lua_State* L_) -> int {
            PCSX::Lua L(L_);
            auto* lambda = reinterpret_cast<LuaLambda*>(L.touserdata(lua_upvalueindex(1)));
            return lambda->operator()(L);
        },
        1);
}

void PCSX::Lua::call(std::string_view f, int i, int nargs) {
    checkstack(1);
    lua_pushlstring(L, f.data(), f.size());
    lua_gettable(L, i);
    lua_insert(L, -1 - nargs);
    call(nargs);
}

void PCSX::Lua::call(int nargs) {
    int r = lua_resume(L, nargs);

    if ((r == LUA_YIELD) || (r == 0)) return;

    pushLuaContext();
    displayStack(true);
    while (gettop()) pop();

    switch (r) {
        case LUA_ERRRUN:
            throw std::runtime_error("Runtime error while running LUA code.");
        case LUA_ERRMEM:
            throw std::runtime_error("Memory allocation error while running LUA code.");
        case LUA_ERRERR:
            throw std::runtime_error("Error in Error function.");
        case LUA_ERRSYNTAX:
            throw std::runtime_error("Syntax error in Lua code.");
        default:
            throw std::runtime_error(std::string("Unknow error while running LUA code (err code: ") +
                                     std::to_string(r) + ")");
    }
}

void PCSX::Lua::unpackErrorRaw(lua_State* L) {
    if (!lua_istable(L, -1)) return;
    const int table = lua_gettop(L);
    int next = 1;
    for (;;) {
        lua_pushnumber(L, next++);
        lua_gettable(L, table);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_remove(L, table);
            return;
        }
    }
}

int PCSX::Lua::pcallRaw(lua_State* L, int nargs, int results, lua_CFunction hidden) {
    struct Context { lua_CFunction hidden; } context{hidden};
    lua_pushlightuserdata(L, &context);
    lua_pushcclosure(L, [](lua_State* state) -> int {
        const auto* context = static_cast<const Context*>(lua_touserdata(state, lua_upvalueindex(1)));
        PCSX::Lua::pushLuaContextRaw(state, true, context->hidden);
        return 1;
    }, 1);
    const int error = lua_gettop(L) - nargs - 1;
    lua_insert(L, error);
    const int status = lua_pcall(L, nargs, results, error);
    lua_remove(L, error);
    return status;
}

int PCSX::Lua::pcall(int nargs) {
    const int prefix = gettop() - nargs - 1;
    const int status = pcallRaw(L, nargs);
    if (!status) return gettop() - prefix;
    PCSX::Lua::unpackErrorRaw(L);
    displayStack(true);
    while (gettop()) pop();
    switch (status) {
        case LUA_ERRRUN: throw std::runtime_error("Runtime error while running LUA code.");
        case LUA_ERRMEM: throw std::runtime_error("Memory allocation error while running LUA code.");
        case LUA_ERRERR: throw std::runtime_error("Error in Error function.");
        case LUA_ERRSYNTAX: throw std::runtime_error("Syntax error in Lua code.");
        default:
            throw std::runtime_error(std::string("Unknow error while running LUA code (err code: ") +
                                     std::to_string(status) + ")");
    }
}

void PCSX::Lua::settable(int i, bool raw) {
    if (raw) {
        lua_rawset(L, i);
    } else {
        lua_settable(L, i);
    }
}

void PCSX::Lua::gettable(int i, bool raw) {
    if (raw) {
        lua_rawget(L, i);
    } else {
        lua_gettable(L, i);
    }
}

void PCSX::Lua::getfieldtable(std::string_view name, int tableIdx, bool raw) {
    tableIdx = getabsolute(tableIdx);
    push(name);
    gettable(tableIdx, raw);
    if (!isnil()) return;

    pop();
    newtable();
    push(name);
    copy(-2);
    settable(tableIdx, raw);
}

void PCSX::Lua::getfieldtable(int idx, int tableIdx, bool raw) {
    tableIdx = getabsolute(tableIdx);
    push(lua_Number(idx));
    gettable(tableIdx, raw);
    if (!isnil()) return;

    pop();
    newtable();
    push(lua_Number(idx));
    copy(-2);
    settable(tableIdx, raw);
}

void PCSX::Lua::pushLuaContextRaw(lua_State* L, bool inTable, lua_CFunction hidden) {
    lua_Debug ar;
    const int nargs = lua_gettop(L);
    int next = 1;
    if (inTable) {
        lua_newtable(L);
        for (int i = 1; i <= nargs; ++i) {
            lua_pushnumber(L, next++);
            lua_pushvalue(L, i);
            lua_settable(L, -3);
        }
    }
    for (int level = 0; lua_getstack(L, level, &ar) == 1; ++level) {
        if (!lua_getinfo(L, "nSlf", &ar)) break;
        const bool skip = hidden && lua_tocfunction(L, -1) == hidden;
        lua_pop(L, 1);
        if (skip) continue;
        if (inTable) lua_pushnumber(L, next++);
        lua_pushfstring(L, "at %s:%d (%s)", ar.source, ar.currentline, ar.name ? ar.name : "[top]");
        if (inTable) lua_settable(L, -3);
    }
}

void PCSX::Lua::pushLuaContext(bool inTable) { pushLuaContextRaw(L, inTable); }

int PCSX::Lua::error(std::string_view msg) {
    push(msg);

    if (yielded()) {
        pushLuaContext();
        displayStack(true);
        while (gettop()) pop();

        throw std::runtime_error("Runtime error while running yielded C code.");
        return 0;
    } else {
        return lua_error(L);
    }
}

bool PCSX::Lua::isobject(int i) {
    bool r = false;
    if (istable(i)) {
        push("__obj");
        gettable(i);
        r = isuserdata();
        pop();
    } else {
        r = isnil(i);
    }
    return r;
}

std::string PCSX::Lua::tostring(int i) {
    switch (type(i)) {
        case LUA_TNIL:
            return "(nil)";
        case LUA_TBOOLEAN:
            return toboolean(i) ? "true" : "false";
        case LUA_TNUMBER:
            return std::to_string(tonumber(i));
        default: {
            size_t l;
            const char* const r = lua_tolstring(L, i, &l);
            return std::string(r, l);
        }
    }
    return "<lua-NULL>";
}

std::string PCSX::Lua::escapeString(std::string_view s) {
    std::string r = "";

    for (int i = 0; i < s.size(); i++) {
        switch (s[i]) {
            case '"':
            case '\\':
                r += '\\';
                r += s[i];
                break;
            case '\n':
                r += "\\n";
                break;
            case '\r':
                r += "\\r";
                break;
            case '\0':
                r += "\\000";
                break;
            default:
                r += s[i];
        }
    }
    return r;
}

void PCSX::Lua::load(std::string_view code, const char* name, bool docall) {
    int status = luaL_loadbuffer(L, code.data(), code.size(), name);

    if (status) {
        pushLuaContext();
        displayStack(true);
        while (gettop()) pop();
        throw std::runtime_error("Error loading lua string");
    }

    if (docall) pcall();
}

namespace {
bool invokeNativePrinter(const std::function<void(std::string_view)>* printer, const char* text,
                         size_t length, std::exception_ptr* exception) {
    if (!exception) {
        (*printer)(std::string_view(text, length));
        return true;
    }
    try {
        (*printer)(std::string_view(text, length));
        return true;
    } catch (...) {
        *exception = std::current_exception();
    }
    return false;
}

int displayLuaStack(lua_State* L, bool error, std::exception_ptr* exception,
                    lua_CFunction hidden, bool recursive) {
    const int count = lua_gettop(L);
    if (!lua_checkstack(L, 12)) return luaL_error(L, "not enough Lua stack for error display");
    lua_pushliteral(L, "IN_DISPLAY_STACK");
    const int key = lua_gettop(L);
    lua_pushvalue(L, key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int oldGuard = lua_gettop(L);
    const bool already = recursive || lua_toboolean(L, oldGuard);
    const auto* native = error ? &PCSX::Lua::errorPrinter : &PCSX::Lua::normalPrinter;
    const char* empty = error ?
        "Got an error but stack is empty: FFI code probably threw an exception. Please report this bug with a reproduction case." :
        "Stack empty";
    if (!count && *native && !invokeNativePrinter(native, empty, strlen(empty), exception)) return 0;
    if (!count && already) {
        printf("%s", empty);
        lua_settop(L, count);
        return 0;
    }
    const bool useLua = !count || ((!PCSX::Lua::normalPrinter && error) ||
                                  (!PCSX::Lua::errorPrinter && !error));
    bool hasPrinter = false;
    int printer = 0;
    if (useLua) {
        lua_getfield(L, LUA_GLOBALSINDEX, error ? "printError" : "print");
        printer = lua_gettop(L);
        hasPrinter = !lua_isnil(L, printer);
    }
    for (int i = count ? 1 : 0; i <= count; ++i) {
        if (useLua) lua_pushvalue(L, printer);
        if (!count) {
            lua_pushstring(L, empty);
        } else {
            int fragments = 3;
            lua_pushnumber(L, i);
            lua_pushliteral(L, ": ");
            switch (lua_type(L, i)) {
                case LUA_TNONE: lua_pushliteral(L, "Invalid"); break;
                case LUA_TNIL: lua_pushliteral(L, "(Nil)"); break;
                case LUA_TNUMBER:
                    lua_pushliteral(L, "(Number) "); lua_pushvalue(L, i); ++fragments; break;
                case LUA_TBOOLEAN:
                    lua_pushstring(L, lua_toboolean(L, i) ? "(Bool)   true" : "(Bool)   false"); break;
                case LUA_TSTRING:
                    lua_pushliteral(L, "(String) "); lua_pushvalue(L, i); ++fragments; break;
                case LUA_TTABLE: lua_pushliteral(L, "(Table)"); break;
                case LUA_TFUNCTION: lua_pushliteral(L, "(Function)"); break;
                case LUA_TUSERDATA: case LUA_TLIGHTUSERDATA: {
                    char pointer[2 * sizeof(void*) + 4];
                    std::snprintf(pointer, sizeof(pointer), "%p", lua_touserdata(L, i));
                    lua_pushstring(L, lua_type(L, i) == LUA_TUSERDATA ? "(Userdata) " : "(Lightuserdata) ");
                    lua_pushstring(L, pointer);
                    ++fragments;
                    break;
                }
                case LUA_TTHREAD: lua_pushliteral(L, "(Thread)"); break;
                case 9: lua_pushliteral(L, "(Proto)"); break;
                case 10: {
                    char pointer[2 * sizeof(void*) + 4];
                    std::snprintf(pointer, sizeof(pointer), "%p", lua_topointer(L, i));
                    lua_pushliteral(L, "(CData)");
                    lua_pushstring(L, pointer);
                    ++fragments;
                    break;
                }
                default: lua_pushliteral(L, "Unknown"); break;
            }
            lua_concat(L, fragments);
        }
        if (useLua && !already && (hasPrinter || !count)) {
            struct Context { lua_CFunction hidden; } context{hidden};
            lua_pushlightuserdata(L, &context);
            lua_pushcclosure(L, [](lua_State* state) -> int {
                const auto* context = static_cast<const Context*>(lua_touserdata(state, lua_upvalueindex(1)));
                PCSX::Lua::pushLuaContextRaw(state, true, context->hidden);
                return 1;
            }, 1);
            const int handler = lua_gettop(L) - 2;
            lua_insert(L, handler);
            lua_pushvalue(L, key);
            lua_pushboolean(L, true);
            lua_rawset(L, LUA_REGISTRYINDEX);
            const int status = lua_pcall(L, 1, LUA_MULTRET, handler);
            lua_remove(L, handler);
            lua_pushvalue(L, key);
            lua_pushvalue(L, oldGuard);
            lua_rawset(L, LUA_REGISTRYINDEX);
            if (status) {
                lua_replace(L, count + 1);
                lua_settop(L, count + 1);
                PCSX::Lua::unpackErrorRaw(L);
                const int secondary = displayLuaStack(L, true, exception, hidden, true);
                return secondary ? secondary : status;
            }
            lua_settop(L, printer);
        } else {
            size_t length = 0;
            const char* text = lua_tolstring(L, -1, &length);
            if (useLua) {
                printf("%s\n", text);
            } else if (!invokeNativePrinter(native, text, length, exception)) {
                return 0;
            }
            lua_pop(L, useLua ? 2 : 1);
        }
    }
    lua_settop(L, count);
    return 0;
}
}

int PCSX::Lua::displayStackRaw(lua_State* L, bool error, std::exception_ptr* exception, lua_CFunction hidden) {
    return displayLuaStack(L, error, exception, hidden, false);
}

void PCSX::Lua::displayStack(bool error) {
    std::exception_ptr exception;
    const int status = displayStackRaw(L, error, &exception);
    if (exception) std::rethrow_exception(exception);
    if (status) throw std::runtime_error("Runtime error while running LUA code.");
}


json PCSX::Lua::toJson(int t) {
    if (!istable(t)) return {};
    if (t < 0) t = gettop() + t + 1;
    push();
    json ret = {};
    while (next(t) != 0) {
        auto keytype = type(-2);
        auto valtype = type(-1);
        bool keyvalid = false;
        bool valvalid = false;
        std::string key;
        json val;
        switch (keytype) {
            case LUA_TSTRING:
                keyvalid = true;
                key = tostring(-2);
                break;
        }
        switch (valtype) {
            case LUA_TNUMBER: {
                valvalid = true;
                auto num = tonumber(-1);
                double fractpart, intpart;
                fractpart = modf(num, &intpart);
                if (fractpart == 0.0) {
                    val = static_cast<int>(intpart);
                } else {
                    val = num;
                }
            } break;
            case LUA_TBOOLEAN:
                valvalid = true;
                val = toboolean(-1);
                break;
            case LUA_TSTRING:
                valvalid = true;
                val = tostring(-1);
                break;
            case LUA_TTABLE:
                valvalid = true;
                val = toJson(-1);
                break;
        }
        pop();
        if (keyvalid && valvalid) {
            ret[key] = val;
        }
    }

    return ret;
}

void PCSX::Lua::fromJson(const json& j, int t) {
    if (!istable(t)) return;
    if (t < 0) t = gettop() + t + 1;
    if (!j.is_object()) return;

    for (auto it = j.begin(); it != j.end(); it++) {
        switch (it.value().type()) {
            case json::value_t::number_integer:
            case json::value_t::number_unsigned:
                push(it.key());
                push(lua_Number(it.value().get<int>()));
                settable(t);
                break;
            case json::value_t::number_float:
                push(it.key());
                push(it.value().get<float>());
                settable(t);
                break;
            case json::value_t::boolean:
                push(it.key());
                push(it.value().get<bool>());
                settable(t);
                break;
            case json::value_t::string:
                push(it.key());
                push(it.value().get<std::string>());
                settable(t);
                break;
            case json::value_t::object:
            case json::value_t::array:
                push(it.key());
                newtable();
                fromJson(it.value(), -1);
                settable(t);
                break;
            case json::value_t::null:
                push(it.key());
                push();
                settable(t);
        }
    }
}
