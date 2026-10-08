// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "Core/Brawlback/include/brawlback-common/BrawlbackConstants.h"
#include "Core/Rollback/DeltaSaveSlot.h"
#include "job.h"

// Set to 1 to enable full-RAM shadow snapshots for rollback validation
#define ROLLBACK_VALIDATE 0
#define BRAWLBACK_DESYNC_DETECTION 1

namespace Core
{
class System;
}

namespace Rollback
{
static constexpr int NUM_SAVE_SLOTS = MAX_ROLLBACK_FRAMES + 1;  // MAX_ROLLBACK_FRAMES + 1
static constexpr int ROLLBACK_NUM_HELPER_THREADS = 5 + 1;  // plus one extra for eviction job
static constexpr u32 SAVESTATE_NUM_WORK_CHUNKS = 5;
class RollbackManager
{
public:
  RollbackManager() = default;
  ~RollbackManager() = default;

  RollbackManager(const RollbackManager&) = delete;
  RollbackManager& operator=(const RollbackManager&) = delete;
  RollbackManager(RollbackManager&&) = delete;
  RollbackManager& operator=(RollbackManager&&) = delete;

  static RollbackManager& Get();

  void Init(Core::System& system);
  void Shutdown();

  void SaveFrame(Core::System& system);
  bool LoadFrame(Core::System& system, int frames_back = 1);

  void AddExcludeRegion(uint32_t virt_addr, uint32_t size_bytes);
  // The game<->Dolphin mailbox (Online/GameBridge.h): never part of rollback state. Kept across
  // Init(); size 0 removes it. CPU thread only.
  void SetMailboxRegion(uint32_t virt_addr, uint32_t size_bytes);

  // Gameplay-only rollback (Core/Rollback/GameplayRollback.h): saves and loads cover only the
  // granules inside `ranges` (effective addresses), plus the CPU registers. No device, timing or
  // CPU-slice state is saved or restored: emulated time keeps running forward through a rollback,
  // as in Slippi. Takes effect from the next save; resets the ring and the base snapshot.
  // `exclude`: bytes never restored even where they share a granule with the set.
  void BeginRegionMode(const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                       const std::vector<std::pair<uint32_t, uint32_t>>& exclude = {});
  void EndRegionMode();
  bool IsRegionMode() const { return m_region_mode; }
  // Bytes of address space in the region set.
  u64 RegionBytes() const { return m_region_bytes; }
  // 1 per granule (MEM1 granules, then MEM2 granules) inside the region set.
  const u8* RegionMask() const { return m_region_mode ? m_region_mask.data() : nullptr; }

  bool IsInitialized() const { return m_initialized; }

  void ToggleFrameSave();

  // Wall-clock cost of SaveFrame/LoadFrame since frame saving was last enabled (for the harness).
  struct TimingStats
  {
    u64 save_count = 0, save_us_total = 0, save_us_max = 0;
    u64 load_count = 0, load_us_total = 0, load_us_max = 0;
    // Dual core: time the snapshot spent waiting for the GPU thread (part of save_us).
    u64 save_sync_count = 0, save_sync_us_total = 0, save_sync_us_max = 0;
  };
  TimingStats GetTimingStats() const;

  // One sample per SaveFrame, for percentiles (harness `rollback_timings`). `compiles` is the number
  // of pipelines the video thread compiled synchronously while the snapshot waited for it.
  struct SaveSample
  {
    u32 save_us = 0;
    u32 sync_us = 0;
    u32 compiles = 0;
    u32 evict_us = 0;    // waiting for the previous eviction job
    u32 dostate_us = 0;  // non-RAM state (includes sync_us)
    u32 ram_us = 0;      // dirty RAM and L1 cache jobs
  };
  static constexpr u64 SAVE_SAMPLE_RING = 1 << 15;
  // Samples with index >= since (at most the last SAVE_SAMPLE_RING); returns the next index.
  u64 GetSaveSamples(u64 since, std::vector<SaveSample>& out) const;

  alignas(64) std::atomic<bool> m_frame_save_enabled{false};
  alignas(64) std::atomic<bool> m_frame_save_pending{false};

  void BeginDoState();
  void EndDoState();
  // Dual core: bring the GPU thread to a stop at a consistent FIFO position before the rollback
  // DoState touches the FIFO and video state (Dolphin's own savestates get this from PauseAndLock).
  // Returns the microseconds spent waiting.
  static u64 SyncGPUForDoState(Core::System& system);
  // Deterministic dual core: snapshots keep the video thread's state separately, captured by the
  // video thread in command order, instead of syncing the GPU thread (see VideoState.h).
  static bool UseSplitVideoState(Core::System& system);

  alignas(64) std::atomic<bool> m_skip_ram_in_dostate{false};
  alignas(64) std::atomic<bool> m_skip_ios_in_dostate{false};
  alignas(64) std::atomic<bool> m_skip_jit_clear_in_dostate{false};

