// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/RollbackManager.h"

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <optional>
#include <thread>
#include <vector>

#include <Core/State.h>
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/ChangeCRC.h"
#include "Core/Rollback/MachineSession.h"
#include "Core/Rollback/Perf.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{

// Brawl's GameFrame::frameCounter at 0x901812a4 (MEM2). This resets when the
// match scene starts and advances inside the real game loop.
static constexpr uint32_t BRAWL_GAME_FRAME_COUNTER_MEM2_OFFSET = 0x001812a4u;

static_assert(BatchedChangeCRC::kGranuleSize == ROLLBACK_PAGE_SIZE);

uint32_t ReadBrawlMatchFrameCounter(const uint8_t* mem2_ptr, size_t mem2_size)
{
  if (!mem2_ptr || BRAWL_GAME_FRAME_COUNTER_MEM2_OFFSET + 4 > mem2_size)
    return 0;

  uint32_t raw;
  std::memcpy(&raw, mem2_ptr + BRAWL_GAME_FRAME_COUNTER_MEM2_OFFSET, sizeof(raw));
  return Common::swap32(raw);
}

void RollbackManager::CaptureFullRamSnapshot(RollbackSnapshot& snap)
{
  if (!snap.mem1)
    snap.mem1 = std::make_unique<uint8_t[]>(m_mem1_size);
  std::memcpy(snap.mem1.get(), m_mem1_ptr, m_mem1_size);

  if (!snap.mem2)
    snap.mem2 = std::make_unique<uint8_t[]>(m_mem2_size);
  std::memcpy(snap.mem2.get(), m_mem2_ptr, m_mem2_size);

  snap.brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  snap.valid = true;
}

#if ROLLBACK_VALIDATE

void RollbackManager::CompareValSnapshot(int target_slot, int frames_back) const
{
  ROLLBACK_ZONE();
  const RollbackSnapshot& snap = m_val_snapshots[target_slot];
  if (!snap.valid || !snap.mem1)
  {
    OSD::AddMessage(fmt::format("VALIDATE: no snapshot for slot {}", target_slot), 3000,
                    OSD::Color::YELLOW);
    return;
  }

  uint32_t mem1_mismatch_count = 0;
  constexpr int MAX_LOG_PAGES = 16;
  uint32_t mismatch_addrs[MAX_LOG_PAGES];  // Store physical addresses instead of page indices
  const uint32_t mem1_end = static_cast<uint32_t>(m_mem1_size);
  // Helper lambda to check if a specific byte address is excluded
  auto IsExcluded = [&](uint32_t addr) -> bool {
    for (const auto& r : m_exclude_regions)
    {
      if (addr >= r.phys_start && addr < r.phys_end)
        return true;
    }
    return false;
  };

  // Scan MEM1 byte-by-byte (optimized by segments)
  uint32_t cursor = 0;
  while (cursor < mem1_end)
  {
    // If cursor is inside an exclusion, jump to the end of it
    if (IsExcluded(cursor))
    {
      uint32_t jump_to = cursor + 1;
      for (const auto& r : m_exclude_regions)
      {
        if (cursor >= r.phys_start && cursor < r.phys_end)
        {
          jump_to = std::max(jump_to, r.phys_end);
          break;
        }
      }
      cursor = std::min(jump_to, mem1_end);
      continue;
    }

    // Find the next exclusion boundary to define the comparison segment
    uint32_t seg_end = mem1_end;
    for (const auto& r : m_exclude_regions)
    {
      if (r.phys_start > cursor && r.phys_start < seg_end)
        seg_end = r.phys_start;
    }
    // Also ensure we don't cross into an exclusion if we started before it
    // (Handled by the IsExcluded check at loop start, but seg_end ensures we stop before next one)

    const size_t copy_size = seg_end - cursor;
    if (copy_size > 0)
    {
      if (std::memcmp(m_mem1_ptr + cursor, snap.mem1.get() + cursor, copy_size) != 0)
      {
        // Report the differing 64-byte granules, not just the segment.
        for (size_t g = 0; g < copy_size; g += 64)
        {
          const size_t n = std::min<size_t>(64, copy_size - g);
          if (std::memcmp(m_mem1_ptr + cursor + g, snap.mem1.get() + cursor + g, n) != 0)
          {
            if (mem1_mismatch_count < MAX_LOG_PAGES)
              mismatch_addrs[mem1_mismatch_count] = static_cast<uint32_t>(cursor + g);
            ++mem1_mismatch_count;
          }
        }
      }
    }
    cursor = seg_end;
  }

  // MEM2 Handling (Similar logic)
  uint32_t mem2_mismatch_count = 0;
  uint32_t mem2_mismatch_addrs[MAX_LOG_PAGES] = {};

  if (snap.mem2 && m_mem2_ptr && m_mem2_size > 0)
  {
    const uint32_t mem2_end = static_cast<uint32_t>(m_mem2_size);
    const uint32_t MEM2_PHYS_BASE = 0x10000000u;

    cursor = 0;
    while (cursor < mem2_end)
    {
      uint32_t phys_addr = MEM2_PHYS_BASE + cursor;

      // Check exclusion
      bool excluded = false;
      for (const auto& r : m_exclude_regions)
      {
        if (phys_addr >= r.phys_start && phys_addr < r.phys_end)
        {
          excluded = true;
          // Jump logic
          uint32_t jump_to = phys_addr + 1;
          if (phys_addr >= r.phys_start && phys_addr < r.phys_end)
            jump_to = std::max(jump_to, r.phys_end);
          cursor = std::min(jump_to - MEM2_PHYS_BASE, mem2_end);
          break;
        }
      }
      if (excluded)
        continue;

      // Find next boundary
      uint32_t seg_end_phys = MEM2_PHYS_BASE + mem2_end;
      for (const auto& r : m_exclude_regions)
      {
        if (r.phys_start > phys_addr && r.phys_start < seg_end_phys)
          seg_end_phys = r.phys_start;
      }

      const size_t seg_size = seg_end_phys - phys_addr;
      if (seg_size > 0)
      {
        if (std::memcmp(m_mem2_ptr + cursor, snap.mem2.get() + cursor, seg_size) != 0)
        {
          for (size_t g = 0; g < seg_size; g += 64)
          {
            const size_t n = std::min<size_t>(64, seg_size - g);
            if (std::memcmp(m_mem2_ptr + cursor + g, snap.mem2.get() + cursor + g, n) != 0)
            {
              if (mem2_mismatch_count < MAX_LOG_PAGES)
                mem2_mismatch_addrs[mem2_mismatch_count] = static_cast<uint32_t>(phys_addr + g);
              ++mem2_mismatch_count;
            }
          }
        }
      }
      cursor = seg_end_phys - MEM2_PHYS_BASE;
    }
  }

  const uint32_t current_brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  const bool frame_ok = (current_brawl_frame == snap.brawl_frame);
  const char* frame_tag = frame_ok ? "frame_ok" : "FRAME_MISMATCH";

  if (mem1_mismatch_count == 0 && mem2_mismatch_count == 0)
  {
    INFO_LOG_FMT(BRAWLBACK, "[Rollback] VALIDATE OK  step={}  slot={}  brawl_frame={} (want {})  {}",
                 frames_back, target_slot, current_brawl_frame, snap.brawl_frame, frame_tag);
    return;
  }

  // Logging MEM1
  std::string mem1_list;
  const uint32_t mem1_logged = std::min(mem1_mismatch_count, static_cast<uint32_t>(MAX_LOG_PAGES));
  for (uint32_t i = 0; i < mem1_logged; ++i)
  {
    if (i)
      mem1_list += ", ";
    mem1_list += fmt::format("0x{:08x}", mismatch_addrs[i]);
  }
  if (mem1_mismatch_count > MAX_LOG_PAGES)
    mem1_list += fmt::format(" (+{} more)", mem1_mismatch_count - MAX_LOG_PAGES);

  // Logging MEM2
  std::string mem2_list;
  const uint32_t mem2_logged = std::min(mem2_mismatch_count, static_cast<uint32_t>(MAX_LOG_PAGES));
  for (uint32_t i = 0; i < mem2_logged; ++i)
  {
    if (i)
      mem2_list += ", ";
    mem2_list += fmt::format("0x{:08x}", mem2_mismatch_addrs[i]);
  }
  if (mem2_mismatch_count > MAX_LOG_PAGES)
    mem2_list += fmt::format(" (+{} more)", mem2_mismatch_count - MAX_LOG_PAGES);

  WARN_LOG_FMT(BRAWLBACK,
               "[Rollback] VALIDATE FAIL  step={}  slot={} - {} MEM1 + "
               "{} MEM2 region(s) wrong.  "
               "brawl_frame={} (want {})  {}
"
               "  First MEM1 addrs: {}
  First MEM2 addrs: {}",
               frames_back, target_slot, mem1_mismatch_count, mem2_mismatch_count,
               current_brawl_frame, snap.brawl_frame, frame_tag, mem1_list, mem2_list);
}

void RollbackManager::InvalidateValSnapshots()
{
  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_val_snapshots[i].valid = false;
}

#endif  // ROLLBACK_VALIDATE

RollbackManager& RollbackManager::Get()
{
  static RollbackManager s_instance;
  return s_instance;
}

bool RollbackManager::UseSplitVideoState(Core::System& system)
{
  // PPR_ROLLBACK_SYNC_VIDEO=1: drain the GPU thread for every snapshot, as before (A/B runs).
  static const bool force_sync = [] {
    const char* env = std::getenv("PPR_ROLLBACK_SYNC_VIDEO");
    return env && std::strcmp(env, "1") == 0;
  }();
  return !force_sync && system.IsDualCoreMode() && system.GetFifo().UseDeterministicGPUThread();
}

u64 RollbackManager::SyncGPUForDoState(Core::System& system)
{
  if (!system.IsDualCoreMode())
    return 0;
  const auto start = std::chrono::steady_clock::now();
  system.GetFifo().SyncGPU(Fifo::SyncGPUReason::Other, true);
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());
}

