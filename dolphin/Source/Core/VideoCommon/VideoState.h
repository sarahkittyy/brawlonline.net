// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace VideoCommon
{
static constexpr size_t TMEM_SNAPSHOT_PAGE_SIZE = 4096;
static constexpr size_t TMEM_SNAPSHOT_PAGE_COUNT = (1024 * 1024) / TMEM_SNAPSHOT_PAGE_SIZE;

// Incremental TMEM snapshot for rollback slots: each 4 KiB page carries a version number, so both
// captures (saves) and restores (loads) copy only the pages written since that slot's copy was
// last updated (typically 1-2 pages = 4-8 KiB instead of 1 MiB).
struct TmemSnapshot
{
  std::unique_ptr<u8[]> data;
  std::array<u64, TMEM_SNAPSHOT_PAGE_COUNT> versions{};

  void Reset()
  {
    data.reset();
    versions.fill(0);
  }
};
}  // namespace VideoCommon

void VideoCommon_DoState(PointerWrap& p);
// TMEM (s_tex_mem) was written at [offset, offset + size): every writer says so, so that rollback
// saves and loads copy only those pages (VideoCommon_SaveTmemSnapshot / LoadTmemSnapshot).
void VideoCommon_TmemWritten(unsigned int offset, unsigned int size);
size_t VideoCommon_SaveTmemSnapshot(VideoCommon::TmemSnapshot& snapshot);
size_t VideoCommon_LoadTmemSnapshot(const VideoCommon::TmemSnapshot& snapshot);

// A whole-machine session's machine image: what only drawing uses and the image's digest leaves
// out (each machine's GPU has its own): the TEV colour registers' both halves
// (PixelShaderManager::DoImageTevColors) and the EFB copies the texture cache keeps in VRAM
// (TextureCacheBase::DoImageCopiesState). On the video thread, as for a savestate; the caller is
// in image mode (Machine::InImageDoState), so the request does not kick the GPU. Returns how many
// EFB copies it saved or loaded.
unsigned VideoCommon_DoImageDrawState(PointerWrap& p);

// Skips GPU readbacks (texture cache, framebuffer, bounding box) in DoState.
// Register state (BP/XF/FIFO) is always saved. Must match between save and load.
void VideoCommon_SetSkipGPUReadbackForRollback(bool skip);
bool VideoCommon_GetSkipGPUReadbackForRollback();

// Skips the VertexManager::Flush() call at the start of DoState during rollback.
// The flush submits a GPU draw which is wasted work when we're about to re-simulate the frame.
void VideoCommon_SetSkipVertexFlushForRollback(bool skip);
bool VideoCommon_GetSkipVertexFlushForRollback();

// Rollback snapshots in deterministic dual core, split by the thread that owns the state, so that
// a snapshot never has to wait for the GPU thread:
// - CPU part: what the CPU thread owns in deterministic mode (the FIFO preprocessing position and
//   the not yet preprocessed bytes, the command processor and pixel engine registers). It is saved
//   in place of the whole video state, on the CPU thread, while
//   VideoCommon_SetRollbackSplitState(true) is in effect.
// - GPU part: BP/CP/XF memory, texture memory, the shader/vertex managers and the presenter.
//   The video thread serializes it when it reaches the snapshot's position in the command stream
//   (Fifo::QueueVideoThreadCapture), so it matches the CPU part exactly.
// The texture cache, framebuffer and bounding box are never part of it (as with the skip flag).
void VideoCommon_SetRollbackSplitState(bool split);
bool VideoCommon_GetRollbackSplitState();
void VideoCommon_DoStateRollbackCPU(PointerWrap& p);
void VideoCommon_DoStateRollbackGPU(PointerWrap& p);
