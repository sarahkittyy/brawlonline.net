// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/FrameProfile.h"

#include <algorithm>

namespace Rollback::FrameProfile
{
std::atomic<bool> g_enabled{false};
bool g_time_events = false;

namespace
{
// Single writer (the CPU thread): a relaxed load and store, no locked add.
struct alignas(64) CpuCounters
{
  std::array<std::atomic<u64>, EVENT_CATS> event_ns{};
  std::atomic<u64> events{0};
  std::atomic<u64> throttle_ns{0};
  std::atomic<u64> jit_ns{0};
  std::atomic<u64> jit_blocks{0};
};
struct alignas(64) WaitCounters
{
  std::array<std::atomic<u64>, WAIT_REASONS> ns{};
  std::atomic<u64> count{0};
  std::atomic<u64> total_ns{0};
};
struct alignas(64) GpuCounters
{
  std::atomic<u64> busy_ns{0};
  std::atomic<u64> present_ns{0};
};

CpuCounters s_cpu;
WaitCounters s_wait;
GpuCounters s_gpu;

void Bump(std::atomic<u64>& c, u64 v)
{
  c.store(c.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

bool StartsWith(std::string_view s, std::string_view p)
{
  return s.substr(0, p.size()) == p;
}
}  // namespace

EventCat CategoryOf(std::string_view n)
{
  if (n == "SyncGPUCallback" || n == "CPInterrupt" || n == "SetTokenFinish" || n == "GPUSleeper")
    return EventCat::Gpu;
  if (n == "VICallback")
    return EventCat::Vi;
  if (n == "AICallback" || n == "AudioDMACallback" || n == "DSPCallback" || n == "DSPint" ||
      n == "ARAMint")
  {
    return EventCat::Audio;
  }
  if (StartsWith(n, "IPC") || StartsWith(n, "IOS") || n == "FinishExecutingCommand" ||
      n == "FinishDICommand" || n == "FinishReadDVDThread")
  {
    return EventCat::Ios;
  }
  return EventCat::Devices;
}

void SetEnabled(bool enabled)
{
  g_enabled.store(enabled, std::memory_order_relaxed);
  if (!enabled)
    g_time_events = false;
}

void AddEvent(EventCat cat, u64 ns)
{
  Bump(s_cpu.event_ns[static_cast<size_t>(cat)], ns);
  Bump(s_cpu.events, 1);
}

void AddThrottle(u64 ns)
{
  Bump(s_cpu.throttle_ns, ns);
}

void AddJit(u64 ns)
{
  Bump(s_cpu.jit_ns, ns);
  Bump(s_cpu.jit_blocks, 1);
}

void AddGpuWait(size_t reason, u64 ns)
{
  s_wait.ns[std::min(reason, WAIT_REASONS - 1)].fetch_add(ns, std::memory_order_relaxed);
  s_wait.count.fetch_add(1, std::memory_order_relaxed);
  s_wait.total_ns.fetch_add(ns, std::memory_order_relaxed);
}

u64 NestedNs()
{
  return s_wait.total_ns.load(std::memory_order_relaxed) +
         s_cpu.throttle_ns.load(std::memory_order_relaxed);
}

void AddGpuBusy(u64 ns)
{
  Bump(s_gpu.busy_ns, ns);
}

void AddGpuPresent(u64 ns)
{
  Bump(s_gpu.present_ns, ns);
}

Counters Snapshot()
{
  Counters c;
  for (size_t i = 0; i < EVENT_CATS; ++i)
    c.event_ns[i] = s_cpu.event_ns[i].load(std::memory_order_relaxed);
  c.events = s_cpu.events.load(std::memory_order_relaxed);
  c.throttle_ns = s_cpu.throttle_ns.load(std::memory_order_relaxed);
  for (size_t i = 0; i < WAIT_REASONS; ++i)
    c.gpu_wait_ns[i] = s_wait.ns[i].load(std::memory_order_relaxed);
  c.gpu_waits = s_wait.count.load(std::memory_order_relaxed);
  c.gpu_busy_ns = s_gpu.busy_ns.load(std::memory_order_relaxed);
  c.gpu_present_ns = s_gpu.present_ns.load(std::memory_order_relaxed);
  c.jit_ns = s_cpu.jit_ns.load(std::memory_order_relaxed);
  c.jit_blocks = s_cpu.jit_blocks.load(std::memory_order_relaxed);
  return c;
}

Counters Delta(const Counters& now, const Counters& then)
{
  Counters d;
  for (size_t i = 0; i < EVENT_CATS; ++i)
    d.event_ns[i] = now.event_ns[i] - then.event_ns[i];
  d.events = now.events - then.events;
  d.throttle_ns = now.throttle_ns - then.throttle_ns;
  for (size_t i = 0; i < WAIT_REASONS; ++i)
    d.gpu_wait_ns[i] = now.gpu_wait_ns[i] - then.gpu_wait_ns[i];
  d.gpu_waits = now.gpu_waits - then.gpu_waits;
  d.gpu_busy_ns = now.gpu_busy_ns - then.gpu_busy_ns;
  d.gpu_present_ns = now.gpu_present_ns - then.gpu_present_ns;
  d.jit_ns = now.jit_ns - then.jit_ns;
  d.jit_blocks = now.jit_blocks - then.jit_blocks;
  return d;
}

void Accumulate(Counters* into, const Counters& d)
{
  for (size_t i = 0; i < EVENT_CATS; ++i)
    into->event_ns[i] += d.event_ns[i];
  into->events += d.events;
  into->throttle_ns += d.throttle_ns;
  for (size_t i = 0; i < WAIT_REASONS; ++i)
    into->gpu_wait_ns[i] += d.gpu_wait_ns[i];
  into->gpu_waits += d.gpu_waits;
  into->gpu_busy_ns += d.gpu_busy_ns;
  into->gpu_present_ns += d.gpu_present_ns;
  into->jit_ns += d.jit_ns;
  into->jit_blocks += d.jit_blocks;
}
}  // namespace Rollback::FrameProfile
