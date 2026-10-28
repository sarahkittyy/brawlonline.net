// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/Assets/CustomTextureData.h"

namespace OSD
{
enum class MessageType
{
  NetPlayPing,
  NetPlayBuffer,
  BrawlbackBuffer,
  // Spectating: a live game was picked and its first frames haven't come yet.
  SpectateWaiting,
  // Spectating: catching up to the live edge ("Catching up to live (5%). Click to watch").
  SpectateCatchUp,
  // Watching the room's game from the room CSS: the players' game is over (Online/RoomWatch.h).
  RoomWatch,
  // Highlights (Rollback/ReplayPlayback.h): loading or fast-forwarding to the next one.
  HighlightSeek,
  // Draws are left out while the shaders a new build doesn't have on disk compile (ShaderCache).
  CompilingShaders,
  // A room session's notice while the chat window doesn't show it (Rollback/MachineSession.cpp).
  MachineNotice,
  // A room's joiner waiting for a link to a member, with the seconds left (MachineSession.cpp).
  MachineJoinWait,

  // This entry must be kept last so that persistent typed messages are
  // displayed before other messages
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

// On-screen message display (colored yellow by default)
void AddMessage(std::string message, u32 ms = Duration::SHORT, u32 argb = Color::YELLOW,
                const VideoCommon::CustomTextureData::ArraySlice::Level* icon = nullptr);
void AddTypedMessage(MessageType type, std::string message, u32 ms = Duration::SHORT,
                     u32 argb = Color::YELLOW,
                     const VideoCommon::CustomTextureData::ArraySlice::Level* icon = nullptr);

// Draw the current messages on the screen. Only call once per frame.
// The messages start below min_top, the bottom edge of the top-left FPS box (0 for none).
void DrawMessages(float min_top = 0.0f);
void ClearMessages();
// The texts of the messages still up (not run out or replaced), for the harness (`osd_messages`):
// its screenshots don't show them.
std::vector<std::string> GetMessageTexts();

void SetObscuredPixelsLeft(int width);
void SetObscuredPixelsTop(int height);
}  // namespace OSD
