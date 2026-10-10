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

#include "core/system.h"
#include "imgui.h"
#include "spu/interface.h"
#include "support/imgui-helpers.h"

bool PCSX::SPU::impl::configure() {
    ImGui::SetNextWindowPos(ImVec2(70, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(550, 220), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SPU configuration", &m_showCfg)) {
        ImGui::End();
        return false;
    }
    bool changed = false;
    bool deviceChanged = false;
    auto backends = m_audioOut.getBackends();
    auto devices = m_audioOut.getDevices();

    std::string currentBackend = settings.get<Backend>();
    std::string currentDevice = settings.get<Device>();
    if (ImGui::BeginCombo("Backend", currentBackend.c_str())) {
        for (auto &b : backends) {
            if (ImGui::Selectable(b.c_str(), currentBackend == b)) {
                settings.get<Backend>() = b;
                deviceChanged = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Device", currentDevice.c_str())) {
        for (auto &d : devices) {
            if (ImGui::Selectable(d.c_str(), currentDevice == d)) {
                settings.get<Device>() = d;
                deviceChanged = true;
            }
        }
        ImGui::EndCombo();
    }
    deviceChanged |= ImGui::Checkbox("Use Null Sync", &settings.get<NullSync>().value);
    ImGuiHelpers::ShowHelpMarker(R"(More precise CPU-SPU synchronization,
at the cost of extra power required.)");
    if (deviceChanged) {
        m_audioOut.reinit();
    }
    changed = deviceChanged;

    if (ImGui::Checkbox("Muted", &settings.get<Mute>().value)) {
        changed = true;
        m_audioOut.updatePlaybackMute();
    }
    changed |= ImGui::Checkbox("Enable streaming", &settings.get<Streaming>().value);
    ImGuiHelpers::ShowHelpMarker(R"(Uncheck this to mute the streaming channel
from the main CPU to the SPU. This includes
XA audio and audio tracks.)");
    const char *volumeValues[] = {"Low", "Medium", "Loud", "Loudest"};
    changed |= ImGui::Combo("Volume", &settings.get<Volume>().value, volumeValues, IM_ARRAYSIZE(volumeValues));
    const char *interpolationValues[] = {"None - fastest", "Simple interpolation",
                                         "Gaussian interpolation - good quality",
                                         "Cubic interpolation - better treble"};
    changed |= ImGui::Combo("Interpolation", &settings.get<Interpolation>().value, interpolationValues,
                            IM_ARRAYSIZE(interpolationValues));
    changed |= ImGui::Checkbox("Mono", &settings.get<Mono>().value);
    ImGuiHelpers::ShowHelpMarker("Downmixes stereo to mono.");
    changed |= ImGui::InputInt("Speed multiplier", &settings.get<Speed>().value);
    ImGuiHelpers::ShowHelpMarker(R"(Emulation speed, applied at the audio sink (the master clock).
1 = realtime, N = N times faster. Frames are skipped, not resampled, so pitch is preserved.
0 or less = unbounded: run as fast as the host allows.)");

    ImGui::End();
    if (changed) invalidateCapture();
    return changed;
}
