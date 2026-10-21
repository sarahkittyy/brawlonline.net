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
//
// Ours, `mirror` (spectating, docs/spectate-protocol.md §4): the queue is a live stream's games,
// still being written; games are added at its end under the same commandId, a game whose rest
// won't come has `"cut": true`, and `streamOver`, `closeOnEnd` and `banner` say what to do at the
// stream's end. The status adds "mirror", "at_end", "live_edge", "buffering", "catching_up",
// "catch_rate", "complete", "closeIn", and the states "between" (the set's next game is coming)
// and "close" (the end's countdown ran out: the launcher closes Dolphin).
//
// Ours, `highlights` (the launcher's stats page): {"mode": "highlights", "queue": [{"path",
// "startFrame", "endFrame"}, ...]}: each replay plays from startFrame to endFrame (its frames, as
// the seek bar's), paused there, then the next one; a next one of the same replay goes on in the
// running match (a seek forward, or a keyframe put back), and so does a new request whose first one
// is of the replay already playing. The way there is a fast-forward (Session::SetPlaybackClip) with
// its progress on screen. Every replay holds at its last frame. The status adds "highlights",
// "clipStart", "clipEnd", "seekFrom" (the frame the fast-forward started from; -1 not yet) and the
// state "seeking" (on the way to the clip's start; "playing" is the clip, "ended" the last one's end).
//
// Ours, `render` (docs/design/highlights.md section 5): highlights as above, written to a video
// instead of watched: {"mode": "render", "output": "<path>.mp4", "height": 720, "queue": [...]}
// (Rollback/ClipRecorder.h). Every clip is a part of the file, one after another. The status adds
// "render", "output", "rendered" (the clips written so far) and "framesWritten"; "ended" once the
// file is complete, "error" (the file deleted) when it could not be. A new request gives up a
// render still going.

#pragma once

#include <optional>
#include <string>

#include <picojson.h>

namespace Gprb::Replay::Playback
{
// Starts following the comm file (from the command line, before the game boots).
void SetCommFile(const std::string& path);
// Whether this Dolphin follows a comm file (the launcher's playback Dolphin). Its GameCube
// controllers read neutral (SI_DeviceGCController.cpp): nothing but the replay plays the game.
bool Following();
// Plays one file at once (the harness); an error when it cannot be played.
std::optional<std::string> PlayFile(const std::string& path, bool play_anyway, bool hold_at_end = false);
picojson::object Status();
// Stops following the comm file (emulation shutdown).
void Shutdown();
}  // namespace Gprb::Replay::Playback
