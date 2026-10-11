// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A set's stage steps from its format (Core/Online/SetFormat.h): Ranked's numbers as parameters.

#include <gtest/gtest.h>

#include "Core/Online/SetFormat.h"

using namespace Online::GameSetup;

namespace
{
// P+'s starters (BF, FD, Smashville, Dream Land, PS2) and a counterpick list.
const std::vector<u16> STARTERS{0x01, 0x02, 0x21, 0x2D, 0x2E};
const std::vector<u16> SEVEN{0x01, 0x02, 0x21, 0x2D, 0x2E, 0x0D, 0x1F};
const std::vector<u16> COUNTERPICKS{0x01, 0x02, 0x21, 0x2D, 0x2E, 0x0D, 0x1F, 0x23};

Steps StepsOf(const Format& f, const SetState& set, u32 game, const std::vector<u8>& p0,
          const std::vector<u8>& p1)
{
  return EvaluateSteps(f, set, game, {&p0, &p1});
}
}  // namespace

TEST(SetFormat, RankedIsSlippis)
{
  const Format f = RankedFormat(STARTERS, COUNTERPICKS);
  EXPECT_FALSE(CheckFormat(f));
  // Game 1: 1-2-1 from the host (port 0), the last strike 10 s.
  Steps e = StepsOf(f, {}, 1, {}, {});
  ASSERT_EQ(e.steps.size(), 3u);
  EXPECT_EQ(e.steps[0].player, 0);
  EXPECT_EQ(e.steps[1].player, 1);
  EXPECT_EQ(e.steps[1].count, 2);
  EXPECT_EQ(e.steps[2].player, 0);
  EXPECT_EQ(e.steps[2].seconds, 10);
  EXPECT_EQ(e.step_idx, 0);
  e = StepsOf(f, {}, 1, {0x01, 0x2E}, {0x02, 0x21});
  EXPECT_EQ(e.step_idx, -1);
  EXPECT_EQ(e.stage, 0x2D);  // the one left
  // Game 2: the winner (port 1) bans 2, the loser picks; Dave's rule for a loser who won before.
  SetState set;
  set.wins = {1, 1};
  set.last_winner = 1;
  set.last_win_stage = {0x21, 0x2D};
  e = StepsOf(f, set, 3, {}, {});
  ASSERT_EQ(e.steps.size(), 2u);
  EXPECT_EQ(e.steps[0].player, 1);
  EXPECT_EQ(e.steps[0].count, 2);
  EXPECT_EQ(e.steps[1].player, 0);
  EXPECT_EQ(e.steps[1].type, StepType::Pick);
  EXPECT_EQ(std::count(e.remaining.begin(), e.remaining.end(), 0x21), 0);  // port 0 won there
  e = StepsOf(f, set, 3, {0x01}, {0x02, 0x0D});
  EXPECT_EQ(e.stage, 0x01);
  // Best of 3.
  EXPECT_FALSE(SetOver(f, set));
  set.wins = {2, 1};
  EXPECT_TRUE(SetOver(f, set));
}

TEST(SetFormat, TournamentFormats)
{
  // Bo5, seven starters struck 1-2-2-1, three bans, no Dave's rule.
  Format f;
  f.starters = SEVEN;
  f.counterpicks = COUNTERPICKS;
  f.strikes = {1, 2, 2, 1};
  f.bans = 3;
  f.dsr = false;
  f.best_of = 5;
  EXPECT_FALSE(CheckFormat(f));
  Steps e = StepsOf(f, {}, 1, {0x01, 0x0D, 0x1F}, {0x02, 0x21, 0x2D});
  ASSERT_EQ(e.steps.size(), 4u);
  EXPECT_EQ(e.steps[3].player, 1);
  EXPECT_EQ(e.steps[3].seconds, 10);
  // P0: 0x01; P1: 0x02 0x21; P0: 0x0D 0x1F; P1: 0x2D -> 0x2E left.
  EXPECT_EQ(e.step_idx, -1);
  EXPECT_EQ(e.stage, 0x2E);
  SetState set;
  set.wins = {1, 2};
  set.last_winner = 1;
  set.last_win_stage = {0x01, 0x21};
  e = StepsOf(f, set, 4, {}, {});
  EXPECT_EQ(e.steps[0].count, 3);
  EXPECT_EQ(std::count(e.remaining.begin(), e.remaining.end(), 0x01), 1);  // no Dave's rule
  EXPECT_FALSE(SetOver(f, set));
  set.wins = {1, 3};
  EXPECT_TRUE(SetOver(f, set));
  // No bans: the loser picks straight away.
  f.bans = 0;
  e = StepsOf(f, set, 4, {}, {});
  ASSERT_EQ(e.steps.size(), 1u);
  EXPECT_EQ(e.steps[0].type, StepType::Pick);
}

TEST(SetFormat, FirstStrikerFromTheBits)
{
  Format f = RankedFormat(STARTERS, COUNTERPICKS);
  f.first_striker_bits = 0b101;
  EXPECT_EQ(FirstStriker(f, 1), 1);
  EXPECT_EQ(FirstStriker(f, 2), 0);
  EXPECT_EQ(FirstStriker(f, 3), 1);
  const Steps e = StepsOf(f, {}, 1, {}, {});
  EXPECT_EQ(e.steps[0].player, 1);
  EXPECT_EQ(e.steps[1].player, 0);
}

