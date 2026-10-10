/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/memorycardlua.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <optional>

#include "core/memorycard.h"
#include "core/psxemulator.h"
#include "core/sio.h"
#include "core/sstate.h"

namespace {
using Image = PCSX::MemoryCardImage;
using Result = PCSX::SaveStates::Result;
constexpr const char* CardMetatable = "PCSX.MemoryCardImage";

struct CardStatus {
    unsigned slot = 0;
    bool inserted = false, pocketstation = false, dirty = false, active = false;
    uint32_t status = 0, mode = 0, control = 0, baud = 0, data = 0, device = 0, bufferIndex = 0;
    uint32_t command = 0, ticks = 0, sector = 0, offset = 0, directoryFlag = 0, reply = 0, directoryIndex = 0;
    uint32_t checksumIn = 0, checksumOut = 0;
    uint64_t cycle = 0;
    std::string rxFIFO;
};

struct CardOutput {
    std::optional<Image> image;
    std::optional<Image::Directory> inventory;
    std::optional<Image::File> file;
    CardStatus status;
    char error[256]{};
    unsigned destinationBlock = 0;
};

struct CardHolder { CardOutput* value; };

void setError(CardOutput& output, const char* message) noexcept {
    std::strncpy(output.error, message, sizeof(output.error) - 1);
    output.error[sizeof(output.error) - 1] = 0;
}

template <typename Callback>
Result checkedCardState(Callback&& callback) noexcept {
    try {
        return PCSX::SaveStates::withCheckedState(std::forward<Callback>(callback));
    } catch (const std::bad_alloc&) {
        return Result::AllocationFailed;
    } catch (...) {
        return Result::InvalidState;
    }
}

int collectCard(lua_State* state) {
    auto* holder = static_cast<CardHolder*>(lua_touserdata(state, 1));
    delete holder->value;
    holder->value = nullptr;
    return 0;
}

CardOutput* newOutput(lua_State* state) {
    auto* holder = static_cast<CardHolder*>(lua_newuserdata(state, sizeof(CardHolder)));
    holder->value = nullptr;
    luaL_getmetatable(state, CardMetatable);
    lua_setmetatable(state, -2);
    try {
        holder->value = new CardOutput;
    } catch (...) {
    }
    if (!holder->value) luaL_error(state, "could not allocate memory card output");
    return holder->value;
}

unsigned integer(lua_State* state, int index, unsigned minimum, unsigned maximum, const char* label) {
    if (lua_type(state, index) != LUA_TNUMBER) luaL_error(state, "%s must be an integer", label);
    const double value = lua_tonumber(state, index);
    if (!std::isfinite(value) || value != std::floor(value) || value < minimum || value > maximum)
        luaL_error(state, "%s is out of range", label);
    return value;
}

void argumentCount(lua_State* state, unsigned minimum, unsigned maximum) {
    const auto count = lua_gettop(state);
    if (count < minimum || count > maximum) luaL_error(state, "invalid memory card argument count");
}

struct CardInput {
    unsigned slot = 0;
    const Image* snapshot = nullptr;
    std::string_view bytes;
};

CardInput input(lua_State* state, int index) {
    CardInput result;
    if (lua_type(state, index) == LUA_TNUMBER) {
        result.slot = integer(state, index, 1, 2, "memory card slot");
    } else if (lua_type(state, index) == LUA_TSTRING) {
        size_t size = 0;
        const auto* bytes = lua_tolstring(state, index, &size);
        if (size != Image::Size) luaL_error(state, "raw memory card requires exactly 131072 bytes");
        result.bytes = std::string_view(bytes, size);
    } else {
        auto* holder = static_cast<CardHolder*>(luaL_checkudata(state, index, CardMetatable));
        if (!holder->value || !holder->value->image) luaL_error(state, "memory card snapshot is unavailable");
        result.snapshot = &*holder->value->image;
    }
    return result;
}

bool booleanOption(lua_State* state, int index, const char* name, bool fallback) {
    if (lua_isnoneornil(state, index)) return fallback;
    if (lua_type(state, index) != LUA_TTABLE) luaL_error(state, "memory card options must be a table");
    lua_getfield(state, index, name);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return fallback;
    }
    if (lua_type(state, -1) != LUA_TBOOLEAN) luaL_error(state, "%s must be a boolean", name);
    const bool value = lua_toboolean(state, -1);
    lua_pop(state, 1);
    return value;
}

