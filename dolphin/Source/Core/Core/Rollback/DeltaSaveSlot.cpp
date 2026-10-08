
#include "Core/Rollback/DeltaSaveSlot.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <span>

#include <Common/Assert.h>
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/Rollback/Perf.h"
#include "Core/PowerPC/Gekko.h"
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
// Callers copy one PAGE_SIZE (64-byte) granule at a time, so a plain memcpy is as fast as any
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
  m_mem1_page_count = static_cast<uint32_t>(mem1_size / PAGE_SIZE);
  m_mem2_page_count = static_cast<uint32_t>(mem2_size / PAGE_SIZE);

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
}

// region_phys_base: Wii physical base of the region
//   MEM1 -> 0,          MEM2 -> 0x10000000
void RestoreRegionDelta(const RegionDelta& delta, uint8_t* region_base, uint32_t region_phys_base,
                        const std::vector<MemoryRegion>& excl)
{
  for (uint32_t i = 0; i < delta.page_count; ++i)
  {
    const uint32_t page_idx = delta.page_indices[i];
    uint8_t* const dst = region_base + static_cast<size_t>(page_idx) * PAGE_SIZE;
    const uint8_t* const src = delta.page_data.data() + static_cast<size_t>(i) * PAGE_SIZE;
    const uint32_t dst_phys = region_phys_base + page_idx * static_cast<uint32_t>(PAGE_SIZE);
    savestateMemcpy(dst, src, PAGE_SIZE, dst_phys, excl);
  }
}

void enqueueSubsectionJobs(u32 first_page, u32 page_count, const uint8_t* region_base,
                           Rollback::RegionDelta& out, JITDirtyBitmap& bitmap,
                           job::JobTaskThread& w, job::Job& root, std::vector<job::Job*>& jar,
                           u32 num_work_chunks, const u8* region_mask)
{
  const uint8_t* entries = bitmap.entries;
  auto dirty_pages = std::make_shared<std::vector<u32>>();
  {
    ROLLBACK_ZONE_N("dirty page population");
    dirty_pages->reserve(page_count);
    for (uint32_t i = 0; i < page_count; ++i)
    {
      if (entries[first_page + i] && (!region_mask || region_mask[i]))
      {
        dirty_pages->push_back(i + first_page);
      }
    }
  }

  u32 num_dirty = static_cast<u32>(dirty_pages->size());
  out.page_count = num_dirty;
  if (!num_dirty)
    return;
  {
    ROLLBACK_ZONE_N("delta buffer alloc");
    if (out.page_indices.size() < num_dirty)
      out.page_indices.reset(num_dirty);
    if (out.page_data.size() < (num_dirty * PAGE_SIZE))
      out.page_data.reset(num_dirty * PAGE_SIZE);
  }

  // Capture pointers to the already-allocated buffers to avoid reference capture issues
  uint32_t* page_indices_ptr = out.page_indices.data();
  uint8_t* page_data_ptr = out.page_data.data();

  auto copySubsectionFn = [dirty_pages, entries, region_base, page_indices_ptr, page_data_ptr,
                           first_page](u32 offset, u32 size) {
    ROLLBACK_ZONE_N("copy dirty pages");
    if (offset >= dirty_pages->size())
      return;  // Nothing to do for this subsection

    u32 this_split_written = 0;
    for (uint32_t dirtyPagesIndex = offset;
         dirtyPagesIndex < offset + size && dirtyPagesIndex < dirty_pages->size();
         ++dirtyPagesIndex)
    {
      u32 pageidx = (*dirty_pages)[dirtyPagesIndex];
      ASSERT(entries[pageidx]);

      // pageidx is global; subtract first_page to get region-relative index
      u32 relative_page_idx = pageidx - first_page;
      page_indices_ptr[dirtyPagesIndex] = relative_page_idx;
      std::memcpy(page_data_ptr + static_cast<size_t>(dirtyPagesIndex) * PAGE_SIZE,
                  region_base + static_cast<size_t>(relative_page_idx) * PAGE_SIZE, PAGE_SIZE);
      ++this_split_written;
    }
#if defined(HAVE_TRACY)
    auto x =
        std::format("page count %u (offset %u size %u)", this_split_written, offset, size);
    ZoneText(x.c_str(), x.size());
#endif
  };

  u32 split_size = (num_dirty + num_work_chunks - 1) / num_work_chunks;
  for (u32 i = 0; i < num_work_chunks; ++i)
  {
    u32 offset = i * split_size;
    u32 size = std::min(split_size, num_dirty - offset);
    jar.push_back(w.create_job_as_child(
        root, [copySubsectionFn, offset, size](job::JobTaskThread& t, job::Job& j) {
          copySubsectionFn(offset, size);
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

void DeltaSaveSlot::Save(Core::System& system)
{
  ROLLBACK_ZONE();
  ASSERT(m_mem1_ptr);
  ASSERT(m_mem1_page_count < MEM2_FIRST_PAGE);

  brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  m_downcount = system.GetPPCState().downcount;
  m_exceptions = system.GetPPCState().Exceptions;
  SaveCPURegisters(system.GetPPCState(), m_cpu);
  auto& bitmap = JITDirtyBitmap::Get();
  auto& rbm = RollbackManager::Get();
  auto* dt = rbm.m_dispatch_thread;
  ASSERT(dt);
  const auto dostate_start = std::chrono::steady_clock::now();

  const u8* region_mask = rbm.RegionMask();

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

  job::Job* root_mem1_job =
      job::KickRootJob(dt, [this, &bitmap, region_mask](job::JobTaskThread& w, job::Job& root) {
        ROLLBACK_ZONE_N("root mem1 save dispatch");
        std::vector<job::Job*> jar;
        enqueueSubsectionJobs(0, m_mem1_page_count, m_mem1_ptr, m_mem1_delta, bitmap, w, root, jar,
                              SAVESTATE_NUM_WORK_CHUNKS, region_mask);
        auto num_jobs = jar.size();
        ASSERT(num_jobs <= UINT16_MAX);
        w.do_work_and_kick_jobs(jar.data(), (uint16_t)num_jobs);
      });

  // Do all mem1 copy jobs + l1 cache job. THEN do mem2 jobs.
  // Goal here is to try not to trash the cache too hard.
  job::Job* root_mem2_job =
      job::KickRootJob(dt, [this, &bitmap, region_mask](job::JobTaskThread& w, job::Job& root) {
        ROLLBACK_ZONE_N("root mem2 save dispatch");
        std::vector<job::Job*> jar;
        enqueueSubsectionJobs(MEM2_FIRST_PAGE, m_mem2_page_count, m_mem2_ptr, m_mem2_delta, bitmap,
                              w, root, jar, SAVESTATE_NUM_WORK_CHUNKS,
                              region_mask ? region_mask + m_mem1_page_count : nullptr);
        auto num_jobs = jar.size();
        ASSERT(num_jobs <= UINT16_MAX);
        w.do_work_and_kick_jobs(jar.data(), (uint16_t)num_jobs);
      });

  job::DrainJobsUntilComplete(dt, root_mem1_job);
  job::DrainJobsUntilComplete(dt, root_mem2_job);
  job::DrainJobsUntilComplete(dt, l1cachejob);
  m_last_ram_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - ram_start)
                                       .count());

  bitmap.ClearRange(0, m_mem1_page_count);
  bitmap.ClearRange(MEM2_FIRST_PAGE, m_mem2_page_count);
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
