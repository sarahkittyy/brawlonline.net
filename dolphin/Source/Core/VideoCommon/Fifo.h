// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "Common/BlockingLoop.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/Functional.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}

namespace Fifo
{
// Used for diagnostics.
enum class SyncGPUReason
{
  Other,
  Wraparound,
  EFBPoke,
  PerfQuery,
  BBox,
  Swap,
  AuxSpace,
};

class FifoManager final
{
public:
  explicit FifoManager(Core::System& system);
  FifoManager(const FifoManager& other) = delete;
  FifoManager(FifoManager&& other) = delete;
  FifoManager& operator=(const FifoManager& other) = delete;
  FifoManager& operator=(FifoManager&& other) = delete;
  ~FifoManager();

  void Init();
  void Shutdown();
  void Prepare();  // Must be called from the CPU thread.
  void DoState(PointerWrap& f);
  void PauseAndLock();
  void RestoreState(bool was_running);
  void UpdateWantDeterminism(bool want);
  // Brawl Online: while a gameplay rollback session runs, GPUDeterminismMode "auto" uses the
  // deterministic GPU thread (as it does for netplay and movies). CPU thread only, between frames.
  void SetRollbackSessionDeterminism(bool active);
  bool UseDeterministicGPUThread() const { return m_use_deterministic_gpu_thread; }
  bool UseSyncGPU() const { return m_config_sync_gpu; }

  // In deterministic GPU thread mode this waits for the GPU to be done with pending work.
  void SyncGPU(SyncGPUReason reason, bool may_move_read_ptr = true);

  // In single core mode, this runs the GPU for a single slice.
  // In dual core mode, this synchronizes with the GPU thread.
  void SyncGPUForRegisterAccess();

  void PushFifoAuxBuffer(const void* ptr, size_t size);
  void* PopFifoAuxBuffer(size_t size);

  void FlushGpu();
  void RunGpu();
  void GpuMaySleep();
  // Wake the GPU thread so it services queued work (async requests) now. RunGpu() alone does not
  // in deterministic dual core, where the GPU loop then only notices a request after its sleep
  // times out.
  void WakeGpuThread();
  void RunGpuLoop();
  void ExitGpuLoop();
  void EmulatorState(bool running);
  void ResetVideoBuffer();

  // Deterministic dual core, CPU thread: runs `capture(true)` on the video thread once it has
  // executed every command the CPU has preprocessed so far, i.e. exactly at this point of the
  // command stream, without waiting for it. If the capture is dropped instead (shutdown, a normal
  // savestate load), it is called with false.
  void QueueVideoThreadCapture(Common::MoveOnlyFunction<void(bool)> capture);
  // Waits until the video thread is idle: every queued command and capture has run (unless the
  // emulation is paused, when the video thread does nothing).
  void WaitForGpuThreadIdle();
  // Drops the captures that have not run yet, calling each with false.
  void DropVideoThreadCaptures();
  // The video state the CPU thread owns in deterministic dual core (see VideoState.h). On load the
  // FIFO bytes are only stored; ApplyRollbackFifo, on the video thread with the CPU thread waiting
  // (or with the video thread synced and not running), puts them in place.
  void DoStateRollbackCPU(PointerWrap& p);
  void ApplyRollbackFifo();

private:
  struct VideoThreadCapture
  {
    u8* position = nullptr;  // in m_video_buffer
    Common::MoveOnlyFunction<void(bool)> capture;
  };
  void RunVideoThreadCaptures(const u8* write_ptr);

  void RefreshConfig();
  void ReadDataFromFifo(u32 read_ptr);
  void ReadDataFromFifoOnCPU(u32 read_ptr);
  int RunGpuOnCpu(int ticks);
  int WaitForGpuThread(int ticks);
  static void SyncGPUCallback(Core::System& system, u64 ticks, s64 cyclesLate);

  static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;

  Common::BlockingLoop m_gpu_mainloop;

  Common::Flag m_emu_running_state;

  // Most of this array is unlikely to be faulted in...
  u8 m_fifo_aux_data[FIFO_SIZE]{};
  u8* m_fifo_aux_write_ptr = nullptr;
  u8* m_fifo_aux_read_ptr = nullptr;

  // This could be in SConfig, but it depends on multiple settings
  // and can change at runtime.
  bool m_use_deterministic_gpu_thread = false;
  bool m_want_determinism = false;
  bool m_rollback_session_determinism = false;

  CoreTiming::EventType* m_event_sync_gpu = nullptr;

  // STATE_TO_SAVE
  u8* m_video_buffer = nullptr;
  u8* m_video_buffer_read_ptr = nullptr;
  std::atomic<u8*> m_video_buffer_write_ptr = nullptr;
  std::atomic<u8*> m_video_buffer_seen_ptr = nullptr;
  u8* m_video_buffer_pp_read_ptr = nullptr;
  // The read_ptr is always owned by the GPU thread.  In normal mode, so is the
  // write_ptr, despite it being atomic.  In deterministic GPU thread mode,
  // things get a bit more complicated:
  // - The seen_ptr is written by the GPU thread, and points to what it's already
  // processed as much of as possible - in the case of a partial command which
  // caused it to stop, not the same as the read ptr.  It's written by the GPU,
  // under the lock, and updating the cond.
  // - The write_ptr is written by the CPU thread after it copies data from the
  // FIFO.  Maybe someday it will be under the lock.  For now, because RunGpuLoop
  // polls, it's just atomic.
  // - The pp_read_ptr is the CPU preprocessing version of the read_ptr.

  // Queued by the CPU thread, run by the video thread (QueueVideoThreadCapture).
  std::mutex m_capture_mutex;      // guards m_captures and the writes of m_captures_pending
  std::mutex m_capture_run_mutex;  // held while a capture runs
  std::deque<VideoThreadCapture> m_captures;
  std::atomic<bool> m_captures_pending = false;

  // Rollback load: the CPU part's FIFO bytes, waiting for ApplyRollbackFifo.
  std::vector<u8> m_rollback_fifo_tail;
  bool m_rollback_fifo_pending = false;

  std::atomic<int> m_sync_ticks = 0;
  bool m_syncing_suspended = false;
  Common::Event m_sync_wakeup_event;

  std::optional<Config::ConfigChangedCallbackID> m_config_callback_id = std::nullopt;
  bool m_config_sync_gpu = false;
  int m_config_sync_gpu_max_distance = 0;
  int m_config_sync_gpu_min_distance = 0;
  float m_config_sync_gpu_overclock = 0.0f;

  Core::System& m_system;
};

bool AtBreakpoint(Core::System& system);
}  // namespace Fifo
