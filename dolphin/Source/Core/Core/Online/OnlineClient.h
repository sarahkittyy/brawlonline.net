// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The online client: the logged-in User, the current Matchmaking and the hand-off of a connected
// match to the online session. This is what Slippi's EXI device does with its `user`,
// `matchmaking` and `slippi_netplay` members (EXI_DeviceSlippi.cpp: handleFindOpponent,
// handleConnectionCleanup, prepareOnlineStatus); the game will reach it through the mailbox of
// docs/backend-design.md 5.2, and the harness reaches it today (online_status, mm_search_direct,
// mm_status, mm_cancel).
//
// All functions are thread-safe.

#pragma once

#include <optional>
#include <string>

#include <picojson.h>

#include "Core/Online/Matchmaking.h"

namespace Online
{
class User;

namespace Client
{
// The user from <User>/Online/user.json. The first call creates it and starts watching for the
// file (Slippi starts the watcher when its EXI device is created).
User& GetUser();

struct SearchOptions
{
  Matchmaking::SearchSettings settings;
  // Hand the connected match to the registered session backend (OnlineSession.h). Without a
  // backend, or with false, the P2P link stays open in the matchmaking until mm_cancel.
  bool hand_off = true;
  // This player's lock-in, passed to the backend.
  picojson::object selections;
};

// Slippi's FIND_OPPONENT: starts a search. Fails only while a search is already running.
std::optional<std::string> FindMatch(const SearchOptions& options);
// Slippi's CLEANUP_CONNECTION: ends the search or the connection and the session, and starts over
// with a fresh, idle matchmaking. Returns at once; the teardown finishes in the background.
void Cleanup();
// Stops everything and waits for it (process exit).
void Shutdown();

// What the game's GET_MATCH_STATE shows (GameBridge.h): the matchmaking state, its error text,
// the match (peer name and code) once known, and the hand-off and session state.
struct MatchState
{
  Matchmaking::State state = Matchmaking::State::Idle;
  std::string error;
  std::optional<Match> match;
  std::string handoff;  // "", "started", "kept", "failed"
  SessionStatus session;
};
MatchState GetMatchState();

// JSON for the harness (docs/harness-protocol.md, "Online").
picojson::object OnlineStatus();
picojson::object MatchmakingStatus();
}  // namespace Client
}  // namespace Online
