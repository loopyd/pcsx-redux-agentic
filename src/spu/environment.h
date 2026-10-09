// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace PCSX::SPU {
struct Environment {
    static constexpr int64_t SETTINGS_VERSION = 2;
    enum SettingSlot : unsigned { Volume = 0, Interpolation = 1, KeyOnDelay = 2, SchemaVersion = 7, Reserved = 8, Speed = 9 };
    inline static constexpr std::array<const char*, 14> settingNames = {
        "volume", "interpolation", "key_on_delay", "mute", "mono", "streaming", "null_sync",
        "schema_version", "reserved", "speed", "forced_irq", "voice_mute", "voice_solo", "xa"};
    std::array<int64_t, 14> settings{};
    std::string backend, device;
    bool initialized = false, stream = false;
};
}
