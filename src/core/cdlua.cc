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

#include "core/cdlua.h"

#include <array>
#include <charconv>
#include <cmath>
#include <memory>

#include "core/cdrom.h"
#include "core/history.h"
#include "core/sstate.h"

namespace {
using namespace PCSX;
using namespace PCSX::SaveStates;

#define CD_NUMBERS(X) \
    X(CDDataFIFOIndex, dataFIFOIndex) \
    X(CDDataFIFOSize, dataFIFOSize) \
    X(CDDataFIFOPending, dataFIFOPending) \
    X(CDRegisterAddress, registerAddress) \
    X(CDReadingState, readingState) \
    X(CDReadingType, readingType) \
    X(CDPlayTrack, playTrack) \
    X(CDPlayStartCycle, playStartCycle) \
    X(CDStatus, status) \
    X(CDSpeed, speed) \
    X(CDReadSpan, readSpan) \
    X(CDInterruptCauseMask, interruptCauseMask) \
    X(CDMode, mode) \
    X(CDFilterFile, filterFile) \
    X(CDFilterChannel, filterChannel) \
    X(CDLidCloseAtCycles, lidCloseAtCycles)

#define CD_BOOLEANS(X) \
    X(CDMotorOn, motorOn) \
    X(CDSpeedChanged, speedChanged) \
    X(CDInvalidLocL, invalidLocL) \
    X(CDDataRequested, dataRequested) \
    X(CDSubheaderFilter, subheaderFilter) \
    X(CDRealtime, realtime) \
    X(CDStartPlaying, startPlaying) \
    X(CDAutoPause, autoPause) \
    X(CDReport, report) \
    X(CDSetLocPending, setLocPending) \
    X(CDMuted, muted) \
    X(CDPeakFlag, peakFlag) \
    X(CDADPCMMuted, adpcmMuted) \
    X(CDXAEnded, xaEnded) \
    X(CDXAFirstSector, xaFirstSector) \
    X(CDSoundMapEnabled, soundMapEnabled) \
    X(CDLidOpen, lidOpen) \
    X(CDWasLidOpened, wasLidOpened) \
    X(CDLidCloseScheduled, lidCloseScheduled)

struct CDObservation {
#define NUMBER(type, name) uint32_t name = 0;
    CD_NUMBERS(NUMBER)
#undef NUMBER
#define BOOLEAN(type, name) bool name = false;
    CD_BOOLEANS(BOOLEAN)
#undef BOOLEAN
    struct Queue {
        std::array<uint8_t, 16> payload{};
        uint8_t value = 0, payload_size = 0, payload_index = 0;
        bool has_value = false, value_read = false, hit_max = false;
    };
    struct Position { uint8_t m = 0, s = 0, f = 0; bool present = false; };
    struct ADPCM { int32_t y0 = 0, y1 = 0; bool present = false; };
    std::array<uint8_t, 2352> dataFIFO{};
    std::array<uint8_t, 4> atv{}, atvPending{};
    std::array<uint8_t, 8> lastLocP{}, lastLocL{};
    std::array<Queue, 4> queues{};
    Position currentPosition, seekPosition;
    ADPCM xaLeft, xaRight;
    uint64_t seed = 0;
};

CDObservation projectCD(const SaveState& state) {
    const auto& cd = state.get<CDRomField>();
    CDObservation out;
#define NUMBER(type, name) out.name = cd.get<type>().decodedValue();
    CD_NUMBERS(NUMBER)
    CD_BOOLEANS(NUMBER)
#undef NUMBER
    const auto bytes = [&](auto& target, const auto& field) {
        std::copy_n(field.decodedData(), target.size(), target.begin());
    };
    bytes(out.dataFIFO, cd.get<CDDataFIFO>());
    bytes(out.atv, cd.get<CDATV>());
    bytes(out.atvPending, cd.get<CDATVPending>());
    bytes(out.lastLocP, cd.get<CDLastLocP>());
    bytes(out.lastLocL, cd.get<CDLastLocL>());
    const auto queue = [&](const auto& field) {
        CDObservation::Queue value;
        bytes(value.payload, field.template get<CDQueuePayload>());
        value.value = field.template get<CDQueueValue>().decodedValue();
        value.has_value = field.template get<CDQueueHasValue>().decodedValue();
        value.value_read = field.template get<CDQueueValueRead>().decodedValue();
        value.hit_max = field.template get<CDQueueHitMax>().decodedValue();
        value.payload_size = field.template get<CDQueuePayloadSize>().decodedValue();
        value.payload_index = field.template get<CDQueuePayloadIndex>().decodedValue();
        return value;
    };
    out.queues = {queue(cd.get<CDCommandFifo>()), queue(cd.get<CDCommandExecuting>()),
                  queue(cd.get<CDResponseFifo0>()), queue(cd.get<CDResponseFifo1>())};
    const auto position = [](const auto& field) {
        return CDObservation::Position{field.template get<CDMSFMinute>().decodedValue(),
            field.template get<CDMSFSecond>().decodedValue(), field.template get<CDMSFFrame>().decodedValue(),
            field.hasData()};
    };
    out.currentPosition = position(cd.get<CDCurrentPosition>());
    out.seekPosition = position(cd.get<CDSeekPosition>());
    const auto adpcm = [](const auto& field) {
        return CDObservation::ADPCM{field.template get<CDADPCMY0>().decodedValue(),
            field.template get<CDADPCMY1>().decodedValue(), field.hasData()};
    };
    out.xaLeft = adpcm(cd.get<CDXALeft>());
    out.xaRight = adpcm(cd.get<CDXARight>());
    out.seed = cd.get<CDSeed>().decodedValue();
    return out;
}

Result capture(CDObservation& out) noexcept {
    return withCheckedState([&](const SaveState& state) { out = projectCD(state); return Result::Ok; });
}

void number(lua_State* state, const char* name, double value) {
    lua_pushnumber(state, value); lua_setfield(state, -2, name);
}
void boolean(lua_State* state, const char* name, bool value) {
    lua_pushboolean(state, value); lua_setfield(state, -2, name);
}
void pushObservation(lua_State* state, const CDObservation& cd) {
    lua_createtable(state, 0, 49);
#define NUMBER(type, name) number(state, #name, cd.name);
    CD_NUMBERS(NUMBER)
#undef NUMBER
#define BOOLEAN(type, name) boolean(state, #name, cd.name);
    CD_BOOLEANS(BOOLEAN)
#undef BOOLEAN
    const auto bytes = [&](const char* name, const auto& value) {
        lua_pushlstring(state, reinterpret_cast<const char*>(value.data()), value.size());
        lua_setfield(state, -2, name);
    };
    bytes("dataFIFO", cd.dataFIFO); bytes("atv", cd.atv); bytes("atvPending", cd.atvPending);
    bytes("lastLocP", cd.lastLocP); bytes("lastLocL", cd.lastLocL);
    constexpr const char* queueNames[] = {"commandFifo", "commandExecuting", "responseFifo0", "responseFifo1"};
    for (unsigned i = 0; i < 4; ++i) {
        const auto& queue = cd.queues[i];
        lua_createtable(state, 0, 7);
        bytes("payload", queue.payload);
        number(state, "value", queue.value); number(state, "payload_size", queue.payload_size);
        number(state, "payload_index", queue.payload_index);
        boolean(state, "has_value", queue.has_value); boolean(state, "value_read", queue.value_read);
        boolean(state, "hit_max", queue.hit_max);
        lua_setfield(state, -2, queueNames[i]);
    }
    const auto position = [&](const char* name, const auto& value) {
        if (!value.present) return;
        lua_createtable(state, 0, 3);
        number(state, "m", value.m); number(state, "s", value.s); number(state, "f", value.f);
        lua_setfield(state, -2, name);
    };
    position("currentPosition", cd.currentPosition); position("seekPosition", cd.seekPosition);
    const auto adpcm = [&](const char* name, const auto& value) {
        if (!value.present) return;
        lua_createtable(state, 0, 2);
        number(state, "y0", value.y0); number(state, "y1", value.y1);
        lua_setfield(state, -2, name);
    };
    adpcm("xaLeft", cd.xaLeft); adpcm("xaRight", cd.xaRight);
    char seed[21]; seed[0] = '#';
    const auto decimal = std::to_chars(seed + 1, seed + sizeof(seed), cd.seed);
    lua_pushlstring(state, seed, decimal.ptr - seed); lua_setfield(state, -2, "seed");
}

int captureChecked(lua_State* state) {
    CDObservation observation;
    const auto result = lua_gettop(state) == 0 ? capture(observation) : Result::InvalidState;
    lua_pushnumber(state, unsigned(result));
    if (result != Result::Ok) return 1;
    luaL_checkstack(state, 6, "CD capture requires more stack space");
    pushObservation(state, observation);
    return 2;
}

int peekLid(lua_State* state) {
    if (lua_gettop(state) != 0 || !History::ownerThread() || !g_emulator || !g_emulator->m_cdrom)
        return luaL_error(state, "CD peekLid requires an available owner thread");
    lua_pushboolean(state, g_emulator->m_cdrom->peekLid());
    return 1;
}

template<void (PCSX::CDRom::*Operation)()>
int changeLid(lua_State* state) {
    const auto result = lua_gettop(state) == 0 ? withCheckedState([](const SaveState&) {
        (g_emulator->m_cdrom.get()->*Operation)();
        return Result::Ok;
    }) : Result::InvalidState;
    lua_pushnumber(state, unsigned(result));
    return 1;
}

int replaceMedia(lua_State* state, bool eject) {
    if ((eject && lua_gettop(state) != 0) || (!eject && (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)))
        return luaL_error(state, "CD mount requires a path; eject takes no arguments");
    size_t length = 0;
    const char* path = eject ? nullptr : lua_tolstring(state, 1, &length);
    if (!eject && (!length || length > 4096 || std::string_view(path, length).find('\0') != std::string_view::npos))
        return luaL_error(state, "invalid CD image path");
    auto result = context();
    bool committed = false;
    if (result == Result::Ok) {
        try {
            std::shared_ptr<CDRIso> prepared = eject ? std::make_shared<CDRIso>(new FailedFile) :
                std::make_shared<CDRIso>(std::filesystem::path(std::string_view(path, length)));
            if (!eject && prepared->failed()) result = Result::Malformed;
            else result = withCheckedState([&](const SaveState& current) {
                const auto& cd = current.get<CDRomField>();
                if (!cd.get<CDLidOpen>().decodedValue()) return Result::InvalidState;
                const auto mask = current.get<RegistersField>().get<ScheduleMask>().decodedValue();
                if (!g_emulator->m_cdrom->acceptsPreparedMedia(cd, mask, prepared)) return Result::TopologyMismatch;
                g_emulator->m_cdrom->swapPreparedIso(prepared);
                committed = true;
                return Result::Ok;
            });
            if (committed) g_emulator->m_cdrom->notifyIsoMounted();
        } catch (const std::bad_alloc&) { result = committed ? Result::CommitFailed : Result::AllocationFailed; }
        catch (...) { result = committed ? Result::CommitFailed : Result::Malformed; }
    }
    if (result == Result::CommitFailed) g_system->pause();
    lua_pushnumber(state, unsigned(result));
    return 1;
}
int mount(lua_State* state) { return replaceMedia(state, false); }
int eject(lua_State* state) { return replaceMedia(state, true); }

constexpr char waitMetatable[] = "PCSX.CDWait";
constexpr char waitQueue[] = "PCSX.CDWaitQueue";
constexpr unsigned maxWaits = 64;

struct Wait {
    int callback = LUA_NOREF;
    std::unique_ptr<EventBus::Listener> listener;
    unsigned maxVsyncs = 60, vsyncs = 0;
    bool settle = false;
    const char* error = nullptr;
    CDObservation observation;
    ~Wait() {
        listener.reset();
    }
};
int destroyWait(lua_State* state) {
    auto* wait = static_cast<Wait*>(lua_touserdata(state, 1));
    if (wait->callback != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, wait->callback);
    wait->~Wait();
    return 0;
}
int cancelWait(lua_State* state) {
    auto* wait = static_cast<Wait*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (wait->callback != LUA_NOREF) {
        wait->error = "CD wait cancelled";
        wait->listener.reset();
    }
    return 0;
}
unsigned option(lua_State* state, const char* name, unsigned fallback, unsigned limit) {
    lua_getfield(state, 1, name);
    if (lua_isnil(state, -1)) { lua_pop(state, 1); return fallback; }
    const double value = lua_tonumber(state, -1);
    if (lua_type(state, -1) != LUA_TNUMBER || !std::isfinite(value) || value < 1 || value > limit ||
        std::floor(value) != value) luaL_error(state, "invalid CD wait %s", name);
    lua_pop(state, 1);
    return unsigned(value);
}
int waitResponse(lua_State* state) {
    if (lua_gettop(state) != 2 || !lua_istable(state, 1) || !lua_isfunction(state, 2))
        return luaL_error(state, "CD waitResponse requires options and a callback");
    const unsigned vsyncs = option(state, "maxVsyncs", 60, 120);
    lua_pushnil(state);
    while (lua_next(state, 1)) {
        size_t length = 0;
        const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tolstring(state, -2, &length) : nullptr;
        if (!key || std::string_view(key, length) != "maxVsyncs")
            return luaL_error(state, "unknown CD wait option");
        lua_pop(state, 1);
    }
    CDObservation initial;
    const auto admission = capture(initial);
    if (admission != Result::Ok) { lua_pushnumber(state, unsigned(admission)); return 1; }
    luaL_checkstack(state, 6, "CD wait requires more stack space");
    lua_getfield(state, LUA_REGISTRYINDEX, waitQueue);
    const int queue = lua_gettop(state);
    unsigned slot = 1;
    for (; slot <= maxWaits; ++slot) {
        lua_rawgeti(state, queue, slot);
        const bool empty = lua_isnil(state, -1);
        lua_pop(state, 1);
        if (empty) break;
    }
    if (slot > maxWaits) return luaL_error(state, "CD wait queue exceeds 64 handles");
    auto* wait = new (lua_newuserdata(state, sizeof(Wait))) Wait();
    const int owner = lua_gettop(state);
    luaL_getmetatable(state, waitMetatable); lua_setmetatable(state, owner);
    wait->maxVsyncs = vsyncs;
    try {
        wait->listener = std::make_unique<EventBus::Listener>(g_system->m_eventBus);
        wait->listener->listen<Events::GPU::VSync>([wait](const auto&) {
            if (wait->error) return;
            ++wait->vsyncs;
            wait->settle = true;
            g_system->pause();
        });
        wait->listener->listen<Events::Quitting>([wait](const auto&) { wait->error = "CD wait interrupted by shutdown"; });
        wait->listener->listen<Events::ExecutionFlow::Reset>([wait](const auto&) { wait->error = "CD wait interrupted by reset"; });
        wait->listener->listen<Events::ExecutionFlow::SaveStateLoaded>([wait](const auto&) {
            wait->error = "CD wait interrupted by state load";
        });
    } catch (...) {
        wait->listener.reset();
        lua_pushnumber(state, unsigned(Result::AllocationFailed));
        return 1;
    }
    lua_createtable(state, 0, 1);
    lua_pushvalue(state, owner); lua_pushcclosure(state, cancelWait, 1); lua_setfield(state, -2, "cancel");
    lua_pushvalue(state, 2); wait->callback = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pushvalue(state, owner); lua_rawseti(state, queue, slot);
    lua_pushnumber(state, unsigned(Result::Ok)); lua_insert(state, -2);
    return 2;
}

bool settled() {
    return !g_system->inExecute() && !g_system->hasPendingSaveStateLoad() && !g_system->m_eventBus->dispatching();
}
}  // namespace

