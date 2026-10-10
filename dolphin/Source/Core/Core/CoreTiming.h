// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// This is a system to schedule events into the emulated machine's future. Time is measured
// in main CPU clock cycles.

// To schedule an event, you first have to register its type. This is where you pass in the
// callback. You then schedule events using the type id you get back.

// See HW/SystemTimers.cpp for the main part of Dolphin's usage of this scheduler.

// The int cyclesLate that the callbacks get is how many cycles late it was.
// So to schedule a new event on a regular basis:
// inside callback:
//   ScheduleEvent(periodInCycles - cyclesLate, callback, "whatever")

#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Functional.h"
#include "Common/HookableEvent.h"
#include "Common/SPSCQueue.h"
#include "Common/Timer.h"
#include "Core/CPUThreadConfigCallback.h"

class PointerWrap;

namespace Core
{
class System;
}

namespace CoreTiming
{
// These really shouldn't be global, but jit64 accesses them directly
struct Globals
{
  s64 global_timer = 0;
  int slice_length = 0;
  u64 fake_TB_start_value = 0;
  u64 fake_TB_start_ticks = 0;
  float last_OC_factor_inverted = 0.0f;
};

using TimedCallback =
    Common::MoveOnlyFunction<void(Core::System& system, u64 userdata, s64 cyclesLate)>;

struct EventType
{
  TimedCallback callback;
  const std::string* name;
};

struct Event
{
  s64 time;
  u64 fifo_order;
  u64 userdata;
  EventType* type;

  // Sort by time, unless the times are the same, in which case sort by the order added to the queue
  constexpr auto operator<=>(const Event& other) const
  {
    return std::tie(time, fifo_order) <=> std::tie(other.time, other.fifo_order);
  }
  constexpr bool operator==(const Event& other) const
  {
    return std::tie(time, fifo_order) == std::tie(other.time, other.fifo_order);
  }
};

enum class FromThread
{
  CPU,
  NON_CPU,
  // Don't use ANY unless you're sure you need to call from
  // both the CPU thread and at least one other thread
  ANY
};

// helpers until the JIT is updated to use the instance
void GlobalAdvance();
void GlobalIdle();

class CoreTimingManager
{
public:
  explicit CoreTimingManager(Core::System& system);

  // CoreTiming begins at the boundary of timing slice -1. An initial call to Advance() is
  // required to end slice -1 and start slice 0 before the first cycle of code is executed.
  void Init();
  void Shutdown();

  // This should only be called from the CPU thread, if you are calling it any other thread, you are
  // doing something evil
  u64 GetTicks() const;
  u64 GetIdleTicks() const;
  TimePoint GetTargetHostTime(s64 target_cycle);

  void RefreshConfig();

  void DoState(PointerWrap& p);

  // Returns the event_type identifier. if name is not unique, an existing event_type will be
  // discarded.
  EventType* RegisterEvent(const std::string& name, TimedCallback callback);
  void UnregisterAllEvents();

  // userdata MAY NOT CONTAIN POINTERS. userdata might get written and reloaded from savestates.
  // After the first Advance, the slice lengths and the downcount will be reduced whenever an event
  // is scheduled earlier than the current values (when scheduled from the CPU Thread only).
  // Scheduling from a callback will not update the downcount until the Advance() completes.
  void ScheduleEvent(s64 cycles_into_future, EventType* event_type, u64 userdata = 0,
                     FromThread from = FromThread::CPU);

  // We only permit one event of each type in the queue at a time.
  void RemoveEvent(EventType* event_type);
  void RemoveAllEvents(EventType* event_type);

  // Advance must be called at the beginning of dispatcher loops, not the end. Advance() ends
  // the previous timing slice and begins the next one, you must Advance from the previous
  // slice to the current one before executing any cycles. CoreTiming starts in slice -1 so an
  // Advance() is required to initialize the slice length before the first cycle of emulated
  // instructions is executed.
  // NOTE: Advance updates the PowerPC downcount and performs a PPC external exception check.
  void Advance();
  void MoveEvents();

  // Pretend that the main CPU has executed enough cycles to reach the next event.
  void Idle();

  // Clear all pending events. This should ONLY be done on exit or state load.
  void ClearPendingEvents();

  void LogPendingEvents() const;

