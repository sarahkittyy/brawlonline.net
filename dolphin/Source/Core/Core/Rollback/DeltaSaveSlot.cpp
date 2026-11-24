
#include "Core/Rollback/DeltaSaveSlot.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>

#include <Common/Assert.h>
#include "Common/ChunkFile.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/ChangeCRC.h"
#include "Core/Rollback/KeptGranules.h"
#include "Core/Rollback/Perf.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{
uint8_t* JITDirtyBitmap::AllocateEntries()
{
  const size_t size = ENTRY_COUNT + ENTRY_SLACK;
  void* entries = nullptr;
#ifdef _WIN32
  // Any free place in the low 2 GB, from 256 MB up (an address has to be asked for by number).
  for (uintptr_t at = 0x10000000; !entries && at < 0x78000000; at += 0x01000000)
  {
    entries = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT,
                           PAGE_READWRITE);
  }
  if (!entries)
    entries = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
#ifdef MAP_32BIT
  entries = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                 -1, 0);
  if (entries == MAP_FAILED)
    entries = nullptr;
#endif
  if (!entries)
  {
    entries = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (entries == MAP_FAILED)
      entries = nullptr;
  }
#endif
  // Both give zeroed pages.
  if (!entries)
    std::abort();
  return static_cast<uint8_t*>(entries);
}

// memcpy that skips subranges that are in our exclusion list.
// Callers copy one ROLLBACK_PAGE_SIZE (64-byte) granule at a time, so a plain memcpy is as fast as any
// wide non-temporal loop (the old AVX2 one never ran for blocks under 256 bytes) and it is
// portable: no AVX2 requirement on x86-64 and nothing x86-specific on ARM64.
// TODO: profile this, if it's slow maybe we can sort the exclusion list beforehand
void savestateMemcpy(void* dst, const void* src, size_t size, uint32_t dst_phys,
                     const std::vector<MemoryRegion>& excl)
{
  if (excl.empty())
  {
    std::memcpy(dst, src, size);
    return;
  }

  auto* d = static_cast<uint8_t*>(dst);
  const auto* s = static_cast<const uint8_t*>(src);

  uint32_t cursor = dst_phys;
  const uint32_t end = dst_phys + static_cast<uint32_t>(size);

  while (cursor < end)
  {
    // Find the next excluded region that overlaps or comes after cursor
    uint32_t seg_end = end;
    bool found_exclusion = false;

    for (const auto& r : excl)
    {
      if (r.phys_end <= cursor)
        continue;  // Region is before current cursor

      if (r.phys_start < end)
      {
        if (r.phys_start > cursor)
        {
          // There's a copyable segment before this exclusion
          seg_end = std::min(seg_end, r.phys_start);
        }
        else if (cursor >= r.phys_start && cursor < r.phys_end)
        {
          // Cursor is inside an excluded region; skip to its end
          cursor = std::min(r.phys_end, end);
          found_exclusion = true;
          break;
        }
      }
    }

    if (found_exclusion)
      continue;  // Restart loop with updated cursor

    // Copy the safe segment [cursor, seg_end)
    const size_t offset = cursor - dst_phys;
    const size_t copy_size = seg_end - cursor;
    if (copy_size > 0)
    {
      std::memcpy(d + offset, s + offset, copy_size);
    }

    cursor = seg_end;
  }
}



void DeltaSaveSlot::Init(uint8_t* mem1_ptr, size_t mem1_size, uint8_t* mem2_ptr, size_t mem2_size,
                         uint8_t* l1_cache_ptr, size_t l1_cache_size)
{
  m_mem1_ptr = mem1_ptr;
  m_mem1_size = mem1_size;
  m_mem2_ptr = mem2_ptr;
  m_mem2_size = mem2_size;
  m_l1_cache_ptr = l1_cache_ptr;
  m_l1_cache_size = l1_cache_size;
  m_mem1_page_count = static_cast<uint32_t>(mem1_size / ROLLBACK_PAGE_SIZE);
  m_mem2_page_count = static_cast<uint32_t>(mem2_size / ROLLBACK_PAGE_SIZE);

  if (l1_cache_ptr && l1_cache_size > 0)
    m_l1_cache_snapshot.reset(l1_cache_size);

  m_has_state = false;
}

void DeltaSaveSlot::Reset()
{
  m_has_state = false;
  m_has_full_state = false;
  m_has_change_crc = false;
  m_last_change_crc = 0;
  brawl_frame = 0;
  m_mem1_delta.Reset();
  m_mem2_delta.Reset();
  m_save_buffer.reset();
  m_aram.Reset();
  m_fake_vmem.Reset();
  m_tmem.Reset();
}