void RollbackManager::BeginDoState()
{
  m_skip_ram_in_dostate.store(true, std::memory_order_seq_cst);
  m_skip_jit_clear_in_dostate.store(true, std::memory_order_seq_cst);
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  VideoCommon_SetSkipVertexFlushForRollback(true);
  // A whole-machine session rolls the IOS devices back too (the NAND by its journal, the Wii
  // Remotes, open files); the Brawlback netplay path keeps only their IPC queues and timing.
  m_skip_ios_in_dostate.store(!m_full_ios_in_snapshots, std::memory_order_seq_cst);
  m_in_snapshot_dostate.store(m_full_ios_in_snapshots, std::memory_order_seq_cst);
  PowerPC_SetSkipDCacheFlushForRollback(true);
  PowerPC_SetSkipCPURegsForRollback(true);
}

void RollbackManager::EndDoState()
{
  PowerPC_SetSkipCPURegsForRollback(false);
  PowerPC_SetSkipDCacheFlushForRollback(false);
  m_skip_ios_in_dostate.store(false, std::memory_order_seq_cst);
  m_in_snapshot_dostate.store(false, std::memory_order_seq_cst);
  VideoCommon_SetSkipVertexFlushForRollback(false);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_seq_cst);
  m_skip_ram_in_dostate.store(false, std::memory_order_seq_cst);
}

void RollbackManager::AddExcludeRegion(uint32_t virt_addr, uint32_t size_bytes)
{
  INFO_LOG_FMT(BRAWLBACK, "Added exclude region {} - {}", virt_addr, virt_addr + size_bytes);
  m_exclude_regions.push_back(MemoryRegion::FromVirt(virt_addr, size_bytes));
}

void RollbackManager::SetMailboxRegion(uint32_t virt_addr, uint32_t size_bytes)
{
  if (m_mailbox_region)
  {
    const MemoryRegion old = *m_mailbox_region;
    std::erase_if(m_exclude_regions, [&](const MemoryRegion& r) {
      return r.phys_start == old.phys_start && r.phys_end == old.phys_end && r.name == old.name;
    });
    m_mailbox_region.reset();
  }
  if (size_bytes == 0)
    return;
  m_mailbox_region = MemoryRegion::FromVirt(virt_addr, size_bytes, "PPOM mailbox");
  m_exclude_regions.push_back(*m_mailbox_region);
  INFO_LOG_FMT(BRAWLBACK, "Excluded the PPOM mailbox {:08x} - {:08x}", virt_addr,
               virt_addr + size_bytes);
}

// Never exclude game or library state here, only pure output buffers. The AX regions that used to
// be listed (0x804e7c00 +0xc00 and 0x8049a4ea +0x1400) hold AX's voice lists (.bss 0x804E7C20 and
// neighbours), not just sample data: restoring the voice parameter blocks but not the lists that
// link them left a cycle in a voice list after some rollbacks, and AX's service routine then spun
// on it forever with interrupts disabled (the host/joiner freezes at scene transitions).
static const std::vector<MemoryRegion> s_brawlback_hardcoded_exclude_regions = {
  MemoryRegion::FromVirt(0x90000800, 0x12c800, "Brawl framebuffer buffers"),
};

static const std::vector<MemoryRegionThroughPtrs> s_brawlback_hardcoded_desync_detection_regions = {
    // GAME_FRAME->persistentFrameCounter
    MemoryRegionThroughPtrs::FromVirt(0x901812a0u + 0x14u, 4),

    // Player damage/percent
    MemoryRegionThroughPtrs::FromVirt(0x80623324u, 4),  // P1
    MemoryRegionThroughPtrs::FromVirt(0x80623568u, 4),  // P2
    MemoryRegionThroughPtrs::FromVirt(0x806237ACu, 4),  // P3
    MemoryRegionThroughPtrs::FromVirt(0x806239F0u, 4),  // P4

    // Player stock count
    MemoryRegionThroughPtrs::FromVirt(0x80623318u, 4),  // P1
    MemoryRegionThroughPtrs::FromVirt(0x8062355Cu, 4),  // P2
    MemoryRegionThroughPtrs::FromVirt(0x806237A0u, 4),  // P3
    MemoryRegionThroughPtrs::FromVirt(0x806239E4u, 4),  // P4

    // Player positions
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x34u, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),    // P1 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x34u, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),   // P1 Y
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x278u, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),   // P2 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x278u, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),  // P2 Y
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x4BCu, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),   // P3 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x4BCu, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),  // P3 Y
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x700u, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),   // P4 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x700u, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),  // P4 Y

    // Player velocities
    MemoryRegionThroughPtrs::FromVirt(0x80494F30u, 8),  // P1 Total Velocity (X, Y)
    MemoryRegionThroughPtrs::FromVirt(0x8049DEE4u, 8),  // P2 Total Velocity (X, Y)
    MemoryRegionThroughPtrs::FromVirt(0x80494F98u, 8),  // P3 Total Velocity (X, Y)
    MemoryRegionThroughPtrs::FromVirt(0x80495000u, 8),  // P4 Total Velocity (X, Y)
};

#if BRAWLBACK_DESYNC_DETECTION
static const u8* GetRegionPointer(const MemoryRegion& region, const u8* mem1_ptr, size_t mem1_size,
                                  const u8* mem2_ptr, size_t mem2_size)
{
  if (region.phys_start >= region.phys_end)
    return nullptr;

  if (region.phys_start >= MEM2_BASE)
  {
    const u32 mem2_start = region.phys_start - MEM2_BASE;
    const u32 mem2_end = region.phys_end - MEM2_BASE;
    if (mem2_ptr && mem2_end <= mem2_size)
      return mem2_ptr + mem2_start;
    return nullptr;
  }

  if (mem1_ptr && region.phys_end <= mem1_size)
    return mem1_ptr + region.phys_start;
  return nullptr;
}

// gfSceneManager::currentScene->sceneName == "scMelee" (see scMelee::create()). The scene
// manager is in MEM1, but the current scene can be allocated in MEM2, so resolve each pointer
// through the guest memory map instead of treating every pointer as a MEM1 offset.
static bool IsCurrentSceneMelee(const Memory::MemoryManager& memory)
{
  constexpr u32 SCENE_MANAGER_PTR_ADDR = 0x805a0060u;
  constexpr u32 CURRENT_SCENE_OFFSET = 0x4u;
  constexpr u32 SCENE_NAME_OFFSET = 0x0u;
  constexpr char kMeleeSceneName[] = "scMelee";

  auto read_u32 = [&](u32 address) -> u32 {
    const u8* const ptr = memory.GetPointerForRange(address, sizeof(u32));
    if (!ptr)
      return 0;
    u32 v;
    std::memcpy(&v, ptr, sizeof(v));
    return Common::swap32(v);
  };

  const u32 scene_manager = read_u32(SCENE_MANAGER_PTR_ADDR);
  if (scene_manager == 0)
    return false;

  const u32 current_scene = read_u32(scene_manager + CURRENT_SCENE_OFFSET);
  if (current_scene == 0)
    return false;

  const u32 scene_name_ptr = read_u32(current_scene + SCENE_NAME_OFFSET);
  const u8* const scene_name = memory.GetPointerForRange(scene_name_ptr, sizeof(kMeleeSceneName));
  if (!scene_name)
    return false;

  return std::memcmp(scene_name, kMeleeSceneName, sizeof(kMeleeSceneName)) == 0;
}
#endif

#if BRAWLBACK_DESYNC_DETECTION
// The fields Brawlback hashed (s_brawlback_hardcoded_desync_detection_regions) were checked live
// (docs/brawl-memory-map.md, "Brawlback's desync checksum"): the "damage" and "stocks" addresses
// are ftEntry words holding the ftOwner* and instance 0's Fighter*, and the "velocity" words are
// static pointers, so none of them changes during a match; only the frame counter and the X/Y of
// instance 0 were really covered. These are the REL-verified fields instead:
//   ftEntryManager* at 0x80624780 -> ftEntry entries[4], stride 0x244:
//     +0x0A u8 active instance, +0x28 ftOwner*, +0x30 + 8*k {u32 kind, Fighter*} for instance k
//   ftOwner -> +0 owner data -> +0x24 f32 damage, +0x34 s32 stocks
//   Fighter -> +0x60 module accesser -> +0xD8 module enumeration
//     -> +0x0C posture module -> +0x0C f32 x, +0x10 f32 y
//     -> +0x70 status module -> +0x34 s32 status kind
static constexpr u32 FT_ENTRY_MANAGER_PTR_ADDR = 0x80624780u;
static constexpr u32 FT_ENTRY_STRIDE = 0x244u;
static constexpr u32 GAME_FRAME_PERSISTENT_COUNTER_ADDR = 0x901812a0u + 0x14u;

