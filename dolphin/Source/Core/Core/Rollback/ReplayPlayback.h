// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// What to play (docs/design/replays.md §6.2): Slippi's playback contract. The launcher starts the
// playback Dolphin with `-i <comm.json>` and writes the file again to play something else:
//   {"mode": "normal" | "queue", "replay": "<path>", "queue": [{"path": "<path>"}, ...],
//    "commandId": "<changes for every new request>", "playAnyway": false}
// A new commandId plays `replay` (normal) or the queue's replays one after another (a set).
// `playAnyway` plays a replay from another version (the launcher asks first). Dolphin writes
// what it is doing to <comm.json>.status.json:
//   {"commandId", "state": "loading" | "waiting" | "playing" | "ended" | "error", "replay",
//    "index", "count", "frame", "lastFrame", "desyncFrame", "error", "needsPlayAnyway"}

#pragma once

#include <optional>
#include <string>

#include <picojson.h>

namespace Gprb::Replay::Playback
{
// Starts following the comm file (from the command line, before the game boots).
void SetCommFile(const std::string& path);
// Plays one file at once (the harness); an error when it cannot be played.
std::optional<std::string> PlayFile(const std::string& path, bool play_anyway, bool hold_at_end = false);
picojson::object Status();
// Stops following the comm file (emulation shutdown).
void Shutdown();
}  // namespace Gprb::Replay::Playback
