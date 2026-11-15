// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Online/ModsRoom.h (core/standalone only): Other Games' rooms
// are Dolphin's.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Online::ModsRoom
{
inline void OnSignedInElsewhere()
{
}
inline bool OnServerMessage(const std::string&)
{
  return false;
}
inline std::vector<std::string> TakeOutbox(u64)
{
  return {};
}
inline void Files(const std::string&)
{
}
}  // namespace Online::ModsRoom
