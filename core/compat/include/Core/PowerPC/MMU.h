// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/PowerPC/MMU.h (core/standalone only): host reads and writes
// of guest RAM by effective address. Brawl runs with the default BATs (0x8/0xC MEM1, 0x9/0xD
// MEM2), which the host's GuestPointer follows.

#pragma once

#include <bit>
#include <concepts>
#include <cstring>

#include "BrawlOnline/Host.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace Core
{
class CPUThreadGuard;
}

namespace PowerPC
{
class MMU
{
public:
  template <std::unsigned_integral T>
  static T HostRead(const Core::CPUThreadGuard&, const u32 address)
  {
    const u8* p = BrawlOnline::GetHost().GuestPointer(address, sizeof(T));
    if (!p)
      return 0;
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (sizeof(T) == 2)
      v = Common::swap16(v);
    else if constexpr (sizeof(T) == 4)
      v = Common::swap32(v);
    else if constexpr (sizeof(T) == 8)
      v = Common::swap64(v);
    return v;
  }

  template <std::unsigned_integral T>
  static void HostWrite(const Core::CPUThreadGuard&, u32 var, u32 address)
  {
    u8* p = BrawlOnline::GetHost().GuestPointer(address, sizeof(T));
    if (!p)
      return;
    T v = static_cast<T>(var);
    if constexpr (sizeof(T) == 2)
      v = Common::swap16(v);
    else if constexpr (sizeof(T) == 4)
      v = Common::swap32(v);
    std::memcpy(p, &v, sizeof(T));
    BrawlOnline::GetHost().MarkDirty(address, sizeof(T));
  }

  static bool HostIsRAMAddress(const Core::CPUThreadGuard&, u32 address)
  {
    return BrawlOnline::GetHost().GuestPointer(address, 1) != nullptr;
  }
};
}  // namespace PowerPC