static u32 FighterChecksum(const Memory::MemoryManager& memory)
{
  auto read_u32 = [&](u32 address) -> std::optional<u32> {
    const u8* const ptr = memory.GetPointerForRange(address, sizeof(u32));
    if (!ptr)
      return std::nullopt;
    u32 v;
    std::memcpy(&v, ptr, sizeof(v));
    return Common::swap32(v);
  };
  auto read_ptr = [&](u32 address) -> std::optional<u32> {
    const auto v = read_u32(address);
    if (!v || *v < 0x80000000u)
      return std::nullopt;
    return v;
  };

  u32 crc = Common::StartCRC32();
  const auto entries = read_ptr(FT_ENTRY_MANAGER_PTR_ADDR);
  if (!entries)
    return crc;
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 entry = *entries + port * FT_ENTRY_STRIDE;
    // Fields that can't be resolved (no fighter in this port) hash as zeroes.
    std::array<u32, 6> fields{};
    const u8* const instance_ptr = memory.GetPointerForRange(entry + 0x0a, 1);
    fields[0] = instance_ptr ? *instance_ptr : 0xff;
    if (const auto owner = read_ptr(entry + 0x28))
    {
      if (const auto owner_data = read_ptr(*owner))
      {
        fields[1] = read_u32(*owner_data + 0x24).value_or(0);
        fields[2] = read_u32(*owner_data + 0x34).value_or(0);
      }
    }
    if (fields[0] < 4)
    {
      if (const auto fighter = read_ptr(entry + 0x34 + 8 * fields[0]))
      {
        if (const auto accesser = read_ptr(*fighter + 0x60))
        {
          if (const auto enumeration = read_ptr(*accesser + 0xd8))
          {
            if (const auto posture = read_ptr(*enumeration + 0x0c))
            {
              fields[3] = read_u32(*posture + 0x0c).value_or(0);
              fields[4] = read_u32(*posture + 0x10).value_or(0);
            }
            if (const auto status = read_ptr(*enumeration + 0x70))
              fields[5] = read_u32(*status + 0x34).value_or(0);
          }
        }
      }
    }
    crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(fields.data()), sizeof(fields));
  }
  return crc;
}
#endif

static uint32_t CalculateLegacyBrawlbackChecksum(Core::System& system);

DesyncChecksums CalculateDesyncChecksums(Core::System& system)
{
  DesyncChecksums sums;
#if BRAWLBACK_DESYNC_DETECTION
  auto& memory = system.GetMemory();
  if (!IsCurrentSceneMelee(memory))
    return sums;

  if (const u8* const ptr = memory.GetPointerForRange(GAME_FRAME_PERSISTENT_COUNTER_ADDR, 4))
  {
    u32 v;
    std::memcpy(&v, ptr, sizeof(v));
    sums.frame_counter = Common::swap32(v);
  }
  sums.fighters = FighterChecksum(memory);
  sums.legacy = CalculateLegacyBrawlbackChecksum(system);

  u32 crc = Common::StartCRC32();
  crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(&sums.frame_counter), 4);
  crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(&sums.fighters), 4);
  sums.combined = crc;
#endif
  return sums;
}

uint32_t CalculateBrawlbackDesyncChecksum(Core::System& system)
{
  return CalculateDesyncChecksums(system).combined;
}

static uint32_t CalculateLegacyBrawlbackChecksum(Core::System& system)
{
#if BRAWLBACK_DESYNC_DETECTION
  auto& memory = system.GetMemory();
  u8* const mem1 = memory.GetRAM();
  u8* const mem2 = memory.GetEXRAM();
  const size_t mem1_size = memory.GetRamSize();
  const size_t mem2_size = memory.GetExRamSize();

  if (!IsCurrentSceneMelee(memory))
    return 0;

  u32 crc = Common::StartCRC32();
  for (const MemoryRegionThroughPtrs& source_region :
       s_brawlback_hardcoded_desync_detection_regions)
  {
    const bool is_pointer_region =
        !source_region.pointer_offsets.empty() || source_region.final_data_size != 0;
    const MemoryRegion region = is_pointer_region ?
                                    source_region.Resolve(mem1, mem1_size, mem2, mem2_size) :
                                    MemoryRegion{source_region.phys_start, source_region.phys_end};
    const u32 region_len = region.phys_end - region.phys_start;
    if (const u8* ptr = GetRegionPointer(region, mem1, mem1_size, mem2, mem2_size))
      crc = Common::UpdateCRC32(crc, ptr, region_len);
  }
  return crc;
#else
  return 0;
#endif
}

void RollbackManager::Init(Core::System& system)
{
  if (m_initialized)
    Shutdown();

  // Initialize WSQ job system once; workers persist across save/load cycles.
  if (!m_dispatch_thread)
  {
    m_job_ctx.activate();
    // Worker 0 is "owned" by the rollback thread — used for job creation/dispatch.
    // All initialize_worker calls must be sequential (no thread safety in ctx setup).
    m_dispatch_thread = m_job_ctx.initialize_worker(0, nullptr);
    for (int i = 1; i <= ROLLBACK_NUM_HELPER_THREADS; ++i)
    {
      job::JobTaskThread* thr =
          m_job_ctx.initialize_worker(static_cast<int64_t>(i) * 0x9e3779b97f4a7c15LL, nullptr);
      m_worker_threads.emplace_back([thr]() {
        ROLLBACK_THREAD_NAME("Rollback Job Pool");
        thr->wait_for_termination();
      });
    }
  }

  if (m_brawl_exclusions)
    m_exclude_regions = s_brawlback_hardcoded_exclude_regions;
  else
    m_exclude_regions.clear();
  if (m_mailbox_region)
    m_exclude_regions.push_back(*m_mailbox_region);
  PerfInit();

  auto& memory = system.GetMemory();
  m_mem1_ptr = memory.GetRAM();
  m_mem1_size = memory.GetRamSize();
  m_mem2_ptr = memory.GetEXRAM();
  // A GameCube game has no MEM2, but its size still reads as the Wii's (bug report #113).
  m_mem2_size = m_mem2_ptr ? memory.GetExRamSize() : 0;
  m_l1_cache_ptr = memory.GetL1Cache();
  m_l1_cache_size = memory.GetL1CacheSize();

  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_slots[i].Init(m_mem1_ptr, m_mem1_size, m_mem2_ptr, m_mem2_size, m_l1_cache_ptr,
                    m_l1_cache_size);

  m_needs_source_mem1.assign(m_mem1_size / ROLLBACK_PAGE_SIZE, 0);
  m_needs_source_mem2.assign(m_mem2_size / ROLLBACK_PAGE_SIZE, 0);

  // Fake VMEM's chunk versions take its marks first: clearing them would lose its writes so far.
  memory.FlushFakeVMEMWrites();
  JITDirtyBitmap::Get().Clear();
  GPUWrittenBitmap::Get().Clear();

  m_ring_next = 0;
  m_ring_count = 0;

  m_skip_ram_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_ios_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_relaxed);
  m_frame_save_pending.store(false, std::memory_order_relaxed);
  m_initialized = true;

#if ROLLBACK_VALIDATE
  InvalidateValSnapshots();
#endif
}

void RollbackManager::Shutdown()
{
  if (!m_initialized)
    return;

  // No video thread capture may write into a slot from here on.
  Core::System::GetInstance().GetFifo().DropVideoThreadCaptures();

  if (m_eviction_job)
  {
    job::DrainJobsUntilComplete(m_dispatch_thread, m_eviction_job);
    m_eviction_job = nullptr;
  }

  m_base_snapshot.valid = false;
  m_shadow_valid = false;
  m_base_snapshot.mem1.reset();
  m_base_snapshot.mem2.reset();

  JITDirtyBitmap::Get().Clear();

  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_slots[i].Reset();

  m_ring_next = 0;
  m_ring_count = 0;

  m_skip_ram_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_ios_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_relaxed);
  m_frame_save_pending.store(false, std::memory_order_relaxed);
  m_frame_save_enabled.store(false, std::memory_order_relaxed);
  VideoCommon_SetSkipGPUReadbackForRollback(false);

  // Signal WSQ workers to exit and wait for them.
  // Workers are persistent — only tear them down on full shutdown.
  if (m_dispatch_thread)
  {
    m_job_ctx.deactivate();
    for (auto& t : m_worker_threads)
      if (t.joinable())
        t.join();
    m_worker_threads.clear();
    m_job_ctx.release_workers();
    m_dispatch_thread = nullptr;
  }

  m_initialized = false;

#if ROLLBACK_VALIDATE
  InvalidateValSnapshots();
#endif
}

void RollbackManager::BeginRegionMode(const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                                      const std::vector<std::pair<uint32_t, uint32_t>>& exclude)
{
  const size_t mem1_pages = m_mem1_size / ROLLBACK_PAGE_SIZE;
  const size_t mem2_pages = m_mem2_size / ROLLBACK_PAGE_SIZE;
  m_region_mask.assign(mem1_pages + mem2_pages, 0);
  m_region_partial.clear();
  m_census.clear();
  {
    std::lock_guard lk(m_partial_stats_mutex);
    m_partial_skipped.clear();
  }
  m_region_bytes = 0;
  // Granule index range (in m_region_mask) and restore-key base of an address range.
  const auto locate = [&](u32 addr, u32 size, size_t* base, size_t* limit, u32* key_base,
                          size_t* first, size_t* last) {
    const u32 seg = addr >> 28;
    const u32 off = addr & 0x0FFFFFFF;
    if (seg == 0x8 || seg == 0xC)
    {
      *base = 0;
      *limit = mem1_pages;
      *key_base = 0;
    }
    else if (seg == 0x9 || seg == 0xD)
    {
      *base = mem1_pages;
      *limit = mem2_pages;
      *key_base = MEM2_FIRST_PAGE;
    }
    else
    {
      return false;
    }
    *first = off / ROLLBACK_PAGE_SIZE;
    *last = (static_cast<size_t>(off) + size - 1) / ROLLBACK_PAGE_SIZE;
    return true;
  };
  // Saves and loads work on whole granules: a range that starts or ends inside a granule takes
  // the rest of it along.
  for (const auto& [addr, size] : ranges)
  {
    if (size == 0)
      continue;
    m_region_bytes += size;
    size_t base, limit, first, last;
    u32 key_base;
    if (!locate(addr, size, &base, &limit, &key_base, &first, &last))
      continue;
    for (size_t p = first; p <= last && p < limit; ++p)
      m_region_mask[base + p] = 1;
  }
  // Except for bytes the set must never restore (live memory of something else that shares a
  // granule with the set, such as the GX FIFO ring's last 32 bytes below the System heap): those
  // granules are restored byte by byte.
  for (const auto& [addr, size] : exclude)
  {
    if (size == 0)
      continue;
    size_t base, limit, first, last;
    u32 key_base;
    if (!locate(addr, size, &base, &limit, &key_base, &first, &last))
      continue;
    const u64 off = addr & 0x0FFFFFFF;
    for (size_t p = first; p <= last && p < limit; ++p)
    {
      if (m_region_mask[base + p] == 0)
        continue;
      const u64 lo = std::max<u64>(off, p * ROLLBACK_PAGE_SIZE) - p * ROLLBACK_PAGE_SIZE;
      const u64 hi = std::min<u64>(off + size, (p + 1) * ROLLBACK_PAGE_SIZE) - p * ROLLBACK_PAGE_SIZE;
      const u64 excluded = (hi - lo == 64 ? ~0ULL : ((1ULL << (hi - lo)) - 1)) << lo;
      const u32 key = key_base + static_cast<u32>(p);
      const auto it = m_region_partial.find(key);
      const u64 keep = (it != m_region_partial.end() ? it->second : ~0ULL) & ~excluded;
      if (keep == 0)
      {
        m_region_mask[base + p] = 0;
        m_region_partial.erase(key);
      }
      else
      {
        m_region_mask[base + p] = 2;
        m_region_partial[key] = keep;
      }
    }
  }
  m_region_mode = true;
  // Fresh ring and base snapshot.
  m_frame_save_enabled.store(false, std::memory_order_relaxed);
  ToggleFrameSave();
  INFO_LOG_FMT(BRAWLBACK,
               "Rollback: region mode on, {} ranges, {} bytes, {} exclusions, {} granules partly inside",
               ranges.size(), m_region_bytes, exclude.size(), m_region_partial.size());
}

