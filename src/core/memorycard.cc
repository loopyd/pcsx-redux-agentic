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

#include "core/memorycard.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <stdexcept>

#include "core/sio.h"
#include "mips-common/util/sjis-fullwidth-ascii.hh"
#include "support/sjis_conv.h"

namespace {
uint32_t cardWord(const uint8_t* bytes, unsigned count) {
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint32_t(bytes[i]) << (i * 8);
    return value;
}

bool cardChecksum(const uint8_t* frame) {
    uint8_t checksum = 0;
    for (unsigned i = 0; i < 127; ++i) checksum ^= frame[i];
    return checksum == frame[127];
}

void fixCardChecksum(uint8_t* frame) {
    frame[127] = 0;
    for (unsigned i = 0; i < 127; ++i) frame[127] ^= frame[i];
}

std::string cardString(const uint8_t* bytes, size_t limit) {
    size_t size = 0;
    while (size < limit && bytes[size]) ++size;
    return std::string(reinterpret_cast<const char*>(bytes), size);
}
}

PCSX::MemoryCardImage::MemoryCardImage(std::string_view raw) {
    if (raw.size() != Size) throw std::runtime_error("memory card requires exactly 131072 bytes");
    m_bytes.assign(raw);
}

PCSX::MemoryCardImage PCSX::MemoryCardImage::parse(std::string_view bytes, std::string_view format) {
    size_t header = 0;
    if (format == "auto") {
        if (bytes.size() == Size) format = "raw";
        else if (bytes.size() == Size + 64 && bytes.substr(0, 4) == "VgsM") format = "vgs";
        else if (bytes.size() == Size + 3904 && bytes.substr(0, 11) == "123-456-STD") format = "dexdrive";
        else throw std::runtime_error("unrecognized memory card image format");
    }
    if (format == "vgs") {
        header = 64;
        if (bytes.substr(0, 4) != "VgsM") throw std::runtime_error("invalid VGS card header");
    } else if (format == "dexdrive") {
        header = 3904;
        if (bytes.substr(0, 11) != "123-456-STD") throw std::runtime_error("invalid DexDrive card header");
    } else if (format != "raw") {
        throw std::runtime_error("memory card format must be auto, raw, vgs, or dexdrive");
    }
    if (bytes.size() != Size + header) throw std::runtime_error("invalid memory card image length");
    return MemoryCardImage(bytes.substr(header));
}

PCSX::MemoryCardImage::Block PCSX::MemoryCardImage::inspectBlock(std::string_view raw, unsigned block, bool icons,
                                                             bool title) {
    if (raw.size() != Size || block < 1 || block > 15) throw std::runtime_error("invalid memory card block");
    Block info;
    info.number = block;
    const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
    const auto* frame = data + block * SectorSize;
    info.allocState = cardWord(frame, 4);
    info.fileSize = cardWord(frame + 4, 4);
    const auto next = cardWord(frame + 8, 2);
    info.nextBlock = next == 0xffff ? -1 : next + 1;
    info.filename = cardString(frame + 10, 20);
    info.id = cardString(frame + 10, 12);
    info.name = cardString(frame + 22, 16);
    info.checksumValid = cardChecksum(frame);
    if (!info.checksumValid) info.diagnostics.emplace_back("directory checksum mismatch");
    if (info.allocState != 0x51 && !info.isChained() && !info.isErased())
        info.diagnostics.emplace_back("invalid allocation state");
    if (next != 0xffff && next >= 15) info.diagnostics.emplace_back("invalid next block");
    if (info.isErased()) {
        if (title) info.titleAscii = info.titleSjis = info.titleUtf8 = "Free Block";
        return info;
    }
    if (info.allocState != 0x51 || (!title && !icons)) return info;
    const auto* header = data + block * BlockSize;
    if (header[0] != 'S' || header[1] != 'C') info.diagnostics.emplace_back("save header signature mismatch");
    info.iconCount = header[2] >= 0x11 && header[2] <= 0x13 ? header[2] & 3 : 0;
    if (title) {
        info.titleSjis = cardString(header + 4, 64);
        info.titleUtf8 = Sjis::toUtf8(info.titleSjis);
        for (size_t i = 0; i < info.titleSjis.size(); ++i) {
            uint16_t c = uint8_t(info.titleSjis[i]);
            if (c >= 0x80 && i + 1 < info.titleSjis.size()) c = (c << 8) | uint8_t(info.titleSjis[++i]);
            bool found = false;
            for (size_t s = 0; s < std::size(::Sjis::c_fullwidthAsciiToSjis); ++s) {
                if (c == ::Sjis::c_fullwidthAsciiToSjis[s]) {
                    info.titleAscii += char(s + 0x20);
                    found = true;
                    break;
                }
            }
            if (!found) info.titleAscii += c >= 0x20 && c < 0x7f ? char(c) : '?';
        }
    }
    if (icons) {
        for (unsigned i = 0; i < info.iconCount * 256; ++i) {
            const auto packed = header[128 + i / 2];
            const auto color = (packed >> ((i & 1) * 4)) & 15;
            info.icon[i] = cardWord(header + 0x60 + color * 2, 2) | 0x8000;
        }
    }
    return info;
}

