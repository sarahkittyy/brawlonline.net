// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/PresentStats.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

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
std::mutex s_copy_decisions_mutex;
std::deque<bool> s_copy_decisions;
std::atomic<u64> s_copy_decision_misses{0};

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
  std::lock_guard lk(s_copy_decisions_mutex);
  s_copy_decisions.push_back(present);
}

bool OnEFBCopy(bool to_xfb, bool preprocessed)
{
  if (!preprocessed)
    return !to_xfb || DecideXFBCopy();

  std::lock_guard lk(s_copy_decisions_mutex);
  if (s_copy_decisions.empty())
  {
    s_copy_decision_misses.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  const bool present = s_copy_decisions.front();
  s_copy_decisions.pop_front();
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
  s_displayed_frames.fetch_add(1, std::memory_order_relaxed);
  if (after_resimulation)
    s_displayed_frames_after_resim.fetch_add(1, std::memory_order_relaxed);
}

void OnPresent(bool duplicate)
{
  s_presents.fetch_add(1, std::memory_order_relaxed);
  if (duplicate)
    s_duplicate_presents.fetch_add(1, std::memory_order_relaxed);
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