std::vector<std::pair<u32, u32>> RollbackManager::CensusRanges() const
{
  std::vector<std::pair<u32, u32>> out;
  const size_t mem1_pages = m_mem1_size / ROLLBACK_PAGE_SIZE;
  for (size_t i = 0; i < m_census.size(); ++i)
  {
    if (!m_census[i])
      continue;
    const u32 addr = i < mem1_pages ? 0x80000000u + static_cast<u32>(i * ROLLBACK_PAGE_SIZE) :
                                      0x90000000u + static_cast<u32>((i - mem1_pages) * ROLLBACK_PAGE_SIZE);
    if (!out.empty() && out.back().first + out.back().second == addr)
      out.back().second += ROLLBACK_PAGE_SIZE;
    else
      out.emplace_back(addr, static_cast<u32>(ROLLBACK_PAGE_SIZE));
  }
  return out;
}

void RollbackManager::EndRegionMode()
{
  if (m_eviction_job)
  {
    job::DrainJobsUntilComplete(m_dispatch_thread, m_eviction_job);
    m_eviction_job = nullptr;
  }
  m_frame_save_enabled.store(false, std::memory_order_relaxed);
  m_region_mode = false;
  m_region_mask.clear();
  m_region_partial.clear();
  m_region_bytes = 0;
  m_ring_next = 0;
  m_ring_count = 0;
  m_base_snapshot.valid = false;
  m_shadow_valid = false;
}

void RollbackManager::ToggleFrameSave()
{
  const bool enabled = !m_frame_save_enabled.load(std::memory_order_relaxed);
  m_frame_save_enabled.store(enabled, std::memory_order_relaxed);

  if (enabled)
  {
    m_ring_next = 0;
    m_ring_count = 0;
    for (auto* stat : {&m_stat_save_count, &m_stat_save_us_total, &m_stat_save_us_max,
                       &m_stat_load_count, &m_stat_load_us_total, &m_stat_load_us_max,
                       &m_stat_sync_count, &m_stat_sync_us_total, &m_stat_sync_us_max,
                       &m_stat_evict_waits, &m_stat_evict_wait_us_max, &m_stat_save_gran_total,
                       &m_stat_save_gran_max, &m_stat_load_gran_total, &m_stat_load_gran_max})
    {
      stat->store(0, std::memory_order_relaxed);
    }
    {
      std::lock_guard lk(m_sample_mutex);
      m_save_samples.clear();
      m_save_sample_next = 0;
    }

    if (m_eviction_job)
    {
      job::DrainJobsUntilComplete(m_dispatch_thread, m_eviction_job);
      m_eviction_job = nullptr;
    }
    m_base_snapshot.valid = false;
    m_shadow_valid = false;

    Core::System::GetInstance().GetMemory().FlushFakeVMEMWrites();
    JITDirtyBitmap::Get().Clear();

#if ROLLBACK_VALIDATE
    InvalidateValSnapshots();
#endif
    OSD::AddMessage(fmt::format("Rollback: frame-save ON  ({} slots)", NUM_SAVE_SLOTS), 3000,
                    OSD::Color::GREEN);
  }
  else
  {
    OSD::AddMessage("Rollback: frame-save OFF", 3000, OSD::Color::YELLOW);
  }
}

s32 Wrap(s32 x, s32 wrap)
{
  if (x < 0)
    x = (wrap + x);
  ASSERT(x >= 0);
  return x % wrap;
}

static u64 RecordTiming(std::chrono::steady_clock::time_point start, std::atomic<u64>& count,
                        std::atomic<u64>& total, std::atomic<u64>& max)
{
  const u64 us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
  count.fetch_add(1, std::memory_order_relaxed);
  total.fetch_add(us, std::memory_order_relaxed);
  if (us > max.load(std::memory_order_relaxed))
    max.store(us, std::memory_order_relaxed);
  return us;
}

RollbackManager::TimingStats RollbackManager::GetTimingStats() const
{
  TimingStats stats;
  stats.save_count = m_stat_save_count.load(std::memory_order_relaxed);
  stats.save_us_total = m_stat_save_us_total.load(std::memory_order_relaxed);
  stats.save_us_max = m_stat_save_us_max.load(std::memory_order_relaxed);
  stats.load_count = m_stat_load_count.load(std::memory_order_relaxed);
  stats.load_us_total = m_stat_load_us_total.load(std::memory_order_relaxed);
  stats.load_us_max = m_stat_load_us_max.load(std::memory_order_relaxed);
  stats.save_sync_count = m_stat_sync_count.load(std::memory_order_relaxed);
  stats.save_sync_us_total = m_stat_sync_us_total.load(std::memory_order_relaxed);
  stats.save_sync_us_max = m_stat_sync_us_max.load(std::memory_order_relaxed);
  stats.load_evict_waits = m_stat_evict_waits.load(std::memory_order_relaxed);
  stats.load_evict_wait_us_max = m_stat_evict_wait_us_max.load(std::memory_order_relaxed);
  stats.save_granules_total = m_stat_save_gran_total.load(std::memory_order_relaxed);
  stats.save_granules_max = m_stat_save_gran_max.load(std::memory_order_relaxed);
  stats.load_granules_total = m_stat_load_gran_total.load(std::memory_order_relaxed);
  stats.load_granules_max = m_stat_load_gran_max.load(std::memory_order_relaxed);
  return stats;
}

u64 RollbackManager::GetSaveSamples(u64 since, std::vector<SaveSample>& out) const
{
  std::lock_guard lk(m_sample_mutex);
  const u64 first = m_save_sample_next > SAVE_SAMPLE_RING ? m_save_sample_next - SAVE_SAMPLE_RING : 0;
  for (u64 i = std::max(since, first); i < m_save_sample_next; ++i)
    out.push_back(m_save_samples[i % SAVE_SAMPLE_RING]);
  return m_save_sample_next;
}

u64 RollbackManager::WaitForEviction(bool* was_pending)
{
  if (was_pending)
    *was_pending = false;
  if (!m_eviction_job)
    return 0;
  const bool pending = m_eviction_job->unfinished_jobs.load(std::memory_order_acquire) != 0;
  const auto start = std::chrono::steady_clock::now();
  job::DrainJobsUntilComplete(m_dispatch_thread, m_eviction_job);
  m_eviction_job = nullptr;
  if (was_pending)
    *was_pending = pending;
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());
}

int RollbackManager::SlotsBackForTag(s64 tag) const
{
  if (tag == NO_TAG)
    return -1;
  for (int back = 1; back < m_ring_count; ++back)
  {
    if (m_slot_tags[Wrap(m_ring_next - 1 - back, NUM_SAVE_SLOTS)] == tag)
      return back;
  }
  return -1;
}