std::vector<unsigned> PCSX::MemoryCardImage::fileChain(std::string_view raw, unsigned firstBlock) {
    if (raw.size() != Size) throw std::runtime_error("invalid memory card image length");
    const auto* header = reinterpret_cast<const uint8_t*>(raw.data());
    if (header[0] != 'M' || header[1] != 'C' || !cardChecksum(header))
        throw std::runtime_error("invalid memory card header");
    std::vector<unsigned> blocks;
    std::bitset<16> visited;
    unsigned block = firstBlock;
    uint32_t size = 0;
    while (true) {
        if (block < 1 || block > 15 || visited[block]) throw std::runtime_error("invalid or cyclic memory card chain");
        visited[block] = true;
        const auto info = inspectBlock(raw, block, false, false);
        if (!info.checksumValid) throw std::runtime_error("memory card directory checksum mismatch");
        if (blocks.empty()) {
            if (info.allocState != 0x51) throw std::runtime_error("memory card file requires a first block");
            size = info.fileSize;
        } else if (info.allocState != (info.nextBlock == -1 ? 0x53u : 0x52u)) {
            throw std::runtime_error("invalid chained memory card allocation state");
        }
        blocks.push_back(block);
        if (info.nextBlock == -1) break;
        block = info.nextBlock;
    }
    if (!size || size > blocks.size() * BlockSize || size <= (blocks.size() - 1) * BlockSize)
        throw std::runtime_error("memory card declared length does not match its chain");
    for (unsigned i = 1; i <= 15; ++i) {
        if (visited[i]) continue;
        const auto info = inspectBlock(raw, i, false, false);
        if ((info.allocState == 0x51 || info.isChained()) && info.nextBlock > 0 && info.nextBlock <= 15 &&
            visited[info.nextBlock]) throw std::runtime_error("memory card file has a shared chain");
    }
    return blocks;
}

PCSX::MemoryCardImage::Directory PCSX::MemoryCardImage::directory(bool icons, bool titles) const {
    Directory result;
    const auto* header = reinterpret_cast<const uint8_t*>(m_bytes.data());
    if (header[0] != 'M' || header[1] != 'C') result.diagnostics.emplace_back("card header signature mismatch");
    if (!cardChecksum(header)) result.diagnostics.emplace_back("card header checksum mismatch");
    std::bitset<16> allocated;
    for (unsigned i = 1; i <= 15; ++i) {
        auto& info = result.blocks[i - 1];
        info = inspectBlock(m_bytes, i, icons, titles);
        if (info.isErased() && info.checksumValid) ++result.freeBlocks;
        if (info.allocState != 0x51) continue;
        ++result.files;
        try {
            info.chain = chain(i);
            for (auto block : info.chain) allocated[block] = true;
        } catch (const std::exception& error) {
            info.diagnostics.emplace_back(error.what());
        }
    }
    for (auto& info : result.blocks) {
        if (info.isChained() && !allocated[info.number]) info.diagnostics.emplace_back("orphaned continuation block");
    }
    return result;
}