// region_phys_base: Wii physical base of the region
//   MEM1 -> 0,          MEM2 -> 0x10000000
void RestoreRegionDelta(const RegionDelta& delta, uint8_t* region_base, uint32_t region_phys_base,
                        const std::vector<MemoryRegion>& excl)
{
  for (uint32_t i = 0; i < delta.page_count; ++i)
  {
    const uint32_t page_idx = delta.page_indices[i];
    uint8_t* const dst = region_base + static_cast<size_t>(page_idx) * ROLLBACK_PAGE_SIZE;
    const uint8_t* const src = delta.page_data.data() + static_cast<size_t>(i) * ROLLBACK_PAGE_SIZE;
    const uint32_t dst_phys = region_phys_base + page_idx * static_cast<uint32_t>(ROLLBACK_PAGE_SIZE);
    savestateMemcpy(dst, src, ROLLBACK_PAGE_SIZE, dst_phys, excl);
  }
}

// What a region's copy jobs kept: job n kept counts[n] granules, written from n * split on
// (CompactKeptGranules puts them together once the jobs are done).
struct KeptChunks
{
  u32 split = 0;
  std::array<u32, SAVESTATE_NUM_WORK_CHUNKS> counts{};
};

// The region's dirty granules (RollbackManager::ScanDirtyMarks: cleared in the bitmap as they are
// found) go to `scan`, and jobs copy them to `out`. `scan` and `kept` must outlive the jobs.
// With a `shadow` (RollbackManager's, whole-machine mode), the jobs keep only the granules whose
// bytes differ from it and bring it up to date with them (UpdateShadowAfterSave).
void enqueueSubsectionJobs(u32 first_page, u32 page_count, const uint8_t* region_base,
                           Rollback::RegionDelta& out, RollbackManager& rbm,
                           job::JobTaskThread& w, job::Job& root, std::vector<job::Job*>& jar,
                           u32 num_work_chunks, std::vector<u32>& scan, u8* shadow,
                           KeptChunks& kept)
{
  ASSERT(num_work_chunks <= kept.counts.size());
  kept = {};
  std::vector<u32>* const dirty_pages = &scan;
  {
    ROLLBACK_ZONE_N("dirty page population");
    rbm.ScanDirtyMarks(first_page, first_page + page_count, dirty_pages);
  }

  u32 num_dirty = static_cast<u32>(dirty_pages->size());
  out.page_count = num_dirty;
  if (!num_dirty)
    return;
  {
    ROLLBACK_ZONE_N("delta buffer alloc");
    if (out.page_indices.size() < num_dirty)
      out.page_indices.reset(num_dirty);
    if (out.page_data.size() < (num_dirty * ROLLBACK_PAGE_SIZE))
      out.page_data.reset(num_dirty * ROLLBACK_PAGE_SIZE);
  }

  // Capture pointers to the already-allocated buffers to avoid reference capture issues
  uint32_t* page_indices_ptr = out.page_indices.data();
  uint8_t* page_data_ptr = out.page_data.data();
  KeptChunks* const kept_ptr = &kept;

  auto copySubsectionFn = [dirty_pages, region_base, page_indices_ptr, page_data_ptr, first_page,
                           shadow, kept_ptr](u32 chunk, u32 offset, u32 size) {
    ROLLBACK_ZONE_N("copy dirty pages");
    if (offset >= dirty_pages->size())
      return;  // Nothing to do for this subsection

    u32 next = offset;  // where the next granule kept goes
    for (uint32_t dirtyPagesIndex = offset;
         dirtyPagesIndex < offset + size && dirtyPagesIndex < dirty_pages->size();
         ++dirtyPagesIndex)
    {
      u32 pageidx = (*dirty_pages)[dirtyPagesIndex];

      // pageidx is global; subtract first_page to get region-relative index
      u32 relative_page_idx = pageidx - first_page;
      const u8* const src =
          region_base + static_cast<size_t>(relative_page_idx) * ROLLBACK_PAGE_SIZE;
      u8* const saved = page_data_ptr + static_cast<size_t>(next) * ROLLBACK_PAGE_SIZE;
      if (shadow)
      {
        // Only what changed since the previous save: a granule written with the bytes it held is
        // in an older slot or the base snapshot with those bytes, and a load finds it there. RAM
        // is read once (the GPU thread may write it meanwhile), as words compared with the
        // shadow's: faster than memcmp and a copy, or comparing a copy just stored.
        u8* const before = shadow + static_cast<size_t>(relative_page_idx) * ROLLBACK_PAGE_SIZE;
        u64 now[ROLLBACK_PAGE_SIZE / 8], was[ROLLBACK_PAGE_SIZE / 8];
        std::memcpy(now, src, ROLLBACK_PAGE_SIZE);
        std::memcpy(was, before, ROLLBACK_PAGE_SIZE);
        u64 differ = 0;
        for (size_t word = 0; word < ROLLBACK_PAGE_SIZE / 8; ++word)
          differ |= now[word] ^ was[word];
        if (differ == 0)
          continue;
        std::memcpy(before, now, ROLLBACK_PAGE_SIZE);
        std::memcpy(saved, now, ROLLBACK_PAGE_SIZE);
      }
      else
      {
        std::memcpy(saved, src, ROLLBACK_PAGE_SIZE);
      }
      page_indices_ptr[next++] = relative_page_idx;
    }
    kept_ptr->counts[chunk] = next - offset;
#if defined(HAVE_TRACY)
    auto x = std::format("page count %u (offset %u size %u)", next - offset, offset, size);
    ZoneText(x.c_str(), x.size());
#endif
  };

  u32 split_size = (num_dirty + num_work_chunks - 1) / num_work_chunks;
  kept.split = split_size;
  for (u32 i = 0; i < num_work_chunks; ++i)
  {
    u32 offset = i * split_size;
    u32 size = std::min(split_size, num_dirty - offset);
    jar.push_back(w.create_job_as_child(
        root, [copySubsectionFn, i, offset, size](job::JobTaskThread& t, job::Job& j) {
          copySubsectionFn(i, offset, size);
        }));
  }
}