void PCSX::LuaFFI::openCD(Lua L) {
    auto* state = L.getState();
    luaL_newmetatable(state, waitMetatable);
    lua_pushcfunction(state, destroyWait); lua_setfield(state, -2, "__gc");
    lua_pop(state, 1);
    lua_newtable(state); lua_setfield(state, LUA_REGISTRYINDEX, waitQueue);
    lua_getglobal(state, "PCSX"); lua_createtable(state, 0, 9);
    const auto bind = [&](const char* name, lua_CFunction function) {
        lua_pushcfunction(state, function); lua_setfield(state, -2, name);
    };
    bind("captureChecked", captureChecked); bind("peekLid", peekLid);
    bind("openLid", changeLid<&PCSX::CDRom::openLid>);
    bind("closeLid", changeLid<&PCSX::CDRom::closeLid>);
    bind("scheduleCloseLid", changeLid<&PCSX::CDRom::scheduleCloseLid>);
    bind("mount", mount); bind("eject", eject); bind("waitResponse", waitResponse);
    lua_setfield(state, -2, "CD"); lua_pop(state, 1);
}

namespace {
int pollWaits(lua_State* state) {
    Lua L(state);
    if (!History::ownerThread() || !g_system) return 0;
    const int base = lua_gettop(state);
    lua_getfield(state, LUA_REGISTRYINDEX, waitQueue);
    if (!lua_istable(state, -1)) { lua_settop(state, base); return 0; }
    const int queue = base + 1;
    bool resume = false, completed = false;
    for (unsigned slot = 1; slot <= maxWaits; ++slot) {
        lua_rawgeti(state, queue, slot);
        auto* wait = static_cast<Wait*>(lua_touserdata(state, -1));
        if (!wait || wait->callback == LUA_NOREF) { lua_pop(state, 1); continue; }
        if (g_system->quitting()) wait->error = "CD wait interrupted by shutdown";
        if (!wait->error && !wait->settle) { lua_pop(state, 1); continue; }
        if (wait->error) wait->listener.reset();
        g_system->pause();
        if (!settled()) { lua_pop(state, 1); continue; }
        if (!wait->error) {
            const auto result = capture(wait->observation);
            if (result != Result::Ok) wait->error = "CD wait checked capture refused";
            else if (!wait->observation.queues[2].has_value || wait->observation.queues[2].value_read) {
                if (wait->vsyncs >= wait->maxVsyncs) wait->error = "no controller IRQ observed within frame bound";
                else {
                    wait->settle = false;
                    lua_pop(state, 1);
                    resume = true;
                    continue;
                }
            }
        }
        completed = true;
        wait->listener.reset();
        const int callback = wait->callback;
        wait->callback = LUA_NOREF;
        lua_pushnil(state); lua_rawseti(state, queue, slot);
        lua_rawgeti(state, LUA_REGISTRYINDEX, callback); luaL_unref(state, LUA_REGISTRYINDEX, callback);
        if (wait->error) { lua_pushnil(state); lua_pushstring(state, wait->error); }
        else {
            lua_createtable(state, 0, 4);
            number(state, "vsyncs", wait->vsyncs);
            number(state, "interrupt", wait->observation.queues[2].value);
            boolean(state, "command_error", wait->observation.queues[2].value == 5);
            pushObservation(state, wait->observation); lua_setfield(state, -2, "state");
            lua_pushnil(state);
        }
        try { L.pcall(2); } catch (...) { lua_settop(state, queue); }
        lua_settop(state, queue);
    }
    lua_settop(state, base);
    if (resume && !completed && !g_system->quitting() && settled()) g_system->resume();
    return 0;
}
}

void PCSX::LuaFFI::pollCD(Lua L) {
    L.push(lua_CFunction(pollWaits));
    try { L.pcall(); } catch (...) {}
}