  void NotifyDBATMappingsWereUpdated() {}

  // WSQ job system: m_dispatch_thread is worker 0, owned by the rollback thread.
  // Background workers run wait_for_termination() on their own std::threads.
  job::JobSysCtx m_job_ctx;
  job::JobTaskThread* m_dispatch_thread = nullptr;
  std::vector<std::thread> m_worker_threads;

  bool m_initialized = false;

  std::atomic<u64> m_stat_save_count{0}, m_stat_save_us_total{0}, m_stat_save_us_max{0};
  std::atomic<u64> m_stat_load_count{0}, m_stat_load_us_total{0}, m_stat_load_us_max{0};
  std::atomic<u64> m_stat_sync_count{0}, m_stat_sync_us_total{0}, m_stat_sync_us_max{0};
  mutable std::mutex m_sample_mutex;
  std::vector<SaveSample> m_save_samples;  // ring, SAVE_SAMPLE_RING entries once used
  u64 m_save_sample_next = 0;

  uint8_t* m_mem1_ptr = nullptr;
  size_t m_mem1_size = 0;
  uint8_t* m_mem2_ptr = nullptr;
  size_t m_mem2_size = 0;
  uint8_t* m_l1_cache_ptr = nullptr;
  size_t m_l1_cache_size = 0;

  DeltaSaveSlot m_slots[NUM_SAVE_SLOTS];

  std::vector<MemoryRegion> m_exclude_regions;
  std::optional<MemoryRegion> m_mailbox_region;

  bool m_region_mode = false;
  u64 m_region_bytes = 0;
  // [mem1 granules + mem2 granules]: 0 outside the set, 1 inside, 2 partly inside (a range of the
  // set starts or ends inside the granule; see m_region_partial).
  std::vector<u8> m_region_mask;
  // Granules only partly inside the set, by restore key (MEM1 granule index, or MEM2_FIRST_PAGE +
  // MEM2 granule index): bit b set = byte b belongs to the set. A load writes only those bytes:
  // the rest of the granule is live memory that must not be rolled back (e.g. the last 32 bytes
  // of the GX FIFO ring at 0x80611F40, right below the System heap).
  std::unordered_map<u32, u64> m_region_partial;
  // Number of granules only partly inside the region set (diagnostics).
  size_t RegionPartialGranules() const { return m_region_partial.size(); }
  // Loads that skipped bytes outside the set which differed from the snapshot, per granule
  // (physical address): what a whole-granule restore would have overwritten with stale data.
  std::map<u32, u64> PartialSkippedStats() const
  {
    std::lock_guard lk(m_partial_stats_mutex);
    return m_partial_skipped;
  }
  mutable std::mutex m_partial_stats_mutex;
  std::map<u32, u64> m_partial_skipped;
  // Diagnostics (PPR_GPRB_CENSUS=1): every granule outside the region set (or only partly inside)
  // written between two region-mode saves, accumulated over the match. Ranges of effective
  // addresses, merged.
  std::vector<u8> m_census;
  std::vector<std::pair<u32, u32>> CensusRanges() const;

  int m_ring_next = 0;  // index of the slot that will be written by the next savestate
  int m_ring_count = 0;

  struct RollbackSnapshot
  {
    std::unique_ptr<uint8_t[]> mem1;
    std::unique_ptr<uint8_t[]> mem2;
    std::shared_mutex mutex;
    uint32_t brawl_frame = 0;
    bool valid = false;
  };

  // Rolling base: full MEM1+MEM2 state at the oldest reachable frame
  RollbackSnapshot m_base_snapshot;

  job::Job* m_eviction_job = nullptr;

  // one byte per page, 1 = needs a source, 0 = satisfied.
  std::vector<u8> m_needs_source_mem1;
  std::vector<u8> m_needs_source_mem2;

  void CaptureFullRamSnapshot(RollbackSnapshot& snap);

#if ROLLBACK_VALIDATE
  RollbackSnapshot m_val_snapshots[NUM_SAVE_SLOTS];

  void CompareValSnapshot(int target_slot, int frames_back) const;
  void InvalidateValSnapshots();
#endif
};

uint32_t ReadBrawlMatchFrameCounter(const uint8_t* mem2_ptr, size_t mem2_size);

// Per-frame desync checksums, all 0 outside scMelee. `combined` is what GekkoNet compares; the
// parts are kept separately so the harness can tell which one diverged.
struct DesyncChecksums
{
  u32 combined = 0;
  u32 legacy = 0;         // Brawlback's original field list (see RollbackManager.cpp)
  u32 frame_counter = 0;  // g_GameFrame persistent frame counter
  u32 fighters = 0;       // per player: damage, stocks, status kind, X/Y of the active fighter
};
DesyncChecksums CalculateDesyncChecksums(Core::System& system);
uint32_t CalculateBrawlbackDesyncChecksum(Core::System& system);

}  // namespace Rollback
