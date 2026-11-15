// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's VideoCommon/OnScreenDisplay.h (core/standalone only): messages go
// to the host (BrawlOnline::Host::OnScreenMessage). Same types, colours and durations as Dolphin.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

namespace OSD
{
enum class MessageType
{
  NetPlayPing,
  NetPlayBuffer,
  BrawlbackBuffer,
  SpectateWaiting,
  SpectateCatchUp,
  RoomWatch,
  HighlightSeek,
  CompilingShaders,
  MachineNotice,
  MachineJoinWait,
  MachineImageSend,

  Typeless,
};

namespace Color
{
constexpr u32 CYAN = 0xFF00FFFF;
constexpr u32 GREEN = 0xFF00FF00;
constexpr u32 RED = 0xFFFF0000;
constexpr u32 YELLOW = 0xFFFFFF30;
}  // namespace Color

namespace Duration
{
constexpr u32 SHORT = 2000;
constexpr u32 NORMAL = 5000;
constexpr u32 VERY_LONG = 10000;
}  // namespace Duration

void AddMessage(std::string message, u32 ms = Duration::SHORT, u32 argb = Color::YELLOW);
// A typed message replaces the one of its type still on the screen.
void AddTypedMessage(MessageType type, std::string message, u32 ms = Duration::SHORT,
                     u32 argb = Color::YELLOW);
}  // namespace OSD