std::string_view stringArgument(lua_State* state, int index, const char* label) {
    if (lua_type(state, index) != LUA_TSTRING) luaL_error(state, "%s must be a string", label);
    size_t size = 0;
    const auto* bytes = lua_tolstring(state, index, &size);
    return std::string_view(bytes, size);
}

std::string_view formatOption(lua_State* state, int index) {
    if (lua_isnoneornil(state, index)) return "raw";
    if (lua_type(state, index) != LUA_TTABLE) luaL_error(state, "memory card options must be a table");
    lua_getfield(state, index, "format");
    const auto format = lua_isnil(state, -1) ? std::string_view("raw") : stringArgument(state, -1, "format");
    return format;
}

std::string_view cardBytes(const PCSX::SaveStates::SaveState& state, unsigned slot) {
    using namespace PCSX::SaveStates;
    const auto& sio = state.get<SIOField>();
    return slot == 1 ? std::string_view(sio.get<SIOMCD1Data>().value) : std::string_view(sio.get<SIOMCD2Data>().value);
}

CardStatus cardStatus(const PCSX::SaveStates::SaveState& state, unsigned slot) {
    using namespace PCSX::SaveStates;
    CardStatus result;
    result.slot = slot;
    const auto& sio = state.get<SIOField>();
    result.inserted = ((sio.get<SIOInsertedMaskPlusOne>().value - 1) & (1u << (slot - 1))) != 0;
    result.pocketstation = ((sio.get<SIOPocketstationMaskPlusOne>().value - 1) & (1u << (slot - 1))) != 0;
    result.dirty = PCSX::g_emulator->m_sio->cardDirty(slot);
    result.active = PCSX::g_emulator->m_sio->cardTransactionActive();
    result.status = sio.get<SIOStatusReg>().decodedValue();
    result.mode = sio.get<SIOModeReg>().decodedValue();
    result.control = sio.get<SIOCtrlReg>().decodedValue();
    result.baud = sio.get<SIOBaudReg>().decodedValue();
    result.data = sio.get<SIODataReg>().value;
    result.device = sio.get<SIOCurrentDevice>().decodedValue();
    result.bufferIndex = sio.get<SIOBufferIndex>().decodedValue();
    result.rxFIFO = sio.get<SIORxFIFO>().value;
    result.cycle = state.get<RegistersField>().get<Cycle>().decodedValue();
    if (slot == 1) {
        result.command = sio.get<SIOMCD1CurrentCommand>().decodedValue();
        result.ticks = sio.get<SIOMCD1CommandTicks>().decodedValue();
        result.sector = sio.get<SIOMCD1Sector>().decodedValue();
        result.offset = sio.get<SIOMCD1DataOffset>().decodedValue();
        result.directoryFlag = sio.get<SIOMCD1DirectoryFlag>().decodedValue();
        result.reply = sio.get<SIOMCD1SPDR>().value;
        result.directoryIndex = sio.get<SIOMCD1DirectoryIndex>().value;
        result.checksumIn = sio.get<SIOMCD1ChecksumIn>().decodedValue();
        result.checksumOut = sio.get<SIOMCD1ChecksumOut>().decodedValue();
    } else {
        result.command = sio.get<SIOMCD2CurrentCommand>().decodedValue();
        result.ticks = sio.get<SIOMCD2CommandTicks>().decodedValue();
        result.sector = sio.get<SIOMCD2Sector>().decodedValue();
        result.offset = sio.get<SIOMCD2DataOffset>().decodedValue();
        result.directoryFlag = sio.get<SIOMCD2DirectoryFlag>().decodedValue();
        result.reply = sio.get<SIOMCD2SPDR>().value;
        result.directoryIndex = sio.get<SIOMCD2DirectoryIndex>().value;
        result.checksumIn = sio.get<SIOMCD2ChecksumIn>().decodedValue();
        result.checksumOut = sio.get<SIOMCD2ChecksumOut>().decodedValue();
    }
    return result;
}

