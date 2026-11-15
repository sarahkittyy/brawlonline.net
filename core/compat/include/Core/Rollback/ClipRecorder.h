// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Rollback/ClipRecorder.h (core/standalone only): Dolphin's clip
// and video rendering; outside it no clip ever opens (Open answers why).

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Gprb::Clip
{
constexpr int DEFAULT_HEIGHT = 720;
constexpr int AUDIO_RATE = 48000;

inline std::optional<std::string> Open(const std::string&, int = DEFAULT_HEIGHT)
{
  return std::string("clips are rendered by Dolphin");
}
inline std::optional<std::string> Close(bool = false)
{
  return std::nullopt;
}
inline bool IsOpen()
{
  return false;
}
inline void BeginSegment(u64)
{
}
inline void MarkSegmentEnd(u64)
{
}
inline u64 SegmentFrames()
{
  return 0;
}
inline u64 LastFrameTicks()
{
  return 0;
}
inline bool EndSegment()
{
  return false;
}
inline u64 FramesWritten()
{
  return 0;
}
inline std::string LastError()
{
  return {};
}
}  // namespace Gprb::Clip
