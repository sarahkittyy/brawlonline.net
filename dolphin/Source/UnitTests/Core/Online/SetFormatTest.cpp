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
  // Without members (a 1v1 ticket may leave them out): each side is its captain.
  EXPECT_EQ(t.members[0], std::vector<u8>{0});
  EXPECT_EQ(t.members[1], std::vector<u8>{2});
  EXPECT_TRUE(t.actions[0].empty() && t.actions[1].empty());
}

TEST(SetFormat, ParsesTeamsAndTheStepsSoFar)
{
  std::string s = TICKET;
  s.replace(s.find(R"("sides": [0, 2])"), 15,
            R"("sides": [0, 1], "members": [[0, 2], [1, 3]], "actions": [[33, 2], [46]])");
  const ParsedSet p = ParseSet(s);
  ASSERT_TRUE(p.set) << p.detail;
  EXPECT_EQ(p.set->members[0], (std::vector<u8>{0, 2}));
  EXPECT_EQ(p.set->members[1], (std::vector<u8>{1, 3}));
  EXPECT_EQ(p.set->actions[0], (std::vector<u8>{33, 2}));
  EXPECT_EQ(p.set->actions[1], (std::vector<u8>{46}));
  for (const char* bad : {
           R"("members": [[2, 0], [1, 3]])",   // the captain not first
           R"("members": [[0, 1], [1, 3]])",   // a port on both sides
           R"("members": [[0, 2], [1]])",      // sides of different sizes
           R"("members": [[0, 2, 3], [1]])",
           R"("members": [[0, 4], [1, 3]])",
           R"("members": [[0, 2]])",
           R"("actions": [[0], []])",
           R"("actions": [[256], []])",
           R"("actions": [[1]])",
       })
  {
    std::string b = TICKET;
    b.replace(b.find(R"("sides": [0, 2])"), 15, std::string(R"("sides": [0, 1], )") + bad);
    EXPECT_FALSE(ParseSet(b).set) << bad;
  }
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

// 2v2, either teammate (room-rules.md D): ports 0 and 2 are side 0, 1 and 3 side 1; side 0
// strikes first, 1-2-1 over the five starters.
namespace
{
constexpr std::array<int, MAX_PORTS_SHARED> TEAMS{0, 1, 0, 1};
Format Doubles()
{
  return RankedFormat({0x01, 0x02, 0x21, 0x2D, 0x2E}, {0x01, 0x02, 0x21, 0x2D, 0x2E, 0x0D, 0x1F});
}
}  // namespace

TEST(SetFormat, TeammatesPressingOnTheSameFrame)
{
  const Format f = Doubles();
  // Both press the same stage for step 0: port 0 (lower) counts, port 2's is dropped.
  SharedSteps host;
  std::array<std::vector<Press>, MAX_PORTS_SHARED> presses{};
  presses[0] = {{0, 0x01}};
  presses[2] = {{0, 0x01}};
  auto dropped = TakePresses(f, {}, 1, TEAMS, presses, host);
  EXPECT_EQ(host.actions[0], std::vector<u8>{0x01});
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].first, 2);
  // Different stages for a one-stage step: still only one counts.
  SharedSteps host2;
  presses[2] = {{0, 0x02}};
  dropped = TakePresses(f, {}, 1, TEAMS, presses, host2);
  EXPECT_EQ(host2.actions[0], std::vector<u8>{0x01});
  EXPECT_EQ(dropped.size(), 1u);
  EXPECT_EQ(host2.taken, (std::array<u32, 4>{1, 0, 1, 0}));
  // Taking again changes nothing.
  EXPECT_TRUE(TakePresses(f, {}, 1, TEAMS, presses, host2).empty());
  EXPECT_EQ(host2.actions[0], std::vector<u8>{0x01});
  // The other side pressing on side 0's step is dropped.
  SharedSteps host3;
  std::array<std::vector<Press>, MAX_PORTS_SHARED> wrong{};
  wrong[1] = {{0, 0x01}};
  EXPECT_EQ(TakePresses(f, {}, 1, TEAMS, wrong, host3).size(), 1u);
  EXPECT_TRUE(host3.actions[0].empty() && host3.actions[1].empty());
}

TEST(SetFormat, TeammatesSharingAStepAndLatePresses)
{
  const Format f = Doubles();
  SharedSteps host;
  std::array<std::vector<Press>, MAX_PORTS_SHARED> presses{};
  presses[0] = {{0, 0x01}};
  TakePresses(f, {}, 1, TEAMS, presses, host);
  // Side 1 strikes two: one each, on nearby frames (two rounds) or the same one.
  presses[1] = {{1, 0x02}};
  presses[3] = {{1, 0x21}};
  EXPECT_TRUE(TakePresses(f, {}, 1, TEAMS, presses, host).empty());
  EXPECT_EQ(host.actions[1], (std::vector<u8>{0x02, 0x21}));
  // Port 2's press made for step 0 (it saw step 0 still open) arrives now, with side 0 on step
  // 2: it is dropped, not taken as step 2's strike.
  presses[2] = {{0, 0x2D}};
  EXPECT_EQ(TakePresses(f, {}, 1, TEAMS, presses, host).size(), 1u);
  EXPECT_EQ(host.actions[0], std::vector<u8>{0x01});
  // Step 2's press counts; the stage is decided.
  presses[2].push_back({2, 0x2D});
  EXPECT_TRUE(TakePresses(f, {}, 1, TEAMS, presses, host).empty());
  const Steps e = StepsOf(f, {}, 1, host.actions[0], host.actions[1]);
  EXPECT_EQ(e.step_idx, -1);
  EXPECT_EQ(e.stage, 0x2E);
}

TEST(SetFormat, PendingPressesShowUntilTheHostAnswers)
{
  const Format f = Doubles();
  const std::array<std::vector<u8>, 2> none{};
  // Port 2's own press shows at once.
  auto shown = WithPending(f, {}, 1, 0, none, {{0, 0x02}});
  EXPECT_EQ(shown[0], std::vector<u8>{0x02});
  // The host took the partner's press for step 0 instead: port 2's no longer fits and is not
  // shown; the host's lists are.
  const std::array<std::vector<u8>, 2> host{std::vector<u8>{0x01}, std::vector<u8>{}};
  shown = WithPending(f, {}, 1, 0, host, {{0, 0x02}});
  EXPECT_EQ(shown, host);
}
