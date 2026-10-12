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

// ---- A side of two (2v2): either teammate strikes, bans and picks (room-rules.md D) ----
// The session host's machine is the single authority: every player sends its presses (each with
// the step it was made for), and the host takes them in the order it sees them (on the same frame:
// one press per port per round, lower ports first). A press counts when its side is on that very
// step and the stage can still be chosen; otherwise it is dropped. Every machine plays the host's
// lists; a player shows its own presses the host has not answered yet on top of them, so a press
// shows at once, and one the host dropped disappears again.

constexpr int MAX_PORTS_SHARED = 4;

// One press: the step (EvaluateSteps' step index) it was made for, and the stage.
struct Press
{
  u8 step = 0;
  u8 stage = 0;
  bool operator==(const Press&) const = default;
};

// The host's lists: each side's actions so far, and how many of each port's presses it has taken.
struct SharedSteps
{
  std::array<std::vector<u8>, 2> actions;
  std::array<u32, MAX_PORTS_SHARED> taken{};
};

// The host takes every press of `presses` (per port, in its order) not taken yet. `side_of_port`
// is each port's side (-1: no player of the set). Returns the presses it dropped (port, press).
std::vector<std::pair<int, Press>>
TakePresses(const Format& f, const SetState& set, u32 game,
            const std::array<int, MAX_PORTS_SHARED>& side_of_port,
            const std::array<std::vector<Press>, MAX_PORTS_SHARED>& presses, SharedSteps& host);

// What a player of `side` shows: the host's lists with its own presses the host has not taken yet
// (`pending`) applied where they still fit the step they were made for.
std::array<std::vector<u8>, 2> WithPending(const Format& f, const SetState& set, u32 game, int side,
                                           const std::array<std::vector<u8>, 2>& host,
                                           const std::vector<Press>& pending);

// A room's set as the server starts its session (get-ticket-resp `set`, ours; the server's
// common::proto::SetTicket): the format, the score so far and the set's game the session starts
// with. Sides are the set's two players or teams (side 0 the one with the lower slot);
// `side_ports` each side's first (lowest) in-game port.
struct SetTicket
{
  Format format;
  SetState state;
  u32 first_game = 1;
  std::array<u8, 2> side_ports{0, 1};
  // Each side's in-game ports, the lowest first: one player, or a team of two.
  std::array<std::vector<u8>, 2> members{std::vector<u8>{0}, std::vector<u8>{1}};
  // `first_game`'s stage actions so far, per side (the server keeps them from room-step): a
  // session that starts again in the middle of a game's steps goes on from them.
  std::array<std::vector<u8>, 2> actions;
};
struct ParsedSet
{
  std::optional<SetTicket> set;  // nullopt: no set in the response
  std::string error;             // not empty: there is one and it cannot be used (log `detail`)
  std::string detail;
};
ParsedSet ParseSetTicket(const picojson::object& get_ticket_resp);
}  // namespace Online::GameSetup