  std::string GetScheduledEventsSummary() const;

  void AdjustEventQueueTimes(u32 new_ppc_clock, u32 old_ppc_clock);

  u32 GetFakeDecStartValue() const;
  void SetFakeDecStartValue(u32 val);
  u64 GetFakeDecStartTicks() const;
  void SetFakeDecStartTicks(u64 val);
  u64 GetFakeTBStartValue() const;
  void SetFakeTBStartValue(u64 val);
  u64 GetFakeTBStartTicks() const;
  void SetFakeTBStartTicks(u64 val);

  void ForceExceptionCheck(s64 cycles);

  // Directly accessed by the JIT.
  Globals& GetGlobals() { return m_globals; }

  // Throttle the CPU to the specified target cycle.
  void Throttle(const s64 target_cycle);

  // Rollback netplay. While resimulating, frames that were already shown re-run unthrottled (only
  // the presented frame should cost wall-clock time). The speed adjustment multiplies the
  // configured emulation speed by a small factor for time sync (1.0 = none).
  // CPU thread only.
  void SetRollbackResimulating(bool resimulating);
  bool IsRollbackResimulating() const { return m_rollback_resimulating; }

  // Rush Frame Presentation in a gameplay rollback session. Rush lets the CPU sleep only at the
  // first throttle (an SI poll) after each frame is presented, and the GPU thread used to re-arm it
  // when it presented. In a session the GPU thread is deterministic: it gets the FIFO only when the
  // CPU thread reads it, so the present raced the CPU, and the first poll after it was sometimes
  // the one just after the XFB copy (before the session's loop top, which reads the local pad: the
  // frame then ran and was shown ~2 ms after its input was read) and sometimes one after the next
  // frame was rendered (that frame waited ~10 ms before its copy, its input already read). The two
  // alternated every few frames: presents 2-4 ms early or late several times a second (visible
  // stutter, only online) and the input's age jumping by ~12 ms. In a session the CPU thread re-arms
  // the throttle itself when it sends a presented XFB copy (OnPresentedXFBCopy), so the sleep is
  // always at the first poll after the copy: VI-locked, like the copies, and before the loop top.
  // The presenter then holds a frame that came late (a rollback's re-run runs between its input
  // and its copy) to keep the presents a field apart (PresentStats::PresentHoldUntil).
  // CPU thread. The session turns it on for its running phase.
  void SetPresentPacing(bool on) { m_pace_on.store(on, std::memory_order_relaxed); }
  bool PresentPacingActive() const;
  void OnPresentedXFBCopy();
  // Gameplay-only rollback leaves the sound system out of the snapshot, so a sound keeps playing
  // through a rollback. With this mode set (for the whole session), the audio clock waits for the
  // resimulated passes: the session adds the fields each one takes to a pause that the audio DMA
  // waits out (no DMA, so no AI interrupt, no AX frame, no voice moves, no wake-up of the game's
  // sound thread: envelopes, fades and sequences wait too). A rollback then pauses every sound and the
  // music for the re-run instead of cutting the re-run's span out of them, and a sound that only
  // the corrected input starts is heard from its start, late, as on Slippi, where the console's
  // audio only ever advances with the presented frames. The pause is counted in fields, not taken
  // while the passes run: a rollback's passes do not begin and end on field boundaries (the
  // presented pass is longer, the resimulated ones shorter), and the audio has to advance by
  // exactly the fields that were presented. Without the mode, resimulated passes run the audio and their samples are
  // dropped (right for a full-memory rollback, which rolls the sound system back too).
  void SetRollbackAudioWaits(bool wait)
  {
    m_rollback_audio_waits = wait;
    if (!wait)
      m_rollback_audio_pause = 0;
  }
  bool RollbackAudioWaits() const { return m_rollback_audio_waits; }
  void AddRollbackAudioPause(s64 ticks)
  {
    if (m_rollback_audio_waits)
      m_rollback_audio_pause += ticks;
  }
  // The audio DMA's event for the next `ticks`: true when it waits instead.
  bool TakeRollbackAudioPause(s64 ticks)
  {
    if (m_rollback_audio_pause <= 0)
      return false;
    m_rollback_audio_pause -= ticks;
    return true;
  }
  void SetRollbackSpeedAdjustment(double factor);
  // Re-anchor the throttle at the current time, e.g. after the CPU thread waited on the host for
  // the peer, so the emulator does not race to make up for that wait.
  void ResetThrottleToNow();
  // Gameplay-only rollback (region snapshots, the emulated ticks are not rewound): call before a
  // rollback's load. The next SetRollbackResimulating(false) resumes the throttle at the host time
  // the timeline was due at here, so the load and the re-run cost no wall-clock time when they fit
  // into the frame's slack (without this, every rollback pushed the frames after it back by the
  // re-run's duration). Ignored while the speed is unlimited.
  void BeginRollbackBurst();
  // How far behind its schedule the throttle found the emulation at worst since the last call
  // (zero: never behind). Only throttled time counts, not re-runs.
  DT TakeMaxBehindSchedule();

