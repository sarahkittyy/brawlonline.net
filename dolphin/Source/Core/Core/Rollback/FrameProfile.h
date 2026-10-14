// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <string_view>

#include "Common/CommonTypes.h"

// Where a gameplay rollback session's frames spend their wall-clock time, re-run and presented
// frames apart (docs/gameplay-rollback-status.md, "Frame cost profile"). Counters only, no
// per-frame logging: GameplaySession takes their differences around each pass.
//
// - CoreTiming events by category (CPU thread), without the waits for the GPU thread and the
//   throttle's sleep that happen inside them (those are counted on their own).
// - The CPU thread's waits for the GPU thread (FifoManager::SyncGPU and friends), by reason.
// - The GPU thread's time executing commands (deterministic dual core: OpcodeDecoder::RunFifo),
//   and of that, presenting (the presenter's hold and the swap).
//
// Off outside sessions; GameplaySession turns it on while one runs. A counter costs two clock
// reads where it is taken.
namespace Rollback::FrameProfile
{
enum class EventCat : u8
{
  Gpu,      // SyncGPUCallback (the deterministic GPU thread's preprocessing), CP/PE interrupts
  Vi,       // VICallback
  Audio,    // AI, audio DMA, DSP, ARAM
  Ios,      // IPC, IOS, DVD/DI completions
  Devices,  // SI, EXI and the rest
  Count
};
constexpr size_t EVENT_CATS = static_cast<size_t>(EventCat::Count);

// FifoManager::SyncGPU's reasons (Fifo::SyncGPUReason, same order), then FlushGpu and
// WaitForGpuThreadIdle.
constexpr size_t WAIT_REASONS = 9;
constexpr std::array<std::string_view, WAIT_REASONS> WAIT_REASON_NAMES = {
    "other", "wraparound", "efb poke", "perf query", "bbox", "swap", "aux space", "flush", "idle"};
constexpr size_t WAIT_FLUSH = 7;
constexpr size_t WAIT_IDLE = 8;

EventCat CategoryOf(std::string_view event_name);

struct Counters
{
  std::array<u64, EVENT_CATS> event_ns{};
  u64 events = 0;
  u64 throttle_ns = 0;
  std::array<u64, WAIT_REASONS> gpu_wait_ns{};
  u64 gpu_waits = 0;
  u64 gpu_busy_ns = 0;     // GPU thread executing commands
  u64 gpu_present_ns = 0;  // of that, presenting (hold + swap)
};

extern std::atomic<bool> g_enabled;
inline bool Enabled()
{
  return g_enabled.load(std::memory_order_relaxed);
}
void SetEnabled(bool enabled);

// CoreTiming events are timed only in some passes (a frame runs about a thousand, and two clock
// reads each are a few percent of it); GameplaySession picks them and scales their sums.
extern bool g_time_events;  // CPU thread
inline bool TimeEvents()
{
  return g_time_events;
}
inline void SetTimeEvents(bool on)
{
  g_time_events = on;
}

// CPU thread (CoreTiming::Advance, Throttle).
void AddEvent(EventCat cat, u64 ns);
void AddThrottle(u64 ns);
// Any thread that waits for the GPU thread (normally the CPU thread).
void AddGpuWait(size_t reason, u64 ns);
// GPU thread.
void AddGpuBusy(u64 ns);
void AddGpuPresent(u64 ns);

// The waits for the GPU thread and the throttle's sleep so far, for taking them out of the events
// they happen in.
u64 NestedNs();

// A copy of all counters (the CPU thread's are exact on the CPU thread).
Counters Snapshot();
Counters Delta(const Counters& now, const Counters& then);
void Accumulate(Counters* into, const Counters& d);

inline u64 NsSince(std::chrono::steady_clock::time_point t0)
{
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count());
}

// RAII timer for the hooks: times the scope only while the profile is on.
template <typename F>
class Scope final
{
public:
  explicit Scope(F add) : m_add(add), m_on(Enabled())
  {
    if (m_on)
      m_t0 = std::chrono::steady_clock::now();
  }
  ~Scope()
  {
    if (m_on)
      m_add(NsSince(m_t0));
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  F m_add;
  bool m_on;
  std::chrono::steady_clock::time_point m_t0{};
};
}  // namespace Rollback::FrameProfile
