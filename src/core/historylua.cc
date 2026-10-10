// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/historylua.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <unordered_set>

#include "core/history.h"
#include "core/psxemulator.h"
#include "core/spu.h"
#include "core/transactions.h"
#include "lua/luawrapper.h"

namespace {
using namespace PCSX;
using namespace PCSX::History;
constexpr const char* metatable = "PCSX.HistoryCapture";

struct Handoff {
    uint32_t record;
    uint64_t epoch, first, last;
    uint32_t count, flags;
};
struct Correlations {
    bool enabled = false;
    CD::WorkerStatus final;
    std::vector<Handoff> handoffs;
    std::vector<uint32_t> unconsumed;
    std::vector<std::pair<uint64_t, uint64_t>> feeds;
};
struct FrozenCapture {
    Snapshot history;
    CD::WorkerSnapshot worker;
    SPU::Capture::Snapshot audio;
    std::optional<Correlations> correlations;
};
struct CaptureOwner { FrozenCapture* value = nullptr; };

void require(bool valid, const char* error) { if (!valid) throw error; }
uint64_t little(const uint8_t* bytes, uint32_t size) {
    uint64_t value = 0;
    for (uint32_t n = 0; n < size; ++n) value |= uint64_t(bytes[n]) << (n * 8);
    return value;
}
const uint8_t* payload(const FrozenCapture& capture, const Record& row, uint32_t size) {
    require(row.length == size && row.offset <= capture.history.payload.size() &&
                size <= capture.history.payload.size() - row.offset,
            "native CD payload exceeds frozen journal");
    return size ? capture.history.payload.data() + row.offset : nullptr;
}
CD::WorkerStatus workerStatus(const uint8_t* bytes) {
    CD::WorkerStatus result;
    result.epoch = little(bytes, 8); result.beginBatch = little(bytes + 8, 8); result.endBatch = little(bytes + 16, 8);
    result.version = little(bytes + 24, 4); result.state = little(bytes + 28, 4); result.events = little(bytes + 32, 4);
    result.limit = little(bytes + 36, 4); result.failures = little(bytes + 40, 4); result.dropped = little(bytes + 44, 4);
    return result;
}

Correlations correlate(const FrozenCapture& capture) {
    const auto& history = capture.history;
    const auto& status = history.status;
    require(status.version == 1 && status.state == 2 && !status.failures && !status.dropped &&
                status.events == history.records.size() && status.bytes == history.payload.size() &&
                status.beginCycle <= status.endCycle,
            "CD correlation requires complete frozen history");
    Correlations result;
    std::optional<CD::WorkerStatus> first, final;
    std::optional<uint64_t> audioEpoch;
    uint64_t previous = status.beginCycle;
    uint32_t offset = 0;
    std::unordered_set<uint64_t> audioJoins;
    for (uint32_t index = 0; index < history.records.size(); ++index) {
        const auto& row = history.records[index];
        require(row.sequence == uint64_t(index) + 1 && row.cycle >= previous && row.cycle <= status.endCycle &&
                    row.offset == offset && row.length <= status.bytes - offset,
                "CD journal order/bounds mismatch");
        previous = row.cycle; offset += row.length;
        if (row.kind == uint32_t(Kind::CdContext) && row.device <= 1) {
            const auto* bytes = payload(capture, row, 384);
            const uint64_t epoch = little(bytes + 24, 8);
            require(little(bytes + 16, 8) == history.epoch, "native CD history epoch differs from owning capture");
            if (row.device == 0) {
                require(!audioEpoch, "duplicate native CD initial context");
                audioEpoch = epoch;
                if (epoch) {
                    const auto& audio = capture.audio.status;
                    require(capture.audio.epoch == epoch && audio.version == 2 && audio.state == 2 && !audio.failures &&
                                !audio.droppedEvents && !audio.droppedFrames && !audio.streamingDropped &&
                                audio.events == capture.audio.events.size() && audio.beginCycle <= row.cycle,
                            "joined Audio lifecycle mismatch");
                }
            } else {
                require(audioEpoch && epoch == *audioEpoch, "unfinished feed or changed Audio epoch");
                if (epoch) require(capture.audio.status.endCycle >= row.cycle, "Audio ended before native CD capture");
            }
        } else if (row.kind == uint32_t(Kind::CdTransition) && row.device >= 16) {
            const auto value = workerStatus(payload(capture, row, 48));
            require(value.version == 3 && !value.failures && !value.dropped && value.limit >= 1 && value.limit <= 65536,
                    "incomplete native worker boundary");
            if (row.device == 16) {
                require(!first && value.state == 1 && !value.events, "invalid native worker start");
                first = value;
            } else {
                require(row.device == 17 && first && !final && value.state == 2 && value.epoch == first->epoch &&
                            value.limit == first->limit && value.events <= value.limit,
                        "invalid native worker freeze");
                final = value;
            }
        } else if (row.kind == uint32_t(Kind::CdIRQ) && row.device == 4) {
            require(first && !final, "SPU handoff outside worker capture");
            const auto* bytes = payload(capture, row, 32);
            Handoff handoff{index, little(bytes, 8), little(bytes + 8, 8), little(bytes + 16, 8),
                            uint32_t(little(bytes + 24, 4)), uint32_t(little(bytes + 28, 4))};
            require(handoff.epoch == first->epoch && handoff.count >= 1 && handoff.flags <= 4 &&
                        !(handoff.flags & 3) && bool(handoff.flags & 4) == (handoff.count > 1),
                    "unqualified SPU producer ancestry");
            result.handoffs.push_back(handoff);
        } else if (row.kind == uint32_t(Kind::CdAudio) && row.device == 1 && audioEpoch && *audioEpoch) {
            const auto& data = row.data;
            const auto* bytes = payload(capture, row, 16);
            require(data[5] == 1 && little(bytes, 8) == *audioEpoch, "native joined feed changed Audio lifecycle");
            if (data[0] < 6) {
                require(!row.related, "unsubmitted native feed invented Audio event");
                continue;
            }
            require(data[6] == 2 && row.related && row.related <= capture.audio.events.size() &&
                        audioJoins.insert(row.related).second,
                    "joined feed lacks unique retained Audio event");
            const auto& event = capture.audio.events[row.related - 1];
            require(event.kind == uint32_t(SPU::Capture::Kind::Feed) && !event.parent && event.cycle == row.cycle &&
                        event.address == data[1] && event.width == data[2] && event.detail == data[3] &&
                        event.value == data[4] && (event.flags & 1) == uint32_t(data[0] == 6) &&
                        ((event.flags >> 1) & 1) == data[7],
                    "native CD/Audio feed receipt mismatch");
            result.feeds.emplace_back(row.sequence, row.related);
        }
    }
    require(offset == status.bytes, "unreferenced native payload bytes");
    if (!first) {
        require(!final && result.handoffs.empty(), "worker evidence without profile");
        return result;
    }
    require(final.has_value(), "worker lane not frozen");
    const auto& native = capture.worker.status;
    require(native.version == 3 && native.state == 2 && !native.failures && !native.dropped && native.epoch == final->epoch &&
                native.events == final->events && native.limit == final->limit && native.beginBatch == final->beginBatch &&
                native.endBatch == final->endBatch && native.events == capture.worker.records.size(),
            "native worker status differs from CPU freeze receipt");
    bool open = false;
    std::optional<uint64_t> batch, published;
    std::vector<bool> producers(native.events), used(native.events);
    for (uint32_t index = 0; index < native.events; ++index) {
        const auto& row = capture.worker.records[index];
        const auto& data = row.data;
        require(row.sequence == uint64_t(index) + 1 && row.epoch == native.epoch && row.kind >= 1 && row.kind <= 6 &&
                    row.flags <= 1 && (!published || row.cpuPublicationCycle >= *published),
                "invalid native worker chronology");
        published = row.cpuPublicationCycle;
        if (row.kind == 1) {
            require(!open && !row.flags && (!batch || row.batch > *batch), "nested/nonmonotonic worker batch");
            batch = row.batch; open = true;
        } else require(open && batch && *batch == row.batch, "worker observation outside complete batch");
        switch (row.kind) {
            case 2:
                require(data[3] <= 1 && data[4] <= 1, "invalid decoded-buffer predicate");
                if (row.flags == 1) require(data[5] >= 1 && bool(data[4]) ==
                    (bool(data[2] & 64) && data[1] != UINT32_MAX && data[1] < 4096),
                    "decoded-buffer native eligibility predicate mismatch");
                break;
            case 3:
                require(data[0] < 4 && data[4] <= 1 && bool(data[4]) ==
                            (data[2] >= data[1] && uint64_t(data[2]) < uint64_t(data[1]) + 2),
                        "decoded-buffer native region predicate mismatch");
                break;
            case 4:
                require(data[0] < 24 && data[4] <= 1 && data[5] <= 1, "invalid native voice IRQ operands");
                if (!(data[2] & 64)) require(!data[4] && !data[5], "disabled voice IRQ invented address predicates");
                break;
            case 5:
                require(!row.flags && data[4] <= 1, "invalid atomic SPU publication");
                producers[index] = true;
                break;
            case 6: open = false; break;
        }
    }
    require(!open && (!native.events || (capture.worker.records.front().batch == final->beginBatch && batch == final->endBatch)),
            "native worker batches incomplete");
    uint64_t last = 0;
    for (const auto& handoff : result.handoffs) {
        require(handoff.first > last && handoff.last >= handoff.first && handoff.last <= native.events,
                "SPU handoff sequence range overlaps or exceeds lane");
        uint32_t count = 0;
        for (uint64_t sequence = handoff.first; sequence <= handoff.last; ++sequence) {
            if (!producers[sequence - 1]) continue;
            require(!used[sequence - 1], "SPU publication consumed twice");
            used[sequence - 1] = true; ++count;
        }
        require(producers[handoff.first - 1] && producers[handoff.last - 1] && count == handoff.count,
                "SPU handoff does not cover exact atomic publications");
        last = handoff.last;
    }
    for (uint32_t index = 0; index < native.events; ++index) if (producers[index] && !used[index]) result.unconsumed.push_back(index + 1);
    result.enabled = true; result.final = *final;
    return result;
}

CaptureOwner* owner(lua_State* state) { return static_cast<CaptureOwner*>(luaL_checkudata(state, 1, metatable)); }
FrozenCapture& frozen(lua_State* state) {
    auto* value = owner(state)->value;
    if (!value) luaL_error(state, "history capture is released");
    return *value;
}
int destroy(lua_State* state) {
    auto* capture = owner(state);
    delete capture->value; capture->value = nullptr;
    return 0;
}
void number(lua_State* state, const char* key, uint64_t value) { lua_pushnumber(state, value); lua_setfield(state, -2, key); }
void wide(lua_State* state, uint64_t value) {
    char bytes[32];
    const auto formatted = std::to_chars(std::begin(bytes), std::end(bytes), value);
    lua_pushlstring(state, bytes, formatted.ptr - bytes);
}
void wide(lua_State* state, const char* key, uint64_t value) { wide(state, value); lua_setfield(state, -2, key); }
void boolean(lua_State* state, const char* key, bool value) { lua_pushboolean(state, value); lua_setfield(state, -2, key); }
void string(lua_State* state, const char* key, const char* value) { lua_pushstring(state, value); lua_setfield(state, -2, key); }
void array(lua_State* state) {
    lua_getglobal(state, "PCSX"); lua_getfield(state, -1, "jsonArray"); lua_remove(state, -2);
    lua_newtable(state); lua_call(state, 1, 1);
}
void reference(lua_State* state, const Record& row) {
    lua_newtable(state);
    wide(state, "sequence", row.sequence); wide(state, "cycle", row.cycle);
    wide(state, "request", row.request); wide(state, "related", row.related);
    lua_newtable(state); string(state, "file", "payload.bin");
    number(state, "offset", row.offset); number(state, "bytes", row.length); lua_setfield(state, -2, "payload");
}
void pushWorkerStatus(lua_State* state, const CD::WorkerStatus& status) {
    lua_newtable(state); wide(state, "epoch", status.epoch); wide(state, "begin_batch", status.beginBatch); wide(state, "end_batch", status.endBatch);
    number(state, "version", status.version); number(state, "state", status.state); number(state, "events", status.events);
    number(state, "limit", status.limit); number(state, "failures", status.failures); number(state, "dropped", status.dropped);
}
void pushDecoded(lua_State* state, const FrozenCapture& capture, const Correlations& result) {
    lua_newtable(state); boolean(state, "enabled", result.enabled);
    array(state);
    if (result.enabled) for (const auto& row : capture.worker.records) {
        lua_newtable(state); number(state, "sequence", row.sequence); wide(state, "epoch", row.epoch); wide(state, "batch", row.batch);
        wide(state, "sample_ordinal", row.sampleOrdinal); wide(state, "last_published_cpu_cycle", row.cpuPublicationCycle);
        wide(state, "last_published_feed", row.relatedFeed); number(state, "kind", row.kind); number(state, "flags", row.flags);
        if (row.kind == 2) boolean(state, "condition_evaluated", row.flags == 1);
        if (row.kind == 4) boolean(state, "address_conditions_evaluated", row.data[2] & 64);
        array(state);
        for (uint32_t n = 0; n < row.data.size(); ++n) { lua_pushnumber(state, row.data[n]); lua_rawseti(state, -2, n + 1); }
        lua_setfield(state, -2, "raw"); lua_rawseti(state, -2, row.sequence);
    }
    lua_setfield(state, -2, "records");
    array(state);
    for (uint32_t n = 0; n < result.handoffs.size(); ++n) {
        const auto& handoff = result.handoffs[n]; reference(state, capture.history.records[handoff.record]);
        wide(state, "epoch", handoff.epoch); wide(state, "first", handoff.first); wide(state, "last", handoff.last);
        number(state, "count", handoff.count); number(state, "flags", handoff.flags); lua_rawseti(state, -2, n + 1);
    }
    lua_setfield(state, -2, "handoffs");
    array(state);
    for (uint32_t n = 0; n < result.unconsumed.size(); ++n) { lua_pushnumber(state, result.unconsumed[n]); lua_rawseti(state, -2, n + 1); }
    lua_setfield(state, -2, "unconsumed_publications");
    if (result.enabled) {
        pushWorkerStatus(state, result.final); lua_setfield(state, -2, "status");
        string(state, "scope", "separate native worker chronology, qualified predicates and atomic CPU handoffs; published CPU cycle is not worker execution time; no DSP or deadlock/performance claim");
    }
}

int correlations(lua_State* state) {
    if (lua_gettop(state) != 1) return luaL_error(state, "correlateCD requires one owning History capture");
    luaL_checkstack(state, 12, "history correlation requires Lua stack space");
    auto& capture = frozen(state);
    const char* error = nullptr;
    try { if (!capture.correlations) capture.correlations = correlate(capture); }
    catch (const char* message) { error = message; }
    catch (...) { error = "history correlation allocation failed"; }
    if (error) return luaL_error(state, "%s", error);
    const auto& result = *capture.correlations;
    lua_newtable(state); pushDecoded(state, capture, result); lua_setfield(state, -2, "decoded");
    lua_newtable(state);
    for (const auto& [sequence, event] : result.feeds) { wide(state, sequence); wide(state, event); lua_settable(state, -3); }
    lua_setfield(state, -2, "feeds");
    return 1;
}
int statusBytes(lua_State* state) {
    if (lua_gettop(state) != 1) return luaL_error(state, "statusBytes requires one History capture");
    const auto& value = frozen(state).history.status;
    lua_pushlstring(state, reinterpret_cast<const char*>(&value), sizeof(value)); return 1;
}
int recordBytes(lua_State* state) {
    if (lua_gettop(state) != 1) return luaL_error(state, "recordBytes requires one History capture");
    const auto& value = frozen(state).history.records;
    lua_pushlstring(state, value.empty() ? "" : reinterpret_cast<const char*>(value.data()), value.size() * sizeof(Record)); return 1;
}
int payloadBytes(lua_State* state) {
    if (lua_gettop(state) != 1) return luaL_error(state, "payload requires one History capture");
    const auto& value = frozen(state).history.payload;
    lua_pushlstring(state, value.empty() ? "" : reinterpret_cast<const char*>(value.data()), value.size()); return 1;
}
uint32_t integer(lua_State* state, int index, uint32_t maximum) {
    if (lua_type(state, index) != LUA_TNUMBER) luaL_error(state, "history range must be numeric");
    const auto value = lua_tonumber(state, index);
    if (!std::isfinite(value) || value < 0 || value > maximum || std::floor(value) != value)
        luaL_error(state, "history range must be an integer within its bound");
    return uint32_t(value);
}
int slice(lua_State* state) {
    if (lua_gettop(state) != 3) return luaL_error(state, "slice requires History capture, offset and length");
    const auto& value = frozen(state).history.payload;
    const auto offset = integer(state, 2, 8388608), length = integer(state, 3, 8388608);
    if (offset > value.size() || length > value.size() - offset) return luaL_error(state, "history slice exceeds frozen payload");
    lua_pushlstring(state, length ? reinterpret_cast<const char*>(value.data() + offset) : "", length); return 1;
}
int snapshot(lua_State* state) {
    if (lua_gettop(state)) return luaL_error(state, "History.snapshot requires no arguments");
    luaL_checkstack(state, 8, "history snapshot requires Lua stack space");
    auto* capture = static_cast<CaptureOwner*>(lua_newuserdata(state, sizeof(CaptureOwner)));
    capture->value = nullptr;
    luaL_getmetatable(state, metatable); lua_setmetatable(state, -2);
    const char* error = nullptr;
    try {
        capture->value = new FrozenCapture;
        require(History::snapshot(capture->value->history), "History.snapshot requires a paused owner and frozen settled journal");
        require(CD::workerSnapshot(capture->value->worker), "History.snapshot requires a frozen worker lane");
        require(capture->value->worker.status.epoch == capture->value->history.epoch,
                "History.snapshot worker epoch differs from journal");
        for (const auto& row : capture->value->history.records) {
            if (row.kind != uint32_t(Kind::CdContext) || row.device != 0) continue;
            const auto* bytes = payload(*capture->value, row, 384);
            const uint64_t audioEpoch = little(bytes + 24, 8);
            if (audioEpoch) {
                require(g_emulator->m_spu->captureSnapshot(capture->value->audio),
                        "History.snapshot requires frozen Audio and stream exclusion");
                require(capture->value->audio.epoch == audioEpoch, "History.snapshot Audio epoch differs from journal");
            }
            break;
        }
    } catch (const char* message) { error = message; }
    catch (...) { error = "History.snapshot allocation failed"; }
    if (error) { delete capture->value; capture->value = nullptr; return luaL_error(state, "%s", error); }
    return 1;
}
}

void PCSX::LuaFFI::openHistoryCapture(Lua L) {
    const auto state = L.getState();
    if (luaL_newmetatable(state, metatable)) {
        lua_pushcfunction(state, destroy); lua_setfield(state, -2, "__gc");
        lua_pushvalue(state, -1); lua_setfield(state, -2, "__index");
        lua_pushliteral(state, "owning immutable History capture"); lua_setfield(state, -2, "__metatable");
        for (const auto& [name, function] : std::array<std::pair<const char*, lua_CFunction>, 5>{{
                 {"statusBytes", statusBytes}, {"recordBytes", recordBytes}, {"payload", payloadBytes},
                 {"slice", slice}, {"correlateCD", correlations}}}) {
            lua_pushcfunction(state, function); lua_setfield(state, -2, name);
        }
    }
    lua_pop(state, 1);
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX); L.getfieldtable("History");
    L.declareFunc("snapshot", snapshot, -1); L.declareFunc("correlateCD", correlations, -1);
    L.pop(2);
}
