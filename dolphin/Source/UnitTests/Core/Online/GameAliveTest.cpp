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
  EXPECT_EQ(Service(t0, false, 1, false), std::nullopt) << "not online";
  EXPECT_EQ(Service(t0, true, 1, false), alive) << "a new connection: at once";
  Beat();
  EXPECT_EQ(Service(t0 + 1s, true, 1, false), std::nullopt) << "every 2 s";
  EXPECT_EQ(Service(t0 + 2s, true, 1, false), alive);
  // Stopped (frozen, paused): nothing, however long, until it beats again.
  EXPECT_EQ(Service(t0 + 4s, true, 1, false), std::nullopt);
  EXPECT_EQ(Service(t0 + 60s, true, 1, false), std::nullopt);
  Beat();
  EXPECT_EQ(Service(t0 + 61s, true, 1, false), alive);
  // A new connection starts over, but still only for a game that beat since the last one.
  EXPECT_EQ(Service(t0 + 62s, true, 2, false), std::nullopt);
  Beat();
  EXPECT_EQ(Service(t0 + 62s, true, 2, false), alive);
}

// In a Ranked set every 500 ms, saying so (2026-10-29: mm times them for whose connection went);
// a switch either way goes out at once.
TEST(GameAlive, EveryHalfSecondInARankedSet)
{
  const auto t0 = std::chrono::steady_clock::now() + 1h;
  const char* alive = R"({"type":"game-alive"})";
  const char* ranked = R"({"type":"game-alive","ranked":true})";
  Beat();
  EXPECT_EQ(Service(t0, true, 7, false), alive);
  Beat();
  EXPECT_EQ(Service(t0 + 100ms, true, 7, true), ranked) << "the set starts";
  Beat();
  EXPECT_EQ(Service(t0 + 400ms, true, 7, true), std::nullopt);
  EXPECT_EQ(Service(t0 + 600ms, true, 7, true), ranked);
  Beat();
  EXPECT_EQ(Service(t0 + 700ms, true, 7, false), alive) << "the set ends";
  Beat();
  EXPECT_EQ(Service(t0 + 1200ms, true, 7, false), std::nullopt);
}
