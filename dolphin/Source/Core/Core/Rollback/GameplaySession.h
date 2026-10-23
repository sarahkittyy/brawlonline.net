// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Gameplay-only (Slippi-style) rollback session for Brawl / Project+.
//
// Two to four players boot and use the menus independently. They connect while on the character
// select (no Dolphin netplay lobby, no shared boot; every pair of players exchanges packets
// directly), exchange their selections, and start the same match. The rollback session begins at
// the first frame of the match simulation, behind a barrier where the host's RNG state, frame
// counters and scheduler task order are copied to every joiner. During the match GekkoNet exchanges
// inputs, and rollbacks save and restore only the gameplay region set (Data/Sys/Rollback/<set>.json,
// see GameplayRollback.h) through RollbackManager's region mode. A player who leaves a 3-4 player
// match is gone from a frame the others agree on, and the match goes on (docs/nplayer/session.md).
//
// A single-instance sync test runs the same machinery with a GekkoNet stress session: every frame
// the state from N frames back is restored and the frames are simulated again, and both the
// gameplay checksum and a hash of the whole region set are compared with the first run.
//
// The CPU-thread entry points are called from the Brawl game-loop HLE hooks (HLE_Misc.cpp). The
// control entry points (Arm*, Connect, SetSelections, Status, Stop) are called by the harness
// server for now; the in-game online menus will drive the same calls later.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Rollback/ReplayRecorder.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Gprb::Session
{
// Input delay in frames. Each player's setting is a profile (Aggressive, Normal, Smooth; Normal
// the default), or a number of frames, which applies to that player's own inputs in Direct and
// rooms only (Ranked and Unranked play Normal for it). A profile picks the delay from the round
// trip when the game's GekkoNet session starts (ProfileInputDelay). Each player plays their own
// profile: a player's delay smooths their own screen only (5be14305), so one player's choice costs
// another nothing (the user's call, 2026-10-22; docs/gameplay-rollback-status.md, "Delay
// profiles").
// Normal (AutoInputDelay): 1 frame below 90 ms, 2 below 140 ms, 3 below 190 ms, else 4, in
// GekkoNet's ping (what players see in game; GameRoundTrip). Low delay is what players feel, but
// the rollbacks it costs must fit in the frame: a re-run frame is 3.7-4.2 ms on an i7-10700KF, and
// at delay 1 and ~95 ms its ~1.7-deep rollbacks took 7-9 ms, past the present hold (bug #27,
// 2026-10-16; was 100/150/200 ms). Skipping the render in re-run passes is not an option (docs/
// gameplay-rollback-status.md, Phase 28).
// A round trip not measured yet: the default.
constexpr int DEFAULT_INPUT_DELAY = 2;
constexpr int MAX_INPUT_DELAY = 9;
// The control channel's pings are answered on the network thread as they arrive; in a game,
// GekkoNet reads packets and answers its pings once per frame, on each side, so its round trip is
// a frame longer on average, and so is the wait for a remote input (half a frame each way). The
// delay is picked from the control channel's round trip plus that frame (prod: 90 ms before the
// game read 103-125 ms in it, 30 ms read 46-67 ms).
constexpr double GAME_POLL_RTT_MS = 1000.0 * 1001.0 / 60000.0;
constexpr double GameRoundTrip(double control_rtt_ms)
{
  return control_rtt_ms < 0 ? control_rtt_ms : control_rtt_ms + GAME_POLL_RTT_MS;
}
constexpr int AutoInputDelay(double rtt_ms)
{
  if (rtt_ms < 0)
    return DEFAULT_INPUT_DELAY;
  return rtt_ms < 90 ? 1 : rtt_ms < 140 ? 2 : rtt_ms < 190 ? 3 : 4;
}
static_assert(AutoInputDelay(-1) == 2 && AutoInputDelay(0) == 1 && AutoInputDelay(89.9) == 1 &&
              AutoInputDelay(90) == 2 && AutoInputDelay(139.9) == 2 &&
              AutoInputDelay(140) == 3 && AutoInputDelay(189.9) == 3 &&
              AutoInputDelay(190) == 4 && AutoInputDelay(400) == 4);
static_assert(AutoInputDelay(GameRoundTrip(-1)) == 2 && AutoInputDelay(GameRoundTrip(30)) == 1 &&
              AutoInputDelay(GameRoundTrip(73)) == 1 && AutoInputDelay(GameRoundTrip(74)) == 2 &&
              AutoInputDelay(GameRoundTrip(90)) == 2);

// The input delay setting ([Online] InputDelay, the launcher's; SetInputDelay): a profile, or
// 1-MAX_INPUT_DELAY frames.
constexpr int DELAY_NORMAL = 0;
constexpr int DELAY_AGGRESSIVE = -1;
constexpr int DELAY_SMOOTH = -2;
// A profile as a level: -1 Aggressive, 0 Normal, 1 Smooth (a number of frames: Normal).
constexpr int DelayLevel(int setting)
{
  return setting == DELAY_AGGRESSIVE ? -1 : setting == DELAY_SMOOTH ? 1 : 0;
}
constexpr const char* DelayLevelName(int level)
{
  return level < 0 ? "aggressive" : level > 0 ? "smooth" : "normal";
}

// The rollback a delay leaves on the other player's screen, measured in prod games (1,126 games of
// 44 players' bug reports, 2026-10-22; 14 with both players' logs): about (ping in game / 33.4 ms
// - 0.5 - that player's delay) frames deep on a steady link, half the round trip in frames. How
// deep a screen can roll back before frames are shown late depends on its PC's cost of a re-run
// frame (median 3.3 ms, 10% of PCs above 5.8 ms): about 2 frames on a fast PC, 1.3 on the others.
constexpr double ROUND_TRIP_FRAME_MS = 2 * 1000.0 * 1001.0 / 60000.0;
// The smallest whole number at least x (std::ceil is not constexpr before C++23).
constexpr int CeilFrames(double x)
{
  const int i = static_cast<int>(x);
  return x > i ? i + 1 : i;
}
// The delay that leaves rollbacks at most `depth` frames deep on a steady link (at least 1).
constexpr int DelayForDepth(double rtt_ms, double depth)
{
  return std::clamp(CeilFrames(rtt_ms / ROUND_TRIP_FRAME_MS - 0.5 - depth), 1, MAX_INPUT_DELAY);
}
// A profile's delay from GekkoNet's ping (GameRoundTrip; -1 not measured: the default).
// Aggressive: rollbacks up to 3 frames deep, the most a ~4 ms re-run frame PC redoes at full speed
// (frames shown late on about 15% of frames even on a fast one). Normal: AutoInputDelay, and never
// less than Aggressive (above ~250 ms it follows it rather than stopping at 4). Smooth: a frame
// more than Normal, but no more than the delay at which a steady link has no rollbacks left.
constexpr int ProfileInputDelay(int level, double rtt_ms)
{
  if (rtt_ms < 0)
    return DEFAULT_INPUT_DELAY;
  const int aggressive = DelayForDepth(rtt_ms, 3);
  const int normal = std::max(AutoInputDelay(rtt_ms), aggressive);
  if (level < 0)
    return std::min(aggressive, normal);
  if (level == 0)
    return normal;
  return std::clamp(std::min(normal + 1, DelayForDepth(rtt_ms, 0)), normal, MAX_INPUT_DELAY);
}
static_assert(ProfileInputDelay(0, 80) == 1 && ProfileInputDelay(0, 120) == 2 &&
              ProfileInputDelay(0, 150) == 3 && ProfileInputDelay(0, 200) == 4 &&
              ProfileInputDelay(0, 250) == 4 && ProfileInputDelay(0, 300) == 6);
static_assert(ProfileInputDelay(-1, 80) == 1 && ProfileInputDelay(-1, 150) == 1 &&
              ProfileInputDelay(-1, 151) == 2 && ProfileInputDelay(-1, 184) == 3 &&
              ProfileInputDelay(-1, 250) == 4 && ProfileInputDelay(-1, 251) == 5);
static_assert(ProfileInputDelay(1, 30) == 1 && ProfileInputDelay(1, 60) == 2 &&
              ProfileInputDelay(1, 100) == 3 && ProfileInputDelay(1, 150) == 4 &&
              ProfileInputDelay(1, 184) == 4 && ProfileInputDelay(1, 300) == 7);
static_assert(ProfileInputDelay(-1, -1) == 2 && ProfileInputDelay(1, -1) == 2);

// The game frame the rollback starts at. The match's first frames are the countdown, during which
// the game loads RNG-chosen resources (Pokemon, Assist Trophies) and each fighter's HUD portrait
// on a loader thread into heaps of the region set (the last at about game frame 95: the last
// fighter is created at frame 92 for any player count); rolling those heaps back under an
// in-flight load corrupts it. So from the first simulation frame (the barrier, where the RNG,
// frame counters and task order are copied) to here, every peer runs with the same fixed input
// (each player's buttons held when its machine reached the barrier, for start-of-match hotkeys:
// GameplaySession.cpp CountdownSlots) and no rollback; GekkoNet starts here behind a second
// barrier, and a file read still in flight is waited out before the base snapshot. The game
// reads input from GO on: game frame 214 (verified offline, P+ v3.2, a 1v1). Was 240 (after GO):
// inputs were thrown away for the first 26 frames of play and the start barrier's hitch (base snapshot, deterministic GPU thread, the
// barrier's round trip) came right after GO. At 120 that hitch falls in the countdown, 1.5 s
// before GO, and the first frame of play takes the player's input.
constexpr u32 DEFAULT_START_FRAME = 120;

struct SyncTestOptions
{
  int distance = 2;                 // frames rolled back on every frame (1..MAX_ROLLBACK_FRAMES)
  std::string region_set = "gp-v23";  // Sys/Rollback/<name>.json or a path
  bool hash_regions = true;          // also compare a hash of the whole region set per frame
  u32 start_frame = DEFAULT_START_FRAME;  // game frame the rollback starts at
  u32 ports = 3;                     // local controller ports that play (bit per port)
  bool suppress_resim_sounds = false;  // see ConnectOptions
  bool dedupe_resim_sounds = false;    // see ConnectOptions
  // The driven ports' input comes from the harness injection table (Gprb::PadsInjectAdd, keyed by
  // game frame) instead of the live controllers, so that runs from the same savestate get the
  // same input frame for frame.
  bool inject_input = false;
  // Misprediction sync test (needs inject_input): the first run of a frame gives these ports the
  // injected input of game frame + mispredict_offset instead of their own, as a peer that
  // predicted wrongly would; the resimulations (distance >= 2) run the true input. Comparing the
  // final per-frame trace with a run without misprediction shows state that a mispredicted pass
  // leaves behind outside the region set. Only frames with frame % mispredict_every == 0.
  u32 mispredict_ports = 0;
  int mispredict_offset = -7;
  int mispredict_every = 1;
  // Replay (diagnostics): instead of a GekkoNet stress session, run the passes a network session
  // recorded with PPR_GPRB_PASS_LOG (loads, frames, every pass's input), from this file.
  std::string replay_path;
  // Ground truth (diagnostics): the stress session runs, but every update only simulates its
  // newest frame, without loading: each frame runs once, as without rollback.
  bool no_rollback = false;
};

struct ConnectOptions
{
  bool host = false;  // the decider: sends the sync block and decides each game's setup
  u16 local_port = 0;        // UDP port to bind (0 = any)
  // Two players (local_slot -1): the joiner needs the host's address. The host may know the
  // joiner's address in advance (matchmaking); it then sends from the start, and the joiner's
  // first packet confirms or corrects the address. The host plays P1, the joiner P2.
  std::string remote_host;
  u16 remote_port = 0;
  // 2-4 players (local_slot 0-3): this player's in-game port, and every other player with its
  // in-game port. Every pair of players exchanges packets directly (full mesh). A peer's address
  // may be unknown (empty host): it is learned from that peer's first control message claiming
  // its port; with a known IP but port 0, only that IP may claim it. Ports may have gaps (P1+P3).
  int local_slot = -1;
  struct Peer
  {
    int slot = -1;
    std::string host;
    u16 port = 0;
  };
  std::vector<Peer> peers;
  // The deciding player's port as the joiners know it (-1: learned from the peers' messages).
  int host_slot = -1;
  std::string region_set = "gp-v23";
  // The game frame the rollback starts at (DEFAULT_START_FRAME). Every peer must use the same:
  // it is part of the setup hash the first barrier compares.
  u32 start_frame = DEFAULT_START_FRAME;
  // This player's input delay in frames (0-MAX_INPUT_DELAY): the harness's, or the player's set
  // number. Unset: the profile, per game (ProfileInputDelay).
  std::optional<int> delay;
  // The player's profile: -1 Aggressive, 0 Normal, 1 Smooth (DelayLevel).
  int delay_level = 0;
  int local_pad = 0;         // local controller port the local player uses, when the game's
                             // lock-in names none (LockIn::local_pad)
  bool sync_task_order = true;
  bool hash_regions = false;  // per-frame region hashes (diagnostics; costs a few ms per frame)
  // Sound effects started again while a rolled-back frame is resimulated play on top of the ones
  // the frame's first run started (the sound system is not rolled back). When set, resimulated
  // passes get "no channel" from the sound archive player instead (Brawlback's resim sound hooks),
  // so nothing plays twice; sounds that only a corrected frame would start are lost, as on Slippi.
  bool suppress_resim_sounds = false;
  // Slippi-style bookkeeping (the default): a resimulated pass does not start a sound (by sound
  // id) that an earlier run of the same frame started and that still plays; the game's handle is
  // re-attached to it. Sounds only the corrected input causes play; sounds only the discarded
  // run started are stopped.
  bool dedupe_resim_sounds = true;
  std::string name;
  // The stages of the match (the server's `stages` list for the mode, Slippi's allowed stages;
  // empty: P+ v3.2's legal list, DefaultStages()): the host draws every random stage from it
  // without repeats until it is used up (Slippi's stage pool). A stage pick (Direct's loser) is
  // not checked against it: Slippi does not restrict Direct's stage select ...
  std::vector<u16> stages;
  // ... unless set (a room whose loser picks from the list): a pick must be one of `stages` (else
  // the stage is drawn as without a pick).
  bool pick_from_stages = false;
  // A pick plays P+'s alternate stage its held L/R/Z selects (the server's `alternateStages`:
  // Direct, Teams, a room whose ruleset has them on); else the stage's normal layout (asl 0).
  bool alternate_stages = false;
  // The room's Teams switch (docs/nplayer/setup.md): 3-4 players play a team battle with each
  // player's lock-in team (Online::GameSetup::DecideTeams); with 2 players it has no effect.
  bool teams = false;
  // With `teams`: P+'s plain team battle, every player in their team's costume (the room's team
  // colours, 2026-10-21); else its rainbow team battle (any costume, a team glow).
  bool team_colors = false;
  // Ports (bits) that pick the first game's stage: a room's game is a session of its own, and the
  // last room game's loser picks (docs/rooms-game-interface.md); every member carries the same.
  u8 initial_pickers = 0;
  // Who plays (matchmaking's ticket), for the replays of this session's games (ReplayRecorder.h).
  Replay::MatchMeta replay_meta;
};

// ---- The lobby: the online character select between matches (Slippi's MATCH_SELECTIONS). ----
// Each game (the game plugin, through GameBridge) reports its player's lock-in; the peers exchange
// them in their control messages. Once every player is locked in for the next game, the host
// decides the match setup (stage: the losing player's pick, else random from `stages`) and sends
// it; every game then starts that match from its own character select. A session has 2 to
// MAX_LOBBY_PLAYERS players, by in-game port (gaps allowed).
constexpr int MAX_LOBBY_PLAYERS = 4;
// A lock-in's team (gmPlayerInitData::m_teamNo: 0 red, 1 blue, 2 green; Brawl's three team
// colours, Online::GameSetup::NUM_TEAMS), or none (free-for-all).
constexpr u8 NO_TEAM = 0xFF;
constexpr u8 MAX_TEAM = 2;

// The gone flags (docs/nplayer/session.md, "The gone flag"): `u8 gone[4]` by in-game port at this
// offset into the game's PPOM SESSION block (game-code/PPOnline/include/ppom.h). In a 3-4 player
// network match, the loop top of every pass writes 1 for a port whose player has left the match
// (from the agreed session frame on) and 0 otherwise. SESSION lives in the plugin's .data, inside
// the region set, so the flags are rolled back with the game.
constexpr u32 GONE_FLAG_OFFSET = 0x20C;
constexpr u16 NO_STAGE = 0xFFFF;
// LockIn::stage_pick of the player who picks the next stage (Direct's loser): locked in with the
// character, and the stage select still to come; the setup waits for the pick (MaybeDecideSetup).
// The game's PPOM::STAGE_PENDING.
constexpr u16 STAGE_PENDING = 0xFFFE;

// A player's port values (design 5.1): their name tag and its controls, in the game's PPOM
// PortValues layout (game-code/PPOnline/include/ppom.h, 0x3C bytes). Opaque to the session: it
// carries them from each player's lock-in into the match setup, so every machine applies the
// same controls to that player's port.
constexpr size_t PORT_VALUES_SIZE = 0x3C;
using PortValues = std::array<u8, PORT_VALUES_SIZE>;

struct LockIn
{
  bool ready = false;     // locked in for `game`
  u8 css = 0xFF;          // CSS id (diagnostics)
  u8 char_kind = 0xFF;    // gmCharacterKind (random already resolved)
  u8 costume = 0;         // colour number
  u16 stage_pick = NO_STAGE;  // a stage picked on the stage select (Direct: the loser's pick),
                              // or STAGE_PENDING
  u8 asl = 0;             // P+ alternate-stage buttons of that pick
  u32 game = 0;           // the game (1-based) this lock-in is for
  PortValues port_values{};  // the player's name tag and controls (all zero: the defaults)
  u8 team = NO_TEAM;      // the player's team colour for a team battle (0-2), NO_TEAM none
  // The controller port (0-3) whose START locked in; the match plays the local player from it.
  // Local only (not sent to the peer). Anything else: none (ConnectOptions::local_pad).
  u8 local_pad = 0xFF;
};
void SetLocalLock(const LockIn& lock);

struct LobbyPlayer
{
  bool present = false;
  u8 char_kind = 0xFF;
  u8 costume = 0;
  PortValues port_values{};
  u8 team = NO_TEAM;  // in a team battle (MatchSetup / Lobby `teams`), else NO_TEAM
};

// A game that starts again after a desync (SetDesyncRecovery; docs/game-code.md, desync
// recovery): every player's stocks and damage and the time left at the last session frame whose
// checksums all machines compared equal. The game starts as a fresh one (spawn points, the
// countdown) with these. Not `valid`: no frame all machines agreed on was found, and the game
// starts from the beginning.
constexpr u8 MAX_RESTORE_STOCKS = 99;
constexpr float MAX_RESTORE_DAMAGE = 999.0f;
// The longest time limit a game can have (P+'s 99 minutes), in frames.
constexpr u32 MAX_RESTORE_FRAMES = 99 * 60 * 60;
struct Restore
{
  bool valid = false;
  s64 frame = -1;  // the agreed session frame
  u32 checksum = 0;  // its checksum (every machine's)
  std::array<u8, MAX_LOBBY_PLAYERS> stocks{};   // by in-game port; 0: out of the game
  std::array<float, MAX_LOBBY_PLAYERS> damage{};
  u32 frames_left = 0;  // the match timer then (0: the match has no time limit)
  bool operator==(const Restore&) const = default;
};
// Whether a desync restarts the game (Ranked and a room's set, the user's, 2026-10-13) rather
// than only ending it (DESYNC DETECTED, no result: Unranked, Direct, a room's game without a set).
// Every machine of a session sets the same.
void SetDesyncRecovery(bool on);

struct Lobby
{
  bool active = false;        // a network session exists (connecting .. running)
  bool connected = false;     // every other player has been heard and none has left
  bool in_match = false;      // barrier .. running
  bool disconnected = false;  // the session ended because the other players left or went silent
  bool desynced = false;      // the last match ended on a desync, until its scene is left
  // This player left a 3-4 player match with the pause screen's quit: its session ended, the
  // others play on (until the next session or match)
  bool quit_left = false;
  int local_port = -1;        // in-game port of this player (2 players: 0 host, 1 joiner)
  int num_players = 0;        // players in the session (this one included)
  bool remote_ready = false;  // every remote player is locked in for `game`
  u32 game = 0;               // the next game (1-based)
  bool setup_ready = false;   // the setup of `game` is decided
  u16 stage = NO_STAGE;
  u8 asl = 0;
  std::array<LobbyPlayer, MAX_LOBBY_PLAYERS> players{};  // by in-game port
  u8 last_winner = 0xFF;      // in-game port of the last game's winner, 0xFE draw, 0xFF none
  bool teams = false;         // the next game is a team battle (players[i].team)
  bool team_colors = false;   // ... in the teams' costumes (else the rainbow team battle)
  u8 stage_pickers = 0;       // ports (bits) that pick the next stage (GameSetup::Outcome)
  u8 setup_error = 0;         // GameSetup::SetupError: why the next game is not set up
  bool after_match = false;   // a match ended and the game has not left its scene yet
  Restore restore;            // the next game restarts a desynced one (with setup_ready)
  // The session gave up connecting: these players (in-game ports as bits) were never heard from
  // (Online::PEER_NEVER_HEARD_TIMEOUT_MS); kept until the next session.
  u8 unheard = 0;
};
Lobby GetLobby();

// A game of a network session that ended with GAME SET or with a pause-screen quit that ended it
// for everyone, read from the state every peer ended on (Online/Ranked.h reports it), or that a
// desync ended where the game starts again (SetDesyncRecovery). Ports are in-game ports
// (2 players: the host is 0, the joiner 1).
enum class GameEnd : u8
{
  GameSet,
  Quit,     // L+R+A+START: Ranked (SetQuitLoses) the quitter lost it, else it did not count
  Runback,  // the same with L+R+Y held (P+'s salty runback): unless a quit loses, played again
  Desync,   // no result: the next game plays it again from the agreed state (game, stage only)
};
struct GameResult
{
  GameEnd end = GameEnd::GameSet;
  u8 quitter = 0xFF;      // in-game port of the player who quit (Quit, Runback)
  u32 game = 0;           // 1-based, counting draws
  u8 winner = 0xFF;       // in-game port, 0xFE draw; teams: the lowest port of the winning team
  // Ports (a bit each) that pick the next game's stage: a 1v1's loser (both after a draw); 3-4
  // players: Online::GameSetup::DecideOutcome (last place, by the elimination order too).
  u8 pickers = 0;
  // 3-4 players: each port's place (1 = first), 0 = not playing (DecideOutcome).
  std::array<u8, MAX_LOBBY_PLAYERS> place{};
  int num_players = 0;
  std::array<bool, MAX_LOBBY_PLAYERS> present{};
  std::array<s32, MAX_LOBBY_PLAYERS> stocks{};
  std::array<float, MAX_LOBBY_PLAYERS> damage{};
  std::array<u8, MAX_LOBBY_PLAYERS> char_kind{};
  std::array<u8, MAX_LOBBY_PLAYERS> team{};
  u16 stage = NO_STAGE;
  u8 asl = 0;      // P+ alternate-stage buttons it was played with
  u32 frames = 0;  // game frames the match ran
};
// Called on the CPU thread for every such game; must not block. nullptr removes it.
void SetGameResultCallback(std::function<void(const GameResult&)> callback);
// Whether a pause-screen quit loses the game for the quitter (Ranked) rather than not counting.
// When it does not, a runback (the quit with L+R+Y) sets the next game up as the same match on
// every machine, and the game goes straight into it.
void SetQuitLoses(bool loses);
// The player's input delay setting changed while Dolphin runs (any thread; the launcher's
// input-delay.json, Online::Rooms): a profile (DELAY_NORMAL, DELAY_AGGRESSIVE, DELAY_SMOOTH) or
// 1-MAX_INPUT_DELAY frames. A running game takes it from its next frame, and so does every later
// game, over an option the session started with.
void SetInputDelay(int setting);
// This player's input delay in the running game (any thread), -1 when none runs.
int CurrentInputDelay();

// A small JSON object of another module (Online/GameSetup.h: Ranked's stage strikes) carried in
// this player's control messages, and the peer's latest one ({} before any). Sizes are capped.
// With more than two players, "the peer" is the other player with the lowest in-game port.
void SetLocalExtra(const picojson::object& extra);
picojson::object GetPeerExtra();
// The extra of the player on in-game `port` ({} for this player, an empty port or one not heard).
picojson::object GetPlayerExtra(int port);
// The peer's lock-in as last heard (its character choice between games).
LockIn GetPeerLock();
// The lock-in of the player on in-game `port` (this player's own too), as last heard.
std::optional<LockIn> GetPlayerLock(int port);
// This player's lock-in as the game last wrote it.
LockIn GetLocalLock();

// Ranked: who decides the stage of a game instead of the picks / the random draw. `wait` holds
// the setup back (the stage is not decided yet); otherwise `stage`/`asl` is the stage. The host
// uses it; the joiner checks the host's setup against it. nullptr removes it.
struct StageDecision
{
  bool applies = false;  // false: the usual rule (picks, then random)
  bool wait = false;
  u16 stage = NO_STAGE;
  u8 asl = 0;
};
void SetStageDecider(std::function<StageDecision(u32 game)> decider);
// P+ v3.2's legal stages: its random-stage switch "Default" preset (pf/stage/switch/Switch00.rss,
// identical to the netplay SwitchFF.rss), as srStageKind values.
const std::vector<u16>& DefaultStages();

// ---- Spectating (Online/Spectate.h) ----
// What the spectate uploader backs off on: counters since the process started and the latest
// averages of the running network session, published on the CPU thread with relaxed atomics (never
// a lock) and read on the Rooms thread.
struct LinkHealth
{
  bool running = false;     // a network session runs a match
  u64 late_frames = 0;      // displayed frames that ran over their schedule
  double rollback_avg = 0;  // rollback depth per displayed frame, averaged over about 2 s
  double ping_ms = -1;      // the worst link's average ping, refreshed every second
};
LinkHealth GetLinkHealth();

// ---- Playback (docs/design/replays.md §6) ----
// A replay plays as a joiner whose host is the file: the game starts the replay's match from the
// SESSION GameBridge writes (PPOM v8's playback request), the barrier checks the match is the one
// recorded and applies the recorded host's values, and the recorded inputs run every frame once.
// `play_anyway`: a replay from another simulation version, or whose setup differs, plays anyway
// (its checksums then show where it stops matching).
// `hold_at_end`: the last replay asked for holds at its last frame (paused, still seekable)
// instead of ending its match.
// `live`: spectating (docs/design/spectating.md §6.2, `mirror`): the file is still growing.
// ExtendPlayback adds what arrives; playback holds with "Buffering" when the frames run out, and
// ends only once the game's ENDG has come. A live file may have no frames yet (only its setup): its
// match loads, and the barrier waits for its first frames. It plays from the game's start at
// normal speed, as far behind the players as the stream is (a few seconds: the upload trickles
// during a game's first 10 s), unless `catch_up` and the file has more than
// LIVE_JOIN_CATCH_UP_FRAMES at the barrier: a spectator who joined a game well under way runs to
// the live edge (the newest frame less a 3 s buffer) first. Go Live, the seek bar and
// fast-forward reach the live edge any time.
// `room_watch`: a room's game watched from the room CSS (Online/RoomWatch.h): GameBridge keeps the
// room in LOCAL and SESSION and marks the replay's SESSION RF_WATCH instead of asking the game for a
// playback; it is a live playback otherwise.
constexpr size_t LIVE_JOIN_CATCH_UP_FRAMES = 20 * 60;
std::optional<std::string> ArmPlayback(std::shared_ptr<const Replay::File> file, bool play_anyway,
                                       bool hold_at_end = false, bool live = false,
                                       bool catch_up = false, bool room_watch = false);
// A live playback's file as read again: its frames past those already known are added, and its
// ENDG once there. `hold_at_end` replaces the armed value (a set's next game arrived: the game
// ends its match and the next one plays). `cut`: the stream says the rest of the game won't come
// (its uploader stopped): what is there is all of it. The file must be the armed one, grown.
void ExtendPlayback(std::shared_ptr<const Replay::File> file, bool hold_at_end, bool cut = false);
// A line the overlay shows above the bar (spectating: "Game 3 starts soon", the close countdown).
void SetPlaybackBanner(std::string text);
// Highlights (the launcher's stats page, ReplayPlayback.h `highlights`): plays frames `start` to
// `end` of the armed replay. A seek to `start` (a fast-forward, or a keyframe put back when it is
// behind; while the match is still loading, it runs once frames do), then play, paused at `end`
// (PlaybackStatus' `clip_done`; a clip past the replay's last frame holds there, `at_end`). While
// the fast-forward runs the game is muted and every CLIP_SHOW_EVERY-th frame is presented, so the
// screen visibly moves and the progress line shows. `record`: the clip's frames, from its first
// to the pause, are a segment of the video being rendered (ClipRecorder.h). false: no replay is
// armed.
bool SetPlaybackClip(s64 start, s64 end, bool record = false);
// Ends a playback (its match ends at the next loop top, and the game then leaves it).
void StopPlayback();
// A stopped playback's game is still in its match: the next replay arms once this is false
// (ArmPlayback refuses until then).
bool PlaybackStopping();
// What GameBridge gives the game while a replay is armed or plays.
struct PlaybackView
{
  bool active = false;
  u32 serial = 0;              // a new replay to start
  std::vector<u8> session;     // the recorded SESSION block (GameBridge checks every field)
  int local_port = -1;         // the recorder's in-game port
  bool in_match = false;       // the barrier passed and the replay's last frame has not run
  bool disconnected = false;   // after the last frame: the live game ended with DISCONNECTED
  bool desynced = false;       // ... with DESYNC DETECTED
  bool quit_left = false;      // ... with this player's quit of a 3-4 player match
  bool room_watch = false;     // ArmPlayback's `room_watch`
  bool starting = false;       // armed, its match not started yet (the game is on its way to it)
  bool stopping = false;       // PlaybackStopping
};
PlaybackView GetPlaybackView();
// For the harness and the launcher: phase, frame, last frame, desync, error.
picojson::object PlaybackStatus();

// Playback controls (docs/design/replays.md §6.4; the replay hotkeys, the overlay's seek bar, the
// harness). They act at the next loop top, between two frames. Frames are the replay's (session
// frames, 60 a second): `Seek` shows that frame, `Jump` moves by `arg` seconds, the steps by one
// frame (pausing), `Speed` sets the speed (0.25-2), `Faster`/`Slower` step through 1/4, 1/2, 3/4,
// 1, 1.5, 2.
enum class PlaybackCommand
{
  TogglePause,
  Pause,
  Play,
  StepForward,
  StepBack,
  Jump,
  Seek,
  Speed,
  Faster,
  Slower,
  GoLive,  // spectating: catch up to the live edge and play live
};
void PlaybackControl(PlaybackCommand command, double arg = 0);
// What the overlay shows (the playback's view, any thread).
struct PlaybackInfo
{
  bool active = false;     // a replay's match runs (or holds at its end)
  s64 first = 0, last = 0;  // its frames
  s64 shown = 0;           // the frame on screen
  bool paused = false;
  bool at_end = false;     // held at its last frame
  bool seeking = false;
  double speed = 1.0;
  s64 desync_frame = -1;
  std::string title;       // "Direct · Game 2"
  std::array<std::string, MAX_LOBBY_PLAYERS> names{};  // "" for an empty port
  std::array<u8, MAX_LOBBY_PLAYERS> chars{};
  size_t keyframes = 0;
  // Spectating (a live playback).
  bool live = false;
  bool ended = false;        // the game's end arrived: no live edge anymore
  s64 live_edge = 0;         // the newest frame less the buffer
  bool at_live = false;      // playing at the live edge
  bool buffering = false;    // no frames to play yet
  bool catching_up = false;  // running to the live edge
  double catch_rate = 0;     // frames run per real frame while catching up (0: not measured yet)
  s64 catch_from = -1;       // the frame the catch-up started from (-1: not running yet)
  std::string banner;
};
PlaybackInfo GetPlaybackInfo();

// Arms a sync test that starts at the next match's first simulation frame.
std::optional<std::string> ArmSyncTest(const SyncTestOptions& options);
// Opens the UDP socket and starts connecting (joiner) or listening (host).
std::optional<std::string> Connect(const ConnectOptions& options);
// This player's selections (free-form JSON object, e.g. {"character": "fox", "costume": 0}); the
// host's may carry the stage and rules. Sent to the peer and shown in Status().
std::optional<std::string> SetSelections(const picojson::object& selections);
// Ends any online session and closes the socket (an armed or playing replay stays: StopPlayback).
void Stop();

picojson::value Status();
// Per-frame records of the running (or last) session, frame >= since: [frame, checksum,
// rng0, rng1, rng2, fighters crc, region hash (hex), game_set].
picojson::value Checksums(s64 since);

// CPU thread, from the Brawl game-loop hooks. Each returns true when it handled the hook (and set
// npc); false lets the hook do its default.
bool OnLoopTop(const Core::CPUThreadGuard& guard);
// CPU thread: a pass of the game's pad thread (early local input for a Wii controller).
void OnPadPass(Core::System& system, const std::array<u8, 8 * 0x40>& pads,
               std::chrono::steady_clock::time_point at);
bool OnFrameEnd(const Core::CPUThreadGuard& guard);
bool OnLoopEnd(const Core::CPUThreadGuard& guard);
// True during resimulated (not presented) iterations of a running session.
bool IsResimulationPass();
// True while a session drives the game loop (between the barrier and the end of the match).
bool IsRunning();
// True while the main loop must run exactly one logic step per pass (countdown, start barrier,
// running): see GprbPacingStepsHook.
bool DrivesLoop();
// Sound archive player allocation during a pass (HLE_Misc's sound alloc hooks, inside
// SoundArchivePlayer::detail_SetupSound with the game's SoundHandle `handle`).
enum class SoundAllocAction
{
  Proceed,    // allocate and start the sound as usual
  NoChannel,  // fail the allocation ("no channel"): the sound does not start
  Reattach,   // do not start it again: attach `handle` to `sound`, which an earlier run of the
              // same frame started and which is still playing, and report success
};
struct SoundAllocDecision
{
  SoundAllocAction action = SoundAllocAction::Proceed;
  u32 sound = 0;
};
SoundAllocDecision OnSoundAlloc(const Core::CPUThreadGuard& guard, u32 sound_id, u32 handle);
// detail_SetupSound attaches `sound` (id `sound_id`) to the game's `handle` (the success path).
void OnSoundAttached(const Core::CPUThreadGuard& guard, u32 handle, u32 sound, u32 sound_id);
// BasicSound::Stop of `sound`: true when the call must not stop it. In a resimulated pass, a sound
// that an earlier run of this frame started (and this pass has not started yet) or that a later
// frame started does not exist yet in the timeline being run again; Brawl's own sound layer
// (sndSystem's slot table, outside the set) still holds it, and the pass's "stop the sounds of
// this owner" would cut it off. If the corrected run does not start it again, it is stopped at
// the end of its frame like any mispredicted sound.
bool OnSoundStop(const Core::CPUThreadGuard& guard, u32 sound);
// The sound archive player's allocated sounds (verification of the sound bookkeeping): per sound
// its id and general handle, and whether a handle points back at it ("owned"); totals of active
// sounds, orphans (no handle owns them: nothing can stop them) and repeated ids.
picojson::value SoundState(const Core::CPUThreadGuard& guard);
// Diagnostics: region-set samples. Every `every` session frames (0 = off), the frame's newest save
// also records a hash of every 4 KiB chunk of the region set and the bytes of `watch` ranges. A
// frame saved again after a rollback overwrites its sample, so once a frame is confirmed its
// sample is the confirmed state's: two peers' samples can be compared to find where their region
// sets first differ. Configure before the match; the samples of the last match stay readable.
void ConfigureSamples(u32 every, std::vector<std::pair<u32, u32>> watch);
// {ranges: [[addr, size]...], frames: [...], digests: [...]} and, for `frames_full`, the chunk
// hashes and watch bytes (hex) of those frames.
picojson::value Samples(const std::vector<s64>& frames_full);
// One mtRand::generate call: kept for the desync log (recent frames), logged with PPR_GPRB_RNG_LOG.
void OnRngCall(u32 rng, const std::array<u32, 8>& callers, u32 state);
// Diagnostics (PPR_GPRB_PROBE): r3, r4, r12, ctr, lr at a probed address.
void OnProbe(u32 pc, const std::array<u32, 7>& regs);
// HLE hook at stMelee's constructor: a network match's setup and seeds are applied before the
// stage is built (it shuffles the fighters' start points with g_mtRand) if not done yet, and the
// RNGs are seeded again if they were.
void OnStageCreate(const Core::CPUThreadGuard& guard);
}  // namespace Gprb::Session
