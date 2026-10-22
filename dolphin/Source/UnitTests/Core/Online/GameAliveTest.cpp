// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// `game-alive` on the online connection (Core/Online/GameAlive.h): sent every 2 s while the game
// beats, none once it stopped (bug report #105: a frozen game's online connection stayed up).

#include <chrono>

#include <gtest/gtest.h>

#include "Core/Online/GameAlive.h"

using namespace Online::GameAlive;
using namespace std::chrono_literals;

TEST(GameAlive, SentWhileTheGameBeats)
{
  const auto t0 = std::chrono::steady_clock::now();
  const char* alive = R"({"type":"game-alive"})";
  Beat();
  EXPECT_EQ(Service(t0, false, 1), std::nullopt) << "not online";
  EXPECT_EQ(Service(t0, true, 1), alive) << "a new connection: at once";
  Beat();
  EXPECT_EQ(Service(t0 + 1s, true, 1), std::nullopt) << "every 2 s";
  EXPECT_EQ(Service(t0 + 2s, true, 1), alive);
  // Stopped (frozen, paused): nothing, however long, until it beats again.
  EXPECT_EQ(Service(t0 + 4s, true, 1), std::nullopt);
  EXPECT_EQ(Service(t0 + 60s, true, 1), std::nullopt);
  Beat();
  EXPECT_EQ(Service(t0 + 61s, true, 1), alive);
  // A new connection starts over, but still only for a game that beat since the last one.
  EXPECT_EQ(Service(t0 + 62s, true, 2), std::nullopt);
  Beat();
  EXPECT_EQ(Service(t0 + 62s, true, 2), alive);
}
