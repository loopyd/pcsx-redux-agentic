// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace PCSX::SPU {
struct Environment {
    std::array<int64_t, 14> settings{};
    std::string backend, device;
    bool initialized = false, stream = false;
};
}
