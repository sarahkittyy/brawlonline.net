// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The video file behind ClipRecorder: H.264 + AAC in MP4, with the operating system's encoders
// (ClipEncoderMF.cpp on Windows) or FFmpeg's (ClipEncoderFFmpeg.cpp elsewhere). Called from one
// thread only.

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"

namespace Gprb::Clip
{
class Encoder
{
public:
  virtual ~Encoder() = default;
  // A frame: `width` x `height` BGRA (top-down); times in 100 ns units.
  virtual bool WriteVideo(const u8* bgra, int stride, s64 pts, s64 duration) = 0;
  // Interleaved stereo 16-bit samples at the rate the encoder was made with.
  virtual bool WriteAudio(const s16* samples, size_t frames, s64 pts) = 0;
  // Writes out the file.
  virtual bool Finish() = 0;
  virtual std::string Error() const = 0;
};

// The video's bitrate for its height: about what keeps 60 frames a second sharp in motion.
inline int VideoBitrate(int height)
{
  return height <= 480 ? 5'000'000 : height <= 720 ? 12'000'000 : height <= 1080 ? 20'000'000 : 32'000'000;
}

std::unique_ptr<Encoder> CreateEncoder(const std::string& path, int width, int height,
                                       int audio_rate, std::string* error);
}  // namespace Gprb::Clip