void RollbackManager::SaveFrame(Core::System& system, s64 tag)
{
  ROLLBACK_ZONE();
  if (!m_initialized)
    return;
  const auto timing_start = std::chrono::steady_clock::now();

  // Lazy init on the first frame, so we can be sure the game is fully booted when taking the base
  // snapshot
  if (!m_base_snapshot.valid)
  {
    ROLLBACK_ZONE_N("BaseSnapshot::Init");
    std::unique_lock lk(m_base_snapshot.mutex);
    CaptureFullRamSnapshot(m_base_snapshot);
    INFO_LOG_FMT(BRAWLBACK, "Captured base snapshot at brawl frame {}",
                 m_base_snapshot.brawl_frame);
  }
  if (!m_region_mode && !m_shadow_valid)
  {
    if (!m_shadow_mem1)
      m_shadow_mem1 = std::make_unique<uint8_t[]>(m_mem1_size);
    if (!m_shadow_mem2 && m_mem2_size)
      m_shadow_mem2 = std::make_unique<uint8_t[]>(m_mem2_size);
    std::memcpy(m_shadow_mem1.get(), m_mem1_ptr, m_mem1_size);
    if (m_mem2_size)
      std::memcpy(m_shadow_mem2.get(), m_mem2_ptr, m_mem2_size);
    m_shadow_valid = true;
  }

  const int slot = m_ring_next;
  m_slot_tags[slot] = tag;
  const auto t_base = std::chrono::steady_clock::now();
  u64 evict_us = 0;

  // Evict the oldest slot, async apply its deltas to the base snapshot
  if (m_ring_count >= NUM_SAVE_SLOTS)
  {
    ROLLBACK_ZONE_N("Prep eviction");
    // Wait for any in-flight eviction — typically completes within the same frame.
    evict_us = WaitForEviction();
    auto evicted = std::make_shared<Rollback::EvictedDelta>(m_slots[slot].ExtractDeltas());
    m_eviction_job = job::KickRootJob(
        m_dispatch_thread, [this, evicted](job::JobTaskThread&, job::Job&) mutable {
          ROLLBACK_ZONE_N("BaseSnapshot::Evict");
          // Diagnostics: a slow eviction, as on a starved machine (docs/gameplay-rollback-status.md,
          // open issue 9).
          static const long delay_us = [] {
            const char* e = std::getenv("PPR_GPRB_EVICT_DELAY_US");
            return e ? std::strtol(e, nullptr, 10) : 0L;
          }();
          if (delay_us > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
          std::unique_lock lk(m_base_snapshot.mutex);

          const uint8_t* src = evicted->mem1.page_data.data();
          for (uint32_t i = 0; i < evicted->mem1.page_count; ++i)
          {
            const size_t dst_off = static_cast<size_t>(evicted->mem1.page_indices[i]) * ROLLBACK_PAGE_SIZE;
            std::memcpy(m_base_snapshot.mem1.get() + dst_off, src + i * ROLLBACK_PAGE_SIZE, ROLLBACK_PAGE_SIZE);
          }

          src = evicted->mem2.page_data.data();
          for (uint32_t i = 0; i < evicted->mem2.page_count; ++i)
          {
            const size_t dst_off = static_cast<size_t>(evicted->mem2.page_indices[i]) * ROLLBACK_PAGE_SIZE;
            std::memcpy(m_base_snapshot.mem2.get() + dst_off, src + i * ROLLBACK_PAGE_SIZE, ROLLBACK_PAGE_SIZE);
          }
        });
  }

  m_ring_next = Wrap(m_ring_next + 1, NUM_SAVE_SLOTS);
  m_ring_count = std::clamp(m_ring_count + 1, 0, NUM_SAVE_SLOTS);
  const auto t_evict = std::chrono::steady_clock::now();

  static const bool census = std::getenv("PPR_GPRB_CENSUS") != nullptr;
  if (census && m_region_mode)
  {
    const u8* entries = JITDirtyBitmap::Get().entries;
    const size_t mem1_pages = m_mem1_size / ROLLBACK_PAGE_SIZE;
    const size_t mem2_pages = m_mem2_size / ROLLBACK_PAGE_SIZE;
    if (m_census.size() != mem1_pages + mem2_pages)
      m_census.assign(mem1_pages + mem2_pages, 0);
    for (size_t i = 0; i < mem1_pages; ++i)
    {
      if (entries[i] && m_region_mask[i] != 1)
        m_census[i] = 1;
    }
    for (size_t i = 0; i < mem2_pages; ++i)
    {
      if (entries[MEM2_FIRST_PAGE + i] && m_region_mask[mem1_pages + i] != 1)
        m_census[mem1_pages + i] = 1;
    }
  }

  {
    m_slots[slot].Save(system);
  }
  if (!m_region_mode && m_shadow_valid)
    UpdateShadowAfterSave(slot);
  {
    const auto t_end = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
      return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
    };
    if (ms(timing_start, t_end) > 100.0)
    {
      WARN_LOG_FMT(BRAWLBACK,
                   "Slow SaveFrame: {:.1f} ms (base snapshot {:.1f}, eviction wait {:.1f}, slot save "
                   "{:.1f}), dirty granules {}",
                   ms(timing_start, t_end), ms(timing_start, t_base), ms(t_base, t_evict),
                   ms(t_evict, t_end),
                   m_slots[slot].m_mem1_delta.page_count + m_slots[slot].m_mem2_delta.page_count);
    }
  }

  {
    const auto& saved = m_slots[slot];
    const u32 dirty_pages = saved.m_mem1_delta.page_count + saved.m_mem2_delta.page_count;
    m_stat_save_gran_total.fetch_add(dirty_pages, std::memory_order_relaxed);
    if (dirty_pages > m_stat_save_gran_max.load(std::memory_order_relaxed))
      m_stat_save_gran_max.store(dirty_pages, std::memory_order_relaxed);
    const size_t total_bytes = static_cast<size_t>(dirty_pages) * ROLLBACK_PAGE_SIZE +
                               saved.m_l1_cache_snapshot.size() + saved.m_save_buffer.size();
    DEBUG_LOG_FMT(BRAWLBACK,
                 "SaveFrame: slot {} dirty granules {} (mem1 {}, mem2 {}), size {:.2f} MB", slot,
                 dirty_pages, saved.m_mem1_delta.page_count, saved.m_mem2_delta.page_count,
                 static_cast<double>(total_bytes) / (1024.0 * 1024.0));
  }

  const u64 save_us = RecordTiming(timing_start, m_stat_save_count, m_stat_save_us_total,
                                   m_stat_save_us_max);
  {
    const auto& saved = m_slots[slot];
    if (saved.m_last_sync_waited)
    {
      m_stat_sync_count.fetch_add(1, std::memory_order_relaxed);
      m_stat_sync_us_total.fetch_add(saved.m_last_sync_us, std::memory_order_relaxed);
      if (saved.m_last_sync_us > m_stat_sync_us_max.load(std::memory_order_relaxed))
        m_stat_sync_us_max.store(saved.m_last_sync_us, std::memory_order_relaxed);
    }
    SaveSample sample;
    sample.save_us = static_cast<u32>(std::min<u64>(save_us, UINT32_MAX));
    sample.sync_us = static_cast<u32>(std::min<u64>(saved.m_last_sync_us, UINT32_MAX));
    sample.compiles = saved.m_last_sync_compiles;
    sample.evict_us = static_cast<u32>(std::min<u64>(evict_us, UINT32_MAX));
    sample.dostate_us = static_cast<u32>(std::min<u64>(saved.m_last_dostate_us, UINT32_MAX));
    sample.ram_us = static_cast<u32>(std::min<u64>(saved.m_last_ram_us, UINT32_MAX));
    std::lock_guard lk(m_sample_mutex);
    if (m_save_samples.size() < SAVE_SAMPLE_RING)
      m_save_samples.push_back(sample);
    else
      m_save_samples[m_save_sample_next % SAVE_SAMPLE_RING] = sample;
    m_save_sample_next++;
  }

#if ROLLBACK_VALIDATE
  RollbackSnapshot& snap = m_val_snapshots[slot];
  CaptureFullRamSnapshot(snap);
#endif
}

bool InSnapshotDoState()
{
  auto& rb = RollbackManager::Get();
  return rb.m_in_snapshot_dostate.load(std::memory_order_relaxed);
}

bool IsResimulating()
{
  return Machine::IsResimulationPass();
}

const u8* RollbackManager::ForChecksum(u32 phys, const u8* src, u8* scratch) const
{
  const u8* out = src;
  for (const MemoryRegion& m : m_checksum_masks)
  {
    if (m.phys_start >= phys + ROLLBACK_PAGE_SIZE || m.phys_end <= phys)
      continue;
    if (out == src)
    {
      std::memcpy(scratch, src, ROLLBACK_PAGE_SIZE);
      out = scratch;
    }
    const u32 from = std::max(m.phys_start, phys) - phys;
    const u32 to = std::min<u32>(m.phys_end, phys + ROLLBACK_PAGE_SIZE) - phys;
    std::memset(scratch + from, 0, to - from);
  }
  return out;
}

void RollbackManager::UpdateShadowAfterSave(int slot)
{
  // The save's copy jobs brought the shadow up to date and flagged the granules that changed it.
  const DeltaSaveSlot& saved = m_slots[slot];
  const u8* gpu = GPUWrittenBitmap::Get().entries;
  // The changed granules' (page, bytes) stream, to crc32 in blocks (GitHub issue #14).
  BatchedChangeCRC change_crc;
  u8 scratch[ROLLBACK_PAGE_SIZE];
  m_parts.changed.clear();
  for (const auto& [delta, first_page, changed] :
       {std::tuple{&saved.m_mem1_delta, 0u, saved.m_mem1_changed.data()},
        std::tuple{&saved.m_mem2_delta, MEM2_FIRST_PAGE, saved.m_mem2_changed.data()}})
  {
    for (u32 i = 0; i < delta->page_count; ++i)
    {
      if (!changed[i])
        continue;
      const u32 page = first_page + delta->page_indices[i];
      const u32 phys = page << ROLLBACK_PAGE_SHIFT;
      const u8* now = delta->page_data.data() + static_cast<size_t>(i) * ROLLBACK_PAGE_SIZE;
      const u8* counted = ForChecksum(phys, now, scratch);
      if (m_collect_parts)
        m_parts.changed.push_back({phys, Common::ComputeCRC32(counted, ROLLBACK_PAGE_SIZE), gpu[page] ? 1u : 0u});
      // Rendered output (GPU copies to RAM) and the regions snapshots exclude are not game state.
      if (gpu[page])
        continue;
      bool excluded = false;
      for (const MemoryRegion& r : m_exclude_regions)
        excluded |= phys < r.phys_end && phys + ROLLBACK_PAGE_SIZE > r.phys_start;
      if (excluded)
      {
        if (m_collect_parts)
          m_parts.changed.back()[2] = 2;
        continue;
      }
      change_crc.Feed(page, counted);
    }
  }
  const u32 crc = change_crc.Finish();
  m_last_change_crc = crc ? crc : 1;
}

