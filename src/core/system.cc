/***************************************************************************
 *   Copyright (C) 2018 PCSX-Redux authors                                 *
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

#include "core/system.h"

#include <stddef.h>
#include <uv.h>

PCSX::System* PCSX::g_system = NULL;

PCSX::System::System() : m_loop(new uv_loop_t) { uv_loop_init(m_loop); }

PCSX::System::~System() {
    if (!m_emergencyExit) uv_loop_close(m_loop);
    delete m_loop;
}

uv_loop_t* PCSX::System::getLoop() { return m_loop; }

bool PCSX::System::findResource(std::function<bool(const std::filesystem::path& path)> walker,
                                const std::filesystem::path& name, const std::filesystem::path& releasePath,
                                const std::filesystem::path& sourcePath) {
    // First, let's try the base filename from the same directory as our main binary.
    if (walker(m_binDir / name)) return true;

    // Then, let's search for our release folders...
    // Also search installed resources next to the executable.
    if (walker(m_binDir / releasePath / name)) return true;
    // Next up, the Unix way: binary is in /bin, and resources are in /share/pcsx-redux.
    if (walker(m_binDir / ".." / "share" / "pcsx-redux" / releasePath / name)) return true;
    // And finally, let's try if we're running from sources.
    // If our main binary is at the root - that's the Unix way.
    if (walker(m_binDir / sourcePath / name)) return true;
    // Make places the executable two directories below the source root in bins/<BUILD>.
    if (walker(m_binDir / ".." / ".." / sourcePath / name)) return true;
    if (walker(m_binDir / ".." / ".." / ".." / sourcePath / name)) return true;

    // No luck here...
    return false;
}

std::filesystem::path PCSX::System::getPersistentDir() const {
    if (getArgs().isPortable()) return getArgs().getPortablePath();
    char* homeDir = getenv("HOME");
    if (!homeDir) {
        return "";
    }
    std::filesystem::path persistentDir = std::filesystem::path(homeDir) / ".config" / "pcsx-redux";
    if (!std::filesystem::exists(persistentDir)) {
        if (!std::filesystem::create_directories(persistentDir)) {
            return "";
        }
    }
    return persistentDir;
}