// The SPRs a game changes at run time without MMU or mode side effects.
static constexpr std::array<u32, 18> kRollbackSPRs{
    SPR_LR,       SPR_CTR,      SPR_DSISR,    SPR_DAR,      SPR_SRR0,     SPR_SRR0 + 1,
    SPR_SPRG0,    SPR_SPRG0 + 1, SPR_SPRG0 + 2, SPR_SPRG0 + 3, SPR_GQR0,     SPR_GQR0 + 1,
    SPR_GQR0 + 2, SPR_GQR0 + 3, SPR_GQR0 + 4, SPR_GQR0 + 5, SPR_GQR0 + 6, SPR_GQR0 + 7,
};

void SaveCPURegisters(const PowerPC::PowerPCState& ppc, DeltaSaveSlot::CPURegisters& out)
{
  std::copy(std::begin(ppc.gpr), std::end(ppc.gpr), out.gpr.begin());
  std::copy(std::begin(ppc.ps), std::end(ppc.ps), out.ps.begin());
  std::copy(std::begin(ppc.cr.fields), std::end(ppc.cr.fields), out.cr.begin());
  out.fpscr = ppc.CurrentFPSCR();
  out.xer_ca = ppc.xer_ca;
  out.xer_so_ov = ppc.xer_so_ov;
  out.xer_stringctrl = ppc.xer_stringctrl;
  for (size_t i = 0; i < kRollbackSPRs.size(); ++i)
    out.spr[i] = ppc.spr[kRollbackSPRs[i]];
}

void RestoreCPURegisters(PowerPC::PowerPCState& ppc, const DeltaSaveSlot::CPURegisters& in)
{
  std::copy(in.gpr.begin(), in.gpr.end(), std::begin(ppc.gpr));
  std::copy(in.ps.begin(), in.ps.end(), std::begin(ppc.ps));
  std::copy(in.cr.begin(), in.cr.end(), std::begin(ppc.cr.fields));
  const bool rounding_changed = ppc.fpscr.Hex != in.fpscr;
  ppc.fpscr.Hex = in.fpscr;
  ppc.fprf_lazy = 0;
  if (rounding_changed)
    PowerPC::RoundingModeUpdated(ppc);
  ppc.xer_ca = in.xer_ca;
  ppc.xer_so_ov = in.xer_so_ov;
  ppc.xer_stringctrl = in.xer_stringctrl;
  for (size_t i = 0; i < kRollbackSPRs.size(); ++i)
    ppc.spr[kRollbackSPRs[i]] = in.spr[i];
}

void RestoreMachineRegisters(Core::System& system, const DeltaSaveSlot& slot)
{
  auto& power_pc = system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto differ = [&](u32 first, u32 count) {
    return !std::equal(slot.m_all_spr.begin() + first, slot.m_all_spr.begin() + first + count,
                       std::begin(ppc.spr) + first);
  };
  const bool bats = differ(SPR_IBAT0U, 16) || differ(SPR_IBAT4U, 16) || differ(SPR_HID4, 1);
  const bool mmcr = differ(SPR_MMCR0, 1) || differ(SPR_MMCR1, 1);
  std::copy(slot.m_all_spr.begin(), slot.m_all_spr.end(), std::begin(ppc.spr));
  if (bats)
  {
    system.GetMMU().IBATUpdated();
    system.GetMMU().DBATUpdated();
  }
  if (mmcr)
    PowerPC::MMCRUpdated(ppc);
  if (ppc.msr.Hex != slot.m_msr)
  {
    ppc.msr.Hex = slot.m_msr;
    power_pc.MSRUpdated();
  }
  ppc.pc = slot.m_pc;
  ppc.npc = slot.m_npc;
  ppc.reserve_address = slot.m_reserve_address;
  ppc.reserve = slot.m_reserve;
}

