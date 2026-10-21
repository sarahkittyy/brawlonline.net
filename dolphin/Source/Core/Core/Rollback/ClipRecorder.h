// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Rendering replays to video (docs/design/highlights.md §5): an MP4 file, H.264 video at 1280x720
// (or 854x480, 1920x1080, 2560x1440: Settings > Clips) and AAC audio at 48 kHz, made from what the
// emulated game shows and plays, frame-exact whatever the PC's speed. The encoders are the
// operating system's (Windows: Media Foundation, hardware when there is one; macOS: AVFoundation;
// Linux: FFmpeg's, as linked), so rendering adds nothing to the download.
//
// A file holds one or more segments, one after another (a clip; a combo movie's clips). The
// session marks a segment's start and end in emulated time (ticks): the frames presented and the
// samples the game's DSP produced in between are kept, timed by those ticks, so audio and video
// stay together whatever skips or repeats around them. Frames are read back by the frame dumper
// (VideoCommon/FrameDumper.cpp) at the size FitSize gives and padded to the video's size; samples come from
// the mixer (AudioCommon/Mixer.cpp) and are resampled from the DSP's rate to 48 kHz. Encoding runs
// on its own thread with a short queue, which holds the emulation back instead of dropping frames.

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Gprb::Clip
{
// The video's height (Settings > Clips): 480, 720 (the default), 1080 or 1440; 16:9.
constexpr int DEFAULT_HEIGHT = 720;
constexpr int AUDIO_RATE = 48000;
// The width for a height (16:9, even).
int WidthFor(int height);

// Opens `path` for writing, `height` lines high (another height is DEFAULT_HEIGHT): the video is
// written to `<path>.part`, named `path` once complete (an existing file is then replaced). An
// error when no encoder could be made.
std::optional<std::string> Open(const std::string& path, int height = DEFAULT_HEIGHT);
// Writes out what is queued and closes the file; an error when it could not be written. With
// `discard` the file is deleted instead.
std::optional<std::string> Close(bool discard = false);
bool IsOpen();

// The session (CPU thread). A segment's frames and samples are those with ticks in
// [start, end): BeginSegment opens it (end unknown yet), MarkSegmentEnd sets its end, and
// EndSegment finishes it once its frames have all come (the presented frames arrive after the CPU
// thread ran them): the file's timeline then goes on from the segment's length.
void BeginSegment(u64 start_ticks);
void MarkSegmentEnd(u64 end_ticks);
// The frames of the open segment that have come so far, and the ticks of the last.
u64 SegmentFrames();
u64 LastFrameTicks();
// Finishes the open segment (its audio padded to its length). False when none was open.
bool EndSegment();

// The frame dumper (GPU thread): whether presented frames are wanted, and at which size (the
// frame's aspect kept within the video's size, even sizes).
bool WantsFrames();
void FitSize(int src_width, int src_height, int* width, int* height);
// The frame dumper's thread: a frame read back (RGBA8), presented at `ticks`. Blocks while the
// encoder's queue is full.
void AddVideoFrame(const u8* rgba, int width, int height, int stride, u64 ticks);
// The mixer (CPU thread): the DSP's samples as pushed (big-endian, right then left), at
// `ticks`, before the output volume.
void AddAudioSamples(const s16* samples, size_t num_samples, u32 sample_rate_divisor,
                     u64 ticks);

// For the status: frames written to the file, and the encoder's last error ("" none).
u64 FramesWritten();
std::string LastError();
}  // namespace Gprb::Clip
