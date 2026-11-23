// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Rollback::PresentStats::HoldOffsetMs (Core/Rollback/PresentStats.h): the present hold's offset
// after a frame's due time, the 95th percentile of the window's arrivals. Arrivals a field or more
// after the window's median are stalls and are left out (bug report #314: a few a second kept
// every frame held the whole 8 ms); without stalls the offset is the one every arrival gave before.

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <random>
#include <vector>

#include "Core/Rollback/PresentStats.h"

namespace
{
using Rollback::PresentStats::HOLD_MAX_MS;
using Rollback::PresentStats::HOLD_STALL_MS;
using Rollback::PresentStats::HOLD_WINDOW;
using Rollback::PresentStats::HoldOffsetMs;

std::optional<double> Offset(const std::vector<double>& arrivals, double stall_ms = HOLD_STALL_MS)
{
  return HoldOffsetMs(arrivals.data(), arrivals.size(), stall_ms);
}

// The 95th percentile as the hold took it before (every arrival, nth_element at 0.95 * (n - 1)).
double OldPercentile(std::vector<double> arrivals)
{
  const auto nth = arrivals.begin() + static_cast<ptrdiff_t>(0.95 * (arrivals.size() - 1));
  std::nth_element(arrivals.begin(), nth, arrivals.end());
  return *nth;
}
}  // namespace

TEST(PresentHold, AStallIsAFieldLate)
{
  EXPECT_NEAR(HOLD_STALL_MS, 16.683, 0.001);
  EXPECT_GT(HOLD_STALL_MS, HOLD_MAX_MS);
}

TEST(PresentHold, NoHoldWithFewerThan20Arrivals)
{
  EXPECT_FALSE(Offset(std::vector<double>(19, 1.0)).has_value());
  EXPECT_FALSE(Offset(std::vector<double>(19, 1.0), 0).has_value());
  EXPECT_EQ(Offset(std::vector<double>(20, 1.0)), 1.0);
  EXPECT_FALSE(HoldOffsetMs(nullptr, 0).has_value());
}

TEST(PresentHold, WithoutStallsTheOffsetIsAsBefore)
{
  std::mt19937 rng(314);
  for (int run = 0; run < 2000; ++run)
  {
    const size_t n = std::uniform_int_distribution<size_t>(20, HOLD_WINDOW)(rng);
    // Early frames and late ones short of a field, as frames that rolled back on a slow PC come in.
    std::uniform_real_distribution<double> arrival(-6.0, HOLD_STALL_MS);
    std::vector<double> arrivals(n);
    for (double& a : arrivals)
      a = arrival(rng);
    const double old_offset = OldPercentile(arrivals);
    EXPECT_EQ(Offset(arrivals), old_offset) << "run " << run;
    EXPECT_EQ(Offset(arrivals, 0), old_offset) << "run " << run;
  }
}

TEST(PresentHold, LateRollbacksStillCount)
{
  // A slow PC's rollbacks: every fourth frame comes 9-12 ms late. They are not stalls: the hold
  // still holds the others up to the cap, as before.
  std::vector<double> arrivals;
  for (size_t i = 0; i < HOLD_WINDOW; ++i)
    arrivals.push_back(i % 4 == 0 ? 9.0 + 3.0 * i / HOLD_WINDOW : 0.5);
  EXPECT_EQ(Offset(arrivals), OldPercentile(arrivals));
  EXPECT_GT(*Offset(arrivals), HOLD_MAX_MS);
}

TEST(PresentHold, StallsDoNotPinTheOffsetAtTheCap)
{
  // Bug report #314's windows: 110 frames on time (0-2 ms after their due times) and 10 that came
  // 20-140 ms late, a stall every 12 frames (more than 5% of the window).
  std::vector<double> arrivals;
  for (int i = 0; i < 110; ++i)
    arrivals.push_back(2.0 * i / 109);
  for (double stall : {20.0, 35.0, 45.0, 50.0, 60.0, 70.0, 80.0, 95.0, 110.0, 140.0})
    arrivals.push_back(stall);
  ASSERT_EQ(arrivals.size(), HOLD_WINDOW);

  // Before: the percentile is a stall, past the cap, so every frame was held the whole 8 ms.
  EXPECT_GE(*Offset(arrivals, 0), 20.0);
  // Now: the percentile of the frames on time.
  std::vector<double> on_time(arrivals.begin(), arrivals.begin() + 110);
  EXPECT_EQ(Offset(arrivals), OldPercentile(on_time));
  EXPECT_LE(*Offset(arrivals), 2.0);
}

TEST(PresentHold, AFieldAfterTheMedianIsAStall)
{
  std::vector<double> arrivals(HOLD_WINDOW, -11.0);  // frames come before their due times
  for (int i = 0; i < 10; ++i)
    arrivals[i * 12] = -11.0 + HOLD_STALL_MS;
  EXPECT_EQ(Offset(arrivals), -11.0);
  EXPECT_EQ(Offset(arrivals, 0), -11.0 + HOLD_STALL_MS);
  for (int i = 0; i < 10; ++i)
    arrivals[i * 12] = -11.0 + HOLD_STALL_MS - 0.01;
  EXPECT_EQ(Offset(arrivals), -11.0 + HOLD_STALL_MS - 0.01);
}

TEST(PresentHold, APCBehindItsScheduleHoldsAsBefore)
{
  // Every frame late (a PC with no slack, Phase 19): that is its pace, not stalls.
  std::vector<double> arrivals;
  for (size_t i = 0; i < HOLD_WINDOW; ++i)
    arrivals.push_back(125.0 + 33.0 * ((i * 37) % HOLD_WINDOW) / HOLD_WINDOW);
  EXPECT_EQ(Offset(arrivals), OldPercentile(arrivals));
}

TEST(PresentHold, HalfAWindowOfStallsMeansNoHold)
{
  // 11 frames on time and 9 stalls: fewer than 20 arrivals are left to take a percentile of.
  std::vector<double> arrivals(11, 1.0);
  arrivals.resize(20, 50.0);
  EXPECT_FALSE(Offset(arrivals).has_value());
  EXPECT_EQ(Offset(arrivals, 0), 50.0);
}

TEST(PresentHold, OnlyTheWindowCounts)
{
  // More arrivals than the window are not read (the ring holds HOLD_WINDOW).
  std::vector<double> arrivals(HOLD_WINDOW, 1.0);
  arrivals.resize(HOLD_WINDOW + 50, 7.0);
  EXPECT_EQ(Offset(arrivals), 1.0);
}