// Fused whole-machine RAM save worker: scans a 64-entry-aligned slice of JITDirtyBitmap directly,
// compares each dirty granule against the rolling shadow, updates the shadow, stores changed
// granules into `scratch`, and accumulates the chunk's change CRC32 while the bytes are hot in
// registers.
void RunFusedRegionChunk(u32 first_page, u32 slice_first, u32 slice_end, const u8* region_base,
                         u8* shadow, const u8* gpu, std::span<const MemoryRegion> exclude_regions,
                         std::span<const MemoryRegion> checksum_masks,
                         DeltaSaveSlot::ChunkScratch& scratch)
{
  ROLLBACK_ZONE_N("fused scan+copy+crc");
  scratch.indices.clear();
  scratch.data.clear();
  scratch.crc = Common::StartCRC32();
  scratch.crc_bytes = 0;
  if (slice_first >= slice_end)
    return;

  BatchedChangeCRC chunk_crc;
  u8 mask_scratch[ROLLBACK_PAGE_SIZE];
  JITDirtyBitmap::Get().ScanAndClear(slice_first, slice_end - slice_first, [&](u32 pageidx) {
    const u32 rel = pageidx - first_page;
    const u8* const src = region_base + static_cast<size_t>(rel) * ROLLBACK_PAGE_SIZE;
    u8* const before = shadow + static_cast<size_t>(rel) * ROLLBACK_PAGE_SIZE;

    u64 now[ROLLBACK_PAGE_SIZE / 8];
    u64 was[ROLLBACK_PAGE_SIZE / 8];
    std::memcpy(now, src, ROLLBACK_PAGE_SIZE);
    std::memcpy(was, before, ROLLBACK_PAGE_SIZE);
    u64 differ = 0;
    for (size_t word = 0; word < ROLLBACK_PAGE_SIZE / 8; ++word)
      differ |= now[word] ^ was[word];
    if (differ == 0)
      return;

    std::memcpy(before, now, ROLLBACK_PAGE_SIZE);
    scratch.indices.push_back(rel);
    DeltaSaveSlot::Granule g;
    std::memcpy(g.bytes, now, ROLLBACK_PAGE_SIZE);
    scratch.data.push_back(g);

    if (gpu[pageidx])
      return;
    const u32 phys = pageidx << ROLLBACK_PAGE_SHIFT;
    for (const MemoryRegion& r : exclude_regions)
    {
      if (phys < r.phys_end && phys + ROLLBACK_PAGE_SIZE > r.phys_start)
        return;
    }
    const u8* const counted =
        MaskedForChecksum(phys, reinterpret_cast<const u8*>(now), checksum_masks, mask_scratch);
    chunk_crc.Feed(pageidx, counted);
  });

  scratch.crc = chunk_crc.Finish();
  scratch.crc_bytes = chunk_crc.TotalBytes();
}

void AssembleRegionChunks(const std::array<DeltaSaveSlot::ChunkScratch, SAVESTATE_NUM_WORK_CHUNKS>& chunks,
                          RegionDelta& out)
{
  u32 total = 0;
  for (const auto& c : chunks)
    total += static_cast<u32>(c.indices.size());
  out.page_count = total;
  if (total == 0)
    return;
  if (out.page_indices.size() < total)
    out.page_indices.reset(total);
  if (out.page_data.size() < static_cast<size_t>(total) * ROLLBACK_PAGE_SIZE)
    out.page_data.reset(static_cast<size_t>(total) * ROLLBACK_PAGE_SIZE);
  u32 offset = 0;
  for (const auto& c : chunks)
  {
    const u32 n = static_cast<u32>(c.indices.size());
    if (n == 0)
      continue;
    std::memcpy(out.page_indices.data() + offset, c.indices.data(), n * sizeof(u32));
    std::memcpy(out.page_data.data() + static_cast<size_t>(offset) * ROLLBACK_PAGE_SIZE,
                c.data.data(), static_cast<size_t>(n) * ROLLBACK_PAGE_SIZE);
    offset += n;
  }
}

