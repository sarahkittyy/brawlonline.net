// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VideoState.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "Common/ChunkFile.h"
#include "Core/Rollback/MachineSession.h"
#include "Core/System.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/Widescreen.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

static bool s_skip_gpu_readback_for_rollback = false;
static bool s_skip_vertex_flush_for_rollback = false;
static bool s_rollback_split_state = false;

void VideoCommon_SetRollbackSplitState(bool split)
{
  s_rollback_split_state = split;
}

bool VideoCommon_GetRollbackSplitState()
{
  return s_rollback_split_state;
}

void VideoCommon_DoStateRollbackCPU(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();
  system.GetFifo().DoStateRollbackCPU(p);
  p.DoMarker("Fifo (rollback CPU part)");

  system.GetCommandProcessor().DoState(p);
  p.DoMarker("CommandProcessor");

  system.GetPixelEngine().DoState(p);
  p.DoMarker("PixelEngine");
}

namespace
{
// TMEM's 4 KiB page versions for incremental rollback saves and loads. Used where TMEM lives:
// on the video thread in dual core (its writers are BP commands; captures and loads run there too).
constexpr size_t TMEM_PAGE_SIZE = VideoCommon::TMEM_SNAPSHOT_PAGE_SIZE;
constexpr size_t TMEM_PAGE_COUNT = VideoCommon::TMEM_SNAPSHOT_PAGE_COUNT;
static_assert(TMEM_PAGE_COUNT * TMEM_PAGE_SIZE == TMEM_SIZE);

std::array<u8, TMEM_PAGE_COUNT> s_tmem_dirty = [] {
  std::array<u8, TMEM_PAGE_COUNT> a{};
  a.fill(1);
  return a;
}();
bool s_tmem_any_dirty = true;
std::array<u64, TMEM_PAGE_COUNT> s_tmem_versions{};
u64 s_tmem_next_version = 1;

void FlushTmemWrites()
{
  if (!s_tmem_any_dirty)
    return;
  for (size_t p = 0; p < TMEM_PAGE_COUNT; ++p)
  {
    if (s_tmem_dirty[p])
    {
      s_tmem_versions[p] = s_tmem_next_version++;
      s_tmem_dirty[p] = 0;
    }
  }
  s_tmem_any_dirty = false;
}

void MarkAllTmemWritten()
{
  s_tmem_dirty.fill(1);
  s_tmem_any_dirty = true;
}
}  // namespace

void VideoCommon_TmemWritten(unsigned int offset, unsigned int size)
{
  if (size == 0 || offset >= TMEM_SIZE)
    return;
  const size_t last = std::min<size_t>(static_cast<size_t>(offset) + size - 1, TMEM_SIZE - 1) /
                      TMEM_PAGE_SIZE;
  for (size_t page = offset / TMEM_PAGE_SIZE; page <= last; ++page)
    s_tmem_dirty[page] = 1;
  s_tmem_any_dirty = true;
}

size_t VideoCommon_SaveTmemSnapshot(VideoCommon::TmemSnapshot& snapshot)
{
  FlushTmemWrites();
  if (!snapshot.data)
  {
    snapshot.data = std::make_unique<u8[]>(TMEM_SIZE);
    snapshot.versions.fill(0);
  }
  size_t copied = 0;
  for (size_t p = 0; p < TMEM_PAGE_COUNT; ++p)
  {
    if (snapshot.versions[p] == s_tmem_versions[p])
      continue;
    std::memcpy(snapshot.data.get() + p * TMEM_PAGE_SIZE, s_tex_mem.data() + p * TMEM_PAGE_SIZE,
                TMEM_PAGE_SIZE);
    snapshot.versions[p] = s_tmem_versions[p];
    copied += TMEM_PAGE_SIZE;
  }
  return copied;
}

size_t VideoCommon_LoadTmemSnapshot(const VideoCommon::TmemSnapshot& snapshot)
{
  if (!snapshot.data)
    return 0;
  FlushTmemWrites();
  size_t copied = 0;
  for (size_t p = 0; p < TMEM_PAGE_COUNT; ++p)
  {
    if (s_tmem_versions[p] == snapshot.versions[p])
      continue;
    std::memcpy(s_tex_mem.data() + p * TMEM_PAGE_SIZE, snapshot.data.get() + p * TMEM_PAGE_SIZE,
                TMEM_PAGE_SIZE);
    s_tmem_versions[p] = snapshot.versions[p];
    copied += TMEM_PAGE_SIZE;
  }
  return copied;
}

void VideoCommon_DoStateRollbackGPU(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();

  p.Do(bpmem);
  p.DoMarker("BP Memory");

  p.Do(g_main_cp_state);
  p.DoMarker("CP Memory");
  if (p.IsReadMode())
    CopyPreprocessCPStateFromMain();

  p.Do(xfmem);
  p.DoMarker("XF Memory");

  // TMEM is kept incrementally per slot (VideoCommon_SaveTmemSnapshot / LoadTmemSnapshot) so
  // neither saves nor loads copy all 1 MiB when only 1-2 pages changed.

  TMEM::DoState(p);
  p.DoMarker("TMEM");

  system.GetPixelShaderManager().DoState(p);
  p.DoMarker("PixelShaderManager");

  system.GetVertexShaderManager().DoState(p);
  p.DoMarker("VertexShaderManager");

  system.GetGeometryShaderManager().DoState(p);
  p.DoMarker("GeometryShaderManager");

  g_vertex_manager->DoState(p);
  p.DoMarker("VertexManager");

  g_presenter->DoState(p);
  g_frame_dumper->DoState(p);
  p.DoMarker("Presenter");

  g_widescreen->DoState(p);
  p.DoMarker("Widescreen");

  system.GetXFStateManager().DoState(p);
  p.DoMarker("XFStateManager");

  if (p.IsReadMode())
  {
    BPReload();
    VertexLoaderManager::MarkAllDirty();
  }
}

