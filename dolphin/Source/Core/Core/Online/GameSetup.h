// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Ranked's game setup: Slippi's stage striking and counterpicking (slippi-ssbm-c
// Scenes/Ranked/GameSetup.c), played on P+'s own stage select.
//
// Game 1 (and the game after a draw): the five starter stages are struck 1-2-1: player 1 (the
// host, in-game port 0) strikes one, player 2 strikes two, player 1 strikes one; the stage left is
// played. The characters are the ones locked in on the CSS before the search.
// Games 2+: the winner of the last game bans one stage of the counterpick list, then the loser
// picks the stage from the rest; the loser cannot pick the stage they last won on once they have
// won a game (Slippi's "Dave's stupid rule"). Then the winner may change character, and after the
// winner has locked in, the loser (who sees the winner's choice).
// Each step has Slippi's timer (strikes 30 s, the last game-1 strike 10 s, ban and pick 30 s);
// when it runs out this player's step is done for them with a random stage, as Slippi does.
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

// What the game shows and allows right now (GP_FETCH_STEP).
struct View
{
  bool active = false;  // a ranked set is on and the next game's setup is not finished
  StepType type = StepType::None;
  bool my_turn = false;
  u8 count = 0;          // selections left in this step
  bool to_sss = false;   // a stage step: the game belongs on the stage select
  bool may_lock = true;  // CSS: START may lock in now
  u8 seconds = 0;        // seconds left in this step (0: no timer)
  std::vector<u8> selectable;  // the stages that can still be struck / picked
  u16 stage = 0xFFFF;          // the decided stage
  std::string text;            // the status line ("" = the game's own)
  u32 game = 0;
};

// The ranked set starts: this player's in-game port (0 host, 1 joiner) and the stage lists.
// Empty lists fall back to P+'s starters / the legal list.
void Begin(int local_port, std::vector<u16> starters, std::vector<u16> counterpicks);
void End();
bool IsActive();
// A game ended: the winner's in-game port (0xFE draw) and the stage it was played on.
void OnGameResult(u8 winner, u16 stage);
// The game's GP_COMPLETE_STEP: this player strikes, bans or picks `kind`. False if refused.
bool Act(u8 kind);
// GP_FETCH_STEP; also runs the step timers. CPU thread.
View Current();
picojson::object Status();

// The default starters (P+'s competitive five: Battlefield, Final Destination, Smashville,
// Dream Land, Pokemon Stadium 2) as srStageKind.
const std::vector<u16>& DefaultStarters();
}  // namespace Online::GameSetup
