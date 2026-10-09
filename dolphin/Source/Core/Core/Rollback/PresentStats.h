// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// Presentation of resimulated frames during rollback netplay.
//
// A rollback reruns several frames in one burst before the newest frame is shown. Each rerun frame
// still renders and copies an XFB, and Dolphin used to present every one of them, so a rollback of
// N frames put N+1 frames on screen within one real frame (a visible flash or stutter on a real
// video backend; the Null backend hides it). The guest still runs every resimulated frame in full,
// including its rendering (skipping guest code changes the simulation), but frames of resimulated
// iterations are no longer sent to the presenter.
//
// Frames reach the presenter in one of two ways:
// - Immediate XFB (this fork's default): when the GPU executes the XFB copy. The decision is made
//   where the CPU thread processes the copy command: in single core when it runs, in deterministic
//   dual core when the CPU thread preprocesses the FIFO; the GPU thread then takes the decisions in
//   command order.
// - Otherwise at the VI field (VideoInterface::OutputField, CPU thread).
// Both use CoreTiming's "rollback resimulating" flag, the one that also keeps their audio out.
namespace Rollback::PresentStats
{
static constexpr int HISTOGRAM_SIZE = 8;  // the last bucket counts HISTOGRAM_SIZE - 1 or more

// CPU thread, VideoInterface::OutputField, for a field with an XFB. Returns whether the field may
// be presented.
bool OnXFBField(bool resimulating);

// CPU thread, deterministic dual core: the FIFO preprocessor saw an EFB copy trigger.
void OnEFBCopyPreprocess(bool to_xfb);

// The EFB copy trigger is executed (GPU thread in dual core, CPU thread in single core).
// `preprocessed`: the decision was made by OnEFBCopyPreprocess. Returns whether an immediate-XFB
// present of this copy may happen.
bool OnEFBCopy(bool to_xfb, bool preprocessed);

// CPU thread, at the start of every displayed (not resimulated) frame of the rollback game loop.
// `after_resimulation` is true when frames were resimulated just before it.
void OnDisplayedFrameStart(bool after_resimulation);

// Video thread: the presenter showed a frame (`duplicate`: the same XFB as the previous one).
void OnPresent(bool duplicate);

// Present cadence as a 59.94 Hz screen without VSync would show it: a hitch is a present that does
// not land in the refresh right after the previous present's (a repeated or a skipped frame),
// averaged over four refresh phases. Taken (and reset) by the online session's telemetry.
struct Cadence
{
  u64 presents = 0;
  double hitches = 0;
  double interval_ms_max = 0;
  double interval_ms_sum = 0, interval_ms_sq = 0;
};
Cadence TakeCadence();

struct Stats
{
  bool present_resimulated = false;  // PPR_ROLLBACK_PRESENT_RESIM=1: old behaviour, for A/B runs
  u64 xfb_fields = 0;                // VI fields with an XFB
  u64 xfb_fields_skipped = 0;        // of those, of resimulated frames
  u64 xfb_copies = 0;                // XFB copies
  u64 xfb_copies_skipped = 0;        // of those, of resimulated frames
  u64 copy_decision_misses = 0;      // dual core: a copy ran without a preprocessed decision
  u64 presents = 0;                  // frames the presenter actually showed
  u64 duplicate_presents = 0;        // of those, the same XFB as the previous present
  u64 displayed_frames = 0;          // displayed frames of the rollback loop
  u64 displayed_frames_after_resim = 0;
  // Frames sent to the presenter between the starts of two displayed frames, i.e. per displayed
  // frame (index = count). The second histogram only counts displayed frames that followed a
  // resimulation.
  std::array<u64, HISTOGRAM_SIZE> outputs_per_frame{};
  std::array<u64, HISTOGRAM_SIZE> outputs_per_frame_after_resim{};
};

Stats Get();
void Reset();

// Writes the video thread makes to guest RAM: EFB copies encoded to RAM (immediately, or later when
// a deferred copy is flushed), XFB copies, and the fill pattern Dolphin writes when a copy is kept
// in VRAM only. In dual core they land at a host-dependent time relative to the emulated CPU.
// PPR_LOG_GPU_RAM_WRITES=<file> also appends one line per write (debugging).
enum class GpuRamWriteKind
{
  EFBCopy,
  EFBCopyDeferred,  // encoded now, written to RAM by a later flush (counted again as a flush)
  EFBCopyFlush,  // a deferred EFB (texture) copy written now; XFB copies count as XFBCopy
  XFBCopy,
  Fill,
};
void OnGpuRamWrite(GpuRamWriteKind kind, u32 address, u32 size);
// Video thread: an EFB copy readback (host GPU -> guest RAM) took `us` microseconds.
void OnGpuReadback(u64 us);

struct GpuRamWriteStats
{
  u64 efb_copies = 0;           // written to RAM at once
  u64 efb_copies_deferred = 0;  // written to RAM by a later flush
  u64 efb_copy_flushes = 0;
  u64 xfb_copies = 0;
  u64 fills = 0;
  u64 bytes = 0;  // all kinds
  u64 readback_us_total = 0;
  u64 readback_us_max = 0;
};
GpuRamWriteStats GetGpuRamWrites();
}  // namespace Rollback::PresentStats
