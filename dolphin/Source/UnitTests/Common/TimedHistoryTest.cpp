// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include <gtest/gtest.h>

#include "Common/TimedHistory.h"

using namespace std::chrono_literals;

TEST(TimedHistory, ValueAtAnEarlierTime)
{
  Common::TimedHistory<int, 4> h;
  const Clock::time_point t0 = Clock::now();
  EXPECT_TRUE(h.Empty());
  EXPECT_EQ(h.At(t0), nullptr);

  h.Push(t0, 1);
  h.Push(t0 + 2ms, 2);
  h.Push(t0 + 4ms, 3);
  EXPECT_EQ(h.At(t0 - 1ms), nullptr);  // older than every value kept
  EXPECT_EQ(*h.At(t0), 1);
  EXPECT_EQ(*h.At(t0 + 1ms), 1);
  EXPECT_EQ(*h.At(t0 + 2ms), 2);  // a value took effect at its own time
  EXPECT_EQ(*h.At(t0 + 3ms), 2);
  EXPECT_EQ(*h.At(t0 + 100ms), 3);  // later than all: the newest
}

TEST(TimedHistory, KeepsTheLastN)
{
  Common::TimedHistory<int, 4> h;
  const Clock::time_point t0 = Clock::now();
  for (int i = 0; i < 10; ++i)
    h.Push(t0 + i * 1ms, i);
  EXPECT_EQ(h.At(t0 + 5ms), nullptr);  // 0-5 are gone
  EXPECT_EQ(*h.At(t0 + 6ms), 6);
  EXPECT_EQ(*h.At(t0 + 8500us), 8);
  EXPECT_EQ(*h.At(t0 + 9ms), 9);

  h.Clear();
  EXPECT_TRUE(h.Empty());
  EXPECT_EQ(h.At(t0 + 9ms), nullptr);
  h.Push(t0 + 20ms, 20);
  EXPECT_EQ(*h.At(t0 + 30ms), 20);
  EXPECT_EQ(h.At(t0 + 19ms), nullptr);
}
