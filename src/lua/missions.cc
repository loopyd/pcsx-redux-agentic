#include "lua/missions.h"

#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "core/sstate.h"
#include "json.hpp"
#include "lua/luawrapper.h"

namespace {
static int lualoader = 53;
static const std::string_view bus = (
#include "lua/missions/bus.lua"
);
static const std::string_view call = (
#include "lua/missions/call.lua"
);
static const std::string_view cdrom = (
#include "lua/missions/cdrom.lua"
);
static const std::string_view command = (
#include "lua/missions/command.lua"
);
static const std::string_view condition = (
#include "lua/missions/condition.lua"
);
static const std::string_view controller = (
#include "lua/missions/controller.lua"
);
static const std::string_view delivery = (
#include "lua/missions/delivery.lua"
);
static const std::string_view devices = (
#include "lua/missions/devices.lua"
);
static const std::string_view difference = (
#include "lua/missions/difference.lua"
);
static const std::string_view display = (
#include "lua/missions/display.lua"
);
static const std::string_view dma = (
#include "lua/missions/dma.lua"
);
static const std::string_view eligibility = (
#include "lua/missions/eligibility.lua"
);
static const std::string_view events = (
#include "lua/missions/events.lua"
);
static const std::string_view execution = (
#include "lua/missions/execution.lua"
);
static const std::string_view feeds = (
#include "lua/missions/feeds.lua"
);
static const std::string_view frames = (
#include "lua/missions/frames.lua"
);
static const std::string_view gpu = (
#include "lua/missions/gpu.lua"
);
static const std::string_view gte = (
#include "lua/missions/gte.lua"
);
static const std::string_view history = (
#include "lua/missions/history.lua"
);
static const std::string_view image = (
#include "lua/missions/image.lua"
);
static const std::string_view ingress = (
#include "lua/missions/ingress.lua"
);
static const std::string_view irq = (
#include "lua/missions/irq.lua"
);
static const std::string_view lid = (
#include "lua/missions/lid.lua"
);
static const std::string_view mdec = (
#include "lua/missions/mdec.lua"
);
static const std::string_view media = (
#include "lua/missions/media.lua"
);
static const std::string_view memory = (
#include "lua/missions/memory.lua"
);
static const std::string_view memcard = (
#include "lua/missions/memcard.lua"
);
static const std::string_view mount = (
#include "lua/missions/mount.lua"
);
static const std::string_view opcode = (
#include "lua/missions/opcode.lua"
);
static const std::string_view packets = (
#include "lua/missions/packets.lua"
);
static const std::string_view pcm = (
#include "lua/missions/pcm.lua"
);
static const std::string_view playback = (
#include "lua/missions/playback.lua"
);
static const std::string_view registers = (
#include "lua/missions/registers.lua"
);
static const std::string_view replay = (
#include "lua/missions/replay.lua"
);
static const std::string_view scheduler = (
#include "lua/missions/scheduler.lua"
);
static const std::string_view sectors = (
#include "lua/missions/sectors.lua"
);
static const std::string_view smoke = (
#include "lua/missions/smoke.lua"
);
static const std::string_view snapshot = (
#include "lua/missions/snapshot.lua"
);
static const std::string_view sound = (
#include "lua/missions/sound.lua"
);
static const std::string_view spu = (
#include "lua/missions/spu.lua"
);
static const std::string_view state = (
#include "lua/missions/state.lua"
);
static const std::string_view step = (
#include "lua/missions/step.lua"
);
static const std::string_view streams = (
#include "lua/missions/streams.lua"
);
static const std::string_view submission = (
#include "lua/missions/submission.lua"
);
static const std::string_view support = (
#include "lua/missions/support.lua"
);
static const std::string_view target = (
#include "lua/missions/target.lua"
);
static const std::string_view timeline = (
#include "lua/missions/timeline.lua"
);
static const std::string_view timers = (
#include "lua/missions/timers.lua"
);
static const std::string_view transactions = (
#include "lua/missions/transactions.lua"
);
static const std::string_view transfers = (
#include "lua/missions/transfers.lua"
);
static const std::string_view transport = (
#include "lua/missions/transport.lua"
);
static const std::string_view video = (
#include "lua/missions/video.lua"
);
static const std::string_view vram = (
#include "lua/missions/vram.lua"
);
static const std::string_view watch = (
#include "lua/missions/watch.lua"
);

const PCSX::Missions::Resource resources[] = {
    {"bus", PCSX::Missions::Role::Module, "src/lua/missions/bus.lua", bus.data(), bus.size()},
    {"call", PCSX::Missions::Role::Mission, "src/lua/missions/call.lua", call.data(), call.size()},
    {"cdrom", PCSX::Missions::Role::Mission, "src/lua/missions/cdrom.lua", cdrom.data(), cdrom.size()},
    {"command", PCSX::Missions::Role::Module, "src/lua/missions/command.lua", command.data(), command.size()},
    {"condition", PCSX::Missions::Role::Module, "src/lua/missions/condition.lua", condition.data(), condition.size()},
    {"controller", PCSX::Missions::Role::Module, "src/lua/missions/controller.lua", controller.data(), controller.size()},
    {"delivery", PCSX::Missions::Role::Module, "src/lua/missions/delivery.lua", delivery.data(), delivery.size()},
    {"devices", PCSX::Missions::Role::Module, "src/lua/missions/devices.lua", devices.data(), devices.size()},
    {"difference", PCSX::Missions::Role::Module, "src/lua/missions/difference.lua", difference.data(), difference.size()},
    {"display", PCSX::Missions::Role::Module, "src/lua/missions/display.lua", display.data(), display.size()},
    {"dma", PCSX::Missions::Role::Mission, "src/lua/missions/dma.lua", dma.data(), dma.size()},
    {"eligibility", PCSX::Missions::Role::Module, "src/lua/missions/eligibility.lua", eligibility.data(), eligibility.size()},
    {"events", PCSX::Missions::Role::Module, "src/lua/missions/events.lua", events.data(), events.size()},
    {"execution", PCSX::Missions::Role::Module, "src/lua/missions/execution.lua", execution.data(), execution.size()},
    {"feeds", PCSX::Missions::Role::Module, "src/lua/missions/feeds.lua", feeds.data(), feeds.size()},
    {"frames", PCSX::Missions::Role::Mission, "src/lua/missions/frames.lua", frames.data(), frames.size()},
    {"gpu", PCSX::Missions::Role::Mission, "src/lua/missions/gpu.lua", gpu.data(), gpu.size()},
    {"gte", PCSX::Missions::Role::Mission, "src/lua/missions/gte.lua", gte.data(), gte.size()},
    {"history", PCSX::Missions::Role::Mission, "src/lua/missions/history.lua", history.data(), history.size()},
    {"image", PCSX::Missions::Role::Module, "src/lua/missions/image.lua", image.data(), image.size()},
    {"ingress", PCSX::Missions::Role::Module, "src/lua/missions/ingress.lua", ingress.data(), ingress.size()},
    {"irq", PCSX::Missions::Role::Mission, "src/lua/missions/irq.lua", irq.data(), irq.size()},
    {"lid", PCSX::Missions::Role::Module, "src/lua/missions/lid.lua", lid.data(), lid.size()},
    {"mdec", PCSX::Missions::Role::Mission, "src/lua/missions/mdec.lua", mdec.data(), mdec.size()},
    {"media", PCSX::Missions::Role::Module, "src/lua/missions/media.lua", media.data(), media.size()},
    {"memcard", PCSX::Missions::Role::Mission, "src/lua/missions/memcard.lua", memcard.data(), memcard.size()},
    {"memory", PCSX::Missions::Role::Mission, "src/lua/missions/memory.lua", memory.data(), memory.size()},
    {"mount", PCSX::Missions::Role::Module, "src/lua/missions/mount.lua", mount.data(), mount.size()},
    {"opcode", PCSX::Missions::Role::Module, "src/lua/missions/opcode.lua", opcode.data(), opcode.size()},
    {"packets", PCSX::Missions::Role::Module, "src/lua/missions/packets.lua", packets.data(), packets.size()},
    {"pcm", PCSX::Missions::Role::Module, "src/lua/missions/pcm.lua", pcm.data(), pcm.size()},
    {"playback", PCSX::Missions::Role::Module, "src/lua/missions/playback.lua", playback.data(), playback.size()},
    {"registers", PCSX::Missions::Role::Mission, "src/lua/missions/registers.lua", registers.data(), registers.size()},
    {"replay", PCSX::Missions::Role::Mission, "src/lua/missions/replay.lua", replay.data(), replay.size()},
    {"scheduler", PCSX::Missions::Role::Module, "src/lua/missions/scheduler.lua", scheduler.data(), scheduler.size()},
    {"sectors", PCSX::Missions::Role::Module, "src/lua/missions/sectors.lua", sectors.data(), sectors.size()},
    {"smoke", PCSX::Missions::Role::Mission, "src/lua/missions/smoke.lua", smoke.data(), smoke.size()},
    {"snapshot", PCSX::Missions::Role::Module, "src/lua/missions/snapshot.lua", snapshot.data(), snapshot.size()},
    {"sound", PCSX::Missions::Role::Module, "src/lua/missions/sound.lua", sound.data(), sound.size()},
    {"spu", PCSX::Missions::Role::Mission, "src/lua/missions/spu.lua", spu.data(), spu.size()},
    {"state", PCSX::Missions::Role::Mission, "src/lua/missions/state.lua", state.data(), state.size()},
    {"step", PCSX::Missions::Role::Mission, "src/lua/missions/step.lua", step.data(), step.size()},
    {"streams", PCSX::Missions::Role::Module, "src/lua/missions/streams.lua", streams.data(), streams.size()},
    {"submission", PCSX::Missions::Role::Module, "src/lua/missions/submission.lua", submission.data(), submission.size()},
    {"support", PCSX::Missions::Role::Module, "src/lua/missions/support.lua", support.data(), support.size()},
    {"target", PCSX::Missions::Role::Mission, "src/lua/missions/target.lua", target.data(), target.size()},
    {"timeline", PCSX::Missions::Role::Module, "src/lua/missions/timeline.lua", timeline.data(), timeline.size()},
    {"timers", PCSX::Missions::Role::Mission, "src/lua/missions/timers.lua", timers.data(), timers.size()},
    {"transactions", PCSX::Missions::Role::Module, "src/lua/missions/transactions.lua", transactions.data(), transactions.size()},
    {"transfers", PCSX::Missions::Role::Module, "src/lua/missions/transfers.lua", transfers.data(), transfers.size()},
    {"transport", PCSX::Missions::Role::Module, "src/lua/missions/transport.lua", transport.data(), transport.size()},
    {"video", PCSX::Missions::Role::Module, "src/lua/missions/video.lua", video.data(), video.size()},
    {"vram", PCSX::Missions::Role::Mission, "src/lua/missions/vram.lua", vram.data(), vram.size()},
    {"watch", PCSX::Missions::Role::Mission, "src/lua/missions/watch.lua", watch.data(), watch.size()},
};
}

