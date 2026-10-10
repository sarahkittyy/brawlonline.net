// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// 3-4 player matches (docs/nplayer/setup.md): free-for-all or teams, and who picks the next stage.

#include <array>
#include <cmath>

#include <gtest/gtest.h>

#include "Core/Online/GameSetup.h"

using namespace Online::GameSetup;

namespace
{
constexpr u8 R = 0, B = 1, G = 2, N = NO_TEAM;

std::array<Seat, MAX_PORTS> Seats(std::array<int, MAX_PORTS> teams)
{
  // -1: empty port; else the team byte of the lock-in.
  std::array<Seat, MAX_PORTS> s{};
  for (int i = 0; i < MAX_PORTS; ++i)
  {
    if (teams[i] >= 0)
      s[i] = {true, static_cast<u8>(teams[i])};
  }
  return s;
}

PortEnd In(s32 stocks, float damage, u8 team = N)
{
  return {true, team, stocks, damage, 0};
}
PortEnd Out(u8 order, u8 team = N)
{
  return {true, team, 0, 0.0f, order};
}
}  // namespace

TEST(NPlayerSetup, TwoPlayersAreAlwaysAOneVersusOne)
{
  for (const bool on : {false, true})
  {
    const TeamSetup t = DecideTeams(on, Seats({R, R, -1, -1}));
    EXPECT_EQ(t.error, SetupError::None);
    EXPECT_FALSE(t.teams);
    EXPECT_EQ(t.team[0], N);
    EXPECT_EQ(t.team[1], N);
  }
  // Ports with a gap: P1 and P3.
  EXPECT_FALSE(DecideTeams(true, Seats({B, -1, B, -1})).teams);
  EXPECT_EQ(DecideTeams(true, Seats({R, -1, -1, -1})).error, SetupError::TooFewPlayers);
  EXPECT_EQ(DecideTeams(false, Seats({-1, -1, -1, -1})).error, SetupError::TooFewPlayers);
}

TEST(NPlayerSetup, FreeForAllIgnoresTheTeamBytes)
{
  const TeamSetup t = DecideTeams(false, Seats({R, R, 0x77, -1}));
  EXPECT_EQ(t.error, SetupError::None);
  EXPECT_FALSE(t.teams);
  for (const u8 team : t.team)
    EXPECT_EQ(team, N);
}

TEST(NPlayerSetup, TeamSplits)
{
  // 2v2, 2v1 with a gap, 3v1, 1v1v1, 2v1v1: allowed.
  for (const auto& seats : {Seats({R, B, R, B}), Seats({R, -1, B, R}), Seats({G, G, G, B}),
                            Seats({R, B, G, -1}), Seats({R, R, B, G})})
  {
    const TeamSetup t = DecideTeams(true, seats);
    EXPECT_EQ(t.error, SetupError::None);
    EXPECT_TRUE(t.teams);
    for (int i = 0; i < MAX_PORTS; ++i)
      EXPECT_EQ(t.team[i], seats[i].present ? seats[i].team : N);
  }
  // Everyone on one colour.
  TeamSetup t = DecideTeams(true, Seats({B, B, B, B}));
  EXPECT_EQ(t.error, SetupError::SameTeam);
  EXPECT_FALSE(t.teams);
  EXPECT_STREQ(SetupErrorText(t.error), "Pick different teams");
  EXPECT_EQ(DecideTeams(true, Seats({G, -1, G, G})).error, SetupError::SameTeam);
  // A player without a colour, or with a byte that is no colour (another machine's).
  EXPECT_EQ(DecideTeams(true, Seats({R, B, N, -1})).error, SetupError::NoTeam);
  EXPECT_EQ(DecideTeams(true, Seats({R, B, 3, -1})).error, SetupError::NoTeam);
  EXPECT_EQ(DecideTeams(true, Seats({R, B, 0x80, B})).error, SetupError::NoTeam);
  EXPECT_STREQ(SetupErrorText(SetupError::None), "");
}

TEST(NPlayerSetup, OneVersusOneOutcome)
{
  // By stocks: P2 lost; the loser picks.
  Outcome o = DecideOutcome(false, {In(2, 30), Out(1), {}, {}});
  EXPECT_EQ(o.winner, 0);
  EXPECT_EQ(o.pickers, 0b10);
  EXPECT_EQ(o.place[0], 1);
  EXPECT_EQ(o.place[1], 2);
  EXPECT_EQ(o.place[2], 0);
  // Time-out: more stocks, then less damage.
  o = DecideOutcome(false, {In(1, 10), In(2, 150), {}, {}});
  EXPECT_EQ(o.winner, 1);
  EXPECT_EQ(o.pickers, 0b01);
  o = DecideOutcome(false, {In(2, 80.5f), In(2, 80.0f), {}, {}});
  EXPECT_EQ(o.winner, 1);
  EXPECT_EQ(o.pickers, 0b01);
  // A draw: both pick (Direct).
  o = DecideOutcome(false, {In(2, 80), In(2, 80), {}, {}});
  EXPECT_EQ(o.winner, 0xFE);
  EXPECT_EQ(o.pickers, 0b11);
  EXPECT_EQ(o.place[0], 1);
  EXPECT_EQ(o.place[1], 1);
}

