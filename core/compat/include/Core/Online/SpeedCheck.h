// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Online/SpeedCheck.h (core/standalone only): the per-game
// speed report is Dolphin's for now.

#pragma once

#include <string>
#include <vector>

namespace Online::SpeedCheck
{
inline std::vector<std::string> TakeOutbox()
{
  return {};
}
}  // namespace Online::SpeedCheck