void VideoCommon_SetSkipGPUReadbackForRollback(bool skip)
{
  s_skip_gpu_readback_for_rollback = skip;
}

bool VideoCommon_GetSkipGPUReadbackForRollback()
{
  return s_skip_gpu_readback_for_rollback;
}

void VideoCommon_SetSkipVertexFlushForRollback(bool skip)
{
  s_skip_vertex_flush_for_rollback = skip;
}

bool VideoCommon_GetSkipVertexFlushForRollback()
{
  return s_skip_vertex_flush_for_rollback;
}

unsigned VideoCommon_DoImageDrawState(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();
  auto run = [&] {
    system.GetPixelShaderManager().DoImageTevColors(p);
    p.DoMarker("TEV colors");
    return g_texture_cache->DoImageCopiesState(p);
  };
  if (!system.IsDualCoreMode())
    return run();
  const u32 count = AsyncRequests::GetInstance()->PushBlockingEvent(run);
  system.GetFifo().GpuMaySleep();
  return count;
}

void VideoCommon_DoState(PointerWrap& p)
{
  bool software = false;
  p.Do(software);

  if (p.IsReadMode() && software == true)
  {
    // change mode to abort load of incompatible save state.
    p.SetVerifyMode();
  }

  // BP Memory
  p.Do(bpmem);
  p.DoMarker("BP Memory");

  // CP Memory
  // We don't save g_preprocess_cp_state separately because the GPU should be
  // synced around state save/load.
  p.Do(g_main_cp_state);
  p.DoMarker("CP Memory");
  if (p.IsReadMode())
    CopyPreprocessCPStateFromMain();

  // XF Memory
  p.Do(xfmem);
  p.DoMarker("XF Memory");

  // Texture decoder
  p.DoArray(s_tex_mem);
  p.DoMarker("texMem");
  if (p.IsReadMode())
    MarkAllTmemWritten();

  // TMEM
  TMEM::DoState(p);
  p.DoMarker("TMEM");

  // FIFO
  auto& system = Core::System::GetInstance();
  system.GetFifo().DoState(p);
  p.DoMarker("Fifo");

  auto& command_processor = system.GetCommandProcessor();
  command_processor.DoState(p);
  p.DoMarker("CommandProcessor");

  system.GetPixelEngine().DoState(p);
  p.DoMarker("PixelEngine");

  // A whole-machine session's machine image carries the guest's GPU state only: what the host
  // derives from it (shader constants and their dirty flags, vertex caches, presentation) depends
  // on how far each host's GPU thread got, so two players' images of the same machine differed.
  // The joiner recomputes it from bpmem/xfmem.
  const bool image = Machine::InImageDoState();

  // the old way of replaying current bpmem as writes to push side effects to pixel shader manager
  // doesn't really work.
  if (!image)
    system.GetPixelShaderManager().DoState(p);
  p.DoMarker("PixelShaderManager");

  if (!image)
    system.GetVertexShaderManager().DoState(p);
  p.DoMarker("VertexShaderManager");

  if (!image)
    system.GetGeometryShaderManager().DoState(p);
  p.DoMarker("GeometryShaderManager");

  if (!image)
    g_vertex_manager->DoState(p);
  p.DoMarker("VertexManager");

  // Skip GPU readbacks during rollback; GPU resources rebuild from restored CPU state.
  if (!s_skip_gpu_readback_for_rollback)
  {
    g_framebuffer_manager->DoState(p);
  }
  p.DoMarker("FramebufferManager");

  if (!s_skip_gpu_readback_for_rollback)
  {
    g_texture_cache->DoState(p);
  }
  p.DoMarker("TextureCache");

  if (!image)
  {
    g_presenter->DoState(p);
    g_frame_dumper->DoState(p);
  }
  p.DoMarker("Presenter");

  if (!s_skip_gpu_readback_for_rollback)
  {
    g_bounding_box->DoState(p);
  }
  p.DoMarker("Bounding Box");

  if (!image)
    g_widescreen->DoState(p);
  p.DoMarker("Widescreen");

  if (!image)
    system.GetXFStateManager().DoState(p);
  p.DoMarker("XFStateManager");

  // Refresh state.
  if (p.IsReadMode())
  {
    if (image)
    {
      // Recomputed from the registers just loaded. The pixel and geometry shaders' constants
      // that a BP register's write sets are set from bpmem again: Dirty() keeps the old ones.
      system.GetXFStateManager().SetAllChanged();
      system.GetPixelShaderManager().SetAllFromRegisters();
      system.GetPixelShaderManager().Dirty();
      auto& geometry_shader_manager = system.GetGeometryShaderManager();
      geometry_shader_manager.SetViewportChanged();
      for (u8 i = 0; i < 8; i++)
        geometry_shader_manager.SetTexCoordChanged(i);
      geometry_shader_manager.Dirty();
      system.GetVertexShaderManager().dirty = true;
    }
    // Inform backend of new state from registers.
    BPReload();
    VertexLoaderManager::MarkAllDirty();
  }
}