u32 RollbackManager::LastSaveChecksum() const
{
  if (!m_initialized || m_ring_count == 0)
    return 0;
  if (!m_region_mode && m_shadow_valid)
    return m_last_change_crc;
  const DeltaSaveSlot& saved = m_slots[Wrap(m_ring_next - 1, NUM_SAVE_SLOTS)];
  const u8* gpu = GPUWrittenBitmap::Get().entries;
  u32 crc = Common::StartCRC32();
  u8 scratch[ROLLBACK_PAGE_SIZE];
  for (const auto& [delta, first_page] :
       {std::pair{&saved.m_mem1_delta, 0u}, std::pair{&saved.m_mem2_delta, MEM2_FIRST_PAGE}})
  {
    for (u32 i = 0; i < delta->page_count; ++i)
    {
      const u32 page = first_page + delta->page_indices[i];
      // Rendered output (GPU copies to RAM) and the regions snapshots exclude are not game state.
      if (gpu[page])
        continue;
      const u32 phys = page << ROLLBACK_PAGE_SHIFT;
      bool excluded = false;
      for (const MemoryRegion& r : m_exclude_regions)
      {
        if (phys < r.phys_end && phys + ROLLBACK_PAGE_SIZE > r.phys_start)
        {
          excluded = true;
          break;
        }
      }
      if (excluded)
        continue;
      crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(&page), sizeof(page));
      crc = Common::UpdateCRC32(
          crc,
          ForChecksum(phys, delta->page_data.data() + static_cast<size_t>(i) * ROLLBACK_PAGE_SIZE,
                      scratch),
          ROLLBACK_PAGE_SIZE);
    }
  }
  // GekkoNet takes 0 as "no checksum"; never report a real one as 0.
  return crc ? crc : 1;
}

u32 RollbackManager::SaveChecksumWithSlice(s64 frame) const
{
  u32 crc = LastSaveChecksum();
  if (!m_initialized || frame < 0)
    return crc;
  const u32 mem1_pages = static_cast<u32>(m_mem1_size / ROLLBACK_PAGE_SIZE);
  const u32 mem2_pages = static_cast<u32>(m_mem2_size / ROLLBACK_PAGE_SIZE);
  const u32 total = mem1_pages + mem2_pages;
  const u32 per = (total + CHECK_SLICES - 1) / CHECK_SLICES;
  const u32 first = static_cast<u32>(frame % CHECK_SLICES) * per;
  const u32 last = std::min(total, first + per);
  const u8* gpu = GPUWrittenBitmap::Get().entries;
  u32 slice_crc = Common::StartCRC32();
  u8 scratch[ROLLBACK_PAGE_SIZE];
  if (m_collect_parts)
  {
    m_parts.slice_first = first;
    m_parts.slice_gpu.clear();
  }
  for (u32 g = first; g < last; ++g)
  {
    const bool mem2 = g >= mem1_pages;
    const u32 page = mem2 ? MEM2_FIRST_PAGE + (g - mem1_pages) : g;
    if (gpu[page])
    {
      if (m_collect_parts)
        m_parts.slice_gpu.push_back(page << ROLLBACK_PAGE_SHIFT);
      continue;
    }
    const u32 phys = page << ROLLBACK_PAGE_SHIFT;
    bool excluded = false;
    for (const MemoryRegion& r : m_exclude_regions)
      excluded |= phys < r.phys_end && phys + ROLLBACK_PAGE_SIZE > r.phys_start;
    if (excluded)
      continue;
    const u8* src = ForChecksum(
        phys,
        mem2 ? m_mem2_ptr + static_cast<size_t>(g - mem1_pages) * ROLLBACK_PAGE_SIZE :
               m_mem1_ptr + static_cast<size_t>(g) * ROLLBACK_PAGE_SIZE,
        scratch);
    crc = Common::UpdateCRC32(crc, src, ROLLBACK_PAGE_SIZE);
    if (m_collect_parts)
      slice_crc = Common::UpdateCRC32(slice_crc, src, ROLLBACK_PAGE_SIZE);
  }
  if (m_collect_parts)
  {
    m_parts.change_crc = LastSaveChecksum();
    m_parts.slice_crc = slice_crc;
  }
  return crc ? crc : 1;
}

u32 RollbackManager::FullChecksum() const
{
  if (!m_initialized)
    return 0;
  const u32 mem1_pages = static_cast<u32>(m_mem1_size / ROLLBACK_PAGE_SIZE);
  const u32 mem2_pages = static_cast<u32>(m_mem2_size / ROLLBACK_PAGE_SIZE);
  const u8* gpu = GPUWrittenBitmap::Get().entries;
  u32 crc = Common::StartCRC32();
  u8 scratch[ROLLBACK_PAGE_SIZE];
  for (u32 g = 0; g < mem1_pages + mem2_pages; ++g)
  {
    const bool mem2 = g >= mem1_pages;
    const u32 page = mem2 ? MEM2_FIRST_PAGE + (g - mem1_pages) : g;
    if (gpu[page])
      continue;
    const u32 phys = page << ROLLBACK_PAGE_SHIFT;
    bool excluded = false;
    for (const MemoryRegion& r : m_exclude_regions)
      excluded |= phys < r.phys_end && phys + ROLLBACK_PAGE_SIZE > r.phys_start;
    if (excluded)
      continue;
    const u8* src = ForChecksum(
        phys,
        mem2 ? m_mem2_ptr + static_cast<size_t>(g - mem1_pages) * ROLLBACK_PAGE_SIZE :
               m_mem1_ptr + static_cast<size_t>(g) * ROLLBACK_PAGE_SIZE,
        scratch);
    crc = Common::UpdateCRC32(crc, src, ROLLBACK_PAGE_SIZE);
  }
  return crc ? crc : 1;
}

std::vector<std::pair<u32, u32>> RollbackManager::LastSaveDigest() const
{
  std::vector<std::pair<u32, u32>> out;
  if (!m_initialized || m_ring_count == 0)
    return out;
  const DeltaSaveSlot& saved = m_slots[Wrap(m_ring_next - 1, NUM_SAVE_SLOTS)];
  const u8* gpu = GPUWrittenBitmap::Get().entries;
  u8 scratch[ROLLBACK_PAGE_SIZE];
  for (const auto& [delta, first_page] :
       {std::pair{&saved.m_mem1_delta, 0u}, std::pair{&saved.m_mem2_delta, MEM2_FIRST_PAGE}})
  {
    for (u32 i = 0; i < delta->page_count; ++i)
    {
      const u32 page = first_page + delta->page_indices[i];
      if (gpu[page])
        continue;
      const u32 phys = page << ROLLBACK_PAGE_SHIFT;
      bool excluded = false;
      for (const MemoryRegion& r : m_exclude_regions)
        excluded |= phys < r.phys_end && phys + ROLLBACK_PAGE_SIZE > r.phys_start;
      if (excluded)
        continue;
      out.emplace_back(
          phys, Common::ComputeCRC32(
                    ForChecksum(phys,
                                delta->page_data.data() + static_cast<size_t>(i) * ROLLBACK_PAGE_SIZE,
                                scratch),
                    ROLLBACK_PAGE_SIZE));
    }
  }
  return out;
}

void RollbackManager::SetBrawlExclusions(bool brawl)
{
  m_brawl_exclusions = brawl;
  m_exclude_regions.clear();
  if (brawl)
    m_exclude_regions = s_brawlback_hardcoded_exclude_regions;
  if (m_mailbox_region)
    m_exclude_regions.push_back(*m_mailbox_region);
}

bool RollbackManager::JitKeepsBlocks(Core::System& system, const DeltaSaveSlot& slot,
                                     std::span<const u32> keys, std::span<const u16> changed)
{
  // The JIT keeps its blocks through this load when it only gained blocks since the snapshot, of
  // code that was there at the snapshot's frame and is again (JitBaseBlockCache::Invalidations,
  // RollbackManager::LoadFrame):
  auto& jit = system.GetJitInterface();
  // nothing compiled went away since (no line invalidated, no hint learned or dropped);
  if (jit.Invalidations() != slot.m_jit_invalidations)
    return false;
  // and no instruction a block was compiled from is changed by the restore (code written since
  // without invalidating the instruction cache; a REX code's data next to it is not code).
  for (size_t n = 0; n < keys.size(); ++n)
  {
    u32 bits = changed[n];
    const u32 granule = keys[n] << ROLLBACK_PAGE_SHIFT;
    while (bits != 0)
    {
      const u32 first = static_cast<u32>(std::countr_zero(bits));
      const u32 run = static_cast<u32>(std::countr_one(bits >> first));
      if (jit.HasCodeAt(granule + first * 4, run * 4))
        return false;
      bits &= ~(((1u << run) - 1) << first);
    }
  }
  return true;
}

