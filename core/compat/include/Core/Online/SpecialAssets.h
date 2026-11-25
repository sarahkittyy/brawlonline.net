// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Online/SpecialAssets.h (core/standalone only): the bundle is
// built from the SD card image with Dolphin's card reader; outside Dolphin it is unavailable and
// the game plugin keeps its own menus.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Online::SpecialAssets
{
enum class State : u8
{
  None = 0,
  Loading = 1,
  Ready = 2,
  Unavailable = 3,
};
constexpr u32 MAGIC = 0x53504258;  // "SPBX"
constexpr u32 MAX_BUNDLE = 0x80000;

inline State Request(const std::string&)
{
  return State::Unavailable;
}
inline std::vector<u8> Bundle()
{
  return {};
}
}  // namespace Online::SpecialAssets