PCSX::MemoryCardImage::File PCSX::MemoryCardImage::readFile(unsigned firstBlock) const {
    File result;
    result.metadata = inspectBlock(m_bytes, firstBlock);
    result.metadata.chain = chain(firstBlock);
    result.bytes.reserve(result.metadata.fileSize);
    for (auto block : result.metadata.chain) {
        const auto count = std::min(BlockSize, result.metadata.fileSize - result.bytes.size());
        result.bytes.append(m_bytes.data() + block * BlockSize, count);
    }
    return result;
}

void PCSX::MemoryCardImage::validate() const {
    const auto inventory = directory(false, false);
    if (!inventory.diagnostics.empty()) throw std::runtime_error(inventory.diagnostics.front());
    for (const auto& block : inventory.blocks) {
        if (!block.diagnostics.empty()) throw std::runtime_error(block.diagnostics.front());
    }
}

PCSX::MemoryCardImage PCSX::MemoryCardImage::erased(unsigned firstBlock) const {
    validate();
    const auto blocks = chain(firstBlock);
    MemoryCardImage result(*this);
    for (auto block : blocks) {
        auto* frame = reinterpret_cast<uint8_t*>(result.m_bytes.data()) + block * SectorSize;
        std::memset(result.m_bytes.data() + block * BlockSize, 0, BlockSize);
        std::memset(frame, 0, SectorSize);
        frame[0] = 0xa0;
        frame[8] = frame[9] = 0xff;
        fixCardChecksum(frame);
    }
    return result;
}

PCSX::MemoryCardImage PCSX::MemoryCardImage::copiedFrom(const MemoryCardImage& source, unsigned firstBlock,
                                                     unsigned& destinationBlock) const {
    validate();
    source.validate();
    const auto sourceBlocks = source.chain(firstBlock);
    std::vector<unsigned> destinationBlocks;
    for (unsigned i = 1; i <= 15 && destinationBlocks.size() < sourceBlocks.size(); ++i) {
        if (inspectBlock(m_bytes, i, false, false).isErased()) destinationBlocks.push_back(i);
    }
    if (destinationBlocks.size() != sourceBlocks.size()) throw std::runtime_error("not enough free memory card blocks");
    const auto filename = inspectBlock(source.m_bytes, firstBlock, false, false).filename;
    for (unsigned i = 1; i <= 15; ++i) {
        const auto info = inspectBlock(m_bytes, i, false, false);
        if (info.allocState == 0x51 && info.filename == filename)
            throw std::runtime_error("destination memory card already contains this filename");
    }
    MemoryCardImage result(*this);
    for (size_t i = 0; i < sourceBlocks.size(); ++i) {
        const auto src = sourceBlocks[i], dst = destinationBlocks[i];
        std::memcpy(result.m_bytes.data() + dst * BlockSize, source.m_bytes.data() + src * BlockSize, BlockSize);
        auto* frame = reinterpret_cast<uint8_t*>(result.m_bytes.data()) + dst * SectorSize;
        std::memcpy(frame, source.m_bytes.data() + src * SectorSize, SectorSize);
        const uint16_t next = i + 1 == destinationBlocks.size() ? 0xffff : destinationBlocks[i + 1] - 1;
        frame[8] = next & 255;
        frame[9] = next >> 8;
        fixCardChecksum(frame);
    }
    result.validate();
    destinationBlock = destinationBlocks.front();
    return result;
}

