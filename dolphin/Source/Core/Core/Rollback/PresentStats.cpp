// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/PresentStats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "Common/Swap.h"
#include "Core/HW/Memmap.h"

#include "Core/CoreTiming.h"
#include "Core/System.h"
#include "VideoCommon/VideoConfig.h"

namespace Rollback::PresentStats
{
namespace
{
std::atomic<u64> s_xfb_fields{0};
std::atomic<u64> s_xfb_fields_skipped{0};
std::atomic<u64> s_xfb_copies{0};
std::atomic<u64> s_xfb_copies_skipped{0};
std::atomic<u64> s_presents{0};
std::atomic<u64> s_duplicate_presents{0};
std::atomic<u64> s_displayed_frames{0};
std::atomic<u64> s_displayed_frames_after_resim{0};
std::array<std::atomic<u64>, HISTOGRAM_SIZE> s_outputs_per_frame{};
std::array<std::atomic<u64>, HISTOGRAM_SIZE> s_outputs_per_frame_after_resim{};

std::atomic<u64> s_outputs_since_displayed_frame{0};
bool s_seen_displayed_frame = false;  // CPU thread

// Deterministic dual core: decisions made by the FIFO preprocessor on the CPU thread, taken by the
// GPU thread in the same command order.
struct CopyDecision
{
  bool present;
  bool paced;     // CoreTiming's present pacing was on
  TimePoint due;  // paced: the copy's due time on the throttle's clock
};
std::mutex s_copy_decisions_mutex;
std::deque<CopyDecision> s_copy_decisions;
// Video thread: the presented copy's decision, for PresentHoldUntil.
CopyDecision s_gpu_copy{};
bool s_gpu_copy_pending = false;
double s_last_hold_ms = 0;  // video thread: the hold of the frame being presented
std::atomic<u64> s_copy_decision_misses{0};

// PPR_PRESENT_LOG
struct CpuStamp
{
  TimePoint copy;
  TimePoint due;  // the throttle's host time for the copy's emulated ticks
  TimePoint throttle_target, throttle_woke;  // the last throttle that slept before the copy
  int throttles = 0;                         // throttles that slept since the previous copy
  TimePoint loop_top;                        // the last displayed frame's start before the copy
  double gpu_wait_ms = 0;
  int bursts = 0;
  double burst_behind_ms = 0;
};
std::mutex s_trace_mutex;
std::deque<CpuStamp> s_cpu_stamps;  // XFB copies preprocessed, not yet executed by the GPU thread
CpuStamp s_cpu_pending;             // CPU thread: since the previous presented XFB copy
CpuStamp s_gpu_stamp;               // GPU thread: the copy being presented
bool s_gpu_have_stamp = false;
TimePoint s_swap_start, s_intended, s_target, s_before_sleep, s_last_done, s_trace_origin;
bool s_have_target = false, s_have_origin = false;
u64 s_trace_index = 0;

bool PresentResimulated()
{
  static const bool value = [] {
    const char* env = std::getenv("PPR_ROLLBACK_PRESENT_RESIM");
    return env && std::strcmp(env, "1") == 0;
  }();
  return value;
}

bool IsResimulating()
{
  return Core::System::GetInstance().GetCoreTiming().IsRollbackResimulating();
}

// CPU thread (or the GPU thread in non-deterministic dual core, where it is approximate).
bool DecideXFBCopy()
{
  s_xfb_copies.fetch_add(1, std::memory_order_relaxed);
  if (IsResimulating() && !PresentResimulated())
  {
    s_xfb_copies_skipped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (g_ActiveConfig.bImmediateXFB)
    s_outputs_since_displayed_frame.fetch_add(1, std::memory_order_relaxed);
  return true;
}
}  // namespace

bool OnXFBField(bool resimulating)
{
  s_xfb_fields.fetch_add(1, std::memory_order_relaxed);
  if (resimulating && !PresentResimulated())
  {
    s_xfb_fields_skipped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!g_ActiveConfig.bImmediateXFB)
    s_outputs_since_displayed_frame.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void OnEFBCopyPreprocess(bool to_xfb)
{
  const bool present = !to_xfb || DecideXFBCopy();
  if (to_xfb && present)
    Core::System::GetInstance().GetCoreTiming().OnPresentedXFBCopy();
  if (to_xfb && present && PresentTraceEnabled())
  {
    s_cpu_pending.copy = Clock::now();
    const CpuStamp last = s_cpu_pending;
    auto& core_timing = Core::System::GetInstance().GetCoreTiming();
    s_cpu_pending.due = core_timing.GetTargetHostTime(core_timing.GetTicks());
    std::lock_guard lk(s_trace_mutex);
    s_cpu_stamps.push_back(std::exchange(s_cpu_pending, {}));
    // A copy without a throttle in between keeps the previous one's.
    s_cpu_pending.throttle_target = last.throttle_target;
    s_cpu_pending.throttle_woke = last.throttle_woke;
    s_cpu_pending.loop_top = last.loop_top;
  }
  CopyDecision decision{present, false, {}};
  if (to_xfb && present)
  {
    auto& core_timing = Core::System::GetInstance().GetCoreTiming();
    if (core_timing.PresentPacingActive())
    {
      decision.paced = true;
      decision.due = core_timing.GetTargetHostTime(core_timing.GetTicks());
    }
  }
  std::lock_guard lk(s_copy_decisions_mutex);
  s_copy_decisions.push_back(decision);
}

bool OnEFBCopy(bool to_xfb, bool preprocessed)
{
  if (!preprocessed)
  {
    const bool present = !to_xfb || DecideXFBCopy();
    // Single core: the copy runs on the CPU thread.
    if (to_xfb && present && !Core::System::GetInstance().IsDualCoreMode())
      Core::System::GetInstance().GetCoreTiming().OnPresentedXFBCopy();
    return present;
  }

  bool present = true;
  {
    std::lock_guard lk(s_copy_decisions_mutex);
    if (s_copy_decisions.empty())
    {
      s_copy_decision_misses.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    const CopyDecision decision = s_copy_decisions.front();
    s_copy_decisions.pop_front();
    present = decision.present;
    if (to_xfb && present)
    {
      s_gpu_copy = decision;
      s_gpu_copy_pending = true;
    }
  }
  if (to_xfb && present && PresentTraceEnabled())
  {
    std::lock_guard lk(s_trace_mutex);
    if (!s_cpu_stamps.empty())
    {
      s_gpu_stamp = s_cpu_stamps.front();
      s_cpu_stamps.pop_front();
      s_gpu_have_stamp = true;
    }
  }
  return present;
}

void OnDisplayedFrameStart(bool after_resimulation)
{
  // What was sent to the presenter since the previous displayed frame started: that frame's own
  // output plus any of the resimulated frames in between.
  const u64 outputs = s_outputs_since_displayed_frame.exchange(0, std::memory_order_relaxed);
  if (s_seen_displayed_frame)
  {
    const size_t bucket = std::min<u64>(outputs, HISTOGRAM_SIZE - 1);
    s_outputs_per_frame[bucket].fetch_add(1, std::memory_order_relaxed);
    if (after_resimulation)
      s_outputs_per_frame_after_resim[bucket].fetch_add(1, std::memory_order_relaxed);
  }
  s_seen_displayed_frame = true;
  if (PresentTraceEnabled())
    s_cpu_pending.loop_top = Clock::now();
  s_displayed_frames.fetch_add(1, std::memory_order_relaxed);
  if (after_resimulation)
    s_displayed_frames_after_resim.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
constexpr double REFRESH_MS = 1000.0 / 59.94;
constexpr int PHASES = 4;
std::mutex s_cadence_mutex;
Cadence s_cadence;
bool s_have_last_present = false;
std::chrono::steady_clock::time_point s_cadence_origin, s_last_present;
std::array<s64, PHASES> s_last_bucket{};
u64 s_cadence_hitches = 0;  // summed over the phases

void RecordPresentCadence()
{
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lk(s_cadence_mutex);
  if (!s_have_last_present)
  {
    s_have_last_present = true;
    s_cadence_origin = now;
  }
  const double t = std::chrono::duration<double, std::milli>(now - s_cadence_origin).count();
  const bool first = s_last_present == std::chrono::steady_clock::time_point{};
  const double ms =
      first ? 0.0 : std::chrono::duration<double, std::milli>(now - s_last_present).count();
  // The first present, or one after a gap of over 0.5 s (menus, a load): no interval to judge.
  const bool judged = !first && ms < 500;
  for (int p = 0; p < PHASES; ++p)
  {
    const s64 bucket = static_cast<s64>(t / REFRESH_MS + static_cast<double>(p) / PHASES);
    if (judged && bucket - s_last_bucket[p] != 1)
      ++s_cadence_hitches;
    s_last_bucket[p] = bucket;
  }
  if (s_last_hold_ms > 0)
  {
    ++s_cadence.holds;
    s_cadence.hold_ms_sum += s_last_hold_ms;
    s_cadence.hold_ms_max = std::max(s_cadence.hold_ms_max, s_last_hold_ms);
    s_last_hold_ms = 0;
  }
  if (judged)
  {
    s_cadence.interval_ms_sum += ms;
    s_cadence.interval_ms_sq += ms * ms;
    s_cadence.interval_ms_max = std::max(s_cadence.interval_ms_max, ms);
    ++s_cadence.presents;
  }
  s_last_present = now;
}


std::FILE* PresentTraceFile()
{
  static std::FILE* const file = [] {
    // "{pid}" in the path becomes the process ID (two instances on one machine).
    const char* env = std::getenv("PPR_PRESENT_LOG");
    std::string path = env ? env : "";
    if (const size_t at = path.find("{pid}"); at != std::string::npos)
      #ifdef _WIN32
      path.replace(at, 5, std::to_string(_getpid()));
#else
      path.replace(at, 5, std::to_string(getpid()));
#endif
    std::FILE* f = !path.empty() ? std::fopen(path.c_str(), "w") : nullptr;
    if (f)
    {
      std::fprintf(f, "index copy_ms due_ms throttle_target_ms throttle_woke_ms throttles loop_top_ms gpu_wait_ms bursts burst_behind_ms swap_ms ready_ms "
                      "intended_ms target_ms done_ms interval_ms\n");
    }
    return f;
  }();
  return file;
}

double Ms(TimePoint a, TimePoint b)
{
  return std::chrono::duration<double, std::milli>(a - b).count();
}

void TracePresent()
{
  std::FILE* f = PresentTraceFile();
  if (!s_gpu_have_stamp)
    return;
  const TimePoint done = Clock::now();
  const CpuStamp& c = s_gpu_stamp;
  if (!s_have_origin)
  {
    s_have_origin = true;
    s_trace_origin = c.copy;
  }
  const double interval = s_trace_index ? Ms(done, s_last_done) : 0.0;
  std::fprintf(f, "%llu %.3f %.3f %.3f %.3f %d %.3f %.3f %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
               static_cast<unsigned long long>(s_trace_index), Ms(c.copy, s_trace_origin),
               Ms(c.due, c.copy), Ms(c.throttle_target, c.copy), Ms(c.throttle_woke, c.copy),
               c.throttles, Ms(c.loop_top, c.copy), c.gpu_wait_ms, c.bursts, c.burst_behind_ms, Ms(s_swap_start, c.copy),
               s_have_target ? Ms(s_before_sleep, c.copy) : -1.0,
               s_have_target ? Ms(s_intended, c.copy) : -1.0,
               s_have_target ? Ms(s_target, c.copy) : -1.0, Ms(done, c.copy), interval);
  if (++s_trace_index % 600 == 0)
    std::fflush(f);
  s_last_done = done;
  s_gpu_have_stamp = false;
  s_have_target = false;
}
}  // namespace

namespace
{
// PresentHoldUntil: the offset is the 95th percentile of the last HOLD_WINDOW frames' arrivals
// (a stall leaves it within about two seconds), and a hold never exceeds HOLD_MAX.
constexpr double HOLD_QUANTILE = 0.95;
constexpr size_t HOLD_WINDOW = 120;
constexpr size_t HOLD_MIN_SAMPLES = 20;
constexpr auto HOLD_MAX = std::chrono::milliseconds{8};
std::array<double, HOLD_WINDOW> s_hold_arrivals{};  // ms after the due time, a ring
size_t s_hold_count = 0;
}  // namespace

TimePoint PresentHoldUntil(TimePoint now)
{
  if (!std::exchange(s_gpu_copy_pending, false) || !s_gpu_copy.paced)
  {
    s_hold_count = 0;
    return now;
  }
  const double arrival_ms = std::chrono::duration<double, std::milli>(now - s_gpu_copy.due).count();
  TimePoint until = now;
  const size_t n = std::min(s_hold_count, HOLD_WINDOW);
  if (n >= HOLD_MIN_SAMPLES)
  {
    std::array<double, HOLD_WINDOW> sorted = s_hold_arrivals;
    const auto nth = sorted.begin() + static_cast<ptrdiff_t>(HOLD_QUANTILE * (n - 1));
    std::nth_element(sorted.begin(), nth, sorted.begin() + n);
    const TimePoint target = s_gpu_copy.due + std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::duration<double, std::milli>(*nth));
    until = std::clamp(target, now, now + HOLD_MAX);
  }
  s_hold_arrivals[s_hold_count++ % HOLD_WINDOW] = arrival_ms;
  s_last_hold_ms = std::chrono::duration<double, std::milli>(until - now).count();
  return until;
}

bool PresentTraceEnabled()
{
  return PresentTraceFile() != nullptr;
}

void TraceCpuWaitForGpu(double ms)
{
  s_cpu_pending.gpu_wait_ms += ms;
}

void TraceRollbackBurstEnd(double behind_ms)
{
  ++s_cpu_pending.bursts;
  s_cpu_pending.burst_behind_ms = std::max(s_cpu_pending.burst_behind_ms, behind_ms);
}

void TraceThrottle(TimePoint target, TimePoint woke)
{
  s_cpu_pending.throttle_target = target;
  s_cpu_pending.throttle_woke = woke;
  ++s_cpu_pending.throttles;
}

void TraceSwapStart()
{
  if (PresentTraceEnabled())
    s_swap_start = Clock::now();
}

void TracePresentTarget(TimePoint intended, TimePoint target)
{
  if (!PresentTraceEnabled())
    return;
  s_intended = intended;
  s_target = target;
  s_before_sleep = Clock::now();
  s_have_target = true;
}

void OnPresent(bool duplicate)
{
  s_presents.fetch_add(1, std::memory_order_relaxed);
  if (duplicate)
    s_duplicate_presents.fetch_add(1, std::memory_order_relaxed);
  else
    RecordPresentCadence();
  if (!duplicate && PresentTraceEnabled())
    TracePresent();
}

Cadence TakeCadence()
{
  std::lock_guard lk(s_cadence_mutex);
  Cadence c = s_cadence;
  c.hitches = static_cast<double>(s_cadence_hitches) / PHASES;
  s_cadence = {};
  s_cadence_hitches = 0;
  return c;
}

Stats Get()
{
  Stats stats;
  stats.present_resimulated = PresentResimulated();
  stats.xfb_fields = s_xfb_fields.load(std::memory_order_relaxed);
  stats.xfb_fields_skipped = s_xfb_fields_skipped.load(std::memory_order_relaxed);
  stats.xfb_copies = s_xfb_copies.load(std::memory_order_relaxed);
  stats.xfb_copies_skipped = s_xfb_copies_skipped.load(std::memory_order_relaxed);
  stats.copy_decision_misses = s_copy_decision_misses.load(std::memory_order_relaxed);
  stats.presents = s_presents.load(std::memory_order_relaxed);
  stats.duplicate_presents = s_duplicate_presents.load(std::memory_order_relaxed);
  stats.displayed_frames = s_displayed_frames.load(std::memory_order_relaxed);
  stats.displayed_frames_after_resim =
      s_displayed_frames_after_resim.load(std::memory_order_relaxed);
  for (int i = 0; i < HISTOGRAM_SIZE; ++i)
  {
    stats.outputs_per_frame[i] = s_outputs_per_frame[i].load(std::memory_order_relaxed);
    stats.outputs_per_frame_after_resim[i] =
        s_outputs_per_frame_after_resim[i].load(std::memory_order_relaxed);
  }
  return stats;
}

namespace
{
std::atomic<u64> s_gpu_efb_copies{0};
std::atomic<u64> s_gpu_efb_copies_deferred{0};
std::atomic<u64> s_gpu_efb_copy_flushes{0};
std::atomic<u64> s_gpu_xfb_copies{0};
std::atomic<u64> s_gpu_fills{0};
std::atomic<u64> s_gpu_ram_bytes{0};
std::atomic<u64> s_gpu_readback_us_total{0};
std::atomic<u64> s_gpu_readback_us_max{0};
std::mutex s_gpu_log_mutex;

std::FILE* GpuRamWriteLog()
{
  static std::FILE* const file = [] {
    const char* path = std::getenv("PPR_LOG_GPU_RAM_WRITES");
    return path && *path ? std::fopen(path, "a") : nullptr;
  }();
  return file;
}
}  // namespace

void OnGpuRamWrite(GpuRamWriteKind kind, u32 address, u32 size)
{
  switch (kind)
  {
  case GpuRamWriteKind::EFBCopy:
    s_gpu_efb_copies.fetch_add(1, std::memory_order_relaxed);
    break;
  case GpuRamWriteKind::EFBCopyDeferred:
    s_gpu_efb_copies_deferred.fetch_add(1, std::memory_order_relaxed);
    return;  // the bytes are counted by the flush
  case GpuRamWriteKind::EFBCopyFlush:
    s_gpu_efb_copy_flushes.fetch_add(1, std::memory_order_relaxed);
    break;
  case GpuRamWriteKind::XFBCopy:
    s_gpu_xfb_copies.fetch_add(1, std::memory_order_relaxed);
    break;
  case GpuRamWriteKind::Fill:
    s_gpu_fills.fetch_add(1, std::memory_order_relaxed);
    break;
  }
  s_gpu_ram_bytes.fetch_add(size, std::memory_order_relaxed);

  if (std::FILE* log = GpuRamWriteLog())
  {
    // Brawl's g_GameFrame-style match frame counter (MEM2 0x901812a4), read racily: a hint of when.
    u32 frame = 0;
    auto& memory = Core::System::GetInstance().GetMemory();
    if (const u8* p = memory.GetPointerForRange(0x901812a4, 4))
    {
      std::memcpy(&frame, p, 4);
      frame = Common::swap32(frame);
    }
    static constexpr const char* names[] = {"efb", "efb_deferred", "efb_flush", "xfb", "fill"};
    std::lock_guard lk(s_gpu_log_mutex);
    std::fprintf(log, "%u %s %08x %u\n", frame, names[static_cast<int>(kind)], address, size);
    std::fflush(log);
  }
}

void OnGpuReadback(u64 us)
{
  s_gpu_readback_us_total.fetch_add(us, std::memory_order_relaxed);
  if (us > s_gpu_readback_us_max.load(std::memory_order_relaxed))
    s_gpu_readback_us_max.store(us, std::memory_order_relaxed);
}

GpuRamWriteStats GetGpuRamWrites()
{
  GpuRamWriteStats stats;
  stats.efb_copies = s_gpu_efb_copies.load(std::memory_order_relaxed);
  stats.efb_copies_deferred = s_gpu_efb_copies_deferred.load(std::memory_order_relaxed);
  stats.efb_copy_flushes = s_gpu_efb_copy_flushes.load(std::memory_order_relaxed);
  stats.xfb_copies = s_gpu_xfb_copies.load(std::memory_order_relaxed);
  stats.fills = s_gpu_fills.load(std::memory_order_relaxed);
  stats.bytes = s_gpu_ram_bytes.load(std::memory_order_relaxed);
  stats.readback_us_total = s_gpu_readback_us_total.load(std::memory_order_relaxed);
  stats.readback_us_max = s_gpu_readback_us_max.load(std::memory_order_relaxed);
  return stats;
}

void Reset()
{
  s_gpu_efb_copies = 0;
  s_gpu_efb_copies_deferred = 0;
  s_gpu_efb_copy_flushes = 0;
  s_gpu_xfb_copies = 0;
  s_gpu_fills = 0;
  s_gpu_ram_bytes = 0;
  s_gpu_readback_us_total = 0;
  s_gpu_readback_us_max = 0;
  s_xfb_fields = 0;
  s_xfb_fields_skipped = 0;
  s_xfb_copies = 0;
  s_xfb_copies_skipped = 0;
  s_copy_decision_misses = 0;
  s_presents = 0;
  s_duplicate_presents = 0;
  s_displayed_frames = 0;
  s_displayed_frames_after_resim = 0;
  for (int i = 0; i < HISTOGRAM_SIZE; ++i)
  {
    s_outputs_per_frame[i] = 0;
    s_outputs_per_frame_after_resim[i] = 0;
  }
  s_outputs_since_displayed_frame = 0;
  s_seen_displayed_frame = false;
  std::lock_guard lk(s_copy_decisions_mutex);
  s_copy_decisions.clear();
}
}  // namespace Rollback::PresentStats