Result acquire(const CardInput& source, CardOutput& output) {
    using namespace PCSX::SaveStates;
    if (source.slot) {
        return checkedCardState([&](const SaveState& state) {
            output.image.emplace(cardBytes(state, source.slot));
            output.status = cardStatus(state, source.slot);
            return Result::Ok;
        });
    }
    output.image.emplace(source.bytes);
    return Result::Ok;
}

void number(lua_State* state, const char* key, unsigned value) {
    lua_pushnumber(state, value);
    lua_setfield(state, -2, key);
}

void boolean(lua_State* state, const char* key, bool value) {
    lua_pushboolean(state, value);
    lua_setfield(state, -2, key);
}

void bytes(lua_State* state, const char* key, std::string_view value) {
    lua_pushlstring(state, value.data(), value.size());
    lua_setfield(state, -2, key);
}

void diagnostics(lua_State* state, const std::vector<std::string>& messages) {
    lua_createtable(state, messages.size(), 0);
    for (size_t i = 0; i < messages.size(); ++i) {
        lua_pushlstring(state, messages[i].data(), messages[i].size());
        lua_rawseti(state, -2, i + 1);
    }
    lua_setfield(state, -2, "diagnostics");
}

void pushBlock(lua_State* state, const Image::Block& block, bool icons) {
    lua_createtable(state, 0, 14);
    number(state, "block", block.number);
    number(state, "allocation", block.allocState);
    number(state, "size", block.fileSize);
    lua_pushnumber(state, block.nextBlock);
    lua_setfield(state, -2, "next_block");
    boolean(state, "checksum_valid", block.checksumValid);
    boolean(state, "free", block.isErased());
    bytes(state, "filename", block.filename);
    bytes(state, "id", block.id);
    bytes(state, "name", block.name);
    bytes(state, "title", block.titleUtf8);
    bytes(state, "title_ascii", block.titleAscii);
    bytes(state, "title_sjis", block.titleSjis);
    number(state, "icon_count", block.iconCount);
    lua_createtable(state, block.chain.size(), 0);
    for (size_t i = 0; i < block.chain.size(); ++i) {
        lua_pushnumber(state, block.chain[i]);
        lua_rawseti(state, -2, i + 1);
    }
    lua_setfield(state, -2, "chain");
    if (icons) {
        lua_createtable(state, block.iconCount, 0);
        for (unsigned frame = 0; frame < block.iconCount; ++frame) {
            char pixels[512];
            for (unsigned i = 0; i < 256; ++i) {
                pixels[i * 2] = block.icon[frame * 256 + i] & 255;
                pixels[i * 2 + 1] = block.icon[frame * 256 + i] >> 8;
            }
            lua_pushlstring(state, pixels, sizeof(pixels));
            lua_rawseti(state, -2, frame + 1);
        }
        lua_setfield(state, -2, "icons_bgr555le");
    }
    diagnostics(state, block.diagnostics);
}