const PCSX::Missions::Resource* PCSX::Missions::select(std::string_view name) {
    if (name.empty() || name.size() > 64) throw std::invalid_argument("Invalid mission name");
    for (char c : name) {
        if (!(c >= 'a' && c <= 'z')) throw std::invalid_argument("Invalid mission name");
    }
    for (const auto& resource : resources) {
        if (name != resource.name) continue;
        if (resource.role != Role::Mission) throw std::invalid_argument("Requested resource is a module");
        return &resource;
    }
    throw std::invalid_argument("Unknown mission name");
}

void PCSX::Missions::catalogue(std::ostream& output) {
    auto rows = nlohmann::json::array();
    for (const auto& resource : resources) {
        rows.push_back({{"name", resource.name}, {"role", resource.role == Role::Mission ? "mission" : "module"},
                        {"path", resource.path}, {"chunk", std::string(resource.chunk, resource.length)}});
    }
    std::ostringstream schema;
    SaveStates::ProtoFile::dumpSchema(schema);
    nlohmann::json value = {
        {"schema", "psx.mission-catalog/v1"}, {"resources", rows},
        {"state", {{"version", SaveStates::Format::Version}, {"version_string", SaveStates::Format::VersionString},
                   {"schema", schema.str()}}}};
    const auto bytes = value.dump();
    if (bytes.size() > 2097152) throw std::length_error("Mission catalogue exceeds its protocol bound");
    output << bytes << '\n';
}

