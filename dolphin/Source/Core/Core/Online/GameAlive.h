// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Whether this game runs, for mm (bug report #105, 2026-10-22): a Ranked opponent's game froze 18
// s into a game while its online connection, served by Rooms' own thread, stayed up for 19
// minutes; mm took it as there, so the other player's "opponent left" waited all that time. The
// CPU thread beats at every emulated field, every game frame (the loop top hook) and every round
// of a gameplay session's wait on its peer (a barrier, the host's setup, a host wait), so a game
// waiting on a slow or silent peer still beats; a frozen one, or one whose emulation is paused,
// does not. While it beats, the online connection sends `game-alive` every GAME_ALIVE_EVERY (to
// an mm whose hello-resp says it takes them: an older one would answer with a shown room-error);
// mm takes a game that said it would (`hello.gameAlive`) and sends none for 30 s as stopped, and
// an "opponent left" about it then counts at once (docs/rooms-protocol.md §2,
// docs/spectate-protocol.md §1.1).

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Online::GameAlive
{
// How often `game-alive` goes out while the game beats (mm's GAME_ALIVE_EVERY).
constexpr std::chrono::seconds GAME_ALIVE_EVERY{2};

// Any thread, lock-free: the game ran just now.
void Beat();

// Rooms' thread, every pass: the `game-alive` due on the online connection (`connection` counts
// its connections: a new one starts at once), if the game beat since the last one.
std::optional<std::string> Service(std::chrono::steady_clock::time_point now, bool online,
                                   u64 connection);
}  // namespace Online::GameAlive
