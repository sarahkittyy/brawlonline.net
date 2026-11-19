// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Boot fast-forward: a Brawl build boots unthrottled until its first menu.
//
// Most of P+'s boot is the game counting frames at 60 a second, not work: from the launcher DOL
// to the first menu it runs ~370 frames, ~6.2 s at full speed and ~1.2 s unthrottled on the same
// PC. Nothing the game computes changes (the same frames run, only sooner), the boot is silent
// (no audio is dropped), and VSync is off while it runs (CoreTimingManager::SetBootFastForward).
// It ends at the first scene that is not the boot's (P+'s strap, the save check, the memory swap
// between them), or after 20 s of game time. Never in a whole-machine session or Dolphin netplay,
// where every frame's timing is shared with the other players. `[Online] BootFastForward` turns
// it off.

#pragma once

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Online::BootFastForward
{
// Every boot (HLE::PatchFixedFunctions); it decides whether this is a Brawl build. CPU thread.
void OnBoot(Core::System& system);
// Every pass through Brawl's game loop top (HLE_Misc), first thing. CPU thread.
void OnLoopTop(const Core::CPUThreadGuard& guard);
}  // namespace Online::BootFastForward