void pushStatus(lua_State* state, const CardStatus& status) {
    lua_createtable(state, 0, 7);
    number(state, "slot", status.slot);
    boolean(state, "inserted", status.inserted);
    boolean(state, "pocketstation", status.pocketstation);
    boolean(state, "dirty", status.dirty);
    boolean(state, "transaction_active", status.active);
    char cycle[20];
    const auto decimal = std::to_chars(std::begin(cycle), std::end(cycle), status.cycle);
    bytes(state, "cycles", std::string_view(cycle, decimal.ptr - cycle));
    lua_createtable(state, 0, 19);
    number(state, "status", status.status);
    number(state, "mode", status.mode);
    number(state, "control", status.control);
    number(state, "baud", status.baud);
    number(state, "data", status.data);
    number(state, "device", status.device);
    number(state, "buffer_index", status.bufferIndex);
    number(state, "command", status.command);
    number(state, "command_ticks", status.ticks);
    number(state, "sector", status.sector);
    number(state, "data_offset", status.offset);
    number(state, "directory_flag", status.directoryFlag);
    number(state, "reply", status.reply);
    number(state, "directory_index", status.directoryIndex);
    number(state, "checksum_in", status.checksumIn);
    number(state, "checksum_out", status.checksumOut);
    bytes(state, "rx_fifo", status.rxFIFO);
    lua_setfield(state, -2, "sio");
}

int checkedResult(lua_State* state, Result result, const CardOutput& output) {
    lua_pushnumber(state, static_cast<unsigned>(result));
    if (result == Result::Ok) return 1;
    if (output.error[0] == 0) return 1;
    lua_pushlstring(state, output.error, std::strlen(output.error));
    return 2;
}

int snapshotBytes(lua_State* state) {
    argumentCount(state, 1, 1);
    const auto source = input(state, 1);
    if (!source.snapshot) return luaL_error(state, "snapshot bytes requires an owning memory card image");
    const auto raw = source.snapshot->bytes();
    lua_pushlstring(state, raw.data(), raw.size());
    return 1;
}

int captureChecked(lua_State* state) {
    argumentCount(state, 1, 3);
    const unsigned slot = integer(state, 1, 1, 2, "memory card slot");
    const unsigned offset = lua_isnoneornil(state, 2) ? 0 : integer(state, 2, 0, Image::Size, "card offset");
    const unsigned length = lua_isnoneornil(state, 3) ? Image::Size - offset
                                                    : integer(state, 3, 0, Image::Size - offset, "card length");
    auto* output = newOutput(state);
    const int owner = lua_gettop(state);
    const auto result = acquire(CardInput{.slot = slot}, *output);
    if (result != Result::Ok) return checkedResult(state, result, *output);
    lua_pushnumber(state, static_cast<unsigned>(result));
    lua_createtable(state, 0, 5);
    lua_pushvalue(state, owner);
    lua_setfield(state, -2, "image");
    bytes(state, "bytes", output->image->bytes().substr(offset, length));
    number(state, "offset", offset);
    number(state, "length", length);
    pushStatus(state, output->status);
    lua_setfield(state, -2, "status");
    return 2;
}

int directory(lua_State* state) {
    argumentCount(state, 1, 2);
    const auto source = input(state, 1);
    const bool icons = booleanOption(state, 2, "icons", false);
    auto* output = newOutput(state);
    Result result = Result::Ok;
    try {
        if (source.snapshot) output->inventory = source.snapshot->directory(icons);
        else {
            result = acquire(source, *output);
            if (result == Result::Ok) output->inventory = output->image->directory(icons);
        }
    } catch (const std::exception& error) {
        setError(*output, error.what());
    }
    if (output->error[0] != 0) return luaL_error(state, "%s", output->error);
    if (result != Result::Ok) return luaL_error(state, "checked memory card capture failed (%u)", unsigned(result));
    lua_createtable(state, 0, 6);
    number(state, "free_blocks", output->inventory->freeBlocks);
    number(state, "free_bytes", output->inventory->freeBlocks * Image::BlockSize);
    number(state, "files", output->inventory->files);
    diagnostics(state, output->inventory->diagnostics);
    lua_createtable(state, 15, 0);
    for (unsigned i = 0; i < 15; ++i) {
        pushBlock(state, output->inventory->blocks[i], icons);
        lua_rawseti(state, -2, i + 1);
    }
    lua_setfield(state, -2, "blocks");
    if (source.slot) {
        pushStatus(state, output->status);
        lua_setfield(state, -2, "status");
    }
    return 1;
}

