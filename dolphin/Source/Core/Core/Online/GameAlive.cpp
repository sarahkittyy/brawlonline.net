// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/GameAlive.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Online::GameAlive
{
namespace
{
using Clock = std::chrono::steady_clock;

// Beats since the emulation started; read and compared by Rooms' thread only.
std::atomic<u64> s_beats{0};

// Rooms' thread only.
u64 s_connection = 0;
u64 s_sent_beats = 0;
Clock::time_point s_next{};
}  // namespace

void Beat()
{
  s_beats.fetch_add(1, std::memory_order_relaxed);
}

std::optional<std::string> Service(Clock::time_point now, bool online, u64 connection)
{
  if (!online)
    return std::nullopt;
  if (connection != s_connection)
  {
    s_connection = connection;
    s_next = now;
  }
  if (now < s_next)
    return std::nullopt;
  const u64 beats = s_beats.load(std::memory_order_relaxed);
  // No beat since the last one: the game stopped running; nothing until it beats again.
  if (beats == s_sent_beats)
    return std::nullopt;
  s_sent_beats = beats;
  s_next = now + GAME_ALIVE_EVERY;
  return R"({"type":"game-alive"})";
}
}  // namespace Online::GameAlive
