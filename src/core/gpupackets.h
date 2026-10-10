// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct lua_State;

namespace PCSX::LuaFFI {

int decodeGPU(lua_State* state);
int readGPUChain(lua_State* state);

}