void DeltaSaveSlot::Save(Core::System& system, bool full_state, s64 slice_frame,
                         u32* out_slice_crc, size_t* out_slice_bytes)
{
  ROLLBACK_ZONE();
  ASSERT(m_mem1_ptr);
  ASSERT(m_mem1_page_count < MEM2_FIRST_PAGE);

  brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  m_downcount = system.GetPPCState().downcount;
  m_jit_invalidations = system.GetJitInterface().Invalidations();
  m_jit_code_mark = system.GetPPCState().iCache.jit_code.Mark();
  m_slice_length = system.GetCoreTiming().GetGlobals().slice_length;
  m_exceptions = system.GetPPCState().Exceptions;
  m_msr = system.GetPPCState().msr.Hex;
  m_pc = system.GetPPCState().pc;
  m_npc = system.GetPPCState().npc;
  m_reserve_address = system.GetPPCState().reserve_address;
  m_reserve = system.GetPPCState().reserve;
  std::copy(std::begin(system.GetPPCState().spr), std::end(system.GetPPCState().spr),
            m_all_spr.begin());
  SaveCPURegisters(system.GetPPCState(), m_cpu);
  auto& rbm = RollbackManager::Get();
  auto* dt = rbm.m_dispatch_thread;
  ASSERT(dt);
  const auto dostate_start = std::chrono::steady_clock::now();

  const u8* region_mask = rbm.RegionMask();
  u8* const shadow1 = rbm.SaveShadow(false);
  u8* const shadow2 = rbm.SaveShadow(true);
  m_has_change_crc = false;
  m_has_full_state = full_state;

  // Whole-machine fused fast path: parallelizes JITDirtyBitmap scanning across worker chunks,
  // computes the change CRC32 in-register while updating the shadow, overlaps State::SaveToBuffer
  // with the worker RAM + L1 + slice-CRC jobs, and skips non-RAM state on already-confirmed
  // frames that can never be loaded again.
  if (!region_mask && shadow1)
  {
    const bool split = RollbackManager::UseSplitVideoState(system);
    const u64 compiles_before = VideoCommon::GetSyncPipelineCompileStats().count;
    m_last_sync_waited = false;
    m_last_sync_us = 0;
    if (full_state && !split)
    {
      m_last_sync_waited = system.IsDualCoreMode();
      m_last_sync_us = RollbackManager::SyncGPUForDoState(system);
    }
    else if (m_video_capture.load(std::memory_order_acquire) == VideoCapture::Pending)
    {
      // The video thread has not reached this slot's previous snapshot yet, i.e. it is a whole
      // ring of snapshots behind. Let it catch up before the slot is reused (rare).
      const auto start = std::chrono::steady_clock::now();
      auto& fifo = system.GetFifo();
      fifo.WaitForGpuThreadIdle();
      if (m_video_capture.load(std::memory_order_acquire) == VideoCapture::Pending)
        fifo.DropVideoThreadCaptures();  // paused: the video thread runs nothing
      m_last_sync_waited = true;
      m_last_sync_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count());
    }
    m_last_sync_compiles =
        static_cast<u32>(VideoCommon::GetSyncPipelineCompileStats().count - compiles_before);

    // Kick RAM scan+copy+CRC, L1 cache copy, and slice CRC jobs before State::SaveToBuffer so
    // they overlap with CPU-thread device serialization (in rollback write mode DoState does not
    // write guest RAM).
    const u8* const gpu = GPUWrittenBitmap::Get().entries;
    const std::span<const MemoryRegion> exclude_regions = rbm.m_exclude_regions;
    const std::span<const MemoryRegion> checksum_masks = rbm.m_checksum_masks;

    job::Job* l1cachejob = nullptr;
    if (full_state && m_l1_cache_ptr && m_l1_cache_size > 0 && m_l1_cache_snapshot.data())
    {
      l1cachejob = job::KickRootJob(dt, [this](job::JobTaskThread&, job::Job&) {
        ROLLBACK_ZONE_N("L1 cache save");
        std::memcpy(m_l1_cache_snapshot.data(), m_l1_cache_ptr, m_l1_cache_size);
      });
    }

    job::Job* slice_job = nullptr;
    const u32 total_ram_pages = m_mem1_page_count + m_mem2_page_count;
    if (slice_frame >= 0 && out_slice_crc && out_slice_bytes && !rbm.m_collect_parts &&
        total_ram_pages > 0)
    {
      const CountedRAM counted_ram = rbm.CountedRam();
      const u32 per =
          (total_ram_pages + RollbackManager::CHECK_SLICES - 1) / RollbackManager::CHECK_SLICES;
      const u32 first = static_cast<u32>(slice_frame % RollbackManager::CHECK_SLICES) * per;
      const u32 last = std::min(total_ram_pages, first + per);
      slice_job = job::KickRootJob(
          dt, [counted_ram, first, last, out_slice_crc, out_slice_bytes](job::JobTaskThread&,
                                                                         job::Job&) {
            ROLLBACK_ZONE_N("slice checksum");
            RunCRC crc;
            FeedCountedGranules(counted_ram, first, last, crc, nullptr, nullptr);
            *out_slice_crc = crc.Finish();
            *out_slice_bytes = crc.TotalBytes();
          });
    }

    for (auto& c : m_mem1_chunks)
    {
      c.indices.clear();
      c.data.clear();
      c.crc = Common::StartCRC32();
      c.crc_bytes = 0;
    }
    for (auto& c : m_mem2_chunks)
    {
      c.indices.clear();
      c.data.clear();
      c.crc = Common::StartCRC32();
      c.crc_bytes = 0;
    }

    job::Job* ram_job = job::KickRootJob(
        dt, [this, shadow1, shadow2, gpu, exclude_regions, checksum_masks](job::JobTaskThread& w,
                                                                           job::Job& root) {
          ROLLBACK_ZONE_N("fused ram save dispatch");
          std::vector<job::Job*> jar;
          jar.reserve(SAVESTATE_NUM_WORK_CHUNKS * 2);

          const auto enqueue_region = [&](u32 first_page, u32 page_count, const u8* region_base,
                                          u8* shadow,
                                          std::array<ChunkScratch, SAVESTATE_NUM_WORK_CHUNKS>& chunks) {
            if (page_count == 0 || !shadow)
              return;
            // Align chunk boundaries to 64 bitmap entries (1 cache line) so ScanAndClear runs full
            // SIMD lines with zero overlap or false sharing between workers.
            const u32 raw_chunk = (page_count + SAVESTATE_NUM_WORK_CHUNKS - 1) / SAVESTATE_NUM_WORK_CHUNKS;
            const u32 chunk_pages = (raw_chunk + 63u) & ~63u;
            for (u32 i = 0; i < SAVESTATE_NUM_WORK_CHUNKS; ++i)
            {
              const u32 rel_begin = std::min(i * chunk_pages, page_count);
              const u32 rel_end = std::min(rel_begin + chunk_pages, page_count);
              ChunkScratch* const scratch = &chunks[i];
              jar.push_back(w.create_job_as_child(
                  root, [first_page, a = first_page + rel_begin, b = first_page + rel_end,
                         region_base, shadow, gpu, exclude_regions, checksum_masks,
                         scratch](job::JobTaskThread&, job::Job&) {
                    RunFusedRegionChunk(first_page, a, b, region_base, shadow, gpu,
                                        exclude_regions, checksum_masks, *scratch);
                  }));
            }
          };

          enqueue_region(0, m_mem1_page_count, m_mem1_ptr, shadow1, m_mem1_chunks);
          enqueue_region(MEM2_FIRST_PAGE, m_mem2_page_count, m_mem2_ptr, shadow2, m_mem2_chunks);
          ASSERT(jar.size() <= UINT16_MAX);
          if (!jar.empty())
            w.do_work_and_kick_jobs(jar.data(), static_cast<uint16_t>(jar.size()));
        });

    if (full_state)
    {
      ROLLBACK_ZONE_N("DoState save");
      m_split_video = split;
      rbm.BeginDoState();
      VideoCommon_SetRollbackSplitState(split);
      State::SaveToBuffer(system, m_save_buffer);
      VideoCommon_SetRollbackSplitState(false);
      rbm.EndDoState();
      m_last_aram_bytes = system.GetDSP().SaveARAMSnapshot(m_aram);
      system.GetMemory().SaveFakeVMEMSnapshot(m_fake_vmem);
      if (split)
      {
        m_video_capture.store(VideoCapture::Pending, std::memory_order_release);
        system.GetFifo().QueueVideoThreadCapture([this](bool run) { CaptureVideoState(run); });
      }
      else
      {
        m_video_capture.store(VideoCapture::None, std::memory_order_release);
      }
    }
    else
    {
      m_split_video = false;
      m_video_capture.store(VideoCapture::None, std::memory_order_release);
      m_last_aram_bytes = 0;
    }

    const auto ram_wait_start = std::chrono::steady_clock::now();
    m_last_dostate_us = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(ram_wait_start - dostate_start)
            .count());

    job::DrainJobsUntilComplete(dt, ram_job);
    if (l1cachejob)
      job::DrainJobsUntilComplete(dt, l1cachejob);
    if (slice_job)
      job::DrainJobsUntilComplete(dt, slice_job);

    AssembleRegionChunks(m_mem1_chunks, m_mem1_delta);
    AssembleRegionChunks(m_mem2_chunks, m_mem2_delta);

    u32 combined_crc = Common::StartCRC32();
    for (const auto& c : m_mem1_chunks)
      combined_crc = Common::CombineCRC32(combined_crc, c.crc, c.crc_bytes);
    for (const auto& c : m_mem2_chunks)
      combined_crc = Common::CombineCRC32(combined_crc, c.crc, c.crc_bytes);
    m_last_change_crc = combined_crc ? combined_crc : 1;
    m_has_change_crc = true;

    m_last_ram_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - ram_wait_start)
                                         .count());
    m_has_state = true;
    return;
  }

  KeptChunks kept1, kept2;

  // Serialize non-RAM state first so writes made by DoState are included in the dirty scan.
  // Gameplay-only rollback (region mode) saves memory and registers only.
  if (!region_mask)
  {
    ROLLBACK_ZONE_N("DoState save");
    const bool split = RollbackManager::UseSplitVideoState(system);
    const u64 compiles_before = VideoCommon::GetSyncPipelineCompileStats().count;
    m_last_sync_waited = false;
    m_last_sync_us = 0;
    if (!split)
    {
      m_last_sync_waited = system.IsDualCoreMode();
      m_last_sync_us = RollbackManager::SyncGPUForDoState(system);
    }
    else if (m_video_capture.load(std::memory_order_acquire) == VideoCapture::Pending)
    {
      // The video thread has not reached this slot's previous snapshot yet, i.e. it is a whole
      // ring of snapshots behind. Let it catch up before the buffer is reused (rare).
      const auto start = std::chrono::steady_clock::now();
      auto& fifo = system.GetFifo();
      fifo.WaitForGpuThreadIdle();
      if (m_video_capture.load(std::memory_order_acquire) == VideoCapture::Pending)
        fifo.DropVideoThreadCaptures();  // paused: the video thread runs nothing
      m_last_sync_waited = true;
      m_last_sync_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count());
    }
    m_last_sync_compiles =
        static_cast<u32>(VideoCommon::GetSyncPipelineCompileStats().count - compiles_before);

    m_split_video = split;
    rbm.BeginDoState();
    VideoCommon_SetRollbackSplitState(split);
    State::SaveToBuffer(system, m_save_buffer);
    VideoCommon_SetRollbackSplitState(false);
    rbm.EndDoState();
    m_last_aram_bytes = system.GetDSP().SaveARAMSnapshot(m_aram);
    system.GetMemory().SaveFakeVMEMSnapshot(m_fake_vmem);
    if (split)
    {
      m_video_capture.store(VideoCapture::Pending, std::memory_order_release);
      system.GetFifo().QueueVideoThreadCapture([this](bool run) { CaptureVideoState(run); });
    }
    else
    {
      m_video_capture.store(VideoCapture::None, std::memory_order_release);
    }
  }
  const auto ram_start = std::chrono::steady_clock::now();
  m_last_dostate_us = static_cast<u64>(
      std::chrono::duration_cast<std::chrono::microseconds>(ram_start - dostate_start).count());

  job::Job* l1cachejob = job::KickRootJob(dt, [this, region_mask](job::JobTaskThread&, job::Job&) {
    ROLLBACK_ZONE_N("L1 cache save");
    Rollback::DeltaSaveSlot* slot = this;
    if (!region_mask && slot->m_l1_cache_ptr && slot->m_l1_cache_size > 0 &&
        slot->m_l1_cache_snapshot.data())
      std::memcpy(slot->m_l1_cache_snapshot.data(), slot->m_l1_cache_ptr, slot->m_l1_cache_size);
  });

  job::Job* root_mem1_job = job::KickRootJob(
      dt, [this, &rbm, shadow1, &kept1](job::JobTaskThread& w, job::Job& root) {
        ROLLBACK_ZONE_N("root mem1 save dispatch");
        std::vector<job::Job*> jar;
        enqueueSubsectionJobs(0, m_mem1_page_count, m_mem1_ptr, m_mem1_delta, rbm, w, root, jar,
                              SAVESTATE_NUM_WORK_CHUNKS, m_mem1_scan, shadow1, kept1);
        auto num_jobs = jar.size();
        ASSERT(num_jobs <= UINT16_MAX);
        w.do_work_and_kick_jobs(jar.data(), (uint16_t)num_jobs);
      });

  // Do all mem1 copy jobs + l1 cache job. THEN do mem2 jobs.
  // Goal here is to try not to trash the cache too hard.
  job::Job* root_mem2_job = job::KickRootJob(
      dt, [this, &rbm, shadow2, &kept2](job::JobTaskThread& w, job::Job& root) {
        ROLLBACK_ZONE_N("root mem2 save dispatch");
        std::vector<job::Job*> jar;
        enqueueSubsectionJobs(MEM2_FIRST_PAGE, m_mem2_page_count, m_mem2_ptr, m_mem2_delta, rbm, w,
                              root, jar, SAVESTATE_NUM_WORK_CHUNKS, m_mem2_scan, shadow2, kept2);
        auto num_jobs = jar.size();
        ASSERT(num_jobs <= UINT16_MAX);
        w.do_work_and_kick_jobs(jar.data(), (uint16_t)num_jobs);
      });

  job::DrainJobsUntilComplete(dt, root_mem1_job);
  job::DrainJobsUntilComplete(dt, root_mem2_job);
  job::DrainJobsUntilComplete(dt, l1cachejob);
  for (const auto& [delta, kept] :
       {std::pair{&m_mem1_delta, &kept1}, std::pair{&m_mem2_delta, &kept2}})
  {
    delta->page_count = CompactKeptGranules(delta->page_indices.data(), delta->page_data.data(),
                                            ROLLBACK_PAGE_SIZE, kept->split, kept->counts);
  }
  m_last_ram_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - ram_start)
                                       .count());

  // The scans cleared the bitmap's marks of the granules saves cover.
  m_has_state = true;
}