bool RollbackManager::LoadFrame(Core::System& system, int frames_back)
{
  ROLLBACK_ZONE();
  if (!m_initialized)
    return false;

  ASSERT(frames_back >= 1 && m_ring_count >= 2 && frames_back < m_ring_count &&
         frames_back <= Rollback::NUM_SAVE_SLOTS - 1);
  const auto timing_start = std::chrono::steady_clock::now();

  // No live-stack exclusion: saves and loads both happen at the top of the game loop with the
  // same call chain, so the stack frames above r1 (mainLoopSub's and its callers') hold the same
  // return addresses and saved registers at both points, and their locals must roll back with the
  // rest of RAM. Excluding them left, e.g., a per-frame counter in an outer frame (0x805B5014) at
  // its post-snapshot value after every load.
  auto sorted_exclude_regions = std::make_shared<std::vector<MemoryRegion>>(m_exclude_regions);
  std::sort(sorted_exclude_regions->begin(), sorted_exclude_regions->end(),
            [](const MemoryRegion& left, const MemoryRegion& right) {
              return left.phys_start < right.phys_start;
            });

  const int most_recent = Wrap(m_ring_next - 1, NUM_SAVE_SLOTS);

  const int target_slot = Wrap(most_recent - frames_back, NUM_SAVE_SLOTS);

  static constexpr u32 BASE_SNAPSHOT_SENTINEL = UINT32_MAX;

  struct SourceEntry
  {
    // source slot index, or BASE_SNAPSHOT_SENTINEL
    u32 slot;
    // position within delta.page_indices/page_data
    u32 local_idx;
  };

  // oldest slot currently alive in the ring
  const int oldest_ring_slot = Wrap(m_ring_next - m_ring_count, NUM_SAVE_SLOTS);

  // Parallel arrays rather than a map: at 64-byte granularity a rollback can name hundreds of
  // thousands of granules, and every one of them is looked up exactly once.
  static std::vector<u32> restore_keys;
  static std::vector<SourceEntry> restore_srcs;
  // Per restored granule, the words whose bytes the restore changed (bit i: word i). Most granules
  // a rollback names hold the same bytes again (about 93% in REX: values written back, stores of
  // the same bytes): those are not copied.
  static std::vector<u16> restore_changed;
  // The granules the forward pass flagged in m_needs_source_mem1/2, which are all clear between
  // loads (each flag is cleared where its source is found): the indexing never walks or clears
  // the whole 1.4 million flags.
  static std::vector<u32> flagged_mem1;
  static std::vector<u32> flagged_mem2;

  DeltaSaveSlot& deltaSave = m_slots[target_slot];

  // The restore takes every granule that no slot in the ring holds from the base snapshot, and the
  // base snapshot is complete only once the eviction the last SaveFrame kicked has merged the
  // evicted slot into it. A load right after a save (every resimulation) used to read it while
  // that job was still copying, whenever the job pool's threads were slow to wake or preempted:
  // granules last written in the evicted slot's frame came back one frame older (or torn), so
  // rolled-back runs went wrong only under host load, mostly in effect/render objects that change
  // every frame (open issue 9 of docs/gameplay-rollback-status.md).
  {
    static const bool no_wait = std::getenv("PPR_GPRB_EVICT_NO_WAIT") != nullptr;  // diagnostics
    bool pending = false;
    if (no_wait)
    {
      pending = m_eviction_job &&
                m_eviction_job->unfinished_jobs.load(std::memory_order_acquire) != 0;
    }
    const u64 wait_us = no_wait ? 0 : WaitForEviction(&pending);
    if (pending)
    {
      m_stat_evict_waits.fetch_add(1, std::memory_order_relaxed);
      if (wait_us > m_stat_evict_wait_us_max.load(std::memory_order_relaxed))
        m_stat_evict_wait_us_max.store(wait_us, std::memory_order_relaxed);
    }
  }

  // indexing + RAM restore happens on a worker thread so they overlap with DoState on the main
  // thread
  auto restore_ram = [&]() {
    return job::KickRootJob(m_dispatch_thread, [&](job::JobTaskThread& w, job::Job& j) {
    u32 remaining = 0;
    {
      ROLLBACK_ZONE_N("ram page indexing - forward");
      flagged_mem1.clear();
      flagged_mem2.clear();

      for (int n = 0; n <= frames_back; n++)
      {
        const int slot = Wrap(target_slot + n, NUM_SAVE_SLOTS);
        const RegionDelta& d1 = m_slots[slot].m_mem1_delta;
        for (u32 i = 0; i < d1.page_count; i++)
        {
          const u32 idx = d1.page_indices[i];
          if (!m_needs_source_mem1[idx])
          {
            m_needs_source_mem1[idx] = 1;
            flagged_mem1.push_back(idx);
            remaining++;
          }
        }
        const RegionDelta& d2 = m_slots[slot].m_mem2_delta;
        for (u32 i = 0; i < d2.page_count; i++)
        {
          const u32 idx = d2.page_indices[i];
          if (!m_needs_source_mem2[idx])
          {
            m_needs_source_mem2[idx] = 1;
            flagged_mem2.push_back(idx);
            remaining++;
          }
        }
      }
    }

    // Walk from target_slot toward oldest. For each slot, satisfy any still-needed
    // pages found there
    {
      ROLLBACK_ZONE_N("ram page indexing - backward");
      restore_keys.clear();
      restore_srcs.clear();
      restore_keys.reserve(remaining);
      restore_srcs.reserve(remaining);

      for (int slot = target_slot;; slot = Wrap(slot - 1, NUM_SAVE_SLOTS))
      {
        const RegionDelta& d1 = m_slots[slot].m_mem1_delta;
        for (u32 i = 0; i < d1.page_count; i++)
        {
          const u32 idx = d1.page_indices[i];
          if (m_needs_source_mem1[idx])
          {
            m_needs_source_mem1[idx] = 0;
            restore_keys.push_back(idx);
            restore_srcs.push_back({static_cast<u32>(slot), i});
            remaining--;
          }
        }
        const RegionDelta& d2 = m_slots[slot].m_mem2_delta;
        for (u32 i = 0; i < d2.page_count; i++)
        {
          const u32 idx = d2.page_indices[i];
          if (m_needs_source_mem2[idx])
          {
            m_needs_source_mem2[idx] = 0;
            restore_keys.push_back(MEM2_FIRST_PAGE + idx);
            restore_srcs.push_back({static_cast<u32>(slot), i});
            remaining--;
          }
        }
        if (remaining == 0 || slot == oldest_ring_slot)
          break;
      }

      // Any pages still marked have no delta anywhere in the ring — use the base snapshot
      for (const u32 i : flagged_mem1)
      {
        if (m_needs_source_mem1[i])
        {
          m_needs_source_mem1[i] = 0;
          restore_keys.push_back(i);
          restore_srcs.push_back({BASE_SNAPSHOT_SENTINEL, 0});
        }
      }
      for (const u32 i : flagged_mem2)
      {
        if (m_needs_source_mem2[i])
        {
          m_needs_source_mem2[i] = 0;
          restore_keys.push_back(MEM2_FIRST_PAGE + i);
          restore_srcs.push_back({BASE_SNAPSHOT_SENTINEL, 0});
        }
      }
    }

    {
      ROLLBACK_ZONE_N("ram page restore");
#if defined(ROLLBACK_PROFILE_TRACY)
      auto x = StringFromFormat("Restored %u pages", static_cast<u32>(restore_keys.size()));
      ZoneText(x.c_str(), x.size());
#endif

      const u32 total = static_cast<u32>(restore_keys.size());
      m_last_load_granules = total;
      m_stat_load_gran_total.fetch_add(total, std::memory_order_relaxed);
      if (total > m_stat_load_gran_max.load(std::memory_order_relaxed))
        m_stat_load_gran_max.store(total, std::memory_order_relaxed);
      const u32 chunk = (total + SAVESTATE_NUM_WORK_CHUNKS - 1) / SAVESTATE_NUM_WORK_CHUNKS;
      restore_changed.resize(total);
      const bool with_shadow = !m_region_mode && m_shadow_valid;
      const std::array<u8*, 2> shadow_base{with_shadow ? m_shadow_mem1.get() : nullptr,
                                           with_shadow ? m_shadow_mem2.get() : nullptr};

      // One job per granule would be millions of jobs, so each job restores a contiguous slice.
      // Every granule is a distinct destination, so slices are safe to run concurrently.
      std::vector<job::Job*> page_jobs;
      page_jobs.reserve(SAVESTATE_NUM_WORK_CHUNKS);

      for (u32 offset = 0; offset < total; offset += chunk)
      {
        const u32 count = std::min(chunk, total - offset);
        page_jobs.push_back(w.create_job_as_child(
            j, [this, sorted_exclude_regions, offset, count, shadow_base](job::JobTaskThread&,
                                                                          job::Job&) {
              for (u32 n = offset; n < offset + count; n++)
              {
                const u32 page_key = restore_keys[n];
                const SourceEntry source_entry = restore_srcs[n];
                const bool isMem2 = (page_key >= MEM2_FIRST_PAGE);
                const u32 local_page = isMem2 ? (page_key - MEM2_FIRST_PAGE) : page_key;
                uint8_t* const dst =
                    (isMem2 ? m_mem2_ptr : m_mem1_ptr) + static_cast<size_t>(local_page) * ROLLBACK_PAGE_SIZE;
                const uint32_t dst_phys =
                    (isMem2 ? MEM2_BASE : 0u) + local_page * static_cast<uint32_t>(ROLLBACK_PAGE_SIZE);

                const uint8_t* src;
                if (source_entry.slot == BASE_SNAPSHOT_SENTINEL)
                {
                  const uint8_t* const snap_base =
                      isMem2 ? m_base_snapshot.mem2.get() : m_base_snapshot.mem1.get();
                  src = snap_base + static_cast<size_t>(local_page) * ROLLBACK_PAGE_SIZE;
                }
                else
                {
                  const Rollback::RegionDelta& src_delta =
                      isMem2 ? m_slots[source_entry.slot].m_mem2_delta :
                               m_slots[source_entry.slot].m_mem1_delta;
                  src = src_delta.page_data.data() +
                        static_cast<size_t>(source_entry.local_idx) * ROLLBACK_PAGE_SIZE;
                }

                if (m_region_mode)
                {
                  const size_t mask_idx =
                      isMem2 ? m_mem1_size / ROLLBACK_PAGE_SIZE + local_page : local_page;
                  if (m_region_mask[mask_idx] == 2)
                  {
                    // Only the bytes inside the set: the rest of the granule is live memory
                    // outside it (another heap, the GX FIFO, ...).
                    const u64 bits = m_region_partial.at(page_key);
                    bool outside_differs = false;
                    for (u32 b = 0; b < ROLLBACK_PAGE_SIZE; ++b)
                    {
                      if (bits & (1ULL << b))
                        dst[b] = src[b];
                      else if (dst[b] != src[b])
                        outside_differs = true;
                    }
                    if (outside_differs)
                    {
                      // A whole-granule restore would have written stale bytes here (diagnostics).
                      std::lock_guard lk(m_partial_stats_mutex);
                      ++m_partial_skipped[dst_phys];
                    }
                    continue;
                  }
                }
                // The shadow follows the machine back: the restored granules hold the loaded
                // frame's bytes.
                u8* const shadow =
                    shadow_base[isMem2] ?
                        shadow_base[isMem2] + static_cast<size_t>(local_page) * ROLLBACK_PAGE_SIZE :
                        nullptr;
                if (std::memcmp(dst, src, ROLLBACK_PAGE_SIZE) == 0)
                {
                  restore_changed[n] = 0;
                  if (shadow)
                    std::memcpy(shadow, dst, ROLLBACK_PAGE_SIZE);
                  continue;
                }
                u32 changed = 0;
                for (u32 w = 0; w < ROLLBACK_PAGE_SIZE / 4; ++w)
                {
                  u32 live, saved;
                  std::memcpy(&live, dst + w * 4, sizeof(live));
                  std::memcpy(&saved, src + w * 4, sizeof(saved));
                  if (live != saved)
                    changed |= 1u << w;
                }
                restore_changed[n] = static_cast<u16>(changed);
                // A granule clear of every exclusion is copied whole: savestateMemcpy walks the
                // exclusions for each byte range (GitHub issue #14).
                bool excluded = false;
                for (const MemoryRegion& r : *sorted_exclude_regions)
                  excluded |= dst_phys < r.phys_end && dst_phys + ROLLBACK_PAGE_SIZE > r.phys_start;
                if (excluded)
                  savestateMemcpy(dst, src, ROLLBACK_PAGE_SIZE, dst_phys, *sorted_exclude_regions);
                else
                  std::memcpy(dst, src, ROLLBACK_PAGE_SIZE);
                if (shadow)
                  std::memcpy(shadow, dst, ROLLBACK_PAGE_SIZE);
              }
            }));
      }

      // Kick all page restore jobs
      if (!page_jobs.empty())
        w.do_work_and_kick_jobs(page_jobs.data(), static_cast<uint16_t>(page_jobs.size()));
    }
    });
  };

  bool ok = true;
  // With the IOS devices in the snapshots (a whole-machine session), RAM must be back before their
  // states load: the emulated Bluetooth stack, for one, re-reads its pending IPC requests from
  // guest RAM while loading (it read the newer timeline's RAM and crashed on a null request). The
  // netplay path overlaps the two.
  job::Job* ram_job = nullptr;
  if (!m_region_mode && m_full_ios_in_snapshots)
  {
    ROLLBACK_ZONE_N("RAM restore before devices");
    job::DrainJobsUntilComplete(m_dispatch_thread, restore_ram());
  }
  if (!m_region_mode)
  {
    ROLLBACK_ZONE_N("DoState restore");
    // The video thread finishes everything queued, including the captures of every snapshot.
    const auto dostate_start = std::chrono::steady_clock::now();
    m_last_load_sync_us = SyncGPUForDoState(system);
    BeginDoState();
    VideoCommon_SetRollbackSplitState(deltaSave.m_split_video);
    ok = State::LoadFromBuffer(
        system, std::span<uint8_t>(deltaSave.m_save_buffer.data(), deltaSave.m_save_buffer.size()));
    VideoCommon_SetRollbackSplitState(false);
    if (deltaSave.m_split_video)
      deltaSave.RestoreVideoState();
    EndDoState();
    m_last_load_aram_bytes = system.GetDSP().LoadARAMSnapshot(deltaSave.m_aram);
    system.GetMemory().LoadFakeVMEMSnapshot(deltaSave.m_fake_vmem);
    m_last_load_dostate_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  std::chrono::steady_clock::now() - dostate_start)
                                                  .count()) -
                             m_last_load_sync_us;
  }

  if (m_region_mode || !m_full_ios_in_snapshots)
    ram_job = restore_ram();

  if (m_region_mode)
  {
    // Gameplay-only: memory and the main thread's registers at the loop top. Time, devices and the
    // CPU's slice position keep running forward.
    const u32 sp_now = system.GetPowerPC().GetPPCState().gpr[1];
    if (deltaSave.m_cpu.gpr[1] != sp_now)
    {
      WARN_LOG_FMT(BRAWLBACK, "Region load: slot {} r1 {:08x} differs from the live r1 {:08x} (pc {:08x})",
                   target_slot, deltaSave.m_cpu.gpr[1], sp_now,
                   system.GetPowerPC().GetPPCState().pc);
    }
    RestoreCPURegisters(system.GetPowerPC().GetPPCState(), deltaSave.m_cpu);
  }
  else
  {
    // CoreTiming::DoState restores global_timer/slice_length, but the rollback DoState skips the
    // CPU registers, which include ppc_state.downcount and the pending Exceptions. Advance()
    // derives elapsed cycles as slice_length - downcount, so restore the downcount saved with this
    // slot too: the current time is then exactly the saved one. (Zeroing slice_length and downcount
    // instead, as before, kept the timer consistent but moved the restored time back to the start
    // of the slice, so every resimulation saw OSGetTick and the next interrupts shifted by up to a
    // slice from the original run.)
    system.GetPowerPC().GetPPCState().downcount = deltaSave.m_downcount;
    // Together with its slice: anything during the load that shortened the slice
    // (ForceExceptionCheck) left slice_length - downcount, the cycles run so far, wrong.
    system.GetCoreTiming().GetGlobals().slice_length = deltaSave.m_slice_length;
    system.GetPowerPC().GetPPCState().Exceptions = deltaSave.m_exceptions;
    RestoreCPURegisters(system.GetPowerPC().GetPPCState(), deltaSave.m_cpu);
    RestoreMachineRegisters(system, deltaSave);
  }

  if (!m_region_mode)
  {
    ROLLBACK_ZONE_N("L1 cache restore");
    if (m_l1_cache_ptr && m_l1_cache_size > 0 && deltaSave.m_l1_cache_snapshot.data())
      std::memcpy(m_l1_cache_ptr, deltaSave.m_l1_cache_snapshot.data(), m_l1_cache_size);
  }

  if (ram_job)
    job::DrainJobsUntilComplete(m_dispatch_thread, ram_job);

  if (!m_region_mode)
  {
    // The JIT's blocks for restored memory were compiled from the timeline being thrown away: a
    // module the newer timeline loaded where the restored one still runs other code (a scene
    // change) left blocks of the wrong code in the cache, and the re-run diverged.
    ROLLBACK_ZONE_N("JIT blocks of restored memory");
    // Only when the JIT lost code since the loaded snapshot (JitBaseBlockCache::Invalidations:
    // the game invalidated lines, a module came in, a hint was learned). Otherwise the cache only
    // gained blocks since, of code that was there at the snapshot's frame, and everything stays:
    // dropping every block of every restored granule at every rollback threw away hundreds of a
    // REX frame's blocks (its codes keep their data next to their code; most restored granules
    // hold the same bytes again), and the re-run compiled them again: 10 of a re-run frame's
    // 14 ms on this PC with fast input (2026-10-14).
    // It is one or the other, never some of the blocks: compiling reads the code through the
    // emulated instruction cache (machine state), and a block recompiled with the hints learned
    // since (a gather pipe write's interrupt check) takes interrupts elsewhere, so a partial rule
    // can make a re-run differ from both the first run and the old full drop.
    // Diagnostics: PPR_MACHINE_NO_JIT_ERASE keeps them all (to tell whether recompiled blocks are
    // what a re-run differs by); PPR_MACHINE_JIT_ERASE_ALL drops those of every restored granule,
    // as before.
    static const bool keep_blocks = std::getenv("PPR_MACHINE_NO_JIT_ERASE") != nullptr;
    static const bool erase_all = std::getenv("PPR_MACHINE_JIT_ERASE_ALL") != nullptr;
    const auto jit_start = std::chrono::steady_clock::now();
    auto& jit = system.GetJitInterface();
    u32 changed_granules = 0;
    for (const u16 changed : restore_changed)
      changed_granules += changed != 0;
    m_last_load_changed = changed_granules;
    m_last_load_jit_dropped = false;
    if (erase_all ||
        (!keep_blocks && !JitKeepsBlocks(system, deltaSave, restore_keys, restore_changed)))
    {
      for (const u32 key : restore_keys)
        jit.EraseBlocksForRestoredRange(key << ROLLBACK_PAGE_SHIFT, ROLLBACK_PAGE_SIZE);
      m_last_load_jit_dropped = true;
    }
    m_last_load_jit_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - jit_start)
                                              .count());
  }

