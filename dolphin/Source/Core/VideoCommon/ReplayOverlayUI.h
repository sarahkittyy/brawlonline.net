// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The replay overlay (docs/design/replays.md §6.3, Slippi's seek bar): what is playing, the time,
// pause and speed, a play / pause button and a bar to click or drag to seek, drawn with the OSD's
// ImGui over the game while a replay plays (Gprb::Session::GetPlaybackInfo). A click on the game
// itself pauses or plays; while a spectated game catches up to live it plays on from there (the
// top-left "Catching up to live (5%). Click to watch").
//
// It shows while paused or seeking and for a few seconds after the mouse moves, and hides itself
// while the replay just plays, so a capture of the window (OBS) has only the game. The "Show /
// Hide Seek Bar" hotkey hides it for good (or brings it back).

#pragma once

struct ImFont;

namespace VideoCommon::ReplayOverlayUI
{
// Under the ImGui lock, once per frame.
void Display(float scale);
// Any thread (the hotkey).
void ToggleHidden();
// The chat's font (names in any script), set with it by OnScreenUI. Under the ImGui lock.
void SetFont(ImFont* font);
}  // namespace VideoCommon::ReplayOverlayUI