void DeltaSaveSlot::CaptureVideoState(bool run)
{
  if (!run)
  {
    m_video_capture.store(VideoCapture::None, std::memory_order_release);
    return;
  }
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    u8* ptr = m_video_state.data();
    PointerWrap p(&ptr, m_video_state.size(), PointerWrap::Mode::Write);
    VideoCommon_DoStateRollbackGPU(p);
    const size_t size = p.GetOffsetFromPreviousPosition(m_video_state.data());
    if (p.IsWriteMode())
    {
      m_video_state_size = size;
      VideoCommon_SaveTmemSnapshot(m_tmem);
      m_video_capture.store(VideoCapture::Ready, std::memory_order_release);
      return;
    }
    m_video_state.reset(size);  // measured: grow and write again
  }
  m_video_capture.store(VideoCapture::None, std::memory_order_release);
}

bool DeltaSaveSlot::BeginRestoreVideoState()
{
  const bool ready = m_video_capture.load(std::memory_order_acquire) == VideoCapture::Ready;
  if (!ready)
  {
    WARN_LOG_FMT(BRAWLBACK, "Rollback: no video thread capture for this snapshot; video state "
                            "not restored");
  }
  m_video_restore_done.store(0, std::memory_order_relaxed);
  // On the video thread: first the FIFO the CPU part restored, then TMEM and the GPU registers.
  AsyncRequests::GetInstance()->PushEvent([this, ready] {
    Core::System::GetInstance().GetFifo().ApplyRollbackFifo();
    if (ready)
    {
      VideoCommon_LoadTmemSnapshot(m_tmem);
      u8* ptr = m_video_state.data();
      PointerWrap p(&ptr, m_video_state_size, PointerWrap::Mode::Read);
      VideoCommon_DoStateRollbackGPU(p);
    }
    m_video_restore_done.store(1, std::memory_order_release);
    m_video_restore_done.notify_one();
  });
  return ready;
}

