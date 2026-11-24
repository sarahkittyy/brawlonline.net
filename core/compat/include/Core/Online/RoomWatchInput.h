// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Online/RoomWatchInput.h (core/standalone only): START or B on
// the player's controllers now, as the host reads them (BrawlOnline::Host::LeaveButtonsHeld: the
// engine's own controllers, a test harness's pads while one drives the game), for a watched room
// game that stands still (buffering) while the game's pad thread stands still with it.

#pragma once

#include "BrawlOnline/Host.h"

namespace Online::RoomWatchInput
{
inline bool HostLeaveDown()
{
  return BrawlOnline::HasHost() && BrawlOnline::GetHost().LeaveButtonsHeld();
}
}  // namespace Online::RoomWatchInput
