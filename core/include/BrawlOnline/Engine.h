// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The game under the online code (docs/design/brawl-online-core.md §2): Dolphin's emulator, or
// EmissaryRecomp's runtime. The session runs as hooks in Brawl's main loop (sora_scene's
// gfApplication loop: LOOP_TOP at 0x800171b4, FRAME_END at 0x80017504, the loop's end); at each
// hook it tells the engine what the game does next. One engine per process, set at start-up
// (SetEngine) before any session runs.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

  // ---- Emulated time and the session's pacing ----
  // The names and types are Dolphin's CoreTiming / SystemTimers / VideoInterface ones (Dolphin's
  // engine forwards each to the call of the same name; CoreTiming.h says what each does).
  using Clock = std::chrono::steady_clock;
  using DT = Clock::duration;
  using TimePoint = Clock::time_point;

  // The emulated CPU's ticks, how many make a second, a video field's worth, the time base.
  virtual u64 GetTicks() const = 0;
  virtual u32 GetTicksPerSecond() const = 0;
  virtual u32 GetTicksPerField() const = 0;
  virtual u64 GetFakeTimeBase() const = 0;
  // A rollback's re-run is running (unthrottled, not shown).
  virtual bool IsRollbackResimulating() const = 0;
  virtual void SetRollbackResimulating(bool resimulating) = 0;
  // The wait at the loop top for the game's file and sound loads.
  virtual void SetRollbackLoadWait(bool waiting) = 0;
  // The audio clock waits for re-runs (by fields added as pauses).
  virtual void SetRollbackAudioWaits(bool wait) = 0;
  virtual void AddRollbackAudioPause(s64 ticks) = 0;
  // Time sync: the emulation speed times this (1.0 = none); the timeline moved earlier.
  virtual void SetRollbackSpeedAdjustment(double factor) = 0;
  virtual void AdvanceThrottle(DT amount) = 0;
  // The frame pacing: present pacing on or off, the sleep lead for a coming rollback, a re-anchor
  // at the current time, a rollback's burst keeping the schedule.
  virtual void SetPresentPacing(bool on) = 0;
  using PacedSleepLead = DT (*)();
  virtual void SetPacedSleepLead(PacedSleepLead lead) = 0;
  virtual void ResetThrottleToNow() = 0;
  virtual void BeginRollbackBurst() = 0;
  // How far behind its schedule the pacing was at worst since the last call; the host time the
  // current emulated instant is due at (empty while unthrottled); sleep until a host time.
  virtual DT TakeMaxBehindSchedule() = 0;
  virtual std::optional<TimePoint> ScheduledHostTime() = 0;
  virtual void SleepUntil(TimePoint time_point) = 0;

  // ---- Snapshots: what a rollback saves and restores ----
  // Dolphin's RollbackManager (RollbackManager.h says what each does); the recomp's snapshot store.
  // A save is tagged with the session's frame; NO_TAG for one that loads go to by count.
  static constexpr s64 NO_TAG = INT64_MIN;
  using Ranges = std::vector<std::pair<u32, u32>>;  // (effective address, size)
  struct PhysRange
  {
    u32 phys_start = 0;  // address & 0x1FFFFFFF
    u32 phys_end = 0;
  };
  // Gameplay-only snapshots of these ranges (never restoring `exclude`), or the whole machine.
  virtual void BeginRegionMode(const Ranges& ranges, const Ranges& exclude) = 0;
  virtual void EndRegionMode() = 0;
  virtual void ToggleFrameSave() = 0;
  virtual void SaveFrame(s64 tag) = 0;
  virtual bool LoadFrame(int frames_back) = 0;
  virtual int SlotsBackForTag(s64 tag) const = 0;
  virtual int RingCount() const = 0;  // how many saves back a load can go
  // The plugin's mailbox (Online/GameBridge.h), never restored; set by the bridge.
  virtual std::optional<PhysRange> MailboxRegion() const = 0;
  virtual void SetMailboxRegion(u32 virt_addr, u32 size_bytes) = 0;
  // For the session's stats file: the snapshots' counters by name, and the granules a partial
  // save skipped by physical address.
  virtual std::vector<std::pair<std::string, double>> SnapshotStats() const = 0;
  virtual std::vector<std::pair<u32, u64>> PartialGranuleSkips() const = 0;

  // ---- Diagnostics ----
  // Called from the session's network thread (any thread but the game's) when the game has not
  // run a session frame for a second or more (GameplaySession's loop watch): what the engine can
  // tell about its game thread then, appended to that log line ("" for nothing more). Dolphin's
  // own stall tracer covers its CPU thread. (BRAWL_ONLINE_ENGINE_STALL_REPORT: an engine built
  // against older cores can leave its override out.)
#define BRAWL_ONLINE_ENGINE_STALL_REPORT 1
  virtual std::string StallReport() const { return {}; }
};

// The process's engine (Dolphin registers its own at start-up; EmissaryRecomp its own).
Engine& GetEngine();
void SetEngine(Engine* engine);
}  // namespace BrawlOnline
