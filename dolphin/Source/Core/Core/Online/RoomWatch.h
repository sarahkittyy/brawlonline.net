// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Watching the room's game from the room CSS (docs/design/rooms.md §1.3; the user's, 2026-10-19).
//
// A room member who is not in the room's game (sitting out, or joined during it) watches it in
// their own game, automatically: mm sends them the game's upload as spectators get it (the same
// kept chunks, docs/spectate-protocol.md) on the online connection's watch channel
// (docs/rooms-protocol.md §2, `room-watch`), and this plays it as a live replay
// (Gprb::Session::ArmPlayback with `live` and `room_watch`). The game leaves the room CSS for its
// match (GameBridge marks SESSION RF_WATCH) and comes back to the room CSS after it, as after the
// room's own games. Nothing here touches the players: they upload as they do for any spectator.
//
// - START or B ends the watch at once (the match quits as a stopped replay does) and nothing more of
//   that game comes; the room's next game is watched again.
// - The watcher runs a few seconds behind the players (the upload's 10 s hold at a game's start, and
//   its backoff). The room never waits for them: while a game plays here, mm counts this player as
//   watching (`room-watch {watching: true}`) and starts the next game without them. Once the
//   players' game is over and it still plays here, a line says so: "Game ended, press B to return to
//   CSS." (the user's, 2026-10-19).
// - A launcher click while watching is not refused as busy: it ends the watch (Abort) and goes ahead
//   once the game is back on the room CSS.
//
// The functions below run on the Rooms thread, which owns the online connection, except Status,
// Watching and Abort (any thread).

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Online::RoomWatch
{
using Clock = std::chrono::steady_clock;

// The watch channel (common::spectate::WATCH_CHANNEL) and its packet kinds: one byte, then for
// BEGIN and END UTF-8 JSON ({matchId, number, uid} / {matchId, uid, bad?}), for CHUNK the chunk's
// index in its game (u32, little-endian) and the chunk as uploaded (its 20-byte header included).
constexpr u8 CHANNEL = 2;
constexpr u8 KIND_BEGIN = 1;
constexpr u8 KIND_CHUNK = 2;
constexpr u8 KIND_END = 3;

// A game held here at most (the server keeps 2 MB of a game), and how many games.
constexpr size_t MAX_GAME_BYTES = 4u << 20;
constexpr size_t MAX_GAMES = 4;

// A watched game as it came: a .brep (the header with the game's uid, then every chunk so far),
// which Gprb::Replay::Parse reads as a live file.
struct Game
{
  std::string match_id;
  std::array<u8, 16> uid{};
  u32 number = 0;
  std::vector<u8> bytes;
  u32 chunks = 0;
  bool ended = false;  // its ENDG came
  std::string bad;     // nothing more of it comes (the upload was cut, or a packet was wrong)
  bool Done() const { return ended || !bad.empty(); }
};

// The watch channel's games put together (no I/O; the unit tests drive it).
class Assembler
{
public:
  // One packet. False if it was malformed (then the game it belongs to is marked bad).
  bool OnPacket(std::span<const u8> data);
  std::deque<Game>& Games() { return m_games; }
  const std::deque<Game>& Games() const { return m_games; }
  void Clear() { m_games.clear(); }

private:
  std::deque<Game> m_games;
};

// What the Rooms thread knows of the room each tick.
struct RoomInfo
{
  bool online = false;        // the online connection is up (hello answered)
  u64 connection = 0;         // counts connections: a new one is told `want` again
  bool in_room = false;
  bool room_in_game = false;  // room-state status `in-game`
  bool me_in_game = false;    // this player is in the room's game
  bool own_game = false;      // this player's own room game runs (its start or session)
  u8 screen = 0;              // the game's LOCAL screen (Rooms::Screen)
};

// A packet on the watch channel (Rooms thread).
void OnPacket(std::span<const u8> data);
// Every Rooms tick: arms, grows and ends the watched game's playback; the `room-watch` messages to
// send to mm.
std::vector<std::string> Service(Clock::time_point now, const RoomInfo& room);
// A watched game is armed or plays here (Rooms' launcher clicks: not busy, Abort first).
bool Watching();
// Ends the watch as START / B does (a launcher click, the harness's `watch_leave`).
void Abort();
picojson::object Status();
}  // namespace Online::RoomWatch