void PCSX::MemoryCardImage::exportFile(std::string_view path, bool overwrite, std::string_view format) const {
    if (format != "raw") throw std::runtime_error("memory card export format must be raw");
    if (path.empty() || path.find('\0') != std::string_view::npos) throw std::runtime_error("invalid card export path");
    const std::string filename(path);
    const int fd = open(filename.c_str(), O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL), 0666);
    if (fd < 0) throw std::runtime_error("could not open memory card export");
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        throw std::runtime_error("could not open memory card export stream");
    }
    bool ok = fwrite(m_bytes.data(), 1, Size, file) == Size;
    if (fflush(file) != 0 || ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) throw std::runtime_error("memory card export write or close failed");
}

void PCSX::MemoryCard::acknowledge() { m_sio->acknowledge(); }

uint8_t PCSX::MemoryCard::transceive(uint8_t value) {
    uint8_t data_out = m_spdr;

    if (m_currentCommand == Commands::None || m_currentCommand == Commands::Access) {
        m_currentCommand = value;
    }

    switch (m_currentCommand) {
        // Access memory card device
        case Commands::Access:  // 81h
            // Incoming value is the device command
            m_spdr = m_directoryFlag;
            acknowledge();
            break;

        // Read a sector
        case Commands::Read:  // 52h
            m_spdr = tickReadCommand(value);
            break;

        // Write a sector
        case Commands::Write:  // 57h
            m_spdr = tickWriteCommand(value);
            break;

        //
        case Commands::PS_GetVersion:  // 58h
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_GetVersion(value);
            }
            break;

        //
        case Commands::PS_PrepFileExec:  // 59h
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_PrepFileExec(value);
            }
            break;

        //
        case Commands::PS_GetDirIndex:  // 5Ah
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_GetDirIndex(value);
            }
            break;

        //
        case Commands::PS_ExecCustom:  // 5Dh
            if (m_pocketstationEnabled) {
                m_spdr = tickPS_ExecCustom(value);
            }
            break;

        case Commands::GetID:  // Un-implemented, need data capture
        case Commands::Error:  // Unexpected/Unsupported command
        default:
            m_spdr = Responses::IdleHighZ;
            break;
    }

    return data_out;
}

