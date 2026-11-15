// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Online/RoomWatch.h (core/standalone only): watching a room's
// game from its waiting room is Dolphin's for now.

#pragma once

#include <chrono>
#include <span>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Online::RoomWatch
{
constexpr u8 CHANNEL = 2;

struct RoomInfo
{
  bool online = false;
  u64 connection = 0;
  bool in_room = false;
  bool room_in_game = false;
  bool me_in_game = false;
  bool own_game = false;
  u8 screen = 0;
};

inline void OnPacket(std::span<const u8>)
{
}
inline std::vector<std::string> Service(Clock::time_point, const RoomInfo&)
{
  return {};
}
inline bool Watching()
{
  return false;
}
inline void Abort()
{
}
inline void HoldFeed(bool)
{
}
inline picojson::object Status()
{
  return {};
}
}  // namespace Online::RoomWatch
