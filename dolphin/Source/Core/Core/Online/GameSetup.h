// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Ranked's game setup: Slippi's stage striking and counterpicking (slippi-ssbm-c
// Scenes/Ranked/GameSetup.c), played on P+'s own stage select.
//
// P+ is character first (the user's, 2026-10-19): every game's characters are locked in before
// its stages are struck or picked.
// Game 1: Ranked's players choose their characters on the CSS once the opponent's name shows
// ("Opponent: alice"), both at once and blind (the search's lock-in is for no game; the CSS starts
// on the character the player searched with). A room's players chose theirs before readying. Then
// the five starter stages are struck 1-2-1: player 1 (the host, in-game port 0) strikes one,
// player 2 strikes two, player 1 strikes one; the stage left is played. After a draw the starters
// are struck again with the last game's characters (Slippi; the CSS locks in again at once,
// time_up).
// Games 2+: the winner of the last game may change character, and after the winner has locked
// in, the loser (who sees the winner's choice). Then the winner bans two stages of the
// counterpick list, and the loser picks the stage from the rest; the loser cannot pick the stage
// they last won on once they have won a game (Slippi's "Dave's stupid rule").
// Each step has Slippi's timer (strikes 30 s, the last game-1 strike 10 s, ban and pick 30 s,
// each character 45 s, game 1's 45 s for both). When it runs out (plus Slippi's 3 s of grace)
// this player's step is done for them: a random stage, or the character on the CSS (time_up). An
// opponent whose step has not ended 15 s after that is gone (Slippi's wait timeout): the set ends
// as if they had left. Once a player has two wins there are no more steps.
//
// Every player's own actions (struck / banned / picked stages, in order) travel in its gameplay
// session control messages (Gprb::Session::SetLocalExtra). Only one player acts at a time, so both
// machines merge the two lists into the same steps without any other agreement. The host sets up
// the match with the stage the steps decided (Gprb::Session::SetStageDecider); a stage is never
// taken from the stage select's own pick.
//
// The game reaches this through the mailbox: GP_FETCH_STEP (0xC1, every frame of the ranked CSS
// and stage select) and GP_COMPLETE_STEP (0xC0, a strike or pick), Slippi's command numbers.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Online/SetFormat.h"