int readFile(lua_State* state) {
    argumentCount(state, 2, 2);
    const auto source = input(state, 1);
    const unsigned block = integer(state, 2, 1, 15, "first block");
    auto* output = newOutput(state);
    Result result = Result::Ok;
    try {
        if (source.snapshot) output->file = source.snapshot->readFile(block);
        else {
            result = acquire(source, *output);
            if (result == Result::Ok) output->file = output->image->readFile(block);
        }
    } catch (const std::exception& error) {
        setError(*output, error.what());
    }
    if (output->error[0] != 0) return luaL_error(state, "%s", output->error);
    if (result != Result::Ok) return luaL_error(state, "checked memory card capture failed (%u)", unsigned(result));
    lua_pushlstring(state, output->file->bytes.data(), output->file->bytes.size());
    pushBlock(state, output->file->metadata, false);
    return 2;
}

int editChecked(lua_State* state) {
    using namespace PCSX::SaveStates;
    argumentCount(state, 1, 1);
    if (lua_type(state, 1) != LUA_TTABLE) return luaL_error(state, "card edit requires an options table");
    lua_getfield(state, 1, "operation");
    const auto operation = stringArgument(state, -1, "operation");
    if (operation != "copy" && operation != "move" && operation != "erase")
        return luaL_error(state, "card edit operation must be copy, move, or erase");
    lua_getfield(state, 1, "source");
    const unsigned source = integer(state, -1, 1, 2, "source slot");
    lua_pop(state, 1);
    lua_getfield(state, 1, "block");
    const unsigned block = integer(state, -1, 1, 15, "first block");
    lua_pop(state, 1);
    unsigned destination = 0;
    if (operation != "erase") {
        lua_getfield(state, 1, "destination");
        destination = integer(state, -1, 1, 2, "destination slot");
        lua_pop(state, 1);
        if (destination == source) return luaL_error(state, "card copy and move require different slots");
    }
    auto* output = newOutput(state);
    const auto result = checkedCardState([&](const SaveState& checked) {
        if (PCSX::g_emulator->m_sio->cardTransactionActive()) return Result::Busy;
        try {
            const Image original(cardBytes(checked, source));
            std::optional<Image> preparedSource, preparedDestination;
            if (operation != "copy") preparedSource = original.erased(block);
            if (operation != "erase") {
                const Image target(cardBytes(checked, destination));
                preparedDestination = target.copiedFrom(original, block, output->destinationBlock);
            }
            if (PCSX::g_emulator->m_sio->cardTransactionActive()) return Result::Busy;
            if (preparedSource) PCSX::g_emulator->m_sio->commitCardImage(source, *preparedSource);
            if (preparedDestination) PCSX::g_emulator->m_sio->commitCardImage(destination, *preparedDestination);
            return Result::Ok;
        } catch (const std::bad_alloc&) {
            return Result::AllocationFailed;
        } catch (const std::exception& error) {
            setError(*output, error.what());
            return Result::InvalidMemoryCard;
        }
    });
    if (result != Result::Ok) return checkedResult(state, result, *output);
    lua_pushnumber(state, static_cast<unsigned>(result));
    lua_createtable(state, 0, 6);
    bytes(state, "operation", operation);
    number(state, "source", source);
    number(state, "block", block);
    if (destination) {
        number(state, "destination", destination);
        number(state, "destination_block", output->destinationBlock);
    }
    boolean(state, "dirty", true);
    return 2;
}

