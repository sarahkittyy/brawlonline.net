// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class PointerWrap;

void VideoCommon_DoState(PointerWrap& p);

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
