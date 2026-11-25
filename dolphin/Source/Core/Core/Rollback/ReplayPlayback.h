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
// The seek bar marks the clips of the replay playing; a click on one plays it, and the queue goes
// on from there (`index` changes to it, even after "ended").
//
// The launcher's lists (docs/design/highlights.md section 3) play one highlight a request and add
// "marks": [{"startFrame", "endFrame", "position"}] (the list's highlights of that replay, marked
// on the seek bar instead of the queue's) and "nav": {"position", "total", "hasPrevReplay",
// "hasNextReplay"}: the overlay's "3/40  << < > >>". A star or < > within the replay plays it
// here at once; the status says so, "highlightPick": {"seq", "startFrame", "endFrame"}. A button
// that needs another replay (<< >>, or < > past this replay's marks) is asked of the launcher,
// "navRequest": {"seq", "action": "prevReplay" | "prevClip" | "nextClip" | "nextReplay",
// "frame"}, which plays the one it picks as a new request. Both count from 1 for each commandId.
// The same commandId written again with other marks or nav (the list changed) updates them.
//
// The Replays tab's replays (`normal` / `queue`) may have "nav" too, with "clips": false: the
// overlay then shows "12/340  << >>" (the replay list's), asked of the launcher as above.
// They may have "marks" too (any replay played, not spectated: its highlights, hotkey clips and
// manual clips), each with an optional "path" (only that replay of the queue; none: every one);
// the launcher writes them once it has them, the same commandId again. A star plays its clip in
// the running match (the session's own) and pauses at its end; < > go from star to star.
//
// Ours, `render` (docs/design/highlights.md section 5): highlights as above, written to a video
// instead of watched: {"mode": "render", "output": "<path>.mp4", "height": 720, "queue": [...]}
// (Rollback/ClipRecorder.h). Every clip is a part of the file, one after another. The status adds
// "render", "output", "rendered" (the clips written so far) and "framesWritten"; "ended" once the
// file is complete, "error" (the file deleted) when it could not be. A new request gives up a
// render still going.
//
// Spectator chat (Online/SpectatorChat.h): the launcher writes it to <comm.json>.chat.json, and
// what the spectator types goes back in the status as "chat".
//
// Manual clips (docs/design/highlights.md section 9): the overlay's Clip on the seek bar, then
// "Save as highlight", keeps the replay playing's frames from one handle to the other (SaveClip).
// The status lists every clip saved in this Dolphin's run, "savedClips": [{"path", "startFrame",
// "endFrame"}, ...] (path: the replay armed, a spectated game's file too); the launcher keeps each
// once, as a Manual Clip. Clip mode's "Render (w/ Music)" / "Render (No Music)" saves the clip so
// and asks the launcher to render it to video: "renderRequests": [{"seq", "path", "startFrame",
// "endFrame", "music", "clip"}, ...] (every one of this run, seq from 1; the launcher renders each
// once). A star's highlight playing has Render too, not saved ("clip": false).
//
// The launcher's side of the overlay, <comm.json>.overlay.json (read every poll; none: nothing):
//   {"accent": "#rrggbb" (the launcher's colour: the seek bar's played part),
//    "toasts": [{"id", "path", "startFrame", "endFrame", "label", "text", "progress" (0..1),
//    "state": "queued" | "rendering" | "done" | "error", "toast", "folder"}, ...] (the renders
//    asked for here: at the top right while `toast`, and on the row of a star of them; a done one's
//    folder button says "showRender": {"seq", "id"} in the status)}
//
// Auto play (spectating, `"autoPlay": true` in a mirror request): at the stream's end the overlay
// counts down "Next game in N s." (an X stays, clip mode holds it), then the state is "next" (the
// launcher plays the next live game, or says it waits for one in `banner`); X makes it "ended"
// for good. The status adds "nextIn" (seconds) while it counts.

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

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
// Whether a clip can be saved: a launcher follows this Dolphin (the comm file) and a replay of its
// request is armed, not rendered.
bool CanSaveClip();
// The overlay's list buttons (the request's "nav"): shown, "position/total", whether there is a
// replay before or after this one in the list, and whether < > (clips) are there too (a list of
// highlights; a list of replays has << >> only). Any thread, without the playback lock.
struct NavView
{
  bool shown = false;
  int position = 0;
  int total = 0;
  bool prev_replay = false;
  bool next_replay = false;
  bool clips = true;
  // The replay armed (its path as the request gave it; empty: none), for the overlay to find its
  // renders in the launcher's overlay file.
  std::string armed_path;
};
NavView GetNav();
enum class NavAction
{
  PrevReplay,
  PrevClip,
  NextClip,
  NextReplay,
};
// Asks the launcher for another replay of its list (status "navRequest"); `frame`: the one shown.
void RequestNav(NavAction action, s64 frame);
// Saves frames `start` to `end` of the replay armed (any thread: the overlay's); false when
// CanSaveClip is not, or the session's replay is no longer the armed one.
bool SaveClip(s64 start, s64 end);
// Asks the launcher to render frames `start` to `end` of the replay armed (status
// "renderRequests"): clip mode's (`save`: saved as SaveClip does first, a Manual Clip), or a
// star's highlight playing (not saved: it is one already).
// `label`: what the launcher's toast calls it ("Clip 2:13 - 2:20 (7 s)").
bool RequestRender(s64 start, s64 end, bool music, bool save = true, const std::string& label = {});
// A finished render's folder button (the launcher's overlay file's `id`): status "showRender"
// {"seq", "id"}, the launcher opens the file explorer at the video.
void ShowRenderFolder(const std::string& id);

// The launcher's side of the overlay (<comm.json>.overlay.json), as last read. Any thread.
// A render asked for from the game, as the launcher has it: which (`id`, the replay and frames),
// what to say, whether it still shows at the top right (`toast`), and whether its video can be
// shown in its folder (`folder`: done).
struct OverlayToast
{
  std::string id;
  std::string path;
  s64 start = -1;
  s64 end = -1;
  std::string label;
  std::string text;
  float progress = 0;
  std::string state;
  bool toast = true;
  bool folder = false;
};
struct OverlayView
{
  bool has_accent = false;
  u8 accent[3] = {0, 0, 0};
  // Clip mode's colour (the launcher's opposite).
  bool has_clip = false;
  u8 clip[3] = {0, 0, 0};
  std::vector<OverlayToast> toasts;
};
OverlayView GetOverlayView();

// Auto play's countdown at a spectated stream's end: shown, until when (none: it ran out and the
// launcher is getting the next game). Any thread.
struct NextCountdown
{
  bool shown = false;
  bool waiting = false;
  std::chrono::steady_clock::time_point deadline{};
  double total_seconds = 5;
};
NextCountdown GetNextCountdown();
// The countdown's X: stay on this game (any thread but the GPU thread: it takes the playback lock).
void CancelNext();
// The overlay's clip mode, which holds the countdown (any thread, every frame).
void SetClipMode(bool on);
// Stops following the comm file (emulation shutdown).
void Shutdown();
}  // namespace Gprb::Replay::Playback
