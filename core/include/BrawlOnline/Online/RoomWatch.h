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
// - The seek bar's Clip, then "Save as highlight" or Render (w/ Music) / Render (No Music), as in
//   the launcher's playback Dolphin (Sarah's board, 2026-11-26: "it should be in room watch, along
//   with render"). The replay overlay asks Gprb::Replay::Playback, which asks here when it follows
//   no comm file (the netplay Dolphin, EmissaryRecomp). A watched game is kept in memory only;
//   once one of its clips is saved it is written to <Online>/RoomWatch/<uid>.brep, and then on as
//   it comes, until its end ("games you watch while sitting out can be saved to file somewhere but
//   probably not saved to replays unless you actually clip something. they should get discarded if
//   you didn't."). The launcher hears of the clips from <Online>/room-watch-clips.json (Files):
//     {"version": 1, "run": "<this run's>", "savedClips": [{"path", "startFrame", "endFrame"}],
//      "renderRequests": [{"seq", "path", "startFrame", "endFrame", "music", "clip", "label"?}],
//      "showRender"?: {"seq", "id"}}
//   (as the playback Dolphin's status says them, ReplayPlayback.h; seq from 1 for each run). It
//   keeps a clip's game as a spectated game's, a copy in <Root Replay Folder>/Spectated, and lists
//   the clip in Highlights > Manual Clips (launcher/src/replays/highlights/manual_clips.ts). Its
//   side of the seek bar (its colour, the renders' progress) is <Online>/room-watch-overlay.json,
//   the playback Dolphin's <comm>.overlay.json. The RoomWatch folder and the clips file are cleared
//   at the next start (the launcher has its copies by then).
//
// The functions below run on the Rooms thread, which owns the online connection, except Status,
// Watching, Abort and the clip ones (any thread).

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
// Tests (the harness's `watch_hold`): what comes of the watched game is kept but not played, as
// when the players' upload pauses: the watch runs out of frames and buffers.
void HoldFeed(bool hold);
picojson::object Status();

// The overlay's Clip (any thread; Gprb::Replay::Playback's when it follows no comm file): the
// watched game plays here and its file can be written (Files has been told the folder).
bool CanSaveClip();
// "Save as highlight": frames `start` to `end` of the watched game, its file kept from now on.
bool SaveClip(s64 start, s64 end);
// Render (w/ Music) / Render (No Music): asks the launcher to render them (`save`: clip mode's,
// saved first; else a star's highlight playing). `label`: what the launcher's toast calls it.
bool RequestRender(s64 start, s64 end, bool music, bool save, const std::string& label);
// A finished render's folder button (the overlay file's toast `id`).
void ShowRenderFolder(const std::string& id);
// The watched game's file as the clips name it (empty: none can be saved), for the overlay to
// find its renders in the launcher's overlay file.
std::string ClipPath();
// The Rooms thread's file step (`online_dir`: <User>/Online/, with its slash): writes what came of
// the kept games, then room-watch-clips.json when it changed, and reads room-watch-overlay.json.
void Files(const std::string& online_dir);
}  // namespace Online::RoomWatch
