// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Gameplay-only (Slippi-style) rollback session for Brawl / Project+.
//
// Two players boot and use the menus independently. They connect while on the character select
// (no Dolphin netplay lobby, no shared boot), exchange their selections, and start the same match.
// The rollback session begins at the first frame of the match simulation, behind a barrier where
// the host's RNG state, frame counters and scheduler task order are copied to the joiner. During
// the match GekkoNet exchanges inputs, and rollbacks save and restore only the gameplay region set
// (Data/Sys/Rollback/<set>.json, see GameplayRollback.h) through RollbackManager's region mode.
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
struct SyncTestOptions
{
  int distance = 2;                 // frames rolled back on every frame (1..MAX_ROLLBACK_FRAMES)
  std::string region_set = "gp-v19";  // Sys/Rollback/<name>.json or a path
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
  bool host = false;
  u16 local_port = 0;        // UDP port to bind (0 = any)
  // Joiner: the host's address (required). Host: the joiner's address when it is known in
  // advance (matchmaking); the host then sends from the start, and the joiner's first packet
  // confirms or corrects the address.
  std::string remote_host;
  u16 remote_port = 0;
  std::string region_set = "gp-v19";
  // The match's first frames are the countdown, during which the game loads RNG-chosen resources
  // (Pokemon, Assist Trophies) on a loader thread into heaps of the region set. Rolling those
  // heaps back under an in-flight load corrupts it, and nobody can act before GO anyway. So from
  // the first simulation frame (the barrier, where the RNG, frame counters and task order are
  // copied) to `start_frame`, both peers run with neutral input and no rollback; GekkoNet starts
  // at `start_frame` behind a second barrier.
  u32 start_frame = 240;
  int delay = 2;             // input delay in frames
  int local_pad = 0;         // local controller port the local player uses
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
};

// ---- The lobby: the online character select between matches (Slippi's MATCH_SELECTIONS). ----
// Each game (the game plugin, through GameBridge) reports its player's lock-in; the peers exchange
// them in their control messages. Once every player is locked in for the next game, the host
// decides the match setup (stage: the losing player's pick, else random from `stages`) and sends
// it; both games then start that match from their own character select. Sized for up to
// MAX_LOBBY_PLAYERS so that 4-player sessions fit later; today a session has 2 players.
constexpr int MAX_LOBBY_PLAYERS = 4;
constexpr u16 NO_STAGE = 0xFFFF;

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
  u16 stage_pick = NO_STAGE;  // a stage picked on the stage select (Direct: the loser's pick)
  u8 asl = 0;             // P+ alternate-stage buttons of that pick
  u32 game = 0;           // the game (1-based) this lock-in is for
  PortValues port_values{};  // the player's name tag and controls (all zero: the defaults)
};
void SetLocalLock(const LockIn& lock);

struct LobbyPlayer
{
  bool present = false;
  u8 char_kind = 0xFF;
  u8 costume = 0;
  PortValues port_values{};
};

struct Lobby
{
  bool active = false;        // a network session exists (connecting .. running)
  bool connected = false;     // the peer has been heard and has not left
  bool in_match = false;      // barrier .. running
  bool disconnected = false;  // the session ended because the peer left or went silent
  int local_port = -1;        // in-game port of this player (0 host, 1 joiner)
  int num_players = 0;
  bool remote_ready = false;  // every remote player is locked in for `game`
  u32 game = 0;               // the next game (1-based)
  bool setup_ready = false;   // the setup of `game` is decided
  u16 stage = NO_STAGE;
  u8 asl = 0;
  std::array<LobbyPlayer, MAX_LOBBY_PLAYERS> players{};
  u8 last_winner = 0xFF;      // in-game port of the last game's winner, 0xFE draw, 0xFF none
};
Lobby GetLobby();

// A game of a network session that ended with GAME SET, read from the state both peers ended on
// (Online/Ranked.h reports it). Ports are in-game ports: the host is 0, the joiner 1.
struct GameResult
{
  u32 game = 0;           // 1-based, counting draws
  u8 winner = 0xFF;       // in-game port, 0xFE draw
  std::array<s32, 2> stocks{};
  std::array<float, 2> damage{};
  std::array<u8, 2> char_kind{};
  u16 stage = NO_STAGE;
  u32 frames = 0;  // game frames the match ran
};
// Called on the CPU thread for every such game; must not block. nullptr removes it.
void SetGameResultCallback(std::function<void(const GameResult&)> callback);

// A small JSON object of another module (Online/GameSetup.h: Ranked's stage strikes) carried in
// this player's control messages, and the peer's latest one ({} before any). Sizes are capped.
void SetLocalExtra(const picojson::object& extra);
picojson::object GetPeerExtra();
// The peer's lock-in as last heard (its character choice between games).
LockIn GetPeerLock();

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