void DeltaSaveSlot::FinishRestoreVideoState()
{
  for (int spin = 0; spin < 512; ++spin)
  {
    if (m_video_restore_done.load(std::memory_order_acquire) != 0)
      return;
    job::pause_thread();
  }
  while (m_video_restore_done.load(std::memory_order_acquire) == 0)
    m_video_restore_done.wait(0, std::memory_order_acquire);
}

bool DeltaSaveSlot::RestoreVideoState()
{
  const bool ready = BeginRestoreVideoState();
  FinishRestoreVideoState();
  return ready;
}

EvictedDelta DeltaSaveSlot::ExtractDeltas()
{
  ROLLBACK_ZONE();
  EvictedDelta out;
  out.mem1 = std::move(m_mem1_delta);
  out.mem2 = std::move(m_mem2_delta);
  m_has_state = false;
  return out;
}

void DeltaSaveSlot::MarkTouchedGlobalPages(std::bitset<JITDirtyBitmap::ENTRY_COUNT>& touched) const
{
  ROLLBACK_ZONE();
  for (uint32_t i = 0; i < m_mem1_delta.page_count; ++i)
    touched.set(m_mem1_delta.page_indices[i]);
  for (uint32_t i = 0; i < m_mem2_delta.page_count; ++i)
    touched.set(MEM2_FIRST_PAGE + m_mem2_delta.page_indices[i]);
}

}  // namespace Rollback