TEST(NPlayerSetup, FreeForAllLastPlacePicks)
{
  // P2 out first, then P4; P1 wins: P2 (last) picks.
  Outcome o = DecideOutcome(false, {In(3, 12), Out(1), {}, Out(2)});
  EXPECT_EQ(o.winner, 0);
  EXPECT_EQ(o.pickers, 0b0010);
  EXPECT_EQ(o.place[0], 1);
  EXPECT_EQ(o.place[1], 3);
  EXPECT_EQ(o.place[3], 2);
  // 4 players, P3 and P4 out on the same frame (tied last): the lower port, P3.
  o = DecideOutcome(false, {Out(2), In(1, 0), Out(1), Out(1)});
  EXPECT_EQ(o.winner, 1);
  EXPECT_EQ(o.pickers, 0b0100);
  EXPECT_EQ(o.place[2], 3);
  EXPECT_EQ(o.place[3], 3);
  EXPECT_EQ(o.place[0], 2);
  // Time-out with three left: last by stocks.
  o = DecideOutcome(false, {In(2, 50), In(1, 0), In(2, 20), {}});
  EXPECT_EQ(o.winner, 2);
  EXPECT_EQ(o.pickers, 0b0010);
  // Two tied for first at the time-out: no single winner, the last place still picks.
  o = DecideOutcome(false, {In(2, 50), Out(1), In(2, 50), {}});
  EXPECT_EQ(o.winner, 0xFE);
  EXPECT_EQ(o.pickers, 0b0010);
  // Everyone tied: each player picks.
  o = DecideOutcome(false, {In(1, 0), In(1, 0), In(1, 0), {}});
  EXPECT_EQ(o.winner, 0xFE);
  EXPECT_EQ(o.pickers, 0b0111);
}

TEST(NPlayerSetup, TeamsLosingTeamsLowerPortPicks)
{
  // Red P1+P3 against blue P2+P4; blue is out (P4 first, then P2). Blue's lower port: P2.
  Outcome o = DecideOutcome(true, {In(1, 90, R), Out(2, B), Out(1, R), Out(1, B)});
  EXPECT_EQ(o.winner, 0);
  EXPECT_EQ(o.pickers, 0b0010);
  EXPECT_EQ(o.place[0], 1);
  EXPECT_EQ(o.place[2], 1);  // a team shares its place
  EXPECT_EQ(o.place[1], 2);
  EXPECT_EQ(o.place[3], 2);
  // Both teams still in at the time-out: red (P2 + P4) has more stocks. The winner is the team's
  // lowest port; blue's lowest port, P1, picks although P3 is the one still in.
  o = DecideOutcome(true, {Out(1, B), In(2, 0, R), In(1, 40, B), Out(2, R)});
  EXPECT_EQ(o.winner, 1);
  EXPECT_EQ(o.pickers, 0b0001);
  // 2v1 with a gap (P1, P3 red; P4 green): green lost.
  o = DecideOutcome(true, {In(2, 0, R), {}, Out(1, R), Out(2, G)});
  EXPECT_EQ(o.winner, 0);
  EXPECT_EQ(o.pickers, 0b1000);
  // Time-out: team stocks, then team damage.
  o = DecideOutcome(true, {In(2, 10, R), In(1, 0, B), In(1, 0, R), In(2, 0, B)});
  EXPECT_EQ(o.winner, 1);  // blue: 3 stocks and 0 damage against 3 and 10
  EXPECT_EQ(o.pickers, 0b0001);
  // A team draw: each team's lower port picks.
  o = DecideOutcome(true, {In(1, 0, R), In(1, 0, B), In(1, 0, R), In(1, 0, B)});
  EXPECT_EQ(o.winner, 0xFE);
  EXPECT_EQ(o.pickers, 0b0011);
  // 2v1v1: green out first, then blue: green picks.
  o = DecideOutcome(true, {In(1, 0, R), Out(2, B), In(3, 0, R), Out(1, G)});
  EXPECT_EQ(o.pickers, 0b1000);
  EXPECT_EQ(o.place[1], 2);
  EXPECT_EQ(o.place[3], 3);
}

TEST(NPlayerSetup, HostileEndStateStaysInRange)
{
  // A NaN or huge damage ranks last among the players still in; negative stocks count as 0.
  Outcome o = DecideOutcome(false, {In(1, std::nanf("")), In(1, 5), In(-3, 0), {}});
  EXPECT_EQ(o.winner, 1);
  EXPECT_EQ(o.pickers, 0b0100);
  o = DecideOutcome(false, {});
  EXPECT_EQ(o.winner, 0xFF);
  EXPECT_EQ(o.pickers, 0);
}

TEST(NPlayerSetup, StagePick)
{
  constexpr u16 X = 0xFFFF;
  // The first picker's pick; anyone else's is not played (random).
  EXPECT_EQ(StagePickPort(0b0010, {0x21, 0x01, X, X}), 1);
  EXPECT_EQ(StagePickPort(0b0010, {0x21, X, 0x02, X}), -1);
  EXPECT_EQ(StagePickPort(0b0011, {X, 0x02, X, X}), 1);
  EXPECT_EQ(StagePickPort(0b1000, {X, X, X, X}), -1);
  EXPECT_EQ(StagePickPort(0, {X, X, X, 0x2E}), -1);
}