#if ROLLBACK_VALIDATE
  CompareValSnapshot(target_slot, frames_back);
#endif


  {
    ROLLBACK_ZONE_N("Clear JIT dirty bitmap");
    auto& bitmap = JITDirtyBitmap::Get();
    bitmap.ScanAndClear(0, static_cast<uint32_t>(m_mem1_size / ROLLBACK_PAGE_SIZE), [](u32) {});
    if (m_mem2_ptr && m_mem2_size > 0)
    {
      bitmap.ScanAndClear(MEM2_FIRST_PAGE, static_cast<uint32_t>(m_mem2_size / ROLLBACK_PAGE_SIZE),
                          [](u32) {});
    }
  }

  RecordTiming(timing_start, m_stat_load_count, m_stat_load_us_total, m_stat_load_us_max);

  // After loading, the target slot becomes the new "oldest" slot,
  // so the next save will overwrite the next slot.
  m_ring_next = Wrap(target_slot + 1, NUM_SAVE_SLOTS);
  m_ring_count = m_ring_count - frames_back;

  if (ok)
  {
    ROLLBACK_ZONE_N("log");
    const u32 loaded_frame = m_slots[target_slot].brawl_frame;
    DEBUG_LOG_FMT(BRAWLBACK, "Rolled back {} frame(s) - loaded slot {} (frame {})",
                 frames_back, target_slot, loaded_frame);
  }
  else
  {
    ERROR_LOG_FMT(BRAWLBACK, "Rollback failed");
    OSD::AddMessage("Rollback state load failed", 3000, OSD::Color::RED);
  }
  return ok;
}

}  // namespace Rollback