namespace Online::GameSetup
{

// What the game shows and allows right now (GP_FETCH_STEP).
struct View
{
  bool active = false;  // a ranked set is on and the next game's setup is not finished
  StepType type = StepType::None;
  bool my_turn = false;
  u8 count = 0;          // selections left in this step
  bool to_sss = false;   // a stage step: the game belongs on the stage select
  bool may_lock = true;  // CSS: START may lock in now
  bool time_up = false;  // CSS: lock in now with the character on the CSS (no START needed)
  u8 seconds = 0;        // seconds left in this step (0: no timer)
  std::vector<u8> selectable;  // the stages that can still be struck / picked
  // The stage select's page (a stage step): every stage of the set, the same in every game (the
  // ones not selectable now are struck): the counterpick list in its order, then any starter it
  // lacks.
  std::vector<u8> page;
  u16 stage = 0xFFFF;          // the decided stage
  std::string text;            // the status line ("" = the game's own)
  u32 game = 0;
};

// The ranked set starts: this player's in-game port (0 host, 1 joiner) and the stage lists.
// Empty or unusable lists (not five starters, fewer than four counterpicks) fall back to P+'s
// starters / the legal list. Game 1 starts with the blind character step.
void Begin(int local_port, std::vector<u16> starters, std::vector<u16> counterpicks);
// A set in another format (SetFormat.h: a room's set, a ruleset's). A format that cannot be played
// (CheckFormat) is logged and Ranked's is used, the same on every machine.
void Begin(int local_port, Format format);
// A room's set (SetTicket: its format, the score so far, the set's game the session starts with,
// each side's in-game ports and the steps made so far). `local_port` is this player's in-game
// port. In 2v2 the sides are teams: either teammate strikes, bans and picks for the side, and the
// session host's machine (`host_port`) decides which presses count (SetFormat.h TakePresses);
// every player of the winning side, then of the losing one, chooses a character.
void Begin(int local_port, const SetTicket& ticket, int host_port);
// The set's game a session game is (a room's session starts at the set's game N).
u32 SetGameOf(u32 session_game);
// The side an in-game port plays (0xFE stays a draw); nullopt for no side.
std::optional<u8> SideOfPort(u8 port);
// The set is won (no more games), or there is no set.
bool SetDone();
// A room set's steps as the server keeps them (room-step): the set's game being set up, each
// side's actions so far, and whether its match has started. Returns one when they changed since
// the last one taken; nullopt otherwise (and without a room set).
struct StepReport
{
  u32 set_game = 0;
  std::array<std::vector<u8>, 2> actions;
  bool match_started = false;
};
std::optional<StepReport> TakeStepReport();
void End();
bool IsActive();
// A game ended: the winner's in-game port (0xFE draw) and the stage it was played on.
void OnGameResult(u8 winner, u16 stage);
// A game of a room's set was quit (or run back): it did not count, and the session's next game is
// the same set game again (SetGameOf), its steps made again unless the session set it up itself.
void OnGameNotCounted();
// A game of the set (Ranked or a room's) desynced and starts again from the state every machine
// agreed on (Gprb::Session::SetDesyncRecovery): the session's next game is the same set game, on
// the same stage, with the same characters. No steps: every player readies up on the CSS
// (StepType::Resume, RESUME_SECONDS, then the character is locked in for them).
void OnGameRestarting(u32 session_game, u16 stage);
constexpr int RESUME_SECONDS = 45;
// The game's GP_COMPLETE_STEP: this player strikes, bans or picks `kind`. False if refused.
bool Act(u8 kind);
// GP_FETCH_STEP; also runs the step timers. CPU thread.
View Current();
// The opponent's step ran out long ago without their game completing it (Slippi's wait timeout):
// the caller treats the opponent as gone.
bool OpponentStalled();
picojson::object Status();

// The default starters (P+'s competitive five: Battlefield, Final Destination, Smashville,
// Dream Land, Pokemon Stadium 2) as srStageKind.
const std::vector<u16>& DefaultStarters();

// ---- 3-4 player matches: free-for-all and teams (rooms; docs/nplayer/setup.md) ----
// Pure functions: the session calls them with what every machine has (the lock-ins, the end state
// of the last game, read from the rolled-back state), so every machine decides the same. Ports
// are in-game ports (0 = P1); a room's slot is its port, and ports may be empty (P1, P3).

constexpr int MAX_PORTS = 4;
// gmCharacterKind -> the name the game shows (P+ v3.2's roster), "a character" for anything else.
const char* CharacterName(u8 kind);

// The players' names by in-game port, for the character step's line ("alice is picking, then
// you"); empty: "Opponent". Set after Begin.
void SetPortNames(const std::array<std::string, MAX_PORTS>& names);
constexpr u8 NO_TEAM = 0xFF;
// Brawl's team colours (gmPlayerInitData+0x0B; Slippi's order): 0 red, 1 blue, 2 green.
constexpr u8 NUM_TEAMS = 3;

enum class SetupError : u8
{
  None = 0,
  SameTeam = 1,       // a team battle with everyone on one colour
  NoTeam = 2,         // a team battle and a player without a colour
  TooFewPlayers = 3,  // fewer than two players
};
// The CSS's status text for an error ("Pick different teams"), "" for None.
const char* SetupErrorText(SetupError error);

// A port's player for the next game: whether the port is taken, and the team colour from their
// lock-in (another machine's byte: anything but 0-2 is "no team").
struct Seat
{
  bool present = false;
  u8 team = NO_TEAM;
};
struct TeamSetup
{
  SetupError error = SetupError::None;
  bool teams = false;                                  // a team battle
  std::array<u8, MAX_PORTS> team{NO_TEAM, NO_TEAM, NO_TEAM, NO_TEAM};  // NO_TEAM in a free-for-all
};
// `teams_on` is the room's Teams switch. With two players it has no effect (a 1v1, as Slippi).
// A team battle needs every player on one of the three colours and at least two colours (2v2, 2v1,
// 3v1, 2v1v1 and 1v1v1 are fine; everyone on one colour is refused, "Pick different teams").
TeamSetup DecideTeams(bool teams_on, const std::array<Seat, MAX_PORTS>& seats);

// One port at the end of a game: as set up, and what the game's end state shows.
struct PortEnd
{
  bool present = false;
  u8 team = NO_TEAM;  // the port's team in a team battle
  s32 stocks = 0;
  float damage = 0;
  u8 out = 0;  // the game's elimination order (SESSION `out`): 1 = first out, 0 = still in
};
struct Outcome
{
  std::array<u8, MAX_PORTS> place{};  // 1 = first (shared by a tie), 0 = not playing
  // Free-for-all: the winner's port; teams: the lowest port of the winning team. 0xFE: no single
  // winner (a draw), 0xFF: nobody played.
  u8 winner = 0xFF;
  // Ports (a bit each) that pick the next game's stage (the loser's pick): the last place; in a
  // team battle the losing team's lowest port; a tie for last place: the lowest port among them;
  // everyone tied (a draw): each side's lowest port (a 1v1 draw: both, as Direct).
  u8 pickers = 0;
};
// A side is a player (free-for-all) or a team. Sides still in at the end come first, ranked by
// their stocks (more is better) and then their damage (less is better), P+'s time-out rule; sides
// that were eliminated come after them, the later out the better.
Outcome DecideOutcome(bool teams, const std::array<PortEnd, MAX_PORTS>& ports);

// The port whose stage pick the next game is played on: the first picker (in port order) that
// picked a stage; -1: none (a random stage). Only a picker's pick counts: anyone else's (a pick
// left over from an earlier game) is not played. `picks` holds each port's pick, 0xFFFF none.
int StagePickPort(u8 pickers, const std::array<u16, MAX_PORTS>& picks);
}  // namespace Online::GameSetup
