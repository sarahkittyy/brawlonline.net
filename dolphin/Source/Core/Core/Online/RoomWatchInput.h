// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Watching the room's game (BrawlOnline/Online/RoomWatch.h): the engine's own read of the player's
// controllers, for START or B while the watched game stands still (buffering), when the game's pad
// thread stands still with it and its newest sample is from before (bug report #171). Dolphin's
// here; core/compat has the standalone build's, over BrawlOnline::Host.

#pragma once

namespace Online::RoomWatchInput
{
// START or B down on any of this player's controllers now (any thread).
bool HostLeaveDown();
}  // namespace Online::RoomWatchInput
