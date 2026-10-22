// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Recording online games into replay files (docs/design/replays.md §5).
//
// Every player records the games they play (Direct, Unranked, Ranked, rooms; 2-4 players) into
// their own replay folder. The gameplay session calls in on the CPU thread with its mutex held:
// - Begin at the barrier, once the host's values are applied: the match setup and everything
//   playback needs to rebuild the match from a fresh boot (the SETP JSON, built by the session);
// - OnPass for every pass: the frame's input (a later pass of the same frame overwrites it, so
//   the ring holds each frame's final run);
// - OnSave for every save: the frame's checksum and the fighters' raw memory (the same rule);
// - Advance once per update: frames no rollback can reach any more (MAX_ROLLBACK_FRAMES + 1 behind
//   the newest) are final and go to the writer;
// - End when the session ends: the rest of the frames, the result, the trailer.
// These only copy small records. A writer thread does all the encoding, compression and file
// writes, so a slow disk never slows the game; a crash loses at most the last second. The writer
// runs below the emulator's threads (Common::SetCurrentThreadBackgroundPriority): on a machine
// short of CPU it falls behind (its queue grows, up to MAX_QUEUED) before the game does.
//
// The same chunks go to spectators (Online/Spectate.h) when the player lets others stream their
// games ([Online] StreamGames): each chunk is encoded once, for the file and for the upload, and
// never fighter memory. A game that only streams (SaveReplays off) reads no fighter memory at all.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Rollback/ReplayFile.h"

namespace Core
{
class System;
}

namespace Gprb::Replay
{
// Bumped by any change to how a match is simulated from its recorded inputs: the barrier's
// writes, the countdown, pacing, the file IO wait, input anchoring and sanitising, the start
// frame. A replay with another SIM_VERSION may not play back the same (docs/design/replays.md
// §6.5): playback says so and offers to play it anyway.
// 2 (2026-10-14): the countdown runs with each player's buttons held at the barrier (SETP "holds",
// GameplaySession's CountdownSlots). A version 1 file has no holds and plays as recorded (a
// neutral countdown), so playback takes both (SimVersionPlays).
// 3 (2026-10-22): Wii Remotes (alone, with a Nunchuk, with a Classic Controller). Each player's
// input record goes into their port's GameCube or Wii Remote pad by its controller type
// (GameplayRollback WriteSlots), and the countdown's holds carry the type. A game where everyone
// plays on GameCube controllers simulates as before and is still written as 2 (SIM_VERSION_GC),
// so that older builds play it; a game with a Wii Remote is 3, which they refuse.
constexpr u32 SIM_VERSION = 3;
constexpr u32 SIM_VERSION_GC = 2;
constexpr bool SimVersionPlays(u64 sim)
{
  return sim >= 1 && sim <= SIM_VERSION;
}

// Who played, by in-game port (the session's view of matchmaking's players).
struct PlayerMeta
{
  bool present = false;
  std::string uid;
  std::string name;
  std::string code;
};

// What the matchmaking ticket says about the match (GameplayOnlineBackend); empty for a harness
// session.
struct MatchMeta
{
  std::string match_id;
  int mode = -1;  // Matchmaking::Mode: 0 ranked, 1 unranked, 2 direct, 3 teams
  bool room = false;
  std::array<PlayerMeta, PORTS> players{};
  picojson::object rules;  // MatchRules::ToJson, {} when the server sent none
};

struct BeginInfo
{
  MatchMeta meta;
  int local_port = 0;
  bool host = false;
  u32 match_ports = 0;
  u32 game = 0;          // the game number the players see (SESSION `game`)
  u32 session_game = 0;  // the session's game (1-based)
  u16 stage = 0;
  u8 asl = 0;
  bool teams = false;
  std::array<u8, PORTS> char_kind{};
  std::array<u8, PORTS> costume{};
  std::array<u8, PORTS> team{};
  u32 start_frame = 0;
  int input_delay = 0;
  std::string region_set;
  // The SETP chunk (JSON, built by the session: GameplaySession.cpp ReplaySetup).
  std::string setup_json;
  // SIM_VERSION, or SIM_VERSION_GC for a game without Wii Remotes.
  u32 sim_version = SIM_VERSION;
  // game_uid: the same on every player's file of this game.
  std::array<u8, 16> uid{};
};

struct EndInfo
{
  // "game" (game set), "quit", "runback", "quit-left" (a 3-4 player match this player left),
  // "disconnect", "desync", "stopped", "error"
  std::string method;
  s64 game_set_frame = -1;
  u8 winner = 0xFF;   // in-game port, 0xFE draw, 0xFF none
  u8 quitter = 0xFF;
  std::array<bool, PORTS> present{};
  std::array<s32, PORTS> stocks{};
  std::array<float, PORTS> damage{};
  std::array<u8, PORTS> place{};
  u64 rollbacks = 0;
  u64 max_rollback = 0;
  u64 desyncs = 0;
  // This PC's speed over the game (network play; 0 frames: not measured), ENDG's `perf`: frames a
  // second, the share of frames late without a rollback before them (this PC's own speed), the
  // share run at the speed of a peer that could not keep up and that peer's frame rate, and
  // whether the game showed "can't keep up" for this PC / a peer (UpdateSlowNotice).
  struct Perf
  {
    u64 frames = 0;
    double fps = 0;
    double own_late = 0;
    double followed = 0;
    double peer_fps = 0;
    bool slow = false;
    bool peer_slow = false;
  } perf;
};

// Whether this match's games are recorded: saved ([Online] SaveReplays) or streamed (Streamed).
// The settings (and ReplayDir, ReplayMonthlyFolders) are read at Begin.
bool Enabled(const MatchMeta& meta);
// Whether this match's games are streamed to mm: a match mm made, with [Online] StreamGames on,
// or a Ranked set whatever it says (the server's record of the set, 2026-10-21; mm lists it only
// when a player of it lets others watch).
bool Streamed(const MatchMeta& meta);

// CPU thread. Begin opens the file (on the writer thread) under its final name; false when
// recording is off.
bool Begin(Core::System& system, const BeginInfo& info);
bool Active();
void OnPass(s64 frame, const Slots& pads);
void OnSave(Core::System& system, s64 frame, u32 checksum, const Parts& parts);
void Advance(s64 newest_frame);
void AddEvent(s64 frame, picojson::object event);
void End(const EndInfo& info);
// A session that never got to run its frames (an error at the start barrier): no file.
void Abort();

// The path of the file being (or last) written, for the harness and the logs.
std::string CurrentPath();
// Blocks until the writer has written everything queued (tests, shutdown).
void Flush();

// The raw fighter memory a file holds per frame and port (FTSP): named blocks of guest memory,
// each found from the port's ftEntry or its active Fighter by a chain of pointers.
struct FighterBlock
{
  const char* name;
  bool from_fighter;           // start at the active Fighter (else at the ftEntry)
  std::vector<u32> chain;      // add, read a pointer, ...; the last one is added only
  u32 size;
};
const std::vector<FighterBlock>& FighterBlocks();
u32 FighterBlockBytes();
std::string FighterSpecJson();
// The port's blocks, concatenated (zeros where a pointer does not resolve).
void ReadFighterMemory(Core::System& system, int port, u8* out);
}  // namespace Gprb::Replay
