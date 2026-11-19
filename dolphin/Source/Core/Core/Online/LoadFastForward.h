// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Loading fast-forward: while a Brawl build only loads, it runs unthrottled.
//
// Most of a load is the game counting frames at 60 a second, not work: from the stage select to
// GO a P+ match runs ~330 frames, ~5.7 s at full speed and ~0.8 s unthrottled on the same PC, and
// the boot to the first menu ~6.2 s against ~1.2 s. So the stretches that only load run as fast
// as this PC can: the boot until the first scene that takes input, the memory swap between two
// scenes (scMemoryChange, and the frames with no scene), and a match scene before its first game
// frame (the frame an online session's barrier waits at). The countdown, the match, GAME! and
// every menu run at normal speed.
//
// Nothing the game computes changes: the same frames run with the same inputs, only sooner (the
// session's barrier copies the host's frame counters and RNGs anyway, and compares the setup).
// While it is on the game's audio is dropped and VSync is off (CoreTimingManager::
// SetLoadFastForward). Never in a whole-machine session or Dolphin netplay, where every frame's
// timing is shared with the other players, nor while a replay or spectated game is armed (a
// spectator's distance behind the players comes from its load). `[Online] LoadFastForward` turns
// it off.

#pragma once

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Online::LoadFastForward
{
// Every boot (HLE::PatchFixedFunctions); it decides whether this is a Brawl build. CPU thread.
void OnBoot(Core::System& system);
// Every pass through Brawl's game loop top (HLE_Misc), first thing. CPU thread.
void OnLoopTop(const Core::CPUThreadGuard& guard);
// Every emulated field (Core::Callback_NewField). CPU thread.
void OnField(Core::System& system);
}  // namespace Online::LoadFastForward