uint8_t PCSX::MemoryCard::tickReadCommand(uint8_t value) {
    uint8_t data_out = 0xFF;

    switch (m_commandTicks) {
        case 0:
            data_out = Responses::ID1;
            break;

        case 1:
            data_out = Responses::ID2;
            break;

        case 2:
            data_out = Responses::Dummy;
            break;

        case 3:  // MSB
            m_sector = (value << 8);
            data_out = value;
            break;

        case 4:  // LSB
            // Store lower 8 bits of sector
            m_sector |= value;
            m_dataOffset = m_sector * 128;
            data_out = Responses::CommandAcknowledge1;
            break;

        case 5:  // 00h
            data_out = Responses::CommandAcknowledge2;
            break;

        case 6:  // 00h
            // Confirm MSB
            data_out = m_sector >> 8;
            break;

        case 7:  // 00h
            // Confirm LSB
            data_out = (m_sector & 0xFF);
            m_checksumOut = (m_sector >> 8) ^ (m_sector & 0xff);
            break;

        // Cases 8 through 135 overloaded to default operator below
        default:
            if (m_commandTicks >= 8 && m_commandTicks <= 135) {  // Stay here for 128 bytes
                if (m_sector >= 1024) {
                    data_out = Responses::BadSector;
                } else {
                    data_out = m_mcdData[m_dataOffset++];
                }

                m_checksumOut ^= data_out;
            } else {
                // Send this till the spooky extra bytes go away
                return Responses::CommandAcknowledge1;
            }
            break;

        case 136:
            data_out = m_checksumOut;
            break;

        case 137:
            data_out = Responses::GoodReadWrite;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickWriteCommand(uint8_t value) {
    uint8_t data_out = 0xFF;

    switch (m_commandTicks) {
            // Data is sent and received simultaneously,
            // so the data we send isn't received by the system
            // until the next bytes are exchanged. In this way,
            // you have to basically respond one byte earlier than
            // actually occurs between Slave and Master.
            // Offset "Send" bytes noted from nocash's psx specs.

        case 0:  // 57h
            data_out = Responses::ID1;
            break;

        case 1:  // 00h
            data_out = Responses::ID2;
            break;

        case 2:  // 00h
            data_out = Responses::Dummy;
            break;

        case 3:  // MSB
            // Store upper 8 bits of sector
            m_sector = (value << 8);
            // Reply with (pre)
            data_out = value;
            break;

        case 4:  // LSB
            // Store lower 8 bits of sector
            m_sector |= value;
            // m_dataOffset = (m_sector * 128);
            m_dataOffset = 0;
            m_checksumOut = (m_sector >> 8) ^ (m_sector & 0xFF);
            data_out = value;
            break;

        // Cases 5 through 135 overloaded to default operator below
        default:
            if (m_commandTicks >= 5 && m_commandTicks <= 132) {
                // Store data in temp buffer until checksum verified
                // no idea if official cards did this, but why not.
                m_tempBuffer[m_dataOffset] = value;

                // Calculate checksum
                m_checksumOut ^= value;

                // Reply with (pre)
                data_out = value;
                m_dataOffset++;
            } else {
                // Send this till the spooky extra bytes go away
                return Responses::CommandAcknowledge1;
            }
            break;

        case 133:  // CHK
            m_checksumIn = value;

            if (m_sector >= 1024) {
                m_commandTicks = 0xFF;

                // To-do: Need to log actual behavior here
                return Responses::BadSector;
            } else if (m_checksumIn != m_checksumOut) {
                // To-do: Log official card behavior
                // Below behavior is from 3rd party hardware
                m_commandTicks = 0xFF;
                return Responses::BadChecksum;
            } else {
                data_out = Responses::CommandAcknowledge1;
            }
            break;

        case 134:  // 00h
            data_out = Responses::CommandAcknowledge2;
            break;

        case 135:  // 00h
            m_directoryFlag = Flags::DirectoryRead;
            data_out = Responses::GoodReadWrite;
            memcpy(&m_mcdData[m_sector * 128], &m_tempBuffer, c_sectorSize);
            m_savedToDisk = false;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_GetDirIndex(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;
    static constexpr uint8_t response_count = 19;
    static constexpr uint8_t responses[response_count] = {0x12, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x13, 0x11, 0x4F,
                                                          0x41, 0x20, 0x01, 0x99, 0x19, 0x27, 0x30, 0x09, 0x04};

    if (m_commandTicks < response_count) {
        data_out = responses[m_commandTicks];

        // Don't ack the last byte
        if (m_commandTicks <= (response_count - 1)) {
            acknowledge();
        }
    }

    m_commandTicks++;

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_ExecCustom(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;

    switch (m_commandTicks) {
        case 0:  // 5D
            data_out = 0x03;
            break;

        default:
            data_out = 0x00;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_PrepFileExec(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;

    switch (m_commandTicks) {
        case 0:  // 59
            data_out = 0x06;
            break;

        default:
            data_out = 0x00;
            break;
    }

    m_commandTicks++;
    acknowledge();

    return data_out;
}

uint8_t PCSX::MemoryCard::tickPS_GetVersion(uint8_t value) {
    uint8_t data_out = Responses::IdleHighZ;
    static constexpr uint8_t response_count = 3;
    static constexpr uint8_t responses[response_count] = {0x02, 0x01, 0x01};

    if (m_commandTicks < response_count) {
        data_out = responses[m_commandTicks];

        // Don't ack the last byte
        if (m_commandTicks <= (response_count - 1)) {
            acknowledge();
        }
    }

    m_commandTicks++;

    return data_out;
}

void PCSX::MemoryCard::loadMcd(PCSX::u8string mcd) {
    if (std::filesystem::path(mcd).is_relative()) mcd = (g_system->getPersistentDir() / mcd).u8string();
    const char* fname = reinterpret_cast<const char*>(mcd.c_str());
    FILE* file = fopen(fname, "rb");
    if (!file && errno == ENOENT) {
        PCSX::g_system->printf("The memory card %s doesn't exist - creating it\n", fname);
        createMcd(mcd);
        file = fopen(fname, "rb");
    }
    if (!file) {
        PCSX::g_system->message("Memory card %s failed to load!\n", fname);
        return;
    }
    struct stat statbuf;
    if (fstat(fileno(file), &statbuf) != 0 || statbuf.st_size < c_cardSize || statbuf.st_size > c_cardSize + 3904) {
        fclose(file);
        throw std::runtime_error("invalid memory card file length");
    }
    std::string bytes;
    try {
        bytes.resize(statbuf.st_size);
    } catch (...) {
        fclose(file);
        throw;
    }
    bool ok = fread(bytes.data(), 1, bytes.size(), file) == bytes.size() && !ferror(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) throw std::runtime_error("memory card read or close failed");
    const auto prepared = MemoryCardImage::parse(bytes);
    std::memcpy(m_mcdData, prepared.bytes().data(), c_cardSize);
    m_directoryFlag = Flags::DirectoryUnread;
    m_savedToDisk = true;
    PCSX::g_system->printf("Loading memory card %s\n", fname);
}

bool PCSX::MemoryCard::saveMcd(PCSX::u8string mcd, const char* data, uint32_t adr, size_t size) {
    if (!data || adr > c_cardSize || size > c_cardSize - adr) return false;
    if (std::filesystem::path(mcd).is_relative()) mcd = (g_system->getPersistentDir() / mcd).u8string();
    const char* fname = reinterpret_cast<const char*>(mcd.c_str());
    FILE* file = fopen(fname, "r+b");
    bool created = false;
    if (!file && errno == ENOENT) {
        file = fopen(fname, "wb");
        created = true;
    }
    bool ok = file != nullptr;
    if (file) {
        struct stat statbuf;
        size_t header = 0;
        if (!created) {
            if (fstat(fileno(file), &statbuf) != 0) ok = false;
            else if (statbuf.st_size == c_cardSize + 64) header = 64;
            else if (statbuf.st_size == c_cardSize + 3904) header = 3904;
            else if (statbuf.st_size != c_cardSize) ok = false;
        }
        if (created) {
            ok = fwrite(data, 1, c_cardSize, file) == c_cardSize;
        } else if (ok) {
            ok = fseek(file, header + adr, SEEK_SET) == 0 && fwrite(data + adr, 1, size, file) == size;
        }
        if (fflush(file) != 0 || ferror(file)) ok = false;
        if (fclose(file) != 0) ok = false;
    }
    if (data == m_mcdData) m_savedToDisk = ok && (created || (adr == 0 && size == c_cardSize));
    if (ok) PCSX::g_system->printf("Saving memory card %s\n", fname);
    else PCSX::g_system->printf("Saving memory card %s failed; card remains dirty\n", fname);
    return ok;
}

void PCSX::MemoryCard::createMcd(PCSX::u8string mcd) {
    if (std::filesystem::path(mcd).is_relative()) mcd = (g_system->getPersistentDir() / mcd).u8string();
    std::string bytes(c_cardSize, '\0');
    auto* data = reinterpret_cast<uint8_t*>(bytes.data());
    data[0] = 'M';
    data[1] = 'C';
    fixCardChecksum(data);
    for (unsigned block = 1; block <= 15; ++block) {
        auto* frame = data + block * c_sectorSize;
        frame[0] = 0xa0;
        frame[8] = frame[9] = 0xff;
        fixCardChecksum(frame);
    }
    for (unsigned sector = 16; sector < 36; ++sector) {
        auto* frame = data + sector * c_sectorSize;
        std::memset(frame, 0xff, 4);
        frame[8] = frame[9] = 0xff;
        fixCardChecksum(frame);
    }
    const MemoryCardImage image(bytes);
    try {
        image.exportFile(std::filesystem::path(mcd).string(), true);
    } catch (const std::exception&) {
        PCSX::g_system->message("Memory card creation failed!\n");
    }
}
