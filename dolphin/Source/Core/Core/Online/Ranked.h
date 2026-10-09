// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Ranked sets: what Slippi's game-reporter and rank fetcher do for a ranked match
// (slippi-rust-extensions game-reporter, user/src/rank_fetcher; docs/backend-design.md 1.6-1.7).
//
// Every online 1v1 game (Ranked, Unranked, Direct) is reported to the accounts service as it ends,
// for the players' match history; only Ranked's are rated.
// A ranked match (search mode 0) is a best-of-three set. This module
// - reports every game to the accounts service as it ends (POST /v1/ranked/report-game: the
//   winner, stage, characters, stocks and damage, from the state both peers ended on), both
//   players' clients doing so independently, as on Slippi;
// - counts the wins and says when the set is over (two wins), so the session ends once the game
//   is back on the character select (GameBridge, GET_MATCH_STATE), as Slippi's ranked set does;
// - reports a set that ends early (POST /v1/ranked/report-leave): "left" when this player leaves
//   (CLEANUP_CONNECTION before the set is over), "opponent_left" when the opponent disconnects or
//   goes silent;
// - then fetches the set's result (GET /v1/ranked/result) for the rating change, which the game
//   reads with GET_RANK.
//
// The server decides the set from both clients' reports (server/crates/common/src/ranked.rs);
// nothing here is trusted beyond being this player's report. Requests run on a worker thread with
// retries (Slippi's reporter tries 5 times with backoff), never on the CPU thread.
//
// The rating is an Elo number on Slippi's scale; there are no rank tiers.

#pragma once

#include <optional>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Gprb::Session
{
struct GameResult;
}

namespace Online
{
struct Match;

namespace Ranked
{
// The hand-off of a connected match to the session (any mode; only ranked ones are tracked).
void OnSessionStart(const Match& match);
// CPU thread: a game of the session ended with GAME SET (the gameplay session's callback).
void OnGameResult(const Gprb::Session::GameResult& result);
// The opponent disconnected or went silent (GameBridge sees the lobby's disconnected flag).
void OnPeerGone();
// Client::Cleanup(): this player leaves the connection, or the finished set is being closed.
void OnCleanup();
// True from the game that decides the set until the cleanup after it.
bool IsSetOver();

// What GET_RANK shows: this player's rating and the change of the last ranked set.
struct RankInfo
{
  enum class State : u8
  {
    Unknown = 0,  // not logged in, or the rating could not be read
    Fetching = 1,  // a set's result is on its way
    Ready = 2,
  };
  State state = State::Unknown;
  float rating = 0;
  u32 sets_played = 0;
  std::optional<float> change;  // the last set's change, once known
};
RankInfo GetRankInfo();

// JSON for the harness (online_status "ranked").
picojson::object Status();
// Process exit: stops the worker (pending reports are dropped after a short wait).
void Shutdown();
}  // namespace Ranked
}  // namespace Online