  // May be used from CPU or GPU thread.
  void SleepUntil(TimePoint time_point);

  // Used by VideoInterface
  bool GetVISkip() const;

  float GetOverclock() const;

  bool UseSyncOnSkipIdle() const;

private:
  Globals m_globals;

  Core::System& m_system;

  // unordered_map stores each element separately as a linked list node so pointers to elements
  // remain stable regardless of rehashes/resizing.
  std::unordered_map<std::string, EventType> m_event_types;

  // STATE_TO_SAVE
  // The queue is a min-heap using std::ranges::make_heap/push_heap/pop_heap.
  // We don't use std::priority_queue because we need to be able to serialize, unserialize and
  // erase arbitrary events (RemoveEvent()) regardless of the queue order. These aren't accommodated
  // by the standard adaptor class.
  std::vector<Event> m_event_queue;
  u64 m_event_fifo_id = 0;
  std::mutex m_ts_write_lock;

  // Event objects created from other threads.
  // The time value of each Event here is a cycles_into_future value.
  Common::SPSCQueue<Event> m_ts_queue;

  float m_last_oc_factor = 0.0f;

  s64 m_idled_cycles = 0;
  u32 m_fake_dec_start_value = 0;
  u64 m_fake_dec_start_ticks = 0;

  // Are we in a function that has been called from Advance()
  bool m_is_global_timer_sane = false;

  EventType* m_ev_lost = nullptr;

  CPUThreadConfigCallback::ConfigChangedCallbackID m_registered_config_callback_id;
  float m_config_oc_factor = 1.0f;
  float m_config_oc_inv_factor = 1.0f;
  bool m_config_sync_on_skip_idle = false;
  bool m_config_rush_frame_presentation = false;
  bool m_config_present_pacing = false;

  std::atomic<bool> m_pace_on{false};  // SetPresentPacing

  s64 m_throttle_reference_cycle = 0;
  TimePoint m_throttle_reference_time = Clock::now();
  u32 m_throttle_adj_clock_per_sec = 0;
  bool m_throttle_disable_vi_int = false;

  DT m_max_fallback = {};
  DT m_max_variance = {};
  bool m_correct_time_drift = false;
  double m_emulation_speed = 1.0;
  bool m_rollback_resimulating = false;
  bool m_rollback_audio_waits = false;
  s64 m_rollback_audio_pause = 0;
  double m_rollback_speed_factor = 1.0;
  bool m_rollback_burst_pending = false;
  TimePoint m_rollback_burst_due{};
  DT m_max_behind_schedule{};

  bool IsSpeedUnlimited() const;
  void UpdateSpeedLimit(s64 cycle, double new_speed);
  void ResetThrottle(s64 cycle);
  TimePoint CalculateTargetHostTimeInternal(s64 target_cycle);
  void UpdateVISkip(TimePoint current_time, TimePoint target_time);

  int DowncountToCycles(int downcount) const;
  int CyclesToDowncount(int cycles) const;

  std::atomic_bool m_use_precision_timer = false;
  Common::PrecisionTimer m_precision_cpu_timer;
  Common::PrecisionTimer m_precision_gpu_timer;

  Common::EventHook m_core_state_changed_hook;
  Common::EventHook m_frame_hook;

  // Used to optionally minimize throttling for improving input latency.
  std::atomic_bool m_throttled_after_presentation = false;
  DT m_max_throttle_skip_time{};
};

}  // namespace CoreTiming