TEST(SetFormat, StrikeEveryGame)
{
  // No set: every game struck from the starters (a tournament's Bo1), never over.
  Format f = RankedFormat(STARTERS, COUNTERPICKS);
  f.best_of = 0;
  f.strike_every_game = true;
  EXPECT_FALSE(CheckFormat(f));
  SetState set;
  set.wins = {5, 4};
  set.last_winner = 0;
  const Steps e = StepsOf(f, set, 10, {}, {});
  EXPECT_TRUE(e.striking);
  EXPECT_EQ(e.steps.size(), 3u);
  EXPECT_FALSE(SetOver(f, set));
}

TEST(SetFormat, DrawsAreStruckAgain)
{
  const Format f = RankedFormat(STARTERS, COUNTERPICKS);
  SetState set;
  set.wins = {1, 0};
  set.last_winner = 0xFE;
  EXPECT_TRUE(StepsOf(f, set, 2, {}, {}).striking);
}

TEST(SetFormat, BadActionsAndFormats)
{
  const Format f = RankedFormat(STARTERS, COUNTERPICKS);
  // A stage that is not selectable (a peer's bad action) is skipped.
  const Steps e = StepsOf(f, {}, 1, {0x99, 0x01}, {});
  EXPECT_EQ(e.step_idx, 1);
  Format bad = f;
  bad.strikes = {1, 2};  // leaves two of five
  EXPECT_TRUE(CheckFormat(bad));
  bad = f;
  bad.best_of = 4;
  EXPECT_TRUE(CheckFormat(bad));
  bad = f;
  bad.bans = 4;
  EXPECT_TRUE(CheckFormat(bad));
  bad = f;
  bad.counterpicks = {0x01, 0x02, 0x21};  // 3: not enough for 2 bans, a pick and Dave's rule
  EXPECT_TRUE(CheckFormat(bad));
  bad.dsr = false;
  EXPECT_FALSE(CheckFormat(bad));
  bad = f;
  bad.strikes = {0, 4};
  EXPECT_TRUE(CheckFormat(bad));
}

namespace
{
ParsedSet ParseSet(const std::string& set_json)
{
  picojson::value v;
  const std::string err = picojson::parse(v, "{\"set\": " + set_json + "}");
  EXPECT_TRUE(err.empty()) << err;
  return ParseSetTicket(v.get<picojson::object>());
}

// The server's SetTicket (server/crates/common/src/proto.rs): a Bo3 at its game 2.
const char* TICKET = R"({"bestOf": 3, "strikeEveryGame": false, "starters": [1, 2, 33, 45, 46],
  "counterpicks": [1, 2, 33, 45, 46, 13, 31], "strikes": [1, 2, 1], "bans": 2, "dsr": true,
  "firstStrikerBits": 5, "sides": [0, 2], "game": 2, "wins": [1, 0], "lastWinner": 0,
  "lastWinStage": [33, 65535]})";
}  // namespace

TEST(SetFormat, ParsesTheServersTicket)
{
  const ParsedSet p = ParseSet(TICKET);
  ASSERT_TRUE(p.set) << p.detail;
  const SetTicket& t = *p.set;
  EXPECT_EQ(t.format.best_of, 3);
  EXPECT_EQ(t.format.counterpicks.size(), 7u);
  EXPECT_EQ(t.format.first_striker_bits, 5u);
  EXPECT_EQ(t.first_game, 2u);
  EXPECT_EQ(t.side_ports, (std::array<u8, 2>{0, 2}));
  EXPECT_EQ(t.state.wins, (std::array<int, 2>{1, 0}));
  EXPECT_EQ(t.state.last_winner, 0);
  EXPECT_EQ(t.state.last_win_stage[0], 0x21);
  EXPECT_EQ(t.state.last_win_stage[1], 0xFFFF);
  // Game 2: the winner (side 0) bans, side 1 picks.
  const Steps e = StepsOf(t.format, t.state, t.first_game, {}, {});
  EXPECT_FALSE(e.striking);
  EXPECT_EQ(e.steps[0].player, 0);
  EXPECT_FALSE(ParseSetTicket(picojson::object{}).set);
}

TEST(SetFormat, RefusesBadTickets)
{
  const std::string ok = TICKET;
  auto with = [&](const std::string& from, const std::string& to) {
    std::string s = ok;
    s.replace(s.find(from), from.size(), to);
    return ParseSet(s);
  };
  for (const auto& [from, to] : std::initializer_list<std::pair<std::string, std::string>>{
           {R"("bestOf": 3)", R"("bestOf": 4)"},
           {R"("strikes": [1, 2, 1])", R"("strikes": [1, 2])"},
           {R"("sides": [0, 2])", R"("sides": [2, 2])"},
           {R"("sides": [0, 2])", R"("sides": [0, 4])"},
           {R"("game": 2)", R"("game": 0)"},
           {R"("lastWinner": 0)", R"("lastWinner": 3)"},
           {R"("wins": [1, 0])", R"("wins": [2, 0])"},  // already won
           {R"("lastWinStage": [33, 65535])", R"("lastWinStage": [300, 65535])"},
           {R"("starters": [1, 2, 33, 45, 46])", R"("starters": "1,2")"},
           {R"("dsr": true)", R"("dsr": 1)"},
       })
  {
    const ParsedSet p = with(from, to);
    EXPECT_FALSE(p.set) << to;
    EXPECT_FALSE(p.error.empty()) << to;
  }
  EXPECT_FALSE(ParseSet("[]").set);
}
