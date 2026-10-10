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

#include <array>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Gprb::Session
{
// Input delay in frames. Each player picks the delay of their own inputs, as on Slippi (in every
// mode, ranked too): a set value, or automatic, from the round trip to the peer when the game's
// GekkoNet session starts: 2 frames below 70 ms, 3 below 150 ms, else 4. The other player's
// inputs then mostly arrive before they are needed, so rollbacks stay short, without adding delay
// on good connections. A round trip not measured yet: the default.
constexpr int DEFAULT_INPUT_DELAY = 2;
constexpr int MAX_INPUT_DELAY = 9;
constexpr int AutoInputDelay(double rtt_ms)
{
  if (rtt_ms < 0)
    return DEFAULT_INPUT_DELAY;
  return rtt_ms < 70 ? 2 : rtt_ms < 150 ? 3 : 4;
}
static_assert(AutoInputDelay(-1) == 2 && AutoInputDelay(69.9) == 2 && AutoInputDelay(70) == 3 &&
              AutoInputDelay(149.9) == 3 && AutoInputDelay(150) == 4 && AutoInputDelay(400) == 4);

struct SyncTestOptions
{
  int distance = 2;                 // frames rolled back on every frame (1..MAX_ROLLBACK_FRAMES)
  std::string region_set = "gp-v21";  // Sys/Rollback/<name>.json or a path
  bool hash_regions = true;          // also compare a hash of the whole region set per frame
  u32 start_frame = 240;             // game frame the rollback starts at (after GO, see Connect)
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
  std::string region_set = "gp-v21";
  // The match's first frames are the countdown, during which the game loads RNG-chosen resources
  // (Pokemon, Assist Trophies) on a loader thread into heaps of the region set. Rolling those
  // heaps back under an in-flight load corrupts it, and nobody can act before GO anyway. So from
  // the first simulation frame (the barrier, where the RNG, frame counters and task order are
  // copied) to `start_frame`, both peers run with neutral input and no rollback; GekkoNet starts
  // at `start_frame` behind a second barrier.
  u32 start_frame = 240;
  // This player's input delay in frames (0-MAX_INPUT_DELAY). Unset: automatic, per game
  // (AutoInputDelay).
  std::optional<int> delay;
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
  // not checked against it: Slippi does not restrict Direct's stage select.
  std::vector<u16> stages;
  // The room's Teams switch (docs/nplayer/setup.md): 3-4 players play a team battle with each
  // player's lock-in team (Online::GameSetup::DecideTeams); with 2 players it has no effect.
  bool teams = false;
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

struct Lobby
{
  bool active = false;        // a network session exists (connecting .. running)
  bool connected = false;     // every other player has been heard and none has left
  bool in_match = false;      // barrier .. running
  bool disconnected = false;  // the session ended because the other players left or went silent
  bool desynced = false;      // the last match ended on a desync, until its scene is left
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
  u8 stage_pickers = 0;       // ports (bits) that pick the next stage (GameSetup::Outcome)
  u8 setup_error = 0;         // GameSetup::SetupError: why the next game is not set up
};
Lobby GetLobby();

// A game of a network session that ended with GAME SET, read from the state every peer ended on
// (Online/Ranked.h reports it). Ports are in-game ports (2 players: the host is 0, the joiner 1).
struct GameResult
{
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
  u32 frames = 0;  // game frames the match ran
};
// Called on the CPU thread for every such game; must not block. nullptr removes it.
void SetGameResultCallback(std::function<void(const GameResult&)> callback);

// A small JSON object of another module (Online/GameSetup.h: Ranked's stage strikes) carried in
// this player's control messages, and the peer's latest one ({} before any). Sizes are capped.
// With more than two players, "the peer" is the other player with the lowest in-game port.
void SetLocalExtra(const picojson::object& extra);
picojson::object GetPeerExtra();
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

// Arms a sync test that starts at the next match's first simulation frame.
std::optional<std::string> ArmSyncTest(const SyncTestOptions& options);
// Opens the UDP socket and starts connecting (joiner) or listening (host).
std::optional<std::string> Connect(const ConnectOptions& options);
// This player's selections (free-form JSON object, e.g. {"character": "fox", "costume": 0}); the
// host's may carry the stage and rules. Sent to the peer and shown in Status().
std::optional<std::string> SetSelections(const picojson::object& selections);
// Ends any session and closes the socket.
void Stop();

picojson::value Status();
// Per-frame records of the running (or last) session, frame >= since: [frame, checksum,
// rng0, rng1, rng2, fighters crc, region hash (hex), game_set].
picojson::value Checksums(s64 since);

// CPU thread, from the Brawl game-loop hooks. Each returns true when it handled the hook (and set
// npc); false lets the hook do its default.
bool OnLoopTop(const Core::CPUThreadGuard& guard);
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
// Diagnostics (PPR_GPRB_RNG_LOG): one mtRand::generate call.
void OnRngCall(u32 rng, const std::array<u32, 8>& callers, u32 state);
// Diagnostics (PPR_GPRB_PROBE): r3, r4, r12, ctr, lr at a probed address.
void OnProbe(u32 pc, const std::array<u32, 7>& regs);
// HLE hook at stMelee's constructor: a network match's setup and seeds are applied before the
// stage is built (it shuffles the fighters' start points with g_mtRand) if not done yet, and the
// RNGs are seeded again if they were.
void OnStageCreate(const Core::CPUThreadGuard& guard);
}  // namespace Gprb::Session
