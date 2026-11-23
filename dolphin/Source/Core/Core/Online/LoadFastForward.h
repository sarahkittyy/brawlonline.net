// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Boot fast-forward: a Brawl build's boot runs unthrottled until its first menu.
//
// Most of the boot is the game counting frames at 60 a second, not work: the boot to the first
// menu (P+'s launcher, the strap, the save check) runs ~370 frames, ~6.2 s at full speed and
// ~1.2 s unthrottled. So the boot's scenes run as fast as this PC can, until the first scene that
// takes input. Nothing the game computes changes: the same frames run with the same inputs, only
// sooner. While it is on the game's audio is dropped and VSync is off (CoreTimingManager::
// SetLoadFastForward); the boot plays no sound.
//
// Only the boot: a load between two scenes (the stage select to the match, the match to the
// results) starts while the last scene's sound still plays (the stage select's pick sound rings
// ~0.8 s into the match's load), and dropping the load's audio cut it 50 ms in.
//
// Never in a whole-machine session or Dolphin netplay, where every frame's timing is shared with
// the other players, nor for a replay or a spectated game. `[Online] LoadFastForward` turns it off.

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
// Every pass through Brawl's game loop top (HLE_Misc), first thing: ends the boot's fast-forward
// at the first scene that takes input. CPU thread.
void OnLoopTop(const Core::CPUThreadGuard& guard);
}  // namespace Online::LoadFastForward
