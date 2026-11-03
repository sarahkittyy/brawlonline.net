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
  // A room's game (Online/Rooms.h): copied into the Match handed to the backend.
  int room_host_port = 0;
  bool room_teams = false;
  bool room_team_colors = false;  // with room_teams: every player in their team's costume
  u8 room_pickers = 0;
  // The game's own search (GameBridge's FIND_OPPONENT): the game stopping cancels it while it has
  // not connected yet, as the game leaving the CSS does (CLEANUP_CONNECTION).
  bool cancel_on_stop = false;
};

// Slippi's FIND_OPPONENT: starts a search. Fails only while a search is already running.
std::optional<std::string> FindMatch(const SearchOptions& options);
// Slippi's CLEANUP_CONNECTION: ends the search or the connection and the session, and starts over
// with a fresh, idle matchmaking. Returns at once; the teardown finishes in the background.
void Cleanup();
// The game is stopping: Cleanup() if the running search is the game's (cancel_on_stop) and has not
// connected yet. Called on Core::State::Stopping.
void CancelGameSearch();
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
// The mode of the search running (or connected, until Cleanup), if any. Lock-free: the loop top
// reads it every frame (SpeedCheck).
std::optional<Matchmaking::Mode> SearchMode();

// ---- A 2v2 party's pair room (docs/design/2v2.md, "The pair room") ----
// While both players of a 2v2 party search (the party's room-state says pairRoom), their games
// connect to each other with a party ticket (a mode 3 search naming the party's code, beside the
// 2v2 search, on a port of its own) and play the waiting room together, in a session of its own
// (Match::pair_room). It ends as soon as the 2v2 search no longer waits for its match (found,
// failed, cancelled), at whatever step of its own connection it is, and a session of it is
// stopped; the 2v2 match's hand-off ends it too, and waits until its match is over. A party
// ticket whose players could not connect, and a pair room whose session ended by itself (either
// player's hold START, the partner gone, a desync), end the 2v2 search as well: they would not
// connect in the match either (Sarah's, 2026-11-03). The search then reads as the pair room's
// error (GetMatchState), "Couldn't connect to bob.", until the game's next search or cleanup.
//
// CPU thread, every frame (GameBridge): the party's room-state, `pair_room` and the party's code
// (false and "" outside one).
void TickPairRoom(bool pair_room, const std::string& party);
// The pair room's session ended by itself (GameBridge reads it from the gameplay session's lobby):
// `error` the line the search ends with, "" for a hold START (no error). `wait`: the partner left
// (or was never heard): the search ends only if it is still waiting for its match a moment later,
// since a partner whose search found the match leaves the pair room as it is found.
void PairRoomOver(const std::string& error, bool wait = false);
// The pair room's match while its session runs (handed off), else nothing.
std::optional<Match> PairRoomMatch();
// The session is the pair room's (lock-free).
bool PairRoomSession();

// JSON for the harness (docs/harness-protocol.md, "Online").
picojson::object OnlineStatus();
picojson::object MatchmakingStatus();
}  // namespace Client
}  // namespace Online
