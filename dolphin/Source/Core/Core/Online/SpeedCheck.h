// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The waiting room's speed check (docs/design/speed-check.md): while an Unranked or Ranked search
// waits in Brawl's Online Training room, this PC's frame rate is measured in windows of 300 game
// frames, and once the room ends (or four windows are in) one `speed-check` goes to mm on the
// online connection. mm only records it for now (2026-10-21: log only, nothing is refused).

#pragma once

#include <string>
#include <vector>

namespace Core
{
class CPUThreadGuard;
}

namespace Online::SpeedCheck
{
// Every game frame (the loop top hook, CPU thread).
void OnLoopTop(const Core::CPUThreadGuard& guard);
// The messages waiting for the online connection (Rooms' thread, which sends them).
std::vector<std::string> TakeOutbox();
}  // namespace Online::SpeedCheck