int importChecked(lua_State* state) {
    using namespace PCSX::SaveStates;
    argumentCount(state, 2, 3);
    const unsigned slot = integer(state, 1, 1, 2, "memory card slot");
    const auto source = stringArgument(state, 2, "memory card bytes");
    const auto format = lua_isnoneornil(state, 3) ? std::string_view("auto") : stringArgument(state, 3, "card format");
    if (source.size() > Image::Size + 3904) return luaL_error(state, "memory card import is too large");
    auto* output = newOutput(state);
    Result result = Result::Ok;
    try {
        output->image = Image::parse(source, format);
        output->image->validate();
        result = checkedCardState([&](const SaveState&) {
            if (PCSX::g_emulator->m_sio->cardTransactionActive()) return Result::Busy;
            PCSX::g_emulator->m_sio->commitCardImage(slot, *output->image);
            return Result::Ok;
        });
    } catch (const std::bad_alloc&) {
        result = Result::AllocationFailed;
    } catch (const std::exception& error) {
        setError(*output, error.what());
        result = Result::InvalidMemoryCard;
    }
    if (result != Result::Ok) return checkedResult(state, result, *output);
    lua_pushnumber(state, static_cast<unsigned>(result));
    lua_createtable(state, 0, 3);
    number(state, "slot", slot);
    number(state, "size", Image::Size);
    boolean(state, "dirty", true);
    return 2;
}

int exportCard(lua_State* state) {
    argumentCount(state, 2, 3);
    const auto source = input(state, 1);
    const auto path = stringArgument(state, 2, "card export path");
    const bool overwrite = booleanOption(state, 3, "overwrite", false);
    const auto format = formatOption(state, 3);
    auto* output = newOutput(state);
    Result result = Result::Ok;
    try {
        if (source.snapshot) source.snapshot->exportFile(path, overwrite, format);
        else {
            result = acquire(source, *output);
            if (result == Result::Ok) output->image->exportFile(path, overwrite, format);
        }
    } catch (const std::exception& error) {
        setError(*output, error.what());
    }
    if (output->error[0] != 0) return luaL_error(state, "%s", output->error);
    if (result != Result::Ok) return luaL_error(state, "checked memory card capture failed (%u)", unsigned(result));
    lua_createtable(state, 0, 3);
    bytes(state, "path", path);
    bytes(state, "format", format);
    number(state, "size", Image::Size);
    return 1;
}

int flushChecked(lua_State* state) {
    using namespace PCSX::SaveStates;
    argumentCount(state, 1, 1);
    const unsigned slot = integer(state, 1, 1, 2, "memory card slot");
    auto* output = newOutput(state);
    const auto result = checkedCardState([&](const SaveState&) {
        if (PCSX::g_emulator->m_sio->cardTransactionActive()) return Result::Busy;
        return PCSX::g_emulator->m_sio->flushCard(slot) ? Result::Ok : Result::CommitFailed;
    });
    if (result != Result::Ok) return checkedResult(state, result, *output);
    lua_pushnumber(state, static_cast<unsigned>(result));
    lua_createtable(state, 0, 2);
    number(state, "slot", slot);
    boolean(state, "dirty", false);
    return 2;
}
}

void PCSX::LuaFFI::openMemoryCards(PCSX::Lua lua) {
    auto* state = lua.getState();
    luaL_newmetatable(state, CardMetatable);
    lua_pushcfunction(state, collectCard);
    lua_setfield(state, -2, "__gc");
    lua_pushliteral(state, "owning memory card image");
    lua_setfield(state, -2, "__metatable");
    lua_newtable(state);
    lua_pushcfunction(state, snapshotBytes);
    lua_setfield(state, -2, "bytes");
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);
    lua_getglobal(state, "PCSX");
    lua_newtable(state);
    const luaL_Reg functions[] = {{"captureChecked", captureChecked}, {"directory", directory},
                                 {"readFile", readFile}, {"editChecked", editChecked},
                                 {"importChecked", importChecked}, {"export", exportCard},
                                 {"flushChecked", flushChecked}, {nullptr, nullptr}};
    luaL_register(state, nullptr, functions);
    lua_setfield(state, -2, "MemoryCards");
    lua_pop(state, 1);
}
