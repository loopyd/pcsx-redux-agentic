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

#include "cdrom/iso9660-reader.h"

#include "cdrom/file.h"
#include "support/strings-helpers.h"
#include "supportpsx/iso9660-lowlevel.h"

PCSX::ISO9660Reader::ISO9660Reader(std::shared_ptr<CDRIso> iso) : m_iso(iso) {
    unsigned pvdSector = 16;
    bool foundPVD = false;

    // Scan the full Volume Descriptor Set (LBA 16..terminator) to both find
    // the PVD and record where the set actually ends.
    while (pvdSector < 4096) {
        IO<File> pvdFile(new CDRIsoFile(iso, pvdSector, 2048));
        if (pvdFile->failed()) {
            m_failed = true;
            return;
        }

        uint8_t vd[7];
        if (pvdFile->readAt(vd, 7, 0) != 7) { m_failed = true; return; }
        if ((vd[1] != 'C') || (vd[2] != 'D') || (vd[3] != '0') || (vd[4] != '0') || (vd[5] != '1') || (vd[6] != 1)) {
            // Malformed descriptor set: even if we already have the PVD, the
            // set is broken so we can't trust m_vdEnd.
            m_failed = true;
            return;
        }

        if (vd[0] == 255) {
            // Terminator: the VD set ends at the sector after this one.
            m_vdEnd = pvdSector + 1;
            if (!foundPVD) m_failed = true;
            return;
        }

        if (vd[0] == 1 && !foundPVD) {
            ISO9660LowLevel::PVD pvd;
            pvd.deserialize(pvdFile);
            m_pvd = pvd;
            foundPVD = true;
        }

        pvdSector++;
    }
    m_failed = true;
}

PCSX::File *PCSX::ISO9660Reader::open(const std::string_view &filename) {
    auto entry = findEntry(filename);
    if (!entry.has_value()) return new FailedFile();

    return new CDRIsoFile(m_iso, entry.value().first.get<ISO9660LowLevel::DirEntry_LBA>(),
                          entry.value().first.get<ISO9660LowLevel::DirEntry_Size>());
}

std::optional<PCSX::ISO9660Reader::FullDirEntry> PCSX::ISO9660Reader::findEntry(const std::string_view &filename) {
    if (m_failed) return {};
    auto parts = StringsHelpers::split(filename, "/");

    FullDirEntry current;
    current.first = m_pvd.get<ISO9660LowLevel::PVD_RootDir>();

    for (auto &part : parts) {
        auto entries = listAllEntriesFrom(current.first);
        for (auto &entry : entries) {
            const auto &entryFilename = entry.first.get<ISO9660LowLevel::DirEntry_Filename>().value;
            if (entryFilename == part) {
                current = entry;
                break;
            }
        }

        if (current.first.get<ISO9660LowLevel::DirEntry_Filename>() != part) return {};
    }

    return current;
}

PCSX::ISO9660Reader::Directory PCSX::ISO9660Reader::readDirectory(
    const ISO9660LowLevel::DirEntry& dirEntry, size_t maxEntries, bool includeSentinels) {
    Directory result;
    if (m_failed) { result.error = "invalid ISO9660 reader"; return result; }
    if (!(dirEntry.get<ISO9660LowLevel::DirEntry_Flags>().value & 2)) {
        result.error = "ISO path is not a directory";
        return result;
    }
    const uint64_t size = dirEntry.get<ISO9660LowLevel::DirEntry_Size>();
    const uint64_t lba = dirEntry.get<ISO9660LowLevel::DirEntry_LBA>();
    const uint64_t volume = m_pvd.get<ISO9660LowLevel::PVD_VolumeSpaceSize>();
    if (lba > volume || size > (volume - lba) * 2048 || size > 8388608) {
        result.error = "ISO directory extent exceeds bounds";
        return result;
    }
    IO<File> dir(new CDRIsoFile(m_iso, lba, size));
    if (dir->failed()) { result.error = "cannot read ISO directory"; return result; }
    while (uint64_t(dir->rTell()) < size) {
        const auto start = dir->rTell();
        uint8_t length;
        if (dir->read(&length, 1) != 1) {
            result.error = "short ISO directory read";
            return result;
        }
        if (!length) {
            dir->rSeek(std::min<uint64_t>((uint64_t(start) / 2048 + 1) * 2048, size), SEEK_SET);
            continue;
        }
        if (length < 34 || uint64_t(start) + length > size || (start % 2048) + length > 2048) {
            result.error = "malformed ISO directory record";
            return result;
        }
        uint8_t header[33];
        if (dir->readAt(header, sizeof(header), start) != sizeof(header) ||
            header[1] != 0 || header[32] == 0 || 33u + header[32] + ((header[32] & 1) ? 0 : 1) > length) {
            result.error = "malformed ISO directory record";
            return result;
        }
        dir->rSeek(start, SEEK_SET);
        FullDirEntry entry;
        entry.first.deserialize(dir);
        const auto& name = entry.first.get<ISO9660LowLevel::DirEntry_Filename>().value;
        if ((name.size() & 1) == 0) dir->byte();
        if (length - 33 - name.size() - ((name.size() & 1) ? 0 : 1) == 14)
            entry.second.deserialize(dir);
        dir->rSeek(start + length, SEEK_SET);
        const bool sentinel = name.size() == 1 && (name[0] == '\0' || name[0] == '\1');
        if (!includeSentinels && sentinel) continue;
        if (result.entries.size() == maxEntries) {
            result.error = "ISO directory exceeds entry bound";
            return result;
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

PCSX::ISO9660Reader::Directory PCSX::ISO9660Reader::listDirectory(std::string_view path, size_t maxEntries) {
    Directory result;
    if (!maxEntries || maxEntries > 65536 || path.size() > 4096 || path.find('\0') != path.npos ||
        path.find('\\') != path.npos) {
        result.error = "invalid ISO directory path or entry bound";
        return result;
    }
    auto current = getRootDirEntry();
    size_t offset = 0;
    while (offset < path.size()) {
        if (path[offset] == '/') { ++offset; continue; }
        const size_t end = path.find('/', offset);
        const auto part = path.substr(offset, end == path.npos ? path.size() - offset : end - offset);
        if (part == "." || part == ".." || part.size() > 255) {
            result.error = "invalid ISO directory path component";
            return result;
        }
        auto children = readDirectory(current, maxEntries, false);
        if (children.error) { result.error = children.error; return result; }
        bool found = false;
        for (const auto& entry : children.entries) {
            if (entry.first.get<ISO9660LowLevel::DirEntry_Filename>().value != part) continue;
            if (!(entry.first.get<ISO9660LowLevel::DirEntry_Flags>().value & 2)) {
                result.error = "ISO path is not a directory";
                return result;
            }
            current = entry.first;
            found = true;
            break;
        }
        if (!found) { result.error = "ISO directory not found"; return result; }
        if (!result.path.empty()) result.path += '/';
        result.path += part;
        offset = end == path.npos ? path.size() : end + 1;
    }
    auto entries = readDirectory(current, maxEntries, false);
    result.entries = std::move(entries.entries);
    result.error = entries.error;
    return result;
}

std::vector<PCSX::ISO9660Reader::FullDirEntry> PCSX::ISO9660Reader::listAllEntriesFrom(
    const ISO9660LowLevel::DirEntry& entry) {
    auto result = readDirectory(entry, 65536, true);
    if (result.error) return {};
    return std::move(result.entries);
}
