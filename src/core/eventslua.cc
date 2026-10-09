/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
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

#include "core/eventslua.h"

#include "core/logger.h"
#include "core/system.h"
#include "support/eventbus.h"

#include <exception>

namespace {

template <typename Event>
void pushEvent(lua_State* L, const Event&) { lua_newtable(L); }

template <>
void pushEvent(lua_State* L, const PCSX::Events::ExecutionFlow::Pause& event) {
    lua_newtable(L);
    lua_pushboolean(L, event.exception);
    lua_setfield(L, -2, "exception");
}

template <>
void pushEvent(lua_State* L, const PCSX::Events::ExecutionFlow::Reset& event) {
    lua_newtable(L);
    lua_pushboolean(L, event.hard);
    lua_setfield(L, -2, "hard");
}

template <>
void pushEvent(lua_State* L, const PCSX::Events::GUI::JumpToPC& event) {
    lua_newtable(L);
    lua_pushnumber(L, event.pc);
    lua_setfield(L, -2, "pc");
}

template <>
void pushEvent(lua_State* L, const PCSX::Events::GUI::JumpToMemory& event) {
    lua_newtable(L);
    lua_pushnumber(L, event.address);
    lua_setfield(L, -2, "address");
    lua_pushnumber(L, event.size);
    lua_setfield(L, -2, "size");
}

template <>
void pushEvent(lua_State* L, const PCSX::Events::Keyboard& event) {
    lua_newtable(L);
    lua_pushnumber(L, event.key);
    lua_setfield(L, -2, "key");
    lua_pushnumber(L, event.scancode);
    lua_setfield(L, -2, "scancode");
    lua_pushnumber(L, event.action);
    lua_setfield(L, -2, "action");
    lua_pushnumber(L, event.mods);
    lua_setfield(L, -2, "mods");
}

template <typename Event>
struct EventInvocation {
    const Event* event;
    int ref;
    int status = 0;
    int displayStatus = 0;
    std::exception_ptr exception;
};

template <typename Event>
int eventBootstrap(lua_State* L) {
    auto* frame = static_cast<EventInvocation<Event>*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    if (!lua_checkstack(L, 16)) return luaL_error(L, "not enough Lua stack for event listener");
    lua_getfield(L, LUA_REGISTRYINDEX, "EVENT_LISTENERS");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "EVENT_LISTENERS");
    }
    lua_pushnumber(L, frame->ref);
    lua_gettable(L, -2);
    lua_getfield(L, -1, "callback");
    pushEvent(L, *frame->event);
    frame->status = PCSX::Lua::pcallRaw(L, 1, LUA_MULTRET, eventBootstrap<Event>);
    if (frame->status) {
        PCSX::Lua::unpackErrorRaw(L);
        frame->displayStatus = PCSX::Lua::displayStackRaw(L, true, &frame->exception, eventBootstrap<Event>);
    }
    return 0;
}

void logListenerStatus(int status) {
    switch (status) {
        case LUA_ERRRUN:
            PCSX::g_system->log(PCSX::LogClass::LUA, "Error in event listener: Runtime error while running LUA code."); break;
        case LUA_ERRMEM:
            PCSX::g_system->log(PCSX::LogClass::LUA, "Error in event listener: Memory allocation error while running LUA code."); break;
        case LUA_ERRERR:
            PCSX::g_system->log(PCSX::LogClass::LUA, "Error in event listener: Error in Error function."); break;
        case LUA_ERRSYNTAX:
            PCSX::g_system->log(PCSX::LogClass::LUA, "Error in event listener: Syntax error in Lua code."); break;
        default:
            PCSX::g_system->log(PCSX::LogClass::LUA, "Error in event listener: Unknow error while running LUA code (err code: %d)", status);
    }
}

