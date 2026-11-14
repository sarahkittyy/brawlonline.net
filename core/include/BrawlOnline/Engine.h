// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The game under the online code (docs/design/brawl-online-core.md §2): Dolphin's emulator, or
// EmissaryRecomp's runtime. The session runs as hooks in Brawl's main loop (sora_scene's
// gfApplication loop: LOOP_TOP at 0x800171b4, FRAME_END at 0x80017504, the loop's end); at each
// hook it tells the engine what the game does next. One engine per process, set at start-up
// (SetEngine) before any session runs.

#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>

#include "BrawlOnline/CommonTypes.h"

namespace BrawlOnline
{
// Brawl's game loop (gfApplication::mainLoop, RSBE01): where the session's hooks sit.
constexpr u32 LOOP_TOP = 0x800171b4;   // li r25,1
constexpr u32 FRAME_END = 0x80017504;  // stw r0,0x100(r23): the frame counter's store
constexpr u32 LOOP_END = 0x80017508;   // b mainLoop top

// The guest registers the session keeps across a call it makes into the game (and puts back after).
struct GuestRegs
{
  std::array<u32, 32> gpr{};
  std::array<std::array<u64, 2>, 32> ps{};  // the paired singles, as two raw 64-bit halves
  std::array<u64, 8> cr{};                  // the CR fields as the engine keeps them
  u32 fpscr = 0;
  u8 xer_ca = 0;
  u8 xer_so_ov = 0;
  u16 xer_stringctrl = 0;
  u32 lr = 0;
  u32 ctr = 0;
};

class Engine
{
public:
  virtual ~Engine() = default;

  // ---- Brawl's main loop, at the session's hooks ----

  // At the loop top: run this pass of the loop (the game's frame).
  virtual void ContinueFrame() = 0;
  // At the loop top: skip this pass (no game logic), on to the loop's end.
  virtual void SkipFrame() = 0;
  // At the loop's end: back to the loop top.
  virtual void BackToLoopTop() = 0;
  // At FRAME_END: the instruction the hook replaced (the frame counter's store), then on.
  virtual void FinishFrameEnd() = 0;
  // At the loop top: call a game function with up to 3 arguments (r3..r5); it returns to the loop
  // top, where the session's hook runs again.
  virtual void CallFromLoopTop(u32 function, std::initializer_list<u32> args) = 0;
  // A guest general-purpose register at the current hook (r24: the game frame at FRAME_END, ...).
  virtual u32 Gpr(int index) const = 0;
  // The registers a guest call changes, kept and put back around it.
  virtual void SaveGuestRegs(GuestRegs* out) const = 0;
  virtual void RestoreGuestRegs(const GuestRegs& in) = 0;
  // The emulated CPU (or the game's thread) is running, not paused or stopped.
  virtual bool CpuRunning() const = 0;
};

// The process's engine (Dolphin registers its own at start-up; EmissaryRecomp its own).
Engine& GetEngine();
void SetEngine(Engine* engine);
}  // namespace BrawlOnline
