// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A set's stage steps as a format (docs/design/room-rules.md §4, §5): Ranked's striking and
// counterpicking (GameSetup.h) with its numbers made parameters, so a room set or a tournament
// ruleset can say "Bo5, 7 starters struck 1-2-2-1, 3 bans, no Dave's rule". Pure functions of the
// format, the set so far and both players' actions: every machine evaluates the same steps.
// Unit-tested (UnitTests/Core/Online/SetFormatTest.cpp).

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Online::GameSetup
{
enum class StepType : u8
{
  None = 0,
  Strike = 1,  // remove stages (striking, or the winner's ban)
  Pick = 2,    // choose the stage (the loser's counterpick)
  Char = 3,    // the characters (winner, then loser)
  Done = 4,    // the stage is decided and nothing is left to do for it
};

struct Format
{
  std::vector<u16> starters;      // srStageKind, struck for game 1 (and after a draw)
  std::vector<u16> counterpicks;  // the games after: the winner bans, the loser picks
  // The striking pattern over the starters, players taking turns (the first striker first):
  // [1, 2, 1] for five starters. It must leave exactly one stage.
  std::vector<u8> strikes{1, 2, 1};
  int bans = 2;      // the winner's bans before the loser picks (0-3)
  bool dsr = true;   // Dave's stupid rule: the loser cannot pick the stage they last won on
  int best_of = 3;   // 3, 5, 7; 0: no set (games go on; see strike_every_game)
  // No set, every game struck from the starters (a tournament's Bo1, room-rules.md #17).
  bool strike_every_game = false;
  // Who strikes first in a struck game: bit (game - 1) % 32 of this (in-game port 0 or 1).
  // Ranked: 0 (the host). Rooms: random per set, from the server.
  u32 first_striker_bits = 0;
};

// Ranked's (Slippi's) format over these lists.
Format RankedFormat(std::vector<u16> starters, std::vector<u16> counterpicks);
// Why `f` cannot be played (a pattern that does not leave one starter, too few counterpicks for
// the bans and Dave's rule, a best-of that is not 0/1/3/5/7...), or nullopt.
std::optional<std::string> CheckFormat(const Format& f);

// The set so far.
struct SetState
{
  std::array<int, 2> wins{};
  u8 last_winner = 0xFF;  // in-game port of the last game's winner; 0xFE draw; 0xFF none yet
  std::array<u16, 2> last_win_stage{0xFFFF, 0xFFFF};
};

struct StepInfo
{
  int player;  // in-game port 0 / 1
  StepType type;
  int count;
  int seconds;
};

struct Steps
{
  bool striking = true;
  std::vector<StepInfo> steps;
  int step_idx = -1;  // the step waiting for selections; -1: the stage is decided
  int left = 0;
  std::vector<u8> remaining;  // the stages still selectable
  u16 stage = 0xFFFF;
};

// The steps of `game` (1-based) with both players' actions applied, in order: `actions[p]` is
// in-game port p's struck / banned / picked stages. A stage that is not selectable is ignored (a
// peer's bad action).
Steps EvaluateSteps(const Format& f, const SetState& set, u32 game,
                    const std::array<const std::vector<u8>*, 2>& actions);
// Someone has won the set (never with best_of 0).
bool SetOver(const Format& f, const SetState& set);
// The striking order's first player for `game`.
int FirstStriker(const Format& f, u32 game);

// A room's set as the server starts its session (get-ticket-resp `set`, ours; the server's
// common::proto::SetTicket): the format, the score so far and the set's game the session starts
// with. Sides are the set's two players (side 0 the lower slot); `side_ports` their in-game ports.
struct SetTicket
{
  Format format;
  SetState state;
  u32 first_game = 1;
  std::array<u8, 2> side_ports{0, 1};
};
struct ParsedSet
{
  std::optional<SetTicket> set;  // nullopt: no set in the response
  std::string error;             // not empty: there is one and it cannot be used (log `detail`)
  std::string detail;
};
ParsedSet ParseSetTicket(const picojson::object& get_ticket_resp);
}  // namespace Online::GameSetup
