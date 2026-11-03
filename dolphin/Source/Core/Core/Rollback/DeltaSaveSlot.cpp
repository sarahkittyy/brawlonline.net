
#include "Core/Rollback/DeltaSaveSlot.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <span>

#include <Common/Assert.h>
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/Rollback/KeptGranules.h"
#include "Core/Rollback/Perf.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{

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
  brawl_frame = 0;
  m_mem1_delta.Reset();
  m_mem2_delta.Reset();
  m_save_buffer.reset();
  m_aram.Reset();
  m_fake_vmem.Reset();
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
  out.fpscr = ppc.fpscr.Hex;
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

void DeltaSaveSlot::Save(Core::System& system)
{
  ROLLBACK_ZONE();
  ASSERT(m_mem1_ptr);
  ASSERT(m_mem1_page_count < MEM2_FIRST_PAGE);

  brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  m_downcount = system.GetPPCState().downcount;
  m_jit_invalidations = system.GetJitInterface().Invalidations();
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
      m_video_capture.store(VideoCapture::Ready, std::memory_order_release);
      return;
    }
    m_video_state.reset(size);  // measured: grow and write again
  }
  m_video_capture.store(VideoCapture::None, std::memory_order_release);
}

bool DeltaSaveSlot::RestoreVideoState()
{
  const bool ready = m_video_capture.load(std::memory_order_acquire) == VideoCapture::Ready;
  if (!ready)
  {
    WARN_LOG_FMT(BRAWLBACK, "Rollback: no video thread capture for this snapshot; video state "
                            "not restored");
  }
  // On the video thread: first the FIFO the CPU part restored, then the video thread's part.
  AsyncRequests::GetInstance()->PushBlockingEvent([this, ready] {
    Core::System::GetInstance().GetFifo().ApplyRollbackFifo();
    if (!ready)
      return;
    u8* ptr = m_video_state.data();
    PointerWrap p(&ptr, m_video_state_size, PointerWrap::Mode::Read);
    VideoCommon_DoStateRollbackGPU(p);
  });
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
