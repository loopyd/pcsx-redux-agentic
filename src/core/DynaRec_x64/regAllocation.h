/***************************************************************************
 *   Copyright (C) 2021 PCSX-Redux authors                                 *
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

#pragma once
#include "core/r3000a.h"
#if defined(DYNAREC_X86_64)

#include <array>

#include "xbyak.h"
using namespace Xbyak;
using namespace Xbyak::util;

constexpr Reg64 contextPointer = rbp;
constexpr Reg64 runningPointer = rbx;
constexpr int ALLOCATEABLE_REG_COUNT = 8;
constexpr int ALLOCATEABLE_NON_VOLATILE_COUNT = 4;
constexpr std::array<Reg32, ALLOCATEABLE_REG_COUNT> allocateableRegisters = {r12d, r13d, r14d, r15d,
                                                                             r8d,  r9d,  r10d, r11d};
constexpr std::array<Reg32, 4> allocateableVolatiles = {r8d, r9d, r10d, r11d};
constexpr std::array<Reg32, 4> allocateableNonVolatiles = {r12d, r13d, r14d, r15d};

constexpr Reg32 arg1 = edi;
constexpr Reg32 arg2 = esi;
constexpr Reg32 arg3 = edx;
constexpr Reg32 arg4 = ecx;
#define IS_VOLATILE(x) ((x) >= ALLOCATEABLE_NON_VOLATILE_COUNT)

#endif  // DYNAREC_X86_64
