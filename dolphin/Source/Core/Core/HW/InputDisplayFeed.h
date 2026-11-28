// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The input display's source in every game but Brawl (Sarah's board: "input display doesnt work on
// 'other games'"): Brawl's display reads the game's own pad records (gfPadSystem, Online/
// InputView.h), which no other game has. Here, what the hardware hands the game, as the SI's
// GameCube controllers and the Bluetooth stack's Wii Remotes give it (a whole-machine session's
// inputs included: the session's frame, input delay and all), is turned into those records'
// buttons, sticks and triggers. Display only: nothing here changes what the game reads.

#pragma once

#include "Common/CommonTypes.h"

struct GCPadStatus;

namespace WiimoteEmu
{
struct DesiredWiimoteState;
}

namespace InputDisplayFeed
{
// CPU thread: the GameCube controller in `port` (0-3) as the game reads it.
void OnGameCubeRead(int port, const GCPadStatus& status);
// CPU thread: Wii Remote `index` (0-3), connected, as the game gets its report.
void OnWiimoteRead(int index, const WiimoteEmu::DesiredWiimoteState& state);
// CPU thread, at the end of each VI field.
void OnViField();
}  // namespace InputDisplayFeed