template <typename Event>
void createListener(PCSX::Lua L) {
    // 1 = event name, 2 = callback
    L.getfieldtable("EVENT_LISTENERS", LUA_REGISTRYINDEX);
    // 3 = event listeners table
    L.newtable();
    // 4 = empty table

    L.push("callback");
    // 1 = event name, 2 = callback, 3 = event listeners table, 4 = empty table, 5 = "callback"
    L.copy(2);
    // 1 = event name, 2 = callback, 3 = event listeners table, 4 = empty table, 5 = "callback", 6 = callback
    L.settable();
    // 1 = event name, 2 = callback, 3 = event listeners table, 4 = event info table

    L.push("thread");
    auto t = L.thread(true);
    L.settable();

    // grabs a reference to the event info table, which looks like this:
    // { callback = function, thread = coroutine }
    int ref = L.ref();

    auto listener = new PCSX::EventBus::Listener(PCSX::g_system->m_eventBus);
    listener->listen<Event>([L = t, ref](const Event& event) mutable {
        lua_State* state = L.getState();
        const int top = lua_gettop(state);
        EventInvocation<Event> frame{&event, ref};
        const int protectedStatus = lua_cpcall(state, eventBootstrap<Event>, &frame);
        lua_settop(state, top);
        try {
            if (frame.exception) std::rethrow_exception(frame.exception);
            if (protectedStatus || frame.displayStatus) {
                PCSX::g_system->log(PCSX::LogClass::LUA, "Error while preparing or displaying event listener error.");
            }
            const int status = frame.status ? frame.status : (protectedStatus ? protectedStatus : frame.displayStatus);
            if (status) logListenerStatus(status);
        } catch (std::exception& exception) {
            PCSX::g_system->log(PCSX::LogClass::LUA, "Error in event listener: %s", exception.what());
        } catch (...) {
            PCSX::g_system->log(PCSX::LogClass::LUA, "Unknown error in event listener");
        }
    });

    int a = L.gettop();

    L.newtable();
    L.newuser(1);
    L.newtable();
    L.push(lua_Number(ref));
    L.push(listener);
    // Neither Lua nor LuaJIT handle nested garbage collectors properly, so
    // don't use the full lambda version here otherwise things will go boom
    L.declareFunc(
        "__gc",
        [](lua_State* L_) -> int {
            PCSX::Lua L(L_);
            int ref = L.tonumber(lua_upvalueindex(1));
            auto listener = L.touserdata<PCSX::EventBus::Listener>(lua_upvalueindex(2));
            L.getfieldtable("EVENT_LISTENERS", LUA_REGISTRYINDEX);
            L.unref(ref);
            delete listener;
            return 0;
        },
        -3, 2);
    int b = L.gettop();
    L.setmetatable();
    L.setfield("_proxy");
    int c = L.gettop();

    bool called = false;
    L.declareFunc(
        "remove",
        [ref, listener, called](PCSX::Lua L) mutable -> int {
            if (called) return 0;
            called = true;
            L.getfield("_proxy");
            L.push();
            L.setmetatable();
            L.pop();
            L.getfieldtable("EVENT_LISTENERS", LUA_REGISTRYINDEX);
            L.unref(ref);
            delete listener;
            return 0;
        },
        -1);
}

}  // namespace

void PCSX::LuaBindings::open_events(Lua L) {
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.getfieldtable("Events");

    L.declareFunc(
        "createEventListener",
        [](lua_State* L_) -> int {
            Lua L(L_);
            if (L.gettop() != 2) {
                return L.error("createEventListener: expected 2 arguments");
            }
            if (!L.isstring(1)) {
                return L.error("createEventListener: 1st argument needs to be a string");
            }
            auto name = L.tostring(1);
            if (name == "Quitting") {
                createListener<Events::Quitting>(L);
            } else if (name == "IsoMounted") {
                createListener<Events::IsoMounted>(L);
            } else if (name == "GPU::Vsync") {
                createListener<Events::GPU::VSync>(L);
            } else if (name == "ExecutionFlow::ShellReached") {
                createListener<Events::ExecutionFlow::ShellReached>(L);
            } else if (name == "ExecutionFlow::Run") {
                createListener<Events::ExecutionFlow::Run>(L);
            } else if (name == "ExecutionFlow::Pause") {
                createListener<Events::ExecutionFlow::Pause>(L);
            } else if (name == "ExecutionFlow::Reset") {
                createListener<Events::ExecutionFlow::Reset>(L);
            } else if (name == "ExecutionFlow::SaveStateLoaded") {
                createListener<Events::ExecutionFlow::SaveStateLoaded>(L);
            } else if (name == "GUI::JumpToPC") {
                createListener<Events::GUI::JumpToPC>(L);
            } else if (name == "GUI::JumpToMemory") {
                createListener<Events::GUI::JumpToMemory>(L);
            } else if (name == "Keyboard") {
                createListener<Events::Keyboard>(L);
            } else if (name == "Memory::SetLuts") {
                createListener<Events::Memory::SetLuts>(L);
            } else {
                return L.error("createListener: unknown event name");
            }
            return 1;
        },
        -1);

    L.pop();
    L.pop();
}