void PCSX::Missions::installModules(Lua L) {
    const int base = L.gettop();
    L.getfield("package", LUA_GLOBALSINDEX);
    if (!L.istable()) throw std::runtime_error("Lua package table is unavailable");
    L.getfield("loaded");
    if (!L.istable()) throw std::runtime_error("Lua loaded table is unavailable");
    const int loaded = L.gettop();
    L.getfield("preload", base + 1);
    if (!L.istable()) throw std::runtime_error("Lua preload table is unavailable");
    const int preload = L.gettop();
    for (const auto& resource : resources) {
        if (resource.role != Role::Module) continue;
        L.getfield(resource.name, loaded, true);
        if (!L.isnil()) throw std::runtime_error(std::string("Loaded module collision: ") + resource.name);
        L.pop();
        L.getfield(resource.name, preload, true);
        if (!L.isnil()) throw std::runtime_error(std::string("Preloaded module collision: ") + resource.name);
        L.pop();
    }
    for (const auto& resource : resources) {
        if (resource.role != Role::Module) continue;
        L.load(std::string_view(resource.chunk, resource.length), resource.path, false);
        L.setfield(resource.name, preload, true);
    }
    L.pop(L.gettop() - base);
}

void PCSX::Missions::run(Lua L, const Resource& selected) {
    if (selected.role != Role::Mission) throw std::invalid_argument("Requested resource is a module");
    L.load(std::string_view(selected.chunk, selected.length), selected.path);
}
