// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GameplaySession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/UdpSocket.hpp>
#include <fmt/format.h>
#include "Core/Online/Timeouts.h"
#include <xxhash.h>

#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/Brawlback/include/brawlback-common/BrawlbackConstants.h"
#include "Core/Brawlback/include/gekkonet/GekkoLib/include/gekkonet.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Online/GameBridge.h"
#include "Core/Online/GameSetup.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/Rollback/PeerData.h"
#include "Core/Rollback/PresentStats.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Gprb::Session
{
namespace
{
using Clock = std::chrono::steady_clock;

// Brawl's game loop (gfApplication::mainLoop), see HLE_Misc.cpp.
constexpr u32 LOOP_TOP = 0x800171b4;       // li r25,1 (BrawlbackGekkoNetUnconditionalFrame)
constexpr u32 FRAME_END = 0x80017504;      // stw r0,0x100(r23)
constexpr u32 LOOP_END = 0x80017508;       // b mainLoop top
constexpr u32 TASK_SCHEDULER_PTR = 0x805A0068;
constexpr u32 SERIAL_COUNTER_START = 0x10000;  // object serial numbers for the match (both peers)
constexpr int MAX_ADVANCE = 16;
constexpr u32 INPUT_SIZE = Addr::PAD_STRIDE;  // one gfPadStatus per player
constexpr int CHECKSUM_HISTORY = 1 << 15;
constexpr s64 PING_DISPLAY_INTERVAL = 60;  // frames (Slippi's SLIPPI_PING_DISPLAY_INTERVAL)
// The session ends this many frames after the first frame showing game set (or a pause-screen
// quit): past the deepest rollback (prediction window) plus the input delay, so the end itself can
// no longer be rolled back. That frame is the same on both peers, so both end on the same frame.
// Brawl tears the match down 4 frames after its own pause-screen quit; online the plugin keeps the
// game paused until the session has ended (docs/game-code.md, the pause-screen quit), so no
// rollback crosses the teardown.
constexpr s64 END_AFTER_GAME_SET = MAX_ROLLBACK_FRAMES + 12;
constexpr u32 REGION_CHUNK = 4096;

enum class Mode
{
  None,
  SyncTest,
  Network,
};

enum class Phase
{
  Idle,
  Connecting,  // network: joiner says hello / host waits for one
  Connected,   // network: peers know each other; menus, waiting for the match
  Armed,       // sync test: waiting for the match
  Barrier,     // network: at the first simulation frame, waiting for the peer
  Countdown,   // neutral input, no rollback, until start_frame
  StartBarrier,  // network: at start_frame, waiting for GekkoNet's handshake and the peer
  Running,
  Ended,
  Error,
};

const char* PhaseName(Phase p)
{
  switch (p)
  {
  case Phase::Idle:
    return "idle";
  case Phase::Connecting:
    return "connecting";
  case Phase::Connected:
    return "connected";
  case Phase::Armed:
    return "armed";
  case Phase::Barrier:
    return "barrier";
  case Phase::Countdown:
    return "countdown";
  case Phase::StartBarrier:
    return "start_barrier";
  case Phase::Running:
    return "running";
  case Phase::Ended:
    return "ended";
  case Phase::Error:
    return "error";
  }
  return "?";
}

struct SyncBlock
{
  std::array<u8, Addr::GAME_FRAME_SIZE> game_frame{};
  u32 app_counter = 0;
  std::array<u32, 3> rng{};
  u32 serial = 0;
  // The stage's fighter start points (stMelee +0x1B4, 4 words): the stage's constructor shuffles
  // them with g_mtRand during the load, before the barrier.
  std::array<u32, 4> start_pos{};
  bool has_start_pos = false;
  bool valid = false;
};

// The setup of one game, decided by the host (lobby, see GameplaySession.h).
struct MatchSetup
{
  u32 game = 0;  // 0: none
  u16 stage = NO_STAGE;
  u8 asl = 0;
  int num_players = 0;
  std::array<LobbyPlayer, MAX_LOBBY_PLAYERS> players{};
  bool teams = false;  // a team battle (players[i].team)
};

// One other player of the session, by in-game port (State::peers). Everything here that came
// from the network was checked on receipt (HandleControl, PeerData.h).
struct PeerView
{
  // The player is part of this session (configured, or the 2-player host's joiner).
  bool expected = false;
  // Where its packets come from. Until `confirmed`, the address is a guess (or unknown): the
  // first control message claiming this port from a matching address confirms it, and from then
  // on only that address counts.
  std::optional<sf::IpAddress> ip;
  u16 port = 0;
  bool confirmed = false;
  bool exact = false;  // only ip:port may claim it (the 2-player joiner's host, as before)
  // It left the session (a "leave" message, silence, or GekkoNet dropped it). In a match of 3-4
  // players the others play on (it is gone from the agreed frame on); otherwise the session ends.
  bool left = false;
  std::string left_reason;
  bool claims_host = false;  // its messages say it is the decider
  // In a match: its GekkoNet handle; it must still be disconnected in GekkoNet (CPU thread); the
  // first session frame its input carried the gone marker (-1: none).
  int handle = -1;
  bool drop_requested = false;
  bool gekko_dropped = false;
  s64 gone_frame = -1;
  bool seen = false;
  picojson::object extra;  // SetLocalExtra of the peer
  LockIn lock;
  MatchSetup setup;  // the host's setup (joiner)
  u8 last_winner = 0xFF;
  picojson::object selections;
  std::string name;
  bool at_s = false;
  bool applied = false;
  bool at_start = false;
  bool go = false;
  u64 setup_hash = 0;
  SyncBlock sync;
  u32 match_ports = 0;  // the host's match ports at the barrier (with its sync block)
  std::vector<std::vector<std::string>> task_order;
  bool task_order_valid = false;
  u32 session_seed = 0;
  u32 match_index = 0;
  // The host's gmGlobalModeMelee init block (stage, rules, stage variant) for match init_match.
  std::vector<u8> init_block;
  s64 init_match = -1;
  Clock::time_point last_heard{};
  double rtt_ms = -1;
};

struct FrameRecord
{
  s64 frame = -1;
  u32 checksum = 0;
  std::array<u32, 3> rng{};
  u32 fighters_crc = 0;
  u64 region_hash = 0;
  u32 game_set = 0;
  u32 game_frame = 0;
};

struct PendingOps
{
  int adv_count = 0;
  std::array<int, MAX_ADVANCE> adv_frame{};
  std::array<bool, MAX_ADVANCE> save_after{};
  std::array<unsigned int*, MAX_ADVANCE> checksum_ptr{};
  std::array<PadSlots, MAX_ADVANCE> slots{};
  void Clear()
  {
    adv_count = 0;
    save_after.fill(false);
    checksum_ptr.fill(nullptr);
  }
};

struct State
{
  std::recursive_timed_mutex mutex;
  // Where the CPU thread is (lock-free, for Status() when the mutex is busy).
  std::atomic<const char*> cpu_where{""};
  std::atomic<s64> cpu_frame{-1};
  Mode mode = Mode::None;
  Phase phase = Phase::Idle;
  std::string error;
  SyncTestOptions st_opts;
  ConnectOptions net_opts;

  // Network.
  std::unique_ptr<sf::UdpSocket> socket;
  // This player's in-game port, the deciding player's (-1: not known yet), and whether the
  // session was configured with ports (ConnectOptions::local_slot) rather than as a 1v1.
  int local_slot = 0;
  int host_slot = -1;
  bool ported = false;
  // Packets from an address that is not a peer's, dropped (status: foreign_packets).
  u64 foreign_packets = 0;
  // The session ends (fewer than two players left): handled on the CPU thread for a match.
  bool peer_left = false;
  std::string peer_left_reason;
  bool disconnected = false;  // reported to the game (Slippi's ONLINE_INPUTS result 3)
  // The two machines' game states differ (GekkoNet's checksums, or the peer's "desync"): the
  // CPU thread ends the match at its next loop top, as Slippi ends a game on a hard desync.
  bool desync_end = false;
  // The last match ended on a desync: reported to the game (LOCAL `desynced`, DESYNC DETECTED
  // and the end without "GAME!") until it has left the match scene.
  bool desynced = false;
  std::thread net_thread;
  std::atomic<bool> net_run{false};
  std::mutex gekko_rx_mutex;
  std::deque<std::pair<int, std::vector<u8>>> gekko_rx;  // (sender's port, packet)
  std::vector<GekkoNetResult*> gekko_rx_ptrs;
  std::array<PeerView, MAX_LOBBY_PLAYERS> peers;  // by in-game port (this player's unused)
  picojson::object local_selections;
  u32 session_seed = 0;  // host's, used to seed the RNG at the start of every match
  bool have_seed = false;
  u32 match_index = 0;
  // Lobby (GameplaySession.h): this player's lock-in, the setup of the next game, the last game.
  LockIn local_lock;
  MatchSetup setup;
  u8 last_winner = 0xFF;
  // Ports (bits) that pick the next game's stage (GameSetup::DecideOutcome; a 1v1: the loser,
  // both after a draw), and why the next game cannot be set up (GameSetup::SetupError).
  u8 last_pickers = 0;
  u8 setup_error = 0;
  u16 last_stage = NO_STAGE;
  // The ports of the running match (a bit per in-game port), from the game's own match setup at
  // the barrier, which every peer compares; GekkoNet's handles follow them in port order.
  u32 match_ports = 0;
  // 3-4 player matches: the gone flags' guest address (0: the game's PPOM block was not found),
  // and the flags last written.
  bool gone_enabled = false;
  u32 gone_addr = 0;
  std::array<u8, MAX_LOBBY_PLAYERS> gone_written{};
  // Host: the stages not drawn yet (Slippi's stage_pool, refilled from the match's list when empty).
  std::vector<u16> stage_pool;

  // Local barrier view (broadcast).
  bool at_s = false;
  bool applied = false;
  bool at_start = false;
  bool go = false;
  u64 setup_hash = 0;
  // This player's input delay for the current game, fixed when its GekkoNet session is created.
  int input_delay = DEFAULT_INPUT_DELAY;
  SyncBlock sync;  // host: the values the joiner copies
  std::vector<std::vector<std::string>> task_order;
  bool did_frame0 = false;
  int barrier_host = -1;  // the host's port when the barrier started
  Clock::time_point barrier_since{};
  std::optional<Clock::time_point> start_at;
  // Joiner: when it leaves the barrier for the countdown (RTT/2 after it applied the host's
  // values, when the host hears that it did), so both countdowns start together.
  std::optional<Clock::time_point> countdown_at;

  // GekkoNet.
  GekkoSession* gekko = nullptr;
  GekkoNetAdapter adapter{};
  bool gekko_started = false;
  // The GekkoNet update that saw the peer synced (during the countdown or at the start barrier)
  // may already return frame 0's events; they are kept here for the first RunFrame.
  bool ops_prefetched = false;
  PendingOps prefetched_ops;
  // During the countdown the handshake's frame 0 save is held back until the start barrier,
  // where the region set is in place: ProcessGekkoUpdate only records it.
  bool defer_initial_save = false;
  bool initial_save_deferred = false;
  unsigned int* deferred_initial_checksum = nullptr;
  // Start-of-match time sync (HardTimeSync): the last session frame it checked; the last frame
  // StartTelemetry logged.
  s64 last_hard_sync_frame = -1;
  s64 last_start_log_frame = -1;
  int num_players = 0;
  std::array<int, 4> handle_port{-1, -1, -1, -1};  // gekko handle -> in-game port
  std::vector<int> local_handles;
  int remote_handle = -1;  // the first remote player's (the ping line)
  // The local controller port the local player uses this game (fixed in CreateGekko).
  int local_pad = 0;

  // Per-frame CPU-thread state.
  PendingOps ops;
  int iteration = 0;
  bool resim_pass = false;
  bool spinning = false;  // this pass through the loop runs no game logic
  struct QueuedSave
  {
    bool pending = false;
    s64 frame = 0;
    unsigned int* checksum = nullptr;
  } queued_save;
  s64 current_frame = -1;
  std::array<u8, 4 * INPUT_SIZE> local_inputs{};
  double speed_factor = 1.0;
  u32 last_timesync_frame = UINT32_MAX;
  s64 next_ping_display_frame = PING_DISPLAY_INTERVAL;
  u32 last_game_frame = 0;

  // Region set of the running match.
  std::vector<Range> ranges;
  u64 region_bytes = 0;

  // Stats.
  u64 rollbacks = 0, max_rollback = 0, frames_resimulated = 0, stall_polls = 0, desyncs = 0;
  s64 last_desync_frame = -1;
  u64 region_mismatches = 0;
  std::vector<std::string> region_mismatch_log;
  u64 frames = 0;
  float frames_ahead = 0;
  Clock::time_point running_since{};
  // Network telemetry (NetStatsOnDisplayedFrame): one window of NET_STATS_WINDOW displayed frames,
  // logged and folded into the session totals.
  struct NetStats
  {
    u64 frames = 0, rollbacks = 0, resim = 0, max_depth = 0;
    u64 bursts = 0;  // rollback bursts (a load and its re-run)
    double burst_ms = 0, burst_ms_max = 0;
    // Displayed frames that ran over their schedule by more than LATE_MS (the screen sees them
    // late), and those of them that followed a rollback.
    u64 late = 0, late_after_rollback = 0;
    double late_ms_max = 0;
    u64 stalls = 0;  // updates with no frame to run (waited for the peer)
    double stall_ms = 0, stall_ms_max = 0;
    double ahead_sum = 0, ahead_min = 1e9, ahead_max = -1e9;
    double speed_min = 1e9, speed_max = -1e9;
    double pad_age_sum = 0, pad_age_max = 0;
    u64 pad_age_n = 0;
    Rollback::PresentStats::Cadence cadence;  // presents, and hitches on a 59.94 Hz screen
  };
  NetStats net_window, net_total;
  s64 net_window_first = -1;
  bool prev_frame_rolled_back = false;
  Clock::time_point burst_t0{};
  bool burst_open = false;
  std::vector<FrameRecord> history = std::vector<FrameRecord>(CHECKSUM_HISTORY);
  std::vector<FrameRecord> first_runs = std::vector<FrameRecord>(64);
  std::vector<std::string> desync_log;
  // Sync test: per-frame region chunk hashes of the first run (ring) for localizing mismatches.
  std::map<s64, std::vector<u64>> region_chunks;
  u32 end_game_frame = 0;
  std::string end_reason;
  s64 game_set_frame = -1;  // first session frame that showed game set or a quit (-1: none)
  bool game_set_quit = false;  // that frame showed a pause-screen quit (no winner)
  // Set when a network session ends inside the match scene (game set): the next match is armed
  // only after the scene was left, so the old match's last frames are not taken for a new one.
  bool await_scene_exit = false;
  // Host: its init block for this match, sent until the next match (see InitBlock).
  std::vector<u8> init_block;
  s64 init_match = -1;
  u64 sound_allocs = 0, resim_sound_allocs = 0, suppressed_sound_allocs = 0;
  // Sound bookkeeping (dedupe_resim_sounds, Slippi-style). Every sound a pass starts is recorded
  // with the game's SoundHandle it was attached to. A frame's list is the one of its newest run,
  // indexed by frame % size (snd_tag says which frame a slot holds). When a frame runs again:
  // - a sound the earlier run started (same id) and that is still playing is not started again;
  //   the game's handle is attached to it (re-attach), so the game can still stop or change it;
  // - a sound only the corrected run starts plays;
  // - a sound the earlier run started and the corrected run does not is stopped (at the next
  //   loop top, through a guest call of its Stop).
  struct SndEntry
  {
    u32 id = 0;
    u32 sound = 0;   // nw4r::snd::detail::BasicSound*
    u32 handle = 0;  // nw4r::snd::SoundHandle* of the game object
    u64 gen = 0;     // which allocation of that sound object (pool objects are reused)
  };
  std::array<std::vector<SndEntry>, 64> snd_played;
  std::array<s64, 64> snd_tag{};
  std::vector<SndEntry> snd_remaining;  // this pass: earlier runs' sounds not yet matched
  std::vector<SndEntry> snd_new;        // this pass: the sounds it started or re-attached
  bool snd_pass_open = false;
  s64 snd_frame = -1;                   // the frame this pass simulates
  s64 snd_max_frame = -1;               // the newest frame simulated so far
  bool snd_first_run = true;
  std::map<u32, u64> snd_gen;  // sound object -> allocation count
  u32 snd_reattach = 0;        // the sound the current detail_SetupSound re-attaches
  u64 snd_reattach_gen = 0;
  u32 snd_player = 0;          // SoundArchivePlayer (from detail_SetupSound)
  std::deque<SndEntry> snd_to_stop;
  u64 sound_reattached = 0, sound_reattach_gone = 0, sound_stopped = 0, sound_stop_gone = 0;
  std::array<u64, 3> snd_gone_why{};  // diagnostics: reallocated, other id, not allocated
  u64 sound_moved = 0;               // re-attached from another handle (playSE table slots)
  // A guest call (BasicSound::Stop) made from the loop top: the registers to restore when it
  // returns there.
  bool gcall_active = false;
  struct GuestRegs
  {
    std::array<u32, 32> gpr{};
    std::array<PowerPC::PairedSingle, 32> ps{};
    std::array<u64, 8> cr{};
    u32 fpscr = 0;
    u8 xer_ca = 0, xer_so_ov = 0;
    u16 xer_stringctrl = 0;
    u32 lr = 0, ctr = 0;
  } gcall_saved;
  // Waiting for the file IO thread at the loop top (RunIoWait): the registers to restore, and
  // counters (loop tops that waited, retraces waited).
  bool io_wait_active = false;
  bool io_wait_update = false;  // the next guest call is the manager's update (else a retrace)
  int io_wait_n = 0;
  GuestRegs io_saved;
  u64 io_waits = 0, io_wait_retraces = 0, io_wait_timeouts = 0;
  // Diagnostics (PPR_GPRB_FORCE_FINAL): ports still to be given their Final Smash before the match
  // starts under rollback, and whether a guest call for it is running.
  u32 force_final_todo = 0;
  bool force_final_inited = false;  // read for the coming match (reset when a match starts)
  bool force_final_active = false;
  GuestRegs force_final_saved;
  u64 pass_serial = 0;  // counts session passes (diagnostics)
  u64 mispredicted_passes = 0;  // misprediction sync test: first runs given a wrong input
  // Pass log (PPR_GPRB_PASS_LOG=path): every GekkoNet update's load and passes, for replays.
  FILE* pass_log = nullptr;
  int last_load_back = 0;
  bool last_initial_save = false;
  // Replay (SyncTestOptions::replay_path).
  FILE* replay = nullptr;
  bool replay_started = false;

  // Diagnostics: region-set samples (ConfigureSamples).
  u32 sample_every = 0;
  std::vector<std::pair<u32, u32>> sample_watch;
  std::map<s64, std::vector<u64>> sample_chunks;
  std::map<s64, std::vector<u8>> sample_bytes;
  std::vector<Range> sample_ranges;  // the region set the samples were taken over
};

State s;

std::string NetStatsLine(const State::NetStats& w);
void NetStatsFold(State::NetStats& t, const State::NetStats& w);
}  // namespace

bool InMatchPhase();

namespace
{
// Sound bookkeeping (defined with OnSoundAlloc below).
bool RunSoundStops(const Core::CPUThreadGuard& guard);
void CommitSoundPass();
// File loads in a match (defined with RunSoundStops below).
bool RunIoWait(const Core::CPUThreadGuard& guard);
bool RunForceFinal(const Core::CPUThreadGuard& guard);

// ---------------------------------------------------------------------------------------------
// Guest helpers

Guest GuestOf(Core::System& system)
{
  return Guest(system.GetMemory());
}

void WriteU32(Core::System& system, u32 addr, u32 value)
{
  const u32 be = Common::swap32(value);
  system.GetMemory().CopyToEmu(addr, &be, 4);
}

constexpr u32 PAD_CONFIG = 0x805B7480;  // g_PadConfig (ipPadConfig, 0x1AC bytes)
constexpr u32 PAD_CONFIG_KEY_SIZE = 0xB9;

// gmGlobalModeMelee fields both peers must agree on: rules/stage block and per player the
// character, state, stocks and costume.
std::vector<u8> SetupKey(const Guest& g)
{
  std::vector<u8> key;
  const auto gg = g.Ptr32(Addr::GAME_GLOBAL_PTR);
  if (!gg)
    return key;
  const auto mm = g.Ptr32(*gg + Addr::GG_MODE_MELEE);
  if (!mm)
    return key;
  const u8* p = g.Ptr(*mm, Addr::MODE_MELEE_SIZE);
  if (!p)
    return key;
  key.insert(key.end(), p + 0x08, p + 0x08 + 0x20);  // init block: stage kind, rules, time limit
  for (u32 i = 0; i < 4; ++i)
  {
    const u8* pl = p + 0x98 + i * 0x5C;
    key.push_back(pl[0]);  // character
    key.push_back(pl[1]);  // state (human/cpu/none)
    key.push_back(pl[4]);  // stocks
    key.push_back(pl[5]);  // costume
    // The team, in team battles only (gmMeleeInitData::m_isTeams, init +0x0B): outside them the
    // byte may hold whatever the character select last left there.
    if (p[0x08 + 0x0B] != 0)
      key.push_back(pl[0x0B]);
  }
  // Each controller's layout (ipPadConfig, g_PadConfig 0x805B7480: GameCube pads 12 bytes each,
  // then player -> pad at +0xB5). Every player plays with their own name tag's controls, which
  // the game applies from SESSION at the match start (docs/game-code.md, per-player controls).
  if (const u8* cfg = g.Ptr(PAD_CONFIG, PAD_CONFIG_KEY_SIZE))
  {
    key.insert(key.end(), cfg, cfg + 0x30);
    key.insert(key.end(), cfg + 0xB5, cfg + 0xB9);
  }
  return key;
}

// gmGlobalModeMelee+0x08..+0x28: stage kind, rules, time limit and the stage variant. The stage
// select fills the variant from things that differ between machines (Smashville's lighting
// follows the console clock), so the joiner takes the host's block before the match loads.
constexpr u32 INIT_BLOCK_OFFSET = 0x08;
constexpr u32 INIT_BLOCK_SIZE = 0x20;

std::optional<u32> ModeMeleeAddr(const Guest& g)
{
  const auto gg = g.Ptr32(Addr::GAME_GLOBAL_PTR);
  if (!gg)
    return std::nullopt;
  return g.Ptr32(*gg + Addr::GG_MODE_MELEE);
}

std::vector<u8> InitBlock(const Guest& g)
{
  const auto mm = ModeMeleeAddr(g);
  if (!mm)
    return {};
  const u8* p = g.Ptr(*mm + INIT_BLOCK_OFFSET, INIT_BLOCK_SIZE);
  return p ? std::vector<u8>(p, p + INIT_BLOCK_SIZE) : std::vector<u8>{};
}

// The ports the match's setup (gmGlobalModeMelee) gives a human player (gmPlayerInitData state 0;
// 1 is a CPU, 3 none), and per port its team when the match is a team battle (else NO_TEAM).
// Every peer builds the same setup (the barrier compares it), so these are the same everywhere.
constexpr u32 MM_PLAYERS = 0x98;
constexpr u32 MM_PLAYER_SIZE = 0x5C;
constexpr u32 MM_IS_TEAMS = 0x08 + 0x0B;  // gmMeleeInitData::m_isTeams
constexpr u32 PI_STATE = 0x01;
constexpr u32 PI_TEAM = 0x0B;  // gmPlayerInitData::m_teamNo

u32 HumanPorts(const Guest& g)
{
  const auto mm = ModeMeleeAddr(g);
  const u8* p = mm ? g.Ptr(*mm, Addr::MODE_MELEE_SIZE) : nullptr;
  if (!p)
    return 0;
  u32 ports = 0;
  for (u32 port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if (p[MM_PLAYERS + port * MM_PLAYER_SIZE + PI_STATE] == 0)
      ports |= 1u << port;
  }
  return ports;
}

std::array<u8, MAX_LOBBY_PLAYERS> MatchTeams(const Guest& g)
{
  std::array<u8, MAX_LOBBY_PLAYERS> teams;
  teams.fill(NO_TEAM);
  const auto mm = ModeMeleeAddr(g);
  const u8* p = mm ? g.Ptr(*mm, Addr::MODE_MELEE_SIZE) : nullptr;
  if (!p || p[MM_IS_TEAMS] == 0)
    return teams;
  for (u32 port = 0; port < MAX_LOBBY_PLAYERS; ++port)
    teams[port] = p[MM_PLAYERS + port * MM_PLAYER_SIZE + PI_TEAM];
  return teams;
}

u64 SetupHash(const Guest& g)
{
  const auto key = SetupKey(g);
  return key.empty() ? 0 : XXH3_64bits(key.data(), key.size());
}

std::string Hex(const std::vector<u8>& v)
{
  std::string out;
  for (u8 c : v)
    out += fmt::format("{:02x}", c);
  return out;
}

// gfTaskScheduler lists (see docs: category lists +0x14 next +8 prev +4; group lists +0x58 next
// +0x10 prev +0xC; priority lists +0xF8 next +0x18 prev +0x14). Task name pointer at +0.
struct TaskListKind
{
  u32 head_base, prev_off, next_off, count;
};
constexpr std::array<TaskListKind, 3> TASK_LISTS{{{0x14, 4, 8, 17}, {0x58, 0xC, 0x10, 40},
                                                  {0xF8, 0x14, 0x18, 14}}};

std::vector<std::vector<std::pair<u32, std::string>>> ReadTaskLists(const Guest& g)
{
  std::vector<std::vector<std::pair<u32, std::string>>> out;
  const auto sched = g.Ptr32(TASK_SCHEDULER_PTR);
  for (const auto& kind : TASK_LISTS)
  {
    for (u32 i = 0; i < kind.count; ++i)
    {
      std::vector<std::pair<u32, std::string>> seq;
      if (sched)
      {
        auto t = g.Ptr32(*sched + kind.head_base + 4 * i);
        while (t && seq.size() < 4096)
        {
          std::string name;
          if (const auto np = g.Ptr32(*t))
            name = g.CStr(*np, 48).value_or("?");
          seq.emplace_back(*t, name);
          t = g.Ptr32(*t + kind.next_off);
        }
      }
      out.push_back(std::move(seq));
    }
  }
  return out;
}

std::vector<std::vector<std::string>> TaskOrderNames(const Guest& g)
{
  std::vector<std::vector<std::string>> out;
  for (const auto& seq : ReadTaskLists(g))
  {
    std::vector<std::string> names;
    for (const auto& [addr, name] : seq)
      names.push_back(name);
    out.push_back(std::move(names));
  }
  return out;
}

// Relinks the scheduler lists so their name order matches `want` (same-name tasks keep their
// relative order). Returns the number of lists changed, or -1 if the task sets differ.
int ApplyTaskOrder(Core::System& system, const std::vector<std::vector<std::string>>& want)
{
  const Guest g(system.GetMemory());
  const auto sched = g.Ptr32(TASK_SCHEDULER_PTR);
  if (!sched)
    return -1;
  const auto lists = ReadTaskLists(g);
  if (lists.size() != want.size())
    return -1;
  int changed = 0;
  size_t li = 0;
  for (const auto& kind : TASK_LISTS)
  {
    for (u32 i = 0; i < kind.count; ++i, ++li)
    {
      const auto& seq = lists[li];
      std::vector<std::string> names;
      for (const auto& e : seq)
        names.push_back(e.second);
      if (names == want[li])
        continue;
      std::vector<std::string> a = names, b = want[li];
      std::sort(a.begin(), a.end());
      std::sort(b.begin(), b.end());
      if (a != b)
        return -1;
      std::map<std::string, std::deque<u32>> pools;
      for (const auto& [addr, name] : seq)
        pools[name].push_back(addr);
      std::vector<u32> order;
      for (const auto& name : want[li])
      {
        order.push_back(pools[name].front());
        pools[name].pop_front();
      }
      WriteU32(system, *sched + kind.head_base + 4 * i, order.front());
      for (size_t k = 0; k < order.size(); ++k)
      {
        WriteU32(system, order[k] + kind.prev_off, k ? order[k - 1] : 0);
        WriteU32(system, order[k] + kind.next_off, k + 1 < order.size() ? order[k + 1] : 0);
      }
      ++changed;
    }
  }
  return changed;
}

// Stage* of the match (sora_melee .bss), and the fighter start point table in it (stMelee).
constexpr u32 STAGE_PTR = 0x80B8A428;
constexpr u32 STAGE_START_POS = 0x1B4;

std::optional<u32> StageStartPosAddr(const Guest& g)
{
  const auto stage = g.Ptr32(STAGE_PTR);
  if (!stage || !g.Ptr(*stage + STAGE_START_POS, 16))
    return std::nullopt;
  return *stage + STAGE_START_POS;
}

SyncBlock ReadSyncBlock(Core::System& system)
{
  const Guest g(system.GetMemory());
  SyncBlock b;
  if (const u8* p = g.Ptr(Addr::GAME_FRAME, Addr::GAME_FRAME_SIZE))
    std::memcpy(b.game_frame.data(), p, b.game_frame.size());
  if (const auto app = g.Ptr32(Addr::APPLICATION_PTR))
    b.app_counter = g.U32(*app + Addr::APP_FRAME_COUNTER_OFF).value_or(0);
  b.rng = {g.U32(Addr::MTRAND_DEFAULT_SEED).value_or(0), g.U32(Addr::MTRAND_OTHER_SEED).value_or(0),
           g.U32(Addr::LIBC_RAND_NEXT).value_or(0)};
  b.serial = g.U32(Addr::OBJECT_SERIAL_COUNTER).value_or(0);
  if (const auto sp = StageStartPosAddr(g))
  {
    for (u32 i = 0; i < 4; ++i)
      b.start_pos[i] = g.U32(*sp + 4 * i).value_or(0);
    b.has_start_pos = true;
  }
  b.valid = true;
  return b;
}

void WriteSyncBlock(Core::System& system, const SyncBlock& b)
{
  auto& memory = system.GetMemory();
  const Guest g(memory);
  // g_GameFrame: only its counters (+0x04 frameCounter, +0x14 persistentFrameCounter) and, when
  // it is a finite number, the frame delta (+0x0C) are taken from the host. The other words are
  // never the host's (they are not known to differ between machines).
  if (const u8* mine = g.Ptr(Addr::GAME_FRAME, Addr::GAME_FRAME_SIZE))
  {
    std::array<u8, Addr::GAME_FRAME_SIZE> frame;
    std::memcpy(frame.data(), mine, frame.size());
    for (u32 off : {0x04u, 0x14u})
      std::memcpy(frame.data() + off, b.game_frame.data() + off, 4);
    u32 delta_be;
    std::memcpy(&delta_be, b.game_frame.data() + 0x0C, 4);
    if (std::isfinite(std::bit_cast<float>(Common::swap32(delta_be))))
      std::memcpy(frame.data() + 0x0C, b.game_frame.data() + 0x0C, 4);
    for (u32 off : {0x00u, 0x08u, 0x10u})
    {
      if (std::memcmp(frame.data() + off, b.game_frame.data() + off, 4) != 0)
      {
        WARN_LOG_FMT(BRAWLBACK, "gprb: g_GameFrame+{:#x} differs from the host's (kept ours)",
                     off);
      }
    }
    memory.CopyToEmu(Addr::GAME_FRAME, frame.data(), frame.size());
  }
  if (const auto app = g.Ptr32(Addr::APPLICATION_PTR))
    WriteU32(system, *app + Addr::APP_FRAME_COUNTER_OFF, b.app_counter);
  WriteU32(system, Addr::MTRAND_DEFAULT_SEED, b.rng[0]);
  WriteU32(system, Addr::MTRAND_OTHER_SEED, b.rng[1]);
  WriteU32(system, Addr::LIBC_RAND_NEXT, b.rng[2]);
  WriteU32(system, Addr::OBJECT_SERIAL_COUNTER, b.serial);
  // The fighters enter after the barrier (game frames 2 and 92), at the start points of this
  // table: if the peers' loads shuffled it differently, P1 would start on the other side.
  if (const auto sp = StageStartPosAddr(g); sp && b.has_start_pos)
  {
    std::array<u32, 4> mine{};
    for (u32 i = 0; i < 4; ++i)
      mine[i] = g.U32(*sp + 4 * i).value_or(0);
    // The host's table must be a shuffle of ours (the stage indexes its start points with it).
    if (mine != b.start_pos && !PeerData::IsPermutation(mine, b.start_pos))
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb: the host's fighter start points are not ours shuffled; kept "
                              "ours ({} {} {} {} against {} {} {} {})",
                   mine[0], mine[1], mine[2], mine[3], b.start_pos[0], b.start_pos[1],
                   b.start_pos[2], b.start_pos[3]);
    }
    else if (mine != b.start_pos)
    {
      for (u32 i = 0; i < 4; ++i)
        WriteU32(system, *sp + 4 * i, b.start_pos[i]);
      WARN_LOG_FMT(BRAWLBACK, "gprb: joiner took the host's fighter start points ({} {} {} {} -> {} {} {} {})",
                   mine[0], mine[1], mine[2], mine[3], b.start_pos[0], b.start_pos[1],
                   b.start_pos[2], b.start_pos[3]);
    }
  }
}

// The RNG state every match starts from: derived from the host's session seed and the match index.
std::array<u32, 3> MatchSeeds(u32 session_seed, u32 match_index)
{
  std::seed_seq seq{session_seed, match_index, 0x50502b31u};
  std::array<u32, 3> out;
  seq.generate(out.begin(), out.end());
  out[0] &= 0x7fffffff;
  out[1] &= 0x7fffffff;
  return out;
}

u32 FrameChecksum(const Guest& g, FrameRecord* rec)
{
  const auto fighters = ReadFighterFields(g);
  const u32 game_frame = g.U32(Addr::GAME_FRAME + 4).value_or(0);
  rec->rng = {g.U32(Addr::MTRAND_DEFAULT_SEED).value_or(0), g.U32(Addr::MTRAND_OTHER_SEED).value_or(0),
              g.U32(Addr::LIBC_RAND_NEXT).value_or(0)};
  rec->fighters_crc = FightersCrc(fighters);
  rec->game_set = IsGameSet(g) ? 1 : 0;
  u32 crc = Common::StartCRC32();
  crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(&game_frame), 4);
  crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(rec->rng.data()), 12);
  crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(&rec->fighters_crc), 4);
  return crc;
}

// ---------------------------------------------------------------------------------------------
// Network

// ---- Peers (under s.mutex) ----

// Every other player of the session, by in-game port.
template <typename F>
void ForEachPeer(F&& f)
{
  for (int slot = 0; slot < MAX_LOBBY_PLAYERS; ++slot)
  {
    if (slot != s.local_slot && s.peers[slot].expected)
      f(slot, s.peers[slot]);
  }
}

// Players still in the session, this one included.
int ActivePlayers()
{
  int n = 1;
  ForEachPeer([&](int, const PeerView& p) { n += p.left ? 0 : 1; });
  return n;
}

// The other player with the lowest in-game port (the 1v1 peer), or -1.
int FirstPeer()
{
  int first = -1;
  ForEachPeer([&](int slot, const PeerView&) {
    if (first < 0)
      first = slot;
  });
  return first;
}

// The deciding player's port: the configured or announced host, or, once it has left, the player
// with the lowest port still in the session (every peer agrees once it has seen the leave). -1
// while a joiner has not heard who the host is.
int HostSlot()
{
  const int h = s.net_opts.host ? s.local_slot : s.host_slot;
  if (h < 0 || h == s.local_slot || !s.peers[h].left)
    return h;
  int lowest = s.local_slot;
  ForEachPeer([&](int slot, const PeerView& p) {
    if (!p.left && slot < lowest)
      lowest = slot;
  });
  return lowest;
}

bool IsHost()
{
  return s.mode == Mode::Network && HostSlot() == s.local_slot;
}

// The host's view (nullptr when this player is the host or the host is not known).
PeerView* HostPeer()
{
  const int h = HostSlot();
  return h >= 0 && h != s.local_slot ? &s.peers[h] : nullptr;
}

// The port whose confirmed address `from:from_port` is, or -1.
int SlotOf(const sf::IpAddress& from, u16 from_port)
{
  int found = -1;
  ForEachPeer([&](int slot, const PeerView& p) {
    if (p.confirmed && p.ip && *p.ip == from && p.port == from_port)
      found = slot;
  });
  return found;
}

void SendRaw(int slot, const std::vector<u8>& data)
{
  if (!s.socket || slot < 0 || slot >= MAX_LOBBY_PLAYERS)
    return;
  const PeerView& p = s.peers[slot];
  if (!p.expected || !p.ip || p.port == 0)
    return;
  (void)s.socket->send(data.data(), data.size(), *p.ip, p.port);
}

void SendToAll(const std::vector<u8>& data)
{
  ForEachPeer([&](int slot, const PeerView& p) {
    if (!p.left)
      SendRaw(slot, data);
  });
}

// GekkoNet addresses are the peers' in-game ports: "p0" .. "p3".
std::array<char[2], MAX_LOBBY_PLAYERS> s_gekko_addr{{{'p', '0'}, {'p', '1'}, {'p', '2'}, {'p', '3'}}};

int GekkoAddrSlot(const GekkoNetAddress* addr)
{
  if (!addr || addr->size != 2 || !addr->data)
    return -1;
  const char* d = static_cast<const char*>(addr->data);
  return d[0] == 'p' && d[1] >= '0' && d[1] < '0' + MAX_LOBBY_PLAYERS ? d[1] - '0' : -1;
}

// Tells the other players that this machine found the game states differ, so they end the match
// too even if their own check has not (yet) seen it. UDP: sent a few times; `m` keeps a late copy
// from ending the next match.
void SendDesync()
{
  if (!s.socket)
    return;
  const std::string text = fmt::format(R"({{"t":"desync","v":1,"m":{}}})", s.match_index);
  std::vector<u8> pkt(text.size() + 1);
  pkt[0] = 'C';
  std::memcpy(pkt.data() + 1, text.data(), text.size());
  for (int i = 0; i < 3; ++i)
    SendToAll(pkt);
}

void GekkoSend(GekkoNetAddress* addr, const char* data, int length)
{
  std::vector<u8> pkt(static_cast<size_t>(length) + 1);
  pkt[0] = 'G';
  std::memcpy(pkt.data() + 1, data, static_cast<size_t>(length));
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  SendRaw(GekkoAddrSlot(addr), pkt);
}

GekkoNetResult** GekkoReceive(int* length)
{
  std::lock_guard lk(s.gekko_rx_mutex);
  s.gekko_rx_ptrs.clear();
  while (!s.gekko_rx.empty())
  {
    auto& [slot, pkt] = s.gekko_rx.front();
    auto* r = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    r->data_len = static_cast<unsigned int>(pkt.size());
    r->data = std::malloc(pkt.size());
    std::memcpy(r->data, pkt.data(), pkt.size());
    r->addr.data = std::malloc(2);
    std::memcpy(r->addr.data, s_gekko_addr[slot], 2);
    r->addr.size = 2;
    s.gekko_rx_ptrs.push_back(r);
    s.gekko_rx.pop_front();
  }
  *length = static_cast<int>(s.gekko_rx_ptrs.size());
  return s.gekko_rx_ptrs.data();
}

void GekkoFree(void* p)
{
  std::free(p);
}

picojson::value SyncBlockJson(const SyncBlock& b)
{
  picojson::object o;
  std::string hex;
  for (u8 c : b.game_frame)
    hex += fmt::format("{:02x}", c);
  o["game_frame"] = picojson::value(hex);
  o["app"] = picojson::value(static_cast<double>(b.app_counter));
  picojson::array rng;
  for (u32 v : b.rng)
    rng.emplace_back(static_cast<double>(v));
  o["rng"] = picojson::value(rng);
  o["serial"] = picojson::value(static_cast<double>(b.serial));
  if (b.has_start_pos)
  {
    picojson::array sp;
    for (u32 v : b.start_pos)
      sp.emplace_back(static_cast<double>(v));
    o["start_pos"] = picojson::value(sp);
  }
  return picojson::value(o);
}

std::optional<SyncBlock> SyncBlockFromJson(const picojson::value& v)
{
  // From the host: every field is checked before use (PeerData.h). A malformed block is ignored
  // as a whole; the joiner then waits at the barrier until the session times out.
  if (!v.is<picojson::object>())
    return std::nullopt;
  const auto& o = v.get<picojson::object>();
  SyncBlock b;
  const auto gf = o.find("game_frame");
  if (gf == o.end() || !gf->second.is<std::string>())
    return std::nullopt;
  const auto frame = PeerData::ParseHex(gf->second.get<std::string>(), b.game_frame.size());
  if (!frame)
    return std::nullopt;
  std::copy(frame->begin(), frame->end(), b.game_frame.begin());
  auto num = [&](const char* k) -> std::optional<u32> {
    const auto it = o.find(k);
    if (it == o.end())
      return 0u;
    const auto n = PeerData::JsonUInt(&it->second, 0xFFFFFFFF);
    return n ? std::optional<u32>(static_cast<u32>(*n)) : std::nullopt;
  };
  auto words = [&](const char* k, auto& out) -> std::optional<bool> {
    const auto it = o.find(k);
    if (it == o.end())
      return false;
    if (!it->second.is<picojson::array>() ||
        it->second.get<picojson::array>().size() != out.size())
    {
      return std::nullopt;
    }
    const auto& a = it->second.get<picojson::array>();
    for (size_t i = 0; i < out.size(); ++i)
    {
      const auto n = PeerData::JsonUInt(&a[i], 0xFFFFFFFF);
      if (!n)
        return std::nullopt;
      out[i] = static_cast<u32>(*n);
    }
    return true;
  };
  const auto app = num("app");
  const auto serial = num("serial");
  const auto rng = words("rng", b.rng);
  const auto sp = words("start_pos", b.start_pos);
  if (!app || !serial || !rng || !sp)
    return std::nullopt;
  b.app_counter = *app;
  b.serial = *serial;
  b.has_start_pos = *sp;
  b.valid = true;
  return b;
}

// ---- Lobby ----

// A whole number in [0, max] under key `k`, else `def` (a missing or malformed field).
u64 JsonNum(const picojson::object& o, const char* k, u64 max, u64 def)
{
  const auto it = o.find(k);
  if (it == o.end())
    return def;
  return PeerData::JsonUInt(&it->second, max).value_or(def);
}

picojson::value LockJson(const LockIn& l)
{
  picojson::object o;
  o["ready"] = picojson::value(l.ready);
  o["css"] = picojson::value(static_cast<double>(l.css));
  o["kind"] = picojson::value(static_cast<double>(l.char_kind));
  o["costume"] = picojson::value(static_cast<double>(l.costume));
  o["stage"] = picojson::value(static_cast<double>(l.stage_pick));
  o["asl"] = picojson::value(static_cast<double>(l.asl));
  o["game"] = picojson::value(static_cast<double>(l.game));
  o["pv"] = picojson::value(Hex({l.port_values.begin(), l.port_values.end()}));
  o["team"] = picojson::value(static_cast<double>(l.team));
  return picojson::value(o);
}

// A team from the peer: 0-3 or none.
u8 TeamFromJson(const picojson::value* v)
{
  const auto t = PeerData::JsonUInt(v, 0xFF);
  return t && PeerData::ValidTeam(static_cast<u32>(*t)) ? static_cast<u8>(*t) : NO_TEAM;
}

// Port values from the peer: exactly PORT_VALUES_SIZE bytes of hex, else none (the defaults).
PortValues PortValuesFromHex(const picojson::value* v)
{
  PortValues pv{};
  if (!v || !v->is<std::string>())
    return pv;
  const auto bytes = PeerData::ParseHex(v->get<std::string>(), pv.size());
  if (!bytes)
    return pv;
  std::copy(bytes->begin(), bytes->end(), pv.begin());
  PeerData::SanitizePortValues(pv);
  return pv;
}

LockIn LockFromJson(const picojson::object& o)
{
  LockIn l;
  const auto r = o.find("ready");
  l.ready = r != o.end() && r->second.is<bool>() && r->second.get<bool>();
  l.css = static_cast<u8>(JsonNum(o, "css", 0xFF, 0xFF));
  l.char_kind = static_cast<u8>(JsonNum(o, "kind", 0xFF, 0xFF));
  l.costume = static_cast<u8>(JsonNum(o, "costume", 0xFF, 0));
  l.stage_pick = static_cast<u16>(JsonNum(o, "stage", 0xFFFF, NO_STAGE));
  l.asl = static_cast<u8>(JsonNum(o, "asl", 0xFF, 0));
  l.game = static_cast<u32>(JsonNum(o, "game", 0xFFFFFFFF, 0));
  const auto pv = o.find("pv");
  l.port_values = PortValuesFromHex(pv != o.end() ? &pv->second : nullptr);
  const auto team = o.find("team");
  l.team = TeamFromJson(team != o.end() ? &team->second : nullptr);
  // A character, costume or stage the game cannot produce is not a lock-in: the peer is not
  // ready, and nothing of it reaches SESSION (the game would index its tables with it).
  if (!PeerData::ValidCharKind(l.char_kind) || !PeerData::ValidCostume(l.costume))
  {
    if (l.ready)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb lobby: peer lock-in refused (char {:#x}, costume {})",
                   l.char_kind, l.costume);
    }
    l.ready = false;
    l.char_kind = 0xFF;
    l.costume = 0;
  }
  if (l.stage_pick != NO_STAGE && !PeerData::ValidStageKind(l.stage_pick))
  {
    WARN_LOG_FMT(BRAWLBACK, "gprb lobby: peer stage pick {:#x} refused", l.stage_pick);
    l.stage_pick = NO_STAGE;
    l.asl = 0;
  }
  return l;
}

picojson::value SetupJson(const MatchSetup& st)
{
  picojson::object o;
  o["game"] = picojson::value(static_cast<double>(st.game));
  o["stage"] = picojson::value(static_cast<double>(st.stage));
  o["asl"] = picojson::value(static_cast<double>(st.asl));
  // Per present player, by port: [char kind, costume, port values] when the players sit on ports
  // 0..n-1 without teams (a 1v1, as before), else [char kind, costume, port values, team, port].
  bool short_form = true;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    const auto& pl = st.players[port];
    if (pl.present && (port >= st.num_players || pl.team != NO_TEAM))
      short_form = false;
  }
  picojson::array players;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    const auto& pl = st.players[port];
    if (!pl.present)
      continue;
    picojson::array p;
    p.emplace_back(static_cast<double>(pl.char_kind));
    p.emplace_back(static_cast<double>(pl.costume));
    p.emplace_back(Hex({pl.port_values.begin(), pl.port_values.end()}));
    if (!short_form)
    {
      p.emplace_back(static_cast<double>(pl.team));
      p.emplace_back(static_cast<double>(port));
    }
    players.emplace_back(p);
  }
  o["players"] = picojson::value(players);
  if (st.teams)
    o["teams"] = picojson::value(true);
  return picojson::value(o);
}

// The host's setup of a game. Every field is checked: a malformed setup is ignored as a whole
// (the joiner then waits for a valid one; nothing of it reaches SESSION).
std::optional<MatchSetup> SetupFromJson(const picojson::object& o)
{
  MatchSetup st;
  st.game = static_cast<u32>(JsonNum(o, "game", 0xFFFFFFFF, 0));
  st.stage = static_cast<u16>(JsonNum(o, "stage", 0xFFFF, NO_STAGE));
  st.asl = static_cast<u8>(JsonNum(o, "asl", 0xFF, 0));
  const auto it = o.find("players");
  if (st.game == 0 || !PeerData::ValidStageKind(st.stage) || it == o.end() ||
      !it->second.is<picojson::array>() ||
      it->second.get<picojson::array>().size() > static_cast<size_t>(MAX_LOBBY_PLAYERS))
  {
    return std::nullopt;
  }
  // A team battle ("teams"): every player carries a team colour (0-2), and the host's DecideTeams
  // allowed the split; the joiner checks it the same way.
  const auto teams_it = o.find("teams");
  st.teams = teams_it != o.end() && teams_it->second.is<bool>() && teams_it->second.get<bool>();
  std::array<Online::GameSetup::Seat, Online::GameSetup::MAX_PORTS> seats{};
  u64 index = 0;
  for (const auto& p : it->second.get<picojson::array>())
  {
    // [kind, costume, pv] (port = index, no team), [kind, costume, pv, team] (port = index) or
    // [kind, costume, pv, team, port].
    const size_t n = p.is<picojson::array>() ? p.get<picojson::array>().size() : 0;
    if (n < 3 || n > 5)
      return std::nullopt;
    const auto& a = p.get<picojson::array>();
    const auto kind = PeerData::JsonUInt(&a[0], 0xFF);
    const auto costume = PeerData::JsonUInt(&a[1], 0xFF);
    const auto team = n >= 4 ? PeerData::JsonUInt(&a[3], 0xFF) : std::optional<u64>(NO_TEAM);
    const auto port = n == 5 ? PeerData::JsonUInt(&a[4], 0xFF) : std::optional<u64>(index);
    ++index;
    if (!kind || !costume || !team || !port || !PeerData::ValidCharKind(static_cast<u32>(*kind)) ||
        !PeerData::ValidCostume(static_cast<u32>(*costume)) ||
        !PeerData::ValidTeam(static_cast<u32>(*team)) || !PeerData::ValidPort(*port) ||
        st.players[*port].present)
    {
      return std::nullopt;
    }
    auto& pl = st.players[*port];
    pl.present = true;
    pl.char_kind = static_cast<u8>(*kind);
    pl.costume = static_cast<u8>(*costume);
    pl.port_values = PortValuesFromHex(&a[2]);
    pl.team = st.teams ? static_cast<u8>(*team) : NO_TEAM;
    if (st.teams && pl.team == NO_TEAM)
      return std::nullopt;
    seats[*port] = {true, pl.team};
    ++st.num_players;
  }
  if (st.teams && !Online::GameSetup::DecideTeams(true, seats).teams)
    return std::nullopt;
  if (st.num_players < 2)
    return std::nullopt;
  return st;
}

u32 NextGame()
{
  return s.match_index + 1;
}

bool LockedFor(const LockIn& l, u32 game)
{
  return l.ready && l.game == game && l.char_kind != 0xFF;
}

// The stages a match may be played on: the server's `stages` list for the mode (Slippi's
// allowed_stages, from get-ticket-resp), else P+'s legal list (Slippi also falls back to a default
// list when the server sends none).
const std::vector<u16>& AllowedStages()
{
  return s.net_opts.stages.empty() ? DefaultStages() : s.net_opts.stages;
}

// Slippi's getRandomStage (EXI_DeviceSlippi.cpp): a random stage from the pool, which is refilled
// from the allowed list when it runs out, and the drawn stage leaves the pool, so a set does not
// repeat a stage until every allowed stage has been played. Host only, under s.mutex.
u16 DrawRandomStage(u32 game)
{
  if (s.stage_pool.empty())
    s.stage_pool = AllowedStages();
  std::mt19937 rng(s.session_seed ^ (game * 0x9E3779B9u));
  const size_t i = std::uniform_int_distribution<size_t>(0, s.stage_pool.size() - 1)(rng);
  const u16 stage = s.stage_pool[i];
  s.stage_pool.erase(s.stage_pool.begin() + static_cast<std::ptrdiff_t>(i));
  return stage;
}

std::mutex s_result_callback_mutex;
std::function<void(const GameResult&)> s_result_callback;
std::function<StageDecision(u32)> s_stage_decider;  // under s_result_callback_mutex
picojson::object s_local_extra;                      // under s.mutex

StageDecision DecideStage(u32 game)
{
  std::function<StageDecision(u32)> d;
  {
    std::lock_guard lk(s_result_callback_mutex);
    d = s_stage_decider;
  }
  return d ? d(game) : StageDecision{};
}

// Host, under s.mutex: once every player is locked in for the next game, decide its setup. The
// stage: the pick of the player who lost the last game, else any pick, else random from the
// match's stage list (DrawRandomStage). Slippi: Unranked random (every game), Direct random for
// game 1 and then the loser's pick. A pick is taken as it is: Slippi does not restrict Direct's
// stage select, so the server's list only feeds the random stages.
void MaybeDecideSetup()
{
  const u32 game = NextGame();
  if (s.mode != Mode::Network || !IsHost() || s.phase != Phase::Connected ||
      s.setup.game == game || s.await_scene_exit)
  {
    return;
  }
  // By in-game port: this player and every other player still in the session.
  std::array<const LockIn*, MAX_LOBBY_PLAYERS> locks{};
  locks[s.local_slot] = &s.local_lock;
  bool all_seen = true;
  ForEachPeer([&](int slot, const PeerView& p) {
    if (p.left)
      return;
    all_seen = all_seen && p.seen;
    locks[slot] = &p.lock;
  });
  int count = 0;
  for (const LockIn* l : locks)
  {
    if (!l)
      continue;
    if (!LockedFor(*l, game))
      return;
    ++count;
  }
  if (!all_seen || count < 2)
    return;
  // Free-for-all or a team battle (the room's Teams switch, each player's lock-in team).
  std::array<Online::GameSetup::Seat, Online::GameSetup::MAX_PORTS> seats{};
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if (locks[port])
      seats[port] = {true, locks[port]->team};
  }
  const Online::GameSetup::TeamSetup teams = Online::GameSetup::DecideTeams(s.net_opts.teams, seats);
  if (teams.error != Online::GameSetup::SetupError::None)
  {
    if (s.setup_error != static_cast<u8>(teams.error))
    {
      NOTICE_LOG_FMT(BRAWLBACK, "gprb lobby: game {} not set up: {}", game,
                     Online::GameSetup::SetupErrorText(teams.error));
    }
    s.setup_error = static_cast<u8>(teams.error);
    return;
  }
  s.setup_error = 0;
  const StageDecision decided = DecideStage(game);
  if (decided.applies && decided.wait)
    return;
  MatchSetup st;
  st.game = game;
  st.num_players = count;
  st.teams = teams.teams;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if (const LockIn* l = locks[port])
      st.players[port] = {true, l->char_kind, l->costume, l->port_values, teams.team[port]};
  }
  // The stage pick: the last game's loser's (s.last_pickers: a 1v1's loser, both after a draw),
  // else any player's, in port order.
  std::array<u16, Online::GameSetup::MAX_PORTS> picks{NO_STAGE, NO_STAGE, NO_STAGE, NO_STAGE};
  for (size_t i = 0; i < locks.size(); ++i)
  {
    if (locks[i])
      picks[i] = locks[i]->stage_pick;
  }
  const int pick_port = Online::GameSetup::StagePickPort(s.last_pickers, picks);
  const LockIn* pick = pick_port >= 0 ? locks[pick_port] : nullptr;
  if (decided.applies)
  {
    st.stage = decided.stage;
    st.asl = decided.asl;
  }
  else if (pick)
  {
    st.stage = pick->stage_pick;
    st.asl = pick->asl;
  }
  else
  {
    st.stage = DrawRandomStage(game);
    st.asl = 0;
  }
  s.setup = st;
  s.last_stage = st.stage;
  std::string who;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    const auto& pl = st.players[port];
    if (!pl.present)
      continue;
    who += fmt::format(", P{} {:#x}/{}", port + 1, pl.char_kind, pl.costume);
    if (pl.team != NO_TEAM)
      who += fmt::format(" team {}", pl.team);
  }
  INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} setup: stage {:#x}{}{}", game, st.stage,
               decided.applies ? " (decided by the ranked game setup)" :
               pick            ? " (picked)" :
                      fmt::format(" (random from {} stages, {})", AllowedStages().size(),
                                  s.net_opts.stages.empty() ? "P+ legal list" : "server list"),
               who);
}

void SendState()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  picojson::object o;
  {
    const bool host = IsHost();
    o["t"] = picojson::value("st");
    o["v"] = picojson::value(1.0);
    o["host"] = picojson::value(host);
    o["slot"] = picojson::value(static_cast<double>(s.local_slot));
    o["name"] = picojson::value(s.net_opts.name);
    o["game"] = picojson::value(SConfig::GetInstance().GetGameID());
    o["sel"] = picojson::value(s.local_selections);
    o["match"] = picojson::value(static_cast<double>(s.match_index));
    if (host && s.have_seed)
      o["seed"] = picojson::value(static_cast<double>(s.session_seed));
    o["at_s"] = picojson::value(s.at_s);
    o["applied"] = picojson::value(s.applied);
    o["at_start"] = picojson::value(s.at_start);
    o["go"] = picojson::value(s.go);
    o["setup"] = picojson::value(fmt::format("{:016x}", s.setup_hash));
    o["lock"] = LockJson(s.local_lock);
    if (host && s.setup.game != 0)
      o["match_setup"] = SetupJson(s.setup);
    o["winner"] = picojson::value(static_cast<double>(s.last_winner));
    if (!s_local_extra.empty())
      o["x"] = picojson::value(s_local_extra);
    if (host && !s.init_block.empty())
    {
      o["init"] = picojson::value(Hex(s.init_block));
      o["init_match"] = picojson::value(static_cast<double>(s.init_match));
    }
    if (host && s.at_s && s.sync.valid)
    {
      o["sync"] = SyncBlockJson(s.sync);
      o["ports"] = picojson::value(static_cast<double>(s.match_ports));
      picojson::array lists;
      for (const auto& l : s.task_order)
      {
        picojson::array names;
        for (const auto& n : l)
          names.emplace_back(n);
        lists.emplace_back(names);
      }
      o["tasks"] = picojson::value(lists);
    }
  }
  const std::string text = picojson::value(o).serialize();
  std::vector<u8> pkt(text.size() + 1);
  pkt[0] = 'C';
  std::memcpy(pkt.data() + 1, text.data(), text.size());
  SendToAll(pkt);
}

// The port a control packet from `from:from_port` belongs to (under s.mutex), or -1. Once a
// peer's address is confirmed only that address counts. Before that, the first message that
// claims the peer's port (its "slot"; a 1v1 has only one peer to claim) from a matching address
// confirms it: with matchmaking only from the peer's IP (its port may differ behind a NAT),
// without (harness sessions, `gprb_connect` with no address) from anywhere.
int ControlSender(const picojson::object& o, const sf::IpAddress& from, u16 from_port)
{
  if (const int slot = SlotOf(from, from_port); slot >= 0)
    return slot;
  int claim = -1;
  if (s.ported)
  {
    const auto it = o.find("slot");
    const auto n = PeerData::JsonUInt(it != o.end() ? &it->second : nullptr, 0xFF);
    if (!n || !PeerData::ValidPort(*n))
      return -1;
    claim = static_cast<int>(*n);
  }
  else
  {
    claim = FirstPeer();
  }
  if (claim < 0 || claim == s.local_slot || !s.peers[claim].expected)
    return -1;
  PeerView& p = s.peers[claim];
  if (p.confirmed || (p.ip && *p.ip != from))
    return -1;
  p.ip = from;
  p.port = from_port;
  p.confirmed = true;
  return claim;
}

// `slot` has left the session (under s.mutex). Fewer than two players left: the session ends
// (in a match on the CPU thread, at the next loop top). Otherwise a match of 3-4 players goes on
// without it: GekkoNet is told on the CPU thread, and the remaining peers agree on the frame its
// input ends (GekkoNet's disconnect claims); the next game is set up without it.
void OnPeerLeft(int slot, const std::string& reason)
{
  PeerView& p = s.peers[slot];
  if (!p.expected || p.left)
    return;
  p.left = true;
  p.left_reason = reason;
  p.drop_requested = true;
  const int active = ActivePlayers();
  INFO_LOG_FMT(BRAWLBACK, "gprb: P{} left the session ({}); {} player(s) left", slot + 1, reason,
               active);
  if (active < 2 && !s.peer_left)
  {
    s.peer_left = true;
    s.peer_left_reason = reason;
  }
}

void HandleControl(std::string_view text, const sf::IpAddress& from, u16 from_port)
{
  // Everything here comes from the other players' machines (or anyone who can send UDP to this
  // port): sizes, depth and every value are checked before use (PeerData.h).
  const auto parsed = PeerData::ParseControl(text);
  if (!parsed)
    return;
  const auto& o = *parsed;
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  const int slot = ControlSender(o, from, from_port);
  if (slot < 0)
  {
    ++s.foreign_packets;
    return;
  }
  if (const auto t = o.find("t"); t != o.end() && t->second.is<std::string>() &&
                                  t->second.get<std::string>() == "leave")
  {
    OnPeerLeft(slot, "peer left");
    return;
  }
  if (const auto t = o.find("t"); t != o.end() && t->second.is<std::string>() &&
                                  t->second.get<std::string>() == "desync")
  {
    const auto m = o.find("m");
    if (s.phase == Phase::Running && !s.desync_end && m != o.end() && m->second.is<double>() &&
        m->second.get<double>() == static_cast<double>(s.match_index))
    {
      s.desync_end = true;
      WARN_LOG_FMT(BRAWLBACK, "gprb: the peer found the game states differ: the game ends");
    }
    return;
  }
  auto& p = s.peers[slot];
  if (p.left)
    return;  // a peer that left stays gone (its last packets may still arrive)
  const bool first = !p.seen;
  p.seen = true;
  p.last_heard = Clock::now();
  auto get = [&](const char* k) -> const picojson::value* {
    const auto it = o.find(k);
    return it == o.end() ? nullptr : &it->second;
  };
  if (const auto* x = get("name"); x && x->is<std::string>())
    p.name = PeerData::TruncateName(x->get<std::string>());
  // The decider: the first peer that says it is, unless the host was configured.
  if (const auto* x = get("host"); x && x->is<bool>())
    p.claims_host = x->get<bool>();
  if (p.claims_host && s.host_slot < 0 && !s.net_opts.host)
  {
    s.host_slot = slot;
    INFO_LOG_FMT(BRAWLBACK, "gprb: P{} is the host", slot + 1);
  }
  const bool from_host = slot == HostSlot() && !IsHost();
  if (const auto* x = get("sel"); x && x->is<picojson::object>())
    p.selections = x->get<picojson::object>();
  if (const auto* x = get("at_s"); x && x->is<bool>())
    p.at_s = x->get<bool>();
  if (const auto* x = get("applied"); x && x->is<bool>())
    p.applied = x->get<bool>();
  if (const auto* x = get("at_start"); x && x->is<bool>())
    p.at_start = x->get<bool>();
  if (const auto* x = get("go"); x && x->is<bool>())
    p.go = x->get<bool>();
  if (const auto n = PeerData::JsonUInt(get("match"), 0xFFFFFFFF))
    p.match_index = static_cast<u32>(*n);
  if (const auto* x = get("init"); x && x->is<std::string>())
  {
    // Exactly the block's size, or nothing (the joiner then waits for a valid one).
    const auto b = PeerData::ParseHex(x->get<std::string>(), PeerData::INIT_BLOCK_SIZE);
    const auto m = PeerData::JsonUInt(get("init_match"), 0xFFFFFFFF);
    if (b && m)
    {
      p.init_block = *b;
      p.init_match = static_cast<s64>(*m);
    }
  }
  if (const auto* x = get("setup"); x && x->is<std::string>())
  {
    if (const auto h = PeerData::ParseHex(x->get<std::string>(), 8))
    {
      u64 v = 0;
      for (u8 c : *h)
        v = (v << 8) | c;
      p.setup_hash = v;
    }
  }
  if (const auto* x = get("lock"); x && x->is<picojson::object>())
    p.lock = LockFromJson(x->get<picojson::object>());
  if (const auto n = PeerData::JsonUInt(get("winner"), 0xFF))
    p.last_winner = static_cast<u8>(*n);
  // Another module's state (GameSetup's strikes): only a small object is kept.
  if (const auto* x = get("x"); x && x->is<picojson::object>() && x->serialize().size() <= 1024)
    p.extra = x->get<picojson::object>();
  if (const auto* x = get("match_setup"); x && x->is<picojson::object>() && from_host)
  {
    auto st = SetupFromJson(x->get<picojson::object>());
    if (st && !st->players[s.local_slot].present)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb lobby: the host's setup of game {} has no player on P{}; ignored",
                   st->game, s.local_slot + 1);
      st.reset();
    }
    if (st)
    {
      if (st->game == NextGame() && s.setup.game != st->game)
      {
        if (const StageDecision d = DecideStage(st->game); d.applies && !d.wait && d.stage != st->stage)
        {
          WARN_LOG_FMT(BRAWLBACK, "gprb lobby: game {}: the host set up stage {:#x}, the stage "
                                  "strikes decided {:#x}",
                       st->game, st->stage, d.stage);
        }
        s.setup = *st;
        s.last_stage = st->stage;
        INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} setup from the host: stage {:#x}", st->game,
                     st->stage);
      }
      p.setup = *st;
    }
  }
  if (const auto n = PeerData::JsonUInt(get("seed"), 0xFFFFFFFF); n && from_host)
  {
    p.session_seed = static_cast<u32>(*n);
    s.session_seed = p.session_seed;
    s.have_seed = true;
  }
  if (const auto n = PeerData::JsonUInt(get("ports"), 0xF))
    p.match_ports = static_cast<u32>(*n);
  if (const auto* x = get("sync"))
  {
    if (auto b = SyncBlockFromJson(*x))
      p.sync = *b;
  }
  // Task order: names only. ApplyTaskOrder only reorders this machine's own tasks, and only
  // when the host's lists hold exactly the same names.
  if (const auto* x = get("tasks"); x && x->is<picojson::array>() &&
                                    x->get<picojson::array>().size() <= PeerData::MAX_TASK_LISTS)
  {
    std::vector<std::vector<std::string>> lists;
    bool ok = true;
    for (const auto& l : x->get<picojson::array>())
    {
      if (!l.is<picojson::array>() ||
          l.get<picojson::array>().size() > PeerData::MAX_TASKS_PER_LIST)
      {
        ok = false;
        break;
      }
      std::vector<std::string> names;
      for (const auto& n : l.get<picojson::array>())
      {
        if (n.is<std::string>() && n.get<std::string>().size() <= PeerData::MAX_TASK_NAME_LEN)
          names.push_back(n.get<std::string>());
        else
          names.push_back("?");
      }
      lists.push_back(std::move(names));
    }
    if (ok)
    {
      p.task_order = std::move(lists);
      p.task_order_valid = true;
    }
  }
  if (first)
  {
    INFO_LOG_FMT(BRAWLBACK, "gprb: peer {}:{} ({}, P{}) heard", from.toString(), from_port, p.name,
                 slot + 1);
  }
  if (s.phase == Phase::Connecting)
  {
    // Connected once every other player has been heard.
    bool all = true;
    ForEachPeer([&](int, const PeerView& q) { all = all && (q.seen || q.left); });
    if (all)
      s.phase = Phase::Connected;
  }
  MaybeDecideSetup();
}

void NetThread()
{
  Common::SetCurrentThreadName("GPRB net");
  sf::SocketSelector selector;
  selector.add(*s.socket);
  std::vector<u8> buf(64 * 1024);
  auto last_state = Clock::now() - std::chrono::seconds(1);
  // Round trip: we time our state packets by sequence and the peer's echo of the newest
  // "ms" it saw; simpler: ping/pong control messages.
  auto last_ping = Clock::now() - std::chrono::seconds(1);
  while (s.net_run.load())
  {
    if (selector.wait(sf::milliseconds(5)))
    {
      while (true)
      {
        std::size_t received = 0;
        std::optional<sf::IpAddress> from;
        unsigned short from_port = 0;
        if (s.socket->receive(buf.data(), buf.size(), received, from, from_port) !=
                sf::Socket::Status::Done ||
            !from)
        {
          break;
        }
        if (received == 0)
          continue;
        // Only control packets may come from an address that is not a peer's yet
        // (HandleControl decides); everything else must come from a peer.
        int slot;
        {
          std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
          slot = SlotOf(*from, from_port);
          if (slot >= 0 && s.peers[slot].left)
            slot = -1;
          if (slot < 0 && buf[0] != 'C')
            ++s.foreign_packets;
        }
        const bool known = slot >= 0;
        if (buf[0] == 'G' && known)
        {
          std::lock_guard lk(s.gekko_rx_mutex);
          // GekkoNet drains this only while a match runs; on the character select a peer could
          // otherwise grow it without bound.
          if (s.gekko_rx.size() < PeerData::MAX_QUEUED_GEKKO_PACKETS)
            s.gekko_rx.emplace_back(slot, std::vector<u8>(buf.begin() + 1, buf.begin() + received));
        }
        else if (buf[0] == 'C')
        {
          if (received - 1 <= PeerData::MAX_CONTROL_SIZE)
          {
            HandleControl(std::string_view(reinterpret_cast<const char*>(buf.data()) + 1,
                                           received - 1),
                          *from, from_port);
          }
        }
        else if (buf[0] == 'P' && received == 9 && known)
        {
          // ping: answer with the same payload
          buf[0] = 'Q';
          (void)s.socket->send(buf.data(), received, *from, from_port);
        }
        else if (buf[0] == 'Q' && received == 9 && known)
        {
          // Our own ping's timestamp, echoed. A pong from the future or older than a few seconds
          // is not a measurement (the host waits RTT/2 before it starts the match).
          u64 sent_us;
          std::memcpy(&sent_us, buf.data() + 1, 8);
          const u64 now_us = static_cast<u64>(
              std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
                  .count());
          if (sent_us <= now_us && (now_us - sent_us) / 1000.0 <= PeerData::MAX_RTT_MS)
          {
            std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
            const double rtt = (now_us - sent_us) / 1000.0;
            auto& p = s.peers[slot];
            p.rtt_ms = p.rtt_ms < 0 ? rtt : 0.8 * p.rtt_ms + 0.2 * rtt;
          }
        }
      }
    }
    const auto now = Clock::now();
    {
      // Outside a match GekkoNet is not running: a peer silent for Slippi's 7.2 s is gone. In a
      // match GekkoNet's disconnect timeout (the same value) reports it.
      std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
      const auto silence = std::chrono::milliseconds(Online::PeerSilenceTimeoutMs(s.input_delay));
      ForEachPeer([&](int slot, PeerView& p) {
        if (p.seen && !p.left && !s.gekko && now - p.last_heard > silence)
        {
          WARN_LOG_FMT(BRAWLBACK, "gprb: nothing from P{} for {} ms", slot + 1, silence.count());
          OnPeerLeft(slot, "peer timed out");
        }
      });
      if (s.peer_left && (s.phase == Phase::Connected || s.phase == Phase::Connecting))
      {
        s.phase = Phase::Ended;
        s.disconnected = true;
        s.error = s.peer_left_reason;
      }
    }
    if (now - last_state >= std::chrono::milliseconds(50))
    {
      last_state = now;
      SendState();
    }
    if (now - last_ping >= std::chrono::milliseconds(200))
    {
      last_ping = now;
      std::vector<u8> ping(9);
      ping[0] = 'P';
      const u64 us = static_cast<u64>(
          std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
      std::memcpy(ping.data() + 1, &us, 8);
      std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
      SendToAll(ping);
    }
  }
}

void CloseNetwork()
{
  s.net_run = false;
  if (s.net_thread.joinable())
    s.net_thread.join();
  {
    // GekkoNet sends from the CPU thread under s.mutex.
    std::lock_guard<std::recursive_timed_mutex> slk(s.mutex);
    if (s.socket)
      s.socket->unbind();
    s.socket.reset();
  }
  std::lock_guard lk(s.gekko_rx_mutex);
  s.gekko_rx.clear();
}

// ---------------------------------------------------------------------------------------------
// Session start / end (CPU thread)

void DestroyGekko()
{
  if (s.gekko)
    gekko_destroy(&s.gekko);
  s.gekko = nullptr;
  s.gekko_started = false;
  s.ops_prefetched = false;
  s.defer_initial_save = false;
  s.initial_save_deferred = false;
  s.deferred_initial_checksum = nullptr;
  for (auto* r : s.gekko_rx_ptrs)
    (void)r;  // owned and freed by GekkoNet
  s.gekko_rx_ptrs.clear();
  for (auto& p : s.peers)
  {
    p.handle = -1;
    p.drop_requested = false;
  }
}

bool CreateGekko(bool stress, int num_players)
{
  DestroyGekko();
  if (!gekko_create(&s.gekko, stress ? GekkoStressSession : GekkoGameSession))
    return false;
  GekkoConfig cfg{};
  cfg.num_players = static_cast<unsigned char>(num_players);
  cfg.max_spectators = 0;
  cfg.input_prediction_window = MAX_ROLLBACK_FRAMES;
  cfg.input_size = INPUT_SIZE;
  cfg.state_size = sizeof(u32);
  cfg.limited_saving = false;
  // A misprediction sync test changes the first run's input on purpose: its checksums cannot match.
  cfg.desync_detection = !(stress && (s.st_opts.mispredict_ports || s.st_opts.no_rollback));
  cfg.check_distance = stress ? static_cast<unsigned int>(s.st_opts.distance) : 0;
  gekko_start(s.gekko, &cfg);
  s.num_players = num_players;
  s.local_handles.clear();
  s.handle_port.fill(-1);
  s.remote_handle = -1;
  if (stress)
  {
    for (int port = 0; port < 4; ++port)
    {
      if (!(s.st_opts.ports & (1u << port)))
        continue;
      const int h = gekko_add_actor(s.gekko, GekkoLocalPlayer, nullptr);
      s.local_handles.push_back(h);
      s.handle_port[h] = port;
    }
    s.gekko_started = true;
  }
  else
  {
    s.adapter.send_data = GekkoSend;
    s.adapter.receive_data = GekkoReceive;
    s.adapter.free_data = GekkoFree;
    gekko_net_adapter_set(s.gekko, &s.adapter);
    // Handles follow the match's in-game ports in order (a 1v1: the host P1, the joiner P2), the
    // same on every peer. A remote player's address is its port ("p0".."p3", GekkoSend).
    for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
    {
      if (!(s.match_ports & (1u << port)))
        continue;
      int h;
      if (port == s.local_slot)
      {
        h = gekko_add_actor(s.gekko, GekkoLocalPlayer, nullptr);
        s.local_handles = {h};
      }
      else
      {
        GekkoNetAddress addr{s_gekko_addr[port], 2};
        h = gekko_add_actor(s.gekko, GekkoRemotePlayer, &addr);
        s.peers[port].handle = h;
        s.peers[port].gekko_dropped = false;
        s.peers[port].gone_frame = -1;
        if (s.remote_handle < 0)
          s.remote_handle = h;
      }
      if (h < 0 || h >= static_cast<int>(s.handle_port.size()))
      {
        DestroyGekko();
        return false;
      }
      s.handle_port[h] = port;
    }
    if (s.local_handles.empty())
    {
      DestroyGekko();
      return false;
    }
    // Each player picks the delay of their own inputs (Slippi): the set value, else from the
    // round trip measured now, to the furthest player still in the session. It stays for the
    // whole game.
    double rtt = -1;
    ForEachPeer([&](int, const PeerView& p) {
      if (!p.left && p.rtt_ms >= 0)
        rtt = std::max(rtt, p.rtt_ms);
    });
    s.input_delay = s.net_opts.delay ? std::clamp(*s.net_opts.delay, 0, MAX_INPUT_DELAY) :
                                       AutoInputDelay(rtt);
    gekko_set_local_delay(s.gekko, s.local_handles[0], static_cast<unsigned char>(s.input_delay));
    gekko_set_disconnect_timeout(s.gekko, Online::PeerSilenceTimeoutMs(s.input_delay));
    // The controller that pressed START to lock in plays this game (Slippi-style: any port).
    s.local_pad = s.local_lock.local_pad < 4 ? s.local_lock.local_pad :
                                               std::clamp(s.net_opts.local_pad, 0, 3);
    INFO_LOG_FMT(BRAWLBACK, "gprb: input delay {} ({}, round trip {:.0f} ms), controller port {}",
                 s.input_delay, s.net_opts.delay ? "set" : "auto", rtt, s.local_pad + 1);
    if (s.gone_enabled)
    {
      // A player who leaves a 3-4 player match gets the gone marker as its input from the frame
      // the remaining peers agree on (docs/nplayer/session.md).
      std::array<u8, INPUT_SIZE> gone{};
      PeerData::SetGoneMarker(gone.data());
      gekko_set_disconnected_input(s.gekko, gone.data());
      // A player who left before GekkoNet started is gone from frame 0 on.
      ForEachPeer([&](int, PeerView& p) {
        if (p.left && p.handle >= 0)
          p.drop_requested = true;
      });
    }
  }
  return true;
}

// CPU thread, under s.mutex: players that left a 3-4 player match are disconnected in GekkoNet,
// which then agrees with the other peers on the last frame of their input (disconnect claims).
void ApplyPeerDrops()
{
  if (!s.gekko || s.mode != Mode::Network)
    return;
  ForEachPeer([&](int slot, PeerView& p) {
    if (!p.drop_requested || p.handle < 0)
      return;
    p.drop_requested = false;
    if (!p.gekko_dropped)
    {
      gekko_disconnect_actor(s.gekko, p.handle);
      INFO_LOG_FMT(BRAWLBACK, "gprb: P{} disconnected from GekkoNet ({})", slot + 1, p.left_reason);
    }
  });
}

std::optional<std::string> PrepareRegionSet(Core::System& system, const std::string& set_name)
{
  if (set_name == "whole")
  {
    // Whole-machine snapshots (device and timing state included) through the same session, for
    // comparing costs and behaviour with the gameplay region set.
    s.ranges.clear();
    s.region_bytes = 0;
    auto& rbm = Rollback::RollbackManager::Get();
    rbm.EndRegionMode();
    rbm.ToggleFrameSave();
    return std::nullopt;
  }
  RegionSetSpec spec;
  if (auto err = LoadRegionSet(set_name, &spec))
    return err;
  const Guest g(system.GetMemory());
  s.ranges = ResolveRegionSet(g, spec);
  std::vector<std::pair<u32, u32>> rs;
  s.region_bytes = 0;
  for (const auto& r : s.ranges)
  {
    rs.emplace_back(r.addr, r.size);
    s.region_bytes += r.size;
  }
  std::vector<std::pair<u32, u32>> excl;
  for (const auto& r : spec.exclude)
    excl.emplace_back(r.addr, r.size);
  // The game<->Dolphin mailbox (Online/GameBridge.h) lives in our game plugin's REL .data, which
  // rel_data takes in: never restore it.
  auto& rbm = Rollback::RollbackManager::Get();
  if (rbm.m_mailbox_region)
  {
    excl.emplace_back(rbm.m_mailbox_region->phys_start | 0x80000000u,
                      rbm.m_mailbox_region->phys_end - rbm.m_mailbox_region->phys_start);
  }
  rbm.BeginRegionMode(rs, excl);
  INFO_LOG_FMT(BRAWLBACK, "gprb: region set {} resolved to {} ranges, {} bytes", spec.name,
               s.ranges.size(), s.region_bytes);
  return std::nullopt;
}

void ResetRunStats()
{
  s.ops.Clear();
  s.iteration = 0;
  s.resim_pass = false;
  s.spinning = false;
  s.queued_save = {};
  s.current_frame = -1;
  s.speed_factor = 1.0;
  s.last_timesync_frame = UINT32_MAX;
  s.last_hard_sync_frame = -1;
  s.last_start_log_frame = -1;
  s.next_ping_display_frame = PING_DISPLAY_INTERVAL;
  s.rollbacks = s.max_rollback = s.frames_resimulated = s.stall_polls = s.desyncs = 0;
  s.net_window = s.net_total = {};
  s.net_window_first = -1;
  s.prev_frame_rolled_back = false;
  Core::System::GetInstance().GetCoreTiming().TakeMaxBehindSchedule();
  Rollback::PresentStats::TakeCadence();
  s.burst_open = false;
  s.last_desync_frame = -1;
  s.region_mismatches = 0;
  s.region_mismatch_log.clear();
  s.frames = 0;
  s.region_chunks.clear();
  std::fill(s.history.begin(), s.history.end(), FrameRecord{});
  std::fill(s.first_runs.begin(), s.first_runs.end(), FrameRecord{});
  s.desync_log.clear();
  s.running_since = Clock::now();
  s.end_reason.clear();
  s.game_set_frame = -1;
  s.game_set_quit = false;
  s.sound_allocs = s.resim_sound_allocs = s.suppressed_sound_allocs = 0;
  for (auto& v : s.snd_played)
    v.clear();
  s.snd_tag.fill(-1);
  s.snd_remaining.clear();
  s.snd_new.clear();
  s.snd_pass_open = false;
  s.snd_frame = s.snd_max_frame = -1;
  s.snd_first_run = true;
  s.snd_gen.clear();
  s.snd_reattach = 0;
  s.snd_to_stop.clear();
  s.sound_reattached = s.sound_reattach_gone = s.sound_stopped = s.sound_stop_gone = 0;
  s.snd_gone_why.fill(0);
  s.sound_moved = 0;
  s.io_wait_active = false;
  s.io_wait_n = 0;
  s.io_waits = s.io_wait_retraces = s.io_wait_timeouts = 0;
  s.force_final_inited = false;  // the next match's countdown reads PPR_GPRB_FORCE_FINAL again
  s.mispredicted_passes = 0;
  s.sample_chunks.clear();
  s.sample_bytes.clear();
  s.sample_ranges = s.ranges;
}

void PassLogClose();


void ReportGameResult(const GameResult& r)
{
  std::function<void(const GameResult&)> cb;
  {
    std::lock_guard lk(s_result_callback_mutex);
    cb = s_result_callback;
  }
  if (cb)
    cb(r);
}

// The result of a game that ended with game set, from the state every peer ended on: per port of
// the match its stocks and damage (ReadFighterFields, by player number), its team and its place in
// the game's elimination order (SESSION `out`, rolled back with the match), the winner and who
// picks the next stage. A 1v1 keeps its rule: more stocks, then less damage (P+'s time-out rule),
// the loser picks (both after a draw). 3-4 players: Online::GameSetup::DecideOutcome. A player who
// left the match (its gone flag, the same on every peer) is not placed.
GameResult ReadGameResult(Core::System& system)
{
  const Guest g = GuestOf(system);
  GameResult r;
  r.game = NextGame();
  r.stage = s.setup.stage;
  r.frames = s.end_game_frame;
  const auto f = ReadFighterFields(g);
  const auto teams = MatchTeams(g);
  const u32 session = Online::GameBridge::SessionAddress();
  std::array<Online::GameSetup::PortEnd, Online::GameSetup::MAX_PORTS> ends{};
  bool team_battle = false;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if (!(s.match_ports & (1u << port)) || (s.gone_enabled && s.gone_written[port]))
      continue;
    r.present[port] = true;
    r.stocks[port] = static_cast<s32>(f[port][2]);
    std::memcpy(&r.damage[port], &f[port][1], 4);
    r.char_kind[port] = s.setup.players[port].char_kind;
    r.team[port] = teams[port];
    team_battle = team_battle || teams[port] != NO_TEAM;
    ++r.num_players;
    auto& e = ends[port];
    e.present = true;
    e.team = teams[port];
    e.stocks = r.stocks[port];
    e.damage = r.damage[port];
    if (const u8* out = session ? g.Ptr(session + Online::GameBridge::SESSION_PLAYERS_OFF +
                                            port * Online::GameBridge::SESSION_PLAYER_SIZE +
                                            Online::GameBridge::SESSION_PLAYER_OUT,
                                        1) :
                                  nullptr)
    {
      e.out = *out;
    }
  }
  if (std::popcount(s.match_ports) == 2)
  {
    const int a = std::countr_zero(s.match_ports);
    const int b = 31 - std::countl_zero(s.match_ports);
    const s32 sa = r.stocks[a], sb = r.stocks[b];
    const float da = r.damage[a], db = r.damage[b];
    r.winner = static_cast<u8>(sa != sb ? (sa > sb ? a : b) : (da != db ? (da < db ? a : b) : 0xFE));
    r.pickers = r.winner == 0xFE ? static_cast<u8>(s.match_ports) :
                                   static_cast<u8>(s.match_ports & ~(1u << r.winner));
    return r;
  }
  const auto outcome = Online::GameSetup::DecideOutcome(team_battle, ends);
  r.winner = outcome.winner;
  r.pickers = outcome.pickers;
  r.place = outcome.place;
  return r;
}
void EndRunning(Core::System& system, const std::string& reason)
{
  PassLogClose();
  DestroyGekko();
  Rollback::RollbackManager::Get().EndRegionMode();
  PadsClearCurrent();
  auto& ct = system.GetCoreTiming();
  ct.ResetThrottleToNow();  // drops a rollback burst still pending
  ct.SetRollbackResimulating(false);
  ct.SetRollbackSpeedAdjustment(1.0);
  system.GetFifo().SetRollbackSessionDeterminism(false);
  s.resim_pass = false;
  s.end_reason = reason;
  if (s.mode == Mode::Network)
  {
    s.net_window.cadence = Rollback::PresentStats::TakeCadence();
    NetStatsFold(s.net_total, s.net_window);
    s.net_window = {};
    if (s.net_total.frames)
      INFO_LOG_FMT(BRAWLBACK, "gprb net: session: {}", NetStatsLine(s.net_total));
  }
  s.end_game_frame = GuestOf(system).U32(Addr::GAME_FRAME + 4).value_or(0);
  INFO_LOG_FMT(BRAWLBACK, "gprb: session ended at game frame {} ({}), {} rollbacks, {} desyncs",
               s.end_game_frame, reason, s.rollbacks, s.desyncs);
  if (s.mode == Mode::SyncTest)
  {
    s.phase = Phase::Ended;
  }
  else
  {
    // The winner (lobby: the loser picks the next stage in Direct), from the state both peers
    // ended on: more stocks, then less damage (P+'s time-out rule); equal is a draw.
    if (reason == "game set")
    {
      const GameResult r = ReadGameResult(system);
      s.last_winner = r.winner;
      s.last_pickers = r.pickers;
      std::string who;
      for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
      {
        if (r.present[port])
        {
          who += fmt::format(" P{} {}/{}", port + 1, r.stocks[port], r.damage[port]);
          if (r.num_players > 2)
            who += fmt::format(" place {}", r.place[port]);
        }
      }
      INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} winner {} stage pickers {:#x} (stocks/damage:{})",
                   r.game, r.winner, r.pickers, who);
      ReportGameResult(r);
    }
    // Ready for the next match on the same connection.
    bool seen = false;
    ForEachPeer([&](int, PeerView& p) {
      seen = seen || (p.seen && !p.left);
      p.at_s = p.applied = p.at_start = p.go = false;
      p.sync = {};
      p.task_order_valid = false;
      p.match_ports = 0;
    });
    s.phase = seen && !s.peer_left ? Phase::Connected : Phase::Ended;
    s.at_s = s.applied = s.at_start = s.go = false;
    s.did_frame0 = false;
    s.sync = {};
    s.start_at.reset();
    s.countdown_at.reset();
    ++s.match_index;
    s.await_scene_exit = true;
  }
  s.match_ports = 0;
  s.gone_enabled = false;
  s.gone_addr = 0;
}

// ---------------------------------------------------------------------------------------------
// Per-frame (CPU thread)

void RecordFrame(s64 frame, u32 checksum, const FrameRecord& r)
{
  FrameRecord rec = r;
  rec.frame = frame;
  rec.checksum = checksum;
  s.history[static_cast<size_t>(frame) % s.history.size()] = rec;
}

std::vector<u64> RegionChunkHashes(Core::System& system, u64* total)
{
  const Guest g(system.GetMemory());
  std::vector<u64> out;
  XXH3_state_t* st = XXH3_createState();
  XXH3_64bits_reset(st);
  for (const auto& r : s.ranges)
  {
    for (u32 off = 0; off < r.size; off += REGION_CHUNK)
    {
      const u32 n = std::min(REGION_CHUNK, r.size - off);
      if (const u8* p = g.Ptr(r.addr + off, n))
      {
        out.push_back(XXH3_64bits(p, n));
        XXH3_64bits_update(st, p, n);
      }
      else
      {
        out.push_back(0);
      }
    }
  }
  *total = XXH3_64bits_digest(st);
  XXH3_freeState(st);
  return out;
}

// Diagnostics (PPR_GPRB_DIFF_FRAME=N): copy the region set at frame N's first save, and at its
// next save (the resimulation) log every differing 32-byte span with both values.
void RegionByteDiff(Core::System& system, s64 frame)
{
  static const char* env = std::getenv("PPR_GPRB_DIFF_FRAME");
  if (!env || frame != static_cast<s64>(strtoll(env, nullptr, 10)))
    return;
  static std::vector<u8> first;
  static int done = 0;
  if (done >= 2)
    return;
  const Guest g(system.GetMemory());
  std::vector<u8> now;

  for (const auto& r : s.ranges)
  {
    if (const u8* p = g.Ptr(r.addr, r.size))
    {
      now.insert(now.end(), p, p + r.size);
    }
  }
  if (done++ == 0)
  {
    first = std::move(now);
    return;
  }
  int shown = 0;
  size_t base = 0;
  for (const auto& r : s.ranges)
  {
    if (!g.Ptr(r.addr, r.size))
      continue;
    for (u32 off = 0; off < r.size && shown < 400; off += 32)
    {
      const u32 n = std::min<u32>(32, r.size - off);
      if (base + off + n <= first.size() && base + off + n <= now.size() &&
          std::memcmp(first.data() + base + off, now.data() + base + off, n) != 0)
      {
        std::string a, b;
        for (u32 k = 0; k < n; ++k)
        {
          a += fmt::format("{:02x}", first[base + off + k]);
          b += fmt::format("{:02x}", now[base + off + k]);
        }
        WARN_LOG_FMT(BRAWLBACK, "gprb diff {:08x}: {} | {}", r.addr + off, a, b);
        ++shown;
      }
    }
    base += r.size;
  }
  WARN_LOG_FMT(BRAWLBACK, "gprb diff: frame {} done ({} spans shown)", frame, shown);
}

// Diagnostics (PPR_GPRB_DUMP_FRAMES=f1,f2,... and PPR_GPRB_DUMP_DIR): every save of those session
// frames writes the region set's bytes to <dir>/f<frame>-<n>.bin (n counts that frame's saves:
// the last one is the confirmed state), and the ranges once to <dir>/ranges.txt.
void RegionDump(Core::System& system, s64 frame)
{
  static const char* frames_env = std::getenv("PPR_GPRB_DUMP_FRAMES");
  static const char* dir = std::getenv("PPR_GPRB_DUMP_DIR");
  if (!frames_env || !dir)
    return;
  static std::vector<s64> frames = [] {
    std::vector<s64> out;
    std::string v = frames_env;
    size_t pos = 0;
    while (pos < v.size())
    {
      const size_t comma = v.find(',', pos);
      out.push_back(std::strtoll(v.substr(pos, comma - pos).c_str(), nullptr, 10));
      if (comma == std::string::npos)
        break;
      pos = comma + 1;
    }
    return out;
  }();
  if (std::find(frames.begin(), frames.end(), frame) == frames.end())
    return;
  static std::map<s64, int> count;
  static bool wrote_ranges = false;
  if (!wrote_ranges)
  {
    wrote_ranges = true;
    if (FILE* f = std::fopen(fmt::format("{}/ranges.txt", dir).c_str(), "w"))
    {
      for (const auto& r : s.ranges)
        std::fprintf(f, "%08x %08x %s\n", r.addr, r.size, r.label.c_str());
      std::fclose(f);
    }
  }
  const int n = count[frame]++;
  if (std::getenv("PPR_GPRB_DUMP_FULL"))
  {
    // Whole MEM1 and MEM2, for finding state outside the region set.
    auto& mem = system.GetMemory();
    if (FILE* f = std::fopen(fmt::format("{}/m1-f{}-{}.bin", dir, frame, n).c_str(), "wb"))
    {
      std::fwrite(mem.GetRAM(), 1, mem.GetRamSizeReal(), f);
      std::fclose(f);
    }
    if (FILE* f = std::fopen(fmt::format("{}/m2-f{}-{}.bin", dir, frame, n).c_str(), "wb"))
    {
      std::fwrite(mem.GetEXRAM(), 1, mem.GetExRamSizeReal(), f);
      std::fclose(f);
    }
  }
  const Guest g(system.GetMemory());
  if (FILE* f = std::fopen(fmt::format("{}/f{}-{}.bin", dir, frame, n).c_str(), "wb"))
  {
    for (const auto& r : s.ranges)
    {
      if (const u8* p = g.Ptr(r.addr, r.size))
        std::fwrite(p, 1, r.size, f);
      else
      {
        std::vector<u8> zeros(r.size);
        std::fwrite(zeros.data(), 1, zeros.size(), f);
      }
    }
    std::fclose(f);
  }
}

u32 ChunkAddress(size_t index)
{
  for (const auto& r : s.ranges)
  {
    const size_t n = (r.size + REGION_CHUNK - 1) / REGION_CHUNK;
    if (index < n)
      return r.addr + static_cast<u32>(index) * REGION_CHUNK;
    index -= n;
  }
  return 0;
}

void PerformQueuedSave(Core::System& system)
{
  if (!s.queued_save.pending)
    return;
  s.queued_save.pending = false;
  Rollback::RollbackManager::Get().SaveFrame(system);
  const Guest g(system.GetMemory());
  FrameRecord rec;
  const u32 crc = FrameChecksum(g, &rec);
  if (s.queued_save.checksum)
    *s.queued_save.checksum = crc;
  RegionDump(system, s.queued_save.frame);
  if (s.sample_every && s.queued_save.frame % s.sample_every == 0)
  {
    u64 total = 0;
    s.sample_chunks[s.queued_save.frame] = RegionChunkHashes(system, &total);
    if (!s.sample_watch.empty())
    {
      std::vector<u8> bytes;
      for (const auto& [addr, size] : s.sample_watch)
      {
        if (const u8* p = g.Ptr(addr, size))
          bytes.insert(bytes.end(), p, p + size);
        else
          bytes.insert(bytes.end(), size, 0);
      }
      s.sample_bytes[s.queued_save.frame] = std::move(bytes);
    }
  }
  const bool hash = s.mode == Mode::SyncTest ? s.st_opts.hash_regions : s.net_opts.hash_regions;
  if (hash)
  {
    u64 total = 0;
    auto chunks = RegionChunkHashes(system, &total);
    RegionByteDiff(system, s.queued_save.frame);
    rec.region_hash = total;
    const s64 f = s.queued_save.frame;
    auto it = s.region_chunks.find(f);
    if (it != s.region_chunks.end())
    {
      if (it->second != chunks)
      {
        ++s.region_mismatches;
        if (s.region_mismatch_log.size() < 200)
        {
          std::string addrs;
          int shown = 0;
          for (size_t i = 0; i < chunks.size() && i < it->second.size(); ++i)
          {
            if (chunks[i] != it->second[i] && shown++ < 24)
              addrs += fmt::format(" {:08x}", ChunkAddress(i));
          }
          s.region_mismatch_log.push_back(fmt::format("frame {}:{}", f, addrs));
          WARN_LOG_FMT(BRAWLBACK, "gprb synctest: frame {} region differs from its first run:{}", f,
                       addrs);
        }
      }
    }
    else
    {
      s.region_chunks.emplace(f, std::move(chunks));
      while (s.region_chunks.size() > 16)
        s.region_chunks.erase(s.region_chunks.begin());
    }
  }
  if (s.mode == Mode::SyncTest)
  {
    // Sync test diagnostics: compare each part with the frame's first run. In a network session a
    // re-run differs from the first run whenever the prediction was wrong: only noise there.
    auto& first = s.first_runs[static_cast<size_t>(s.queued_save.frame) % s.first_runs.size()];
    if (first.frame == s.queued_save.frame)
    {
      if (first.checksum != crc && s.desync_log.size() < 200)
      {
        const Guest g2(system.GetMemory());
        const u32 gf = g2.U32(Addr::GAME_FRAME + 4).value_or(0);
        std::string what = fmt::format("frame {} (game frame {}):", s.queued_save.frame, gf);
        if (first.rng != rec.rng)
          what += fmt::format(" rng {:08x} {:08x} {:08x} -> {:08x} {:08x} {:08x}", first.rng[0], first.rng[1],
                              first.rng[2], rec.rng[0], rec.rng[1], rec.rng[2]);
        if (first.fighters_crc != rec.fighters_crc)
          what += fmt::format(" fighters {:08x} -> {:08x}", first.fighters_crc, rec.fighters_crc);
        if (first.game_frame != gf)
          what += fmt::format(" game frame {} -> {}", first.game_frame, gf);
        s.desync_log.push_back(what);
        WARN_LOG_FMT(BRAWLBACK, "gprb synctest: checksum differs from the first run, {}", what);
      }
    }
    else
    {
      first = rec;
      first.frame = s.queued_save.frame;
      first.checksum = crc;
      first.game_frame = GuestOf(system).U32(Addr::GAME_FRAME + 4).value_or(0);
    }
  }
  RecordFrame(s.queued_save.frame, crc, rec);
}

// GekkoNet's save of frame 0 (the session's first state): the first snapshot of the region set.
void InitialSave(Core::System& system, unsigned int* checksum)
{
  Rollback::RollbackManager::Get().SaveFrame(system);
  FrameRecord rec;
  const u32 crc = FrameChecksum(GuestOf(system), &rec);
  if (checksum)
    *checksum = crc;
}

bool ProcessGekkoUpdate(Core::System& system, GekkoGameEvent** events, int count);

// A rollback is about to load: the frames it re-runs must not push the presented frame after them
// back in wall-clock time (CoreTimingManager::BeginRollbackBurst). PPR_GPRB_OLD_THROTTLE=1 keeps
// the old behaviour (the throttle re-anchored at the presented frame), for comparisons.
void BeginBurst(Core::System& system)
{
  static const bool old_throttle = std::getenv("PPR_GPRB_OLD_THROTTLE") != nullptr;
  if (!old_throttle)
    system.GetCoreTiming().BeginRollbackBurst();
  if (!s.burst_open)
  {
    s.burst_open = true;
    s.burst_t0 = Clock::now();
  }
}

// The link to the remote player with the highest ping (a 1v1: the peer); false without one.
bool WorstLink(GekkoNetworkStats* out)
{
  if (!s.gekko)
    return false;
  bool any = false;
  ForEachPeer([&](int, const PeerView& p) {
    if (p.handle < 0 || p.gekko_dropped)
      return;
    GekkoNetworkStats st{};
    gekko_network_stats(s.gekko, p.handle, &st);
    if (!any || st.avg_ping > out->avg_ping)
      *out = st;
    any = true;
  });
  return any;
}

constexpr u64 NET_STATS_WINDOW = 600;  // displayed frames (10 s)
constexpr double LATE_MS = 2.0;

std::string NetStatsLine(const State::NetStats& w)
{
  const double n = static_cast<double>(std::max<u64>(w.frames, 1));
  std::string line = fmt::format(
      "{} frames, rollbacks {} ({:.1f}%), resimulated {} (avg depth {:.2f}, max {}), "
      "rollback cost avg {:.2f} ms max {:.2f} ms, late frames {} ({} after a rollback, max {:.1f} ms), "
      "waits for the peer {} ({:.0f} ms, max {:.1f} ms), frames ahead avg {:.2f} [{:.2f}, {:.2f}], "
      "speed [{:.4f}, {:.4f}]",
      w.frames, w.rollbacks, 100.0 * w.rollbacks / n, w.resim,
      w.rollbacks ? static_cast<double>(w.resim) / w.rollbacks : 0.0, w.max_depth,
      w.bursts ? w.burst_ms / w.bursts : 0.0, w.burst_ms_max, w.late, w.late_after_rollback,
      w.late_ms_max, w.stalls,
      w.stall_ms, w.stall_ms_max, w.ahead_sum / n, w.frames ? w.ahead_min : 0.0,
      w.frames ? w.ahead_max : 0.0, w.frames ? w.speed_min : 1.0, w.frames ? w.speed_max : 1.0);
  if (const auto& c = w.cadence; c.presents)
  {
    const double mean = c.interval_ms_sum / c.presents;
    const double sd = std::sqrt(std::max(0.0, c.interval_ms_sq / c.presents - mean * mean));
    line += fmt::format(", presents {} (interval sd {:.2f} ms, max {:.1f} ms), screen hitches {:.1f}",
                        c.presents, sd, c.interval_ms_max, c.hitches);
  }
  if (w.pad_age_n)
  {
    line += fmt::format(", pad sample age avg {:.1f} ms max {:.1f} ms", w.pad_age_sum / w.pad_age_n,
                        w.pad_age_max);
  }
  return line;
}

void NetStatsFold(State::NetStats& t, const State::NetStats& w)
{
  t.frames += w.frames;
  t.rollbacks += w.rollbacks;
  t.resim += w.resim;
  t.max_depth = std::max(t.max_depth, w.max_depth);
  t.bursts += w.bursts;
  t.late += w.late;
  t.late_after_rollback += w.late_after_rollback;
  t.burst_ms += w.burst_ms;
  t.burst_ms_max = std::max(t.burst_ms_max, w.burst_ms_max);
  t.late_ms_max = std::max(t.late_ms_max, w.late_ms_max);
  t.stalls += w.stalls;
  t.stall_ms += w.stall_ms;
  t.stall_ms_max = std::max(t.stall_ms_max, w.stall_ms_max);
  t.ahead_sum += w.ahead_sum;
  t.ahead_min = std::min(t.ahead_min, w.ahead_min);
  t.ahead_max = std::max(t.ahead_max, w.ahead_max);
  t.speed_min = std::min(t.speed_min, w.speed_min);
  t.speed_max = std::max(t.speed_max, w.speed_max);
  t.pad_age_sum += w.pad_age_sum;
  t.pad_age_max = std::max(t.pad_age_max, w.pad_age_max);
  t.pad_age_n += w.pad_age_n;
  t.cadence.presents += w.cadence.presents;
  t.cadence.hitches += w.cadence.hitches;
  t.cadence.interval_ms_sum += w.cadence.interval_ms_sum;
  t.cadence.interval_ms_sq += w.cadence.interval_ms_sq;
  t.cadence.interval_ms_max = std::max(t.cadence.interval_ms_max, w.cadence.interval_ms_max);
}

// Network sessions: every NET_STATS_WINDOW displayed frames, one log line of what the netcode did,
// with the link's numbers from GekkoNet.
void NetStatsOnDisplayedFrame(Core::System& system)
{
  if (s.mode != Mode::Network)
    return;
  auto& w = s.net_window;
  if (s.net_window_first < 0)
    s.net_window_first = s.current_frame;
  ++w.frames;
  if (s.ops.adv_count > 1)
  {
    ++w.rollbacks;
    w.resim += static_cast<u64>(s.ops.adv_count - 1);
    w.max_depth = std::max<u64>(w.max_depth, static_cast<u64>(std::max(s.last_load_back, 0)));
  }
  if (s.burst_open)
  {
    s.burst_open = false;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - s.burst_t0).count();
    ++w.bursts;
    w.burst_ms += ms;
    w.burst_ms_max = std::max(w.burst_ms_max, ms);
  }
  {
    // The throttle points since the previous displayed frame started: that frame's own.
    const double ms = std::chrono::duration<double, std::milli>(
                          system.GetCoreTiming().TakeMaxBehindSchedule())
                          .count();
    if (ms > LATE_MS)
    {
      ++w.late;
      if (s.prev_frame_rolled_back)
        ++w.late_after_rollback;
      w.late_ms_max = std::max(w.late_ms_max, ms);
    }
    s.prev_frame_rolled_back = s.ops.adv_count > 1;
  }
  w.ahead_sum += s.frames_ahead;
  w.ahead_min = std::min<double>(w.ahead_min, s.frames_ahead);
  w.ahead_max = std::max<double>(w.ahead_max, s.frames_ahead);
  w.speed_min = std::min(w.speed_min, s.speed_factor);
  w.speed_max = std::max(w.speed_max, s.speed_factor);
  if (w.frames < NET_STATS_WINDOW)
    return;
  w.cadence = Rollback::PresentStats::TakeCadence();

  std::string link;
  if (GekkoNetworkStats st{}; WorstLink(&st))
  {
    link = fmt::format("; ping {} ms (avg {:.1f}, jitter {:.1f}), {:.1f}/{:.1f} kB/s out/in",
                       st.last_ping, st.avg_ping, st.jitter, st.kb_sent, st.kb_received);
  }
  INFO_LOG_FMT(BRAWLBACK, "gprb net: frames {}-{}: {}{}", s.net_window_first, s.current_frame,
               NetStatsLine(w), link);
  NetStatsFold(s.net_total, w);
  w = {};
  s.net_window_first = -1;
}

bool HandleGekkoFrame(Core::System& system)
{
  s.ops.Clear();
  if (!s.gekko)
    return false;
  if (s.mode == Mode::Network)
    gekko_network_poll(s.gekko);
  for (int h : s.local_handles)
    gekko_add_local_input(s.gekko, h, s.local_inputs.data() + s.handle_port[h] * INPUT_SIZE);
  int count = 0;
  GekkoGameEvent** events = gekko_update_session(s.gekko, &count);
  return ProcessGekkoUpdate(system, events, count);
}

// The session events and game events of one gekko_update_session: fills s.ops, performs the
// load and the initial save.
bool ProcessGekkoUpdate(Core::System& system, GekkoGameEvent** events, int count)
{
  s.ops.Clear();
  int scount = 0;
  GekkoSessionEvent** sev = gekko_session_events(s.gekko, &scount);
  for (int i = 0; i < scount; ++i)
  {
    switch (sev[i]->type)
    {
    case GekkoSessionStarted:
      s.gekko_started = true;
      break;
    case GekkoPlayerDisconnected:
      if (s.mode == Mode::Network && s.num_players >= 3)
      {
        // A 3-4 player match goes on without the player (GekkoNet's disconnect claims agree on
        // the frame its input ends; from then on it carries the gone marker) as long as two
        // players remain.
        const int h = sev[i]->data.disconnected.handle;
        const int port = h >= 0 && h < static_cast<int>(s.handle_port.size()) ? s.handle_port[h] : -1;
        if (port >= 0 && port != s.local_slot)
        {
          PeerView& p = s.peers[port];
          p.gekko_dropped = true;
          WARN_LOG_FMT(BRAWLBACK, "gprb: GekkoNet dropped P{} at session frame {}", port + 1,
                       s.current_frame);
          OnPeerLeft(port, "peer timed out");
          p.drop_requested = false;
          if (!s.peer_left)
            break;
        }
        else if (port == s.local_slot)
        {
          // GekkoNet dropped every peer (it could not stay in agreement): the session ends.
          if (!s.peer_left)
          {
            s.peer_left = true;
            s.peer_left_reason = "peer timed out";
          }
        }
        else
        {
          break;
        }
      }
      // GekkoNet's silence timeout (Online::PeerSilenceTimeoutMs). Reported once: remember it,
      // the next loop top ends the session even if this update ran inside HostWait.
      s.error = "peer disconnected";
      if (s.mode == Mode::Network && !s.peer_left)
      {
        s.peer_left = true;
        s.peer_left_reason = "peer timed out";
        WARN_LOG_FMT(BRAWLBACK, "gprb: GekkoNet dropped the silent peer");
      }
      return false;
    case GekkoDesyncDetected:
      ++s.desyncs;
      s.last_desync_frame = sev[i]->data.desynced.frame;
      WARN_LOG_FMT(BRAWLBACK, "gprb: desync at frame {} (local {:08x}, remote {:08x})",
                   sev[i]->data.desynced.frame, sev[i]->data.desynced.local_checksum,
                   sev[i]->data.desynced.remote_checksum);
      if (s.mode == Mode::Network && !s.desync_end)
      {
        // A desync does not heal: the machines play different games from here on, and one
        // of them may reach game set while the other plays on and waits for inputs that never
        // come. End the game on both (the peer detects it too; the message makes sure).
        s.desync_end = true;
        WARN_LOG_FMT(BRAWLBACK, "gprb: the game states differ from frame {}: the game ends",
                     sev[i]->data.desynced.frame);
        SendDesync();
      }
      break;
    default:
      break;
    }
  }

  int num_adv = 0;
  bool pending_load = false;
  int load_frame = 0;
  int load_before = -1;
  bool initial_save = false;
  unsigned int* initial_checksum = nullptr;
  for (int i = 0; i < count; ++i)
  {
    auto& e = *events[i];
    switch (e.type)
    {
    case GekkoSaveEvent:
      if (e.data.save.state_len)
        *e.data.save.state_len = sizeof(u32);
      if (num_adv > 0)
      {
        s.ops.save_after[num_adv - 1] = true;
        s.ops.checksum_ptr[num_adv - 1] = e.data.save.checksum;
      }
      else
      {
        initial_save = true;
        initial_checksum = e.data.save.checksum;
      }
      break;
    case GekkoLoadEvent:
      pending_load = true;
      load_frame = e.data.load.frame;
      break;
    case GekkoAdvanceEvent:
      if (num_adv >= MAX_ADVANCE)
        break;
      if (pending_load && load_before < 0)
        load_before = num_adv;
      pending_load = false;
      s.ops.adv_frame[num_adv] = e.data.adv.frame;
      {
        // Ports nobody plays read as "no controller" on both machines, whatever is plugged in.
        PadSlots slots{};
        for (u32 port = 0; port < 4; ++port)
          slots[port * INPUT_SIZE + 0x38] = 0xFF;  // gfPadError NO_CONTROLLER
        if (e.data.adv.inputs)
        {
          for (int h = 0; h < s.num_players; ++h)
          {
            const int port = s.handle_port[h];
            if (port >= 0)
            {
              std::memcpy(slots.data() + port * INPUT_SIZE, e.data.adv.inputs + h * INPUT_SIZE,
                          INPUT_SIZE);
              // The remote player's bytes come straight from the network: every player's input
              // is sanitised the same way on both machines before it reaches the pad slots.
              PeerData::SanitizePad(slots.data() + port * INPUT_SIZE);
            }
          }
        }
        s.ops.slots[num_adv] = slots;
      }
      ++num_adv;
      break;
    default:
      break;
    }
  }

  if (s.mode == Mode::SyncTest && s.st_opts.no_rollback && num_adv > 1)
  {
    // Ground truth: only the newest frame runs, from the state the previous update left.
    s.ops.adv_frame[0] = s.ops.adv_frame[num_adv - 1];
    s.ops.save_after[0] = s.ops.save_after[num_adv - 1];
    s.ops.slots[0] = s.ops.slots[num_adv - 1];
    num_adv = 1;
  }
  if (s.mode == Mode::SyncTest && s.st_opts.no_rollback)
    load_before = -1;
  s.last_load_back = 0;
  s.last_initial_save = initial_save;
  if (load_before >= 0 && num_adv > 0)
  {
    // The load names the frame whose end state to restore; our newest slot holds the end of the
    // frame before this update's normal advance (see NetPlayClient::HandleGekkoFrame).
    const int normal_frame = s.ops.adv_frame[num_adv - 1];
    const int frames_back = (normal_frame - 1) - load_frame;
    auto& rbm = Rollback::RollbackManager::Get();
    if (frames_back >= 1 && frames_back <= MAX_ROLLBACK_FRAMES && rbm.m_ring_count >= 2 &&
        frames_back < rbm.m_ring_count)
    {
      BeginBurst(system);
      rbm.LoadFrame(system, frames_back);
      ++s.rollbacks;
      s.max_rollback = std::max<u64>(s.max_rollback, static_cast<u64>(frames_back));
      s.iteration = 0;
      s.last_load_back = frames_back;
    }
    else if (frames_back != 0)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb: cannot roll back {} frames (ring {})", frames_back,
                   rbm.m_ring_count);
    }
  }
  if (initial_save && s.defer_initial_save)
  {
    // The countdown's handshake (PumpGekkoInCountdown): frame 0 is saved at the start barrier.
    s.initial_save_deferred = true;
    s.deferred_initial_checksum = initial_checksum;
  }
  else if (initial_save)
  {
    InitialSave(system, initial_checksum);
  }
  s.ops.adv_count = num_adv;
  if (num_adv == 0)
    ++s.stall_polls;
  if (num_adv > 1)
    s.frames_resimulated += static_cast<u64>(num_adv - 1);
  if (num_adv > 0)
    s.current_frame = s.ops.adv_frame[num_adv - 1];
  if (s.mode == Mode::Network)
    s.frames_ahead = gekko_frames_ahead(s.gekko);
  return true;
}

void UpdateTimeSync(Core::System& system)
{
  if (s.mode != Mode::Network || s.current_frame < 0)
    return;
  const u32 frame = static_cast<u32>(s.current_frame);
  if (frame % 15 != 0 || frame == s.last_timesync_frame)
    return;
  s.last_timesync_frame = frame;
  const float ahead = s.frames_ahead;
  double factor = 1.0;
  if (ahead > 0.25f)
    factor = 1.0 - std::min(ahead / 2.0f, 1.0f) * 0.02;
  else if (ahead < -0.25f)
    factor = 1.0 + std::min(-ahead / 2.0f, 1.0f) * 0.01;
  s.speed_factor = factor;
}

// Slippi's ping line (SlippiNetplayClient, ack packets): the last round trip to the opponent in ms,
// cyan, under the FPS box. Refreshed every PING_DISPLAY_INTERVAL frames and left up for
// Duration::NORMAL, so it stays on screen through the match and fades out after it.
void UpdatePingDisplay()
{
  if (s.mode != Mode::Network || s.remote_handle < 0 || s.current_frame < s.next_ping_display_frame)
    return;
  s.next_ping_display_frame = s.current_frame + PING_DISPLAY_INTERVAL;
  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING))
    return;

  GekkoNetworkStats stats{};
  if (!WorstLink(&stats))
    return;
  OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, fmt::format("Ping: {}", stats.last_ping),
                       OSD::Duration::NORMAL, OSD::Color::CYAN);
}

void CaptureLocalInputs()
{
  const PadSlots latest = PadsLatestRaw();
  if (s.mode == Mode::SyncTest)
  {
    std::memcpy(s.local_inputs.data(), latest.data(), latest.size());
  }
  else
  {
    // The local player's controller (local port `local_pad`) plays the in-game port of this peer.
    const int port = s.local_slot;
    std::memcpy(s.local_inputs.data() + port * INPUT_SIZE, latest.data() + s.local_pad * INPUT_SIZE,
                INPUT_SIZE);
    // Real input never carries the gone marker.
    if (s.gone_enabled)
      PeerData::ClearGoneMarkerBytes(s.local_inputs.data() + port * INPUT_SIZE);
  }
}

void SetLoopDefault(PowerPC::PowerPCState& ppc)
{
  ppc.gpr[25] = 1;  // the replaced li r25,1
  ppc.npc = LOOP_TOP + 4;
}

void Spin(PowerPC::PowerPCState& ppc)
{
  // Skip this pass through the loop without running any game logic.
  ppc.gpr[25] = 1;
  ppc.npc = LOOP_END;
  s.spinning = true;
}

bool CpuRunning(Core::System& system)
{
  return system.GetCPU().GetState() == CPU::State::Running;
}

// Waits on the host (not in guest code, so emulated time stands still) until `pred` holds.
template <typename Pred>
bool HostWait(Core::System& system, Pred pred, std::chrono::milliseconds max)
{
  s.cpu_where = "hostwait";
  const auto deadline = Clock::now() + max;
  while (!pred())
  {
    if (!CpuRunning(system) || Clock::now() > deadline)
      return false;
    s.mutex.unlock();
    // A sub-millisecond sleep_for can take a whole scheduler quantum (1-2 ms or more on Windows);
    // the precision timer keeps the poll at about 100 us.
    static Common::PrecisionTimer timer;
    timer.SleepUntil(Clock::now() + std::chrono::microseconds(100));
    s.mutex.lock();
  }
  return true;
}

// Slippi's start-of-match time sync (CEXISlippi::shouldSkipOnlineFrame): in the first
// HARD_SYNC_UNTIL session frames, every HARD_SYNC_INTERVAL, a peer that is a frame or more ahead
// of the other waits on the host for the whole frames it is ahead (at most HARD_SYNC_MAX_FRAMES).
// The speed nudge alone (UpdateTimeSync, at most -2% / +1%) takes seconds to close a gap of a
// few frames, and the peer that is ahead rolls back on nearly every frame meanwhile.
constexpr s64 HARD_SYNC_UNTIL = 120;
constexpr s64 HARD_SYNC_INTERVAL = 30;
constexpr int HARD_SYNC_MAX_FRAMES = 5;
constexpr double FRAME_US = 1e6 * 1001.0 / 60000.0;

void HardTimeSync(Core::System& system)
{
  if (s.mode != Mode::Network || !s.gekko || s.current_frame <= 0 ||
      s.current_frame > HARD_SYNC_UNTIL || s.current_frame % HARD_SYNC_INTERVAL != 0 ||
      s.current_frame == s.last_hard_sync_frame)
  {
    return;
  }
  s.last_hard_sync_frame = s.current_frame;
  const float ahead = s.frames_ahead;
  if (ahead < 1.0f)
    return;
  const int frames = std::min(static_cast<int>(std::lround(ahead)), HARD_SYNC_MAX_FRAMES);
  const auto t0 = Clock::now();
  const auto until = t0 + std::chrono::microseconds(static_cast<s64>(frames * FRAME_US));
  HostWait(
      system,
      [&] {
        gekko_network_poll(s.gekko);
        return Clock::now() >= until;
      },
      std::chrono::milliseconds(frames * 17 + 100));
  system.GetCoreTiming().ResetThrottleToNow();
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  ++s.net_window.stalls;
  s.net_window.stall_ms += ms;
  s.net_window.stall_ms_max = std::max(s.net_window.stall_ms_max, ms);
  INFO_LOG_FMT(BRAWLBACK, "gprb: time sync at frame {}: {:.2f} frames ahead, held {} frames ({:.1f} ms)",
               s.current_frame, ahead, frames, ms);
}

// The first seconds of a network session in more detail than the NET_STATS_WINDOW lines: the
// totals so far at these session frames.
void StartTelemetry()
{
  static constexpr std::array<s64, 7> AT{30, 60, 120, 180, 300, 600, 900};
  if (s.mode != Mode::Network || std::find(AT.begin(), AT.end(), s.current_frame) == AT.end() ||
      s.current_frame == s.last_start_log_frame)
  {
    return;
  }
  s.last_start_log_frame = s.current_frame;
  std::string link;
  if (GekkoNetworkStats st{}; WorstLink(&st))
  {
    link = fmt::format(", ping {} ms (avg {:.1f}, jitter {:.1f})", st.last_ping, st.avg_ping,
                       st.jitter);
  }
  INFO_LOG_FMT(BRAWLBACK,
               "gprb net start: frame {}: {:.2f} frames ahead, speed {:.4f}, rollbacks {} "
               "(resimulated {}, max depth {}), waits for the peer {}{}",
               s.current_frame, s.frames_ahead, s.speed_factor, s.rollbacks, s.frames_resimulated,
               s.max_rollback, s.stall_polls, link);
}

// Countdown (network): GekkoNet's handshake (a sync request and NUM_TO_SYNC responses, about five
// round trips) runs here, while the countdown plays, instead of at the start barrier, where the
// game stood still for it right after GO. The update that sees the peer synced may return frame
// 0's save (held back for the start barrier, see defer_initial_save) and advance events (kept for
// the first RunFrame); after that only the network is polled. False: the session failed.
bool PumpGekkoInCountdown(Core::System& system)
{
  if (!s.gekko)
    return true;
  if (s.gekko_started)
  {
    gekko_network_poll(s.gekko);
    return true;
  }
  int count = 0;
  GekkoGameEvent** events = gekko_update_session(s.gekko, &count);
  if (!ProcessGekkoUpdate(system, events, count))
    return false;
  if (s.ops.adv_count > 0)
  {
    s.prefetched_ops = s.ops;
    s.ops_prefetched = true;
  }
  if (s.gekko_started)
  {
    INFO_LOG_FMT(BRAWLBACK, "gprb: GekkoNet handshake done in the countdown ({:.0f} ms)",
                 std::chrono::duration<double, std::milli>(Clock::now() - s.barrier_since).count());
  }
  return true;
}

bool IsSimStart(const Guest& g, u32* frame)
{
  const u32 fc = g.U32(Addr::GAME_FRAME + 4).value_or(0);
  const u32 pc = g.U32(Addr::GAME_FRAME + 0x14).value_or(0);
  *frame = fc;
  return fc == 1 && pc > fc;
}

u32 StartFrame()
{
  return s.mode == Mode::SyncTest ? s.st_opts.start_frame : s.net_opts.start_frame;
}

// Countdown: every port reads as a neutral, connected GameCube controller (ports nobody plays as
// unplugged), identically on both peers, and nothing is rolled back.
PadSlots NeutralSlots()
{
  PadSlots slots{};
  const u32 ports = s.mode == Mode::SyncTest ? s.st_opts.ports : s.match_ports ? s.match_ports : 0x3u;
  for (u32 port = 0; port < 4; ++port)
  {
    if (!(ports & (1u << port)))
      slots[port * INPUT_SIZE + 0x38] = 0xFF;  // gfPadError NO_CONTROLLER
  }
  return slots;
}

// Pass log / replay format (little-endian host order): a header
//   u32 'GPRH', u8 game_frame[0x18], u32 rng[3], u32 serial
// then per GekkoNet update with at least one advance
//   u32 'GPRU', i32 load_back, u8 initial_save, u8 adv_count, then adv_count times
//   i32 frame, u8 save_after, u8 slots[0x100]
constexpr u32 PASS_LOG_HEADER = 0x48525047;  // "GPRH"
constexpr u32 PASS_LOG_UPDATE = 0x55525047;  // "GPRU"

void PassLogOpen(Core::System& system)
{
  static const char* path = std::getenv("PPR_GPRB_PASS_LOG");
  // Network sessions and sync tests (not replays): a sync test's passes replay the same way.
  if (!path || (s.mode != Mode::Network && s.mode != Mode::SyncTest) ||
      (s.mode == Mode::SyncTest && !s.st_opts.replay_path.empty()))
  {
    return;
  }
  const std::string role = s.mode == Mode::SyncTest ? "synctest" :
                           IsHost()                 ? "host" :
                           s.ported                 ? fmt::format("p{}", s.local_slot + 1) :
                                                      "join";
  s.pass_log = std::fopen(fmt::format("{}.{}.m{}", path, role, s.match_index).c_str(), "wb");
  if (!s.pass_log)
    return;
  const SyncBlock b = ReadSyncBlock(system);
  std::fwrite(&PASS_LOG_HEADER, 4, 1, s.pass_log);
  std::fwrite(b.game_frame.data(), 1, b.game_frame.size(), s.pass_log);
  std::fwrite(b.rng.data(), 4, 3, s.pass_log);
  std::fwrite(&b.serial, 4, 1, s.pass_log);
}

void PassLogWrite()
{
  if (!s.pass_log || s.ops.adv_count <= 0)
    return;
  const s32 load_back = s.last_load_back;
  const u8 initial = s.last_initial_save ? 1 : 0;
  const u8 n = static_cast<u8>(s.ops.adv_count);
  std::fwrite(&PASS_LOG_UPDATE, 4, 1, s.pass_log);
  std::fwrite(&load_back, 4, 1, s.pass_log);
  std::fwrite(&initial, 1, 1, s.pass_log);
  std::fwrite(&n, 1, 1, s.pass_log);
  for (int i = 0; i < s.ops.adv_count; ++i)
  {
    const s32 frame = s.ops.adv_frame[i];
    const u8 save = s.ops.save_after[i] ? 1 : 0;
    std::fwrite(&frame, 4, 1, s.pass_log);
    std::fwrite(&save, 1, 1, s.pass_log);
    std::fwrite(s.ops.slots[i].data(), 1, s.ops.slots[i].size(), s.pass_log);
  }
}

void PassLogClose()
{
  if (s.pass_log)
    std::fclose(s.pass_log);
  s.pass_log = nullptr;
  if (s.replay)
    std::fclose(s.replay);
  s.replay = nullptr;
}

// Replay: the next recorded update. The first call also applies the header's RNG and frame
// counters (the recording session's values at its start barrier).
bool ReplayNextUpdate(Core::System& system)
{
  s.ops.Clear();
  if (!s.replay_started)
  {
    s.replay_started = true;
    u32 magic = 0;
    SyncBlock b;
    if (std::fread(&magic, 4, 1, s.replay) != 1 || magic != PASS_LOG_HEADER ||
        std::fread(b.game_frame.data(), 1, b.game_frame.size(), s.replay) != b.game_frame.size() ||
        std::fread(b.rng.data(), 4, 3, s.replay) != 3 || std::fread(&b.serial, 4, 1, s.replay) != 1)
    {
      return false;
    }
    const Guest g(system.GetMemory());
    b.app_counter = g.Ptr32(Addr::APPLICATION_PTR) ?
                        g.U32(*g.Ptr32(Addr::APPLICATION_PTR) + Addr::APP_FRAME_COUNTER_OFF).value_or(0) :
                        0;
    WriteSyncBlock(system, b);
  }
  u32 magic = 0;
  s32 load_back = 0;
  u8 initial = 0, n = 0;
  if (std::fread(&magic, 4, 1, s.replay) != 1 || magic != PASS_LOG_UPDATE ||
      std::fread(&load_back, 4, 1, s.replay) != 1 || std::fread(&initial, 1, 1, s.replay) != 1 ||
      std::fread(&n, 1, 1, s.replay) != 1 || n > MAX_ADVANCE)
  {
    return false;
  }
  for (int i = 0; i < n; ++i)
  {
    s32 frame = 0;
    u8 save = 0;
    if (std::fread(&frame, 4, 1, s.replay) != 1 || std::fread(&save, 1, 1, s.replay) != 1 ||
        std::fread(s.ops.slots[i].data(), 1, s.ops.slots[i].size(), s.replay) != s.ops.slots[i].size())
    {
      return false;
    }
    s.ops.adv_frame[i] = frame;
    s.ops.save_after[i] = save != 0;
  }
  auto& rbm = Rollback::RollbackManager::Get();
  if (load_back > 0)
  {
    if (load_back < rbm.m_ring_count)
    {
      BeginBurst(system);
      rbm.LoadFrame(system, load_back);
      ++s.rollbacks;
      s.max_rollback = std::max<u64>(s.max_rollback, static_cast<u64>(load_back));
    }
    else
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb replay: cannot roll back {} frames (ring {})", load_back,
                   rbm.m_ring_count);
    }
  }
  if (initial)
    rbm.SaveFrame(system);
  s.ops.adv_count = n;
  if (n > 1)
    s.frames_resimulated += static_cast<u64>(n - 1);
  if (n > 0)
    s.current_frame = s.ops.adv_frame[n - 1];
  return true;
}

void StartRunning(Core::System& system)
{
  // Before the first frame (and its base snapshot): GPUDeterminismMode "auto" turns the
  // deterministic GPU thread on for the session; the menus run without it.
  system.GetFifo().SetRollbackSessionDeterminism(true);
  ResetRunStats();
  PassLogOpen(system);
  if (s.mode == Mode::SyncTest && !s.st_opts.replay_path.empty())
  {
    s.replay = std::fopen(s.st_opts.replay_path.c_str(), "rb");
    s.replay_started = false;
    if (!s.replay)
      ERROR_LOG_FMT(BRAWLBACK, "gprb replay: cannot open {}", s.st_opts.replay_path);
  }
  if (s.ops_prefetched)
  {
    s.ops = s.prefetched_ops;
    if (s.ops.adv_count > 0)
      s.current_frame = s.ops.adv_frame[s.ops.adv_count - 1];
  }
  s.phase = Phase::Running;
  s.frames = 0;
  INFO_LOG_FMT(BRAWLBACK, "gprb: session running ({}; dual core {}, deterministic GPU thread {})",
               s.mode == Mode::SyncTest ? "sync test" :
               !s.ported ? std::string(IsHost() ? "host" : "joiner") :
                                          fmt::format("{} P{} of {} players", IsHost() ? "host" : "joiner",
                                                      s.local_slot + 1, s.num_players),
               system.IsDualCoreMode(),
               system.GetFifo().UseDeterministicGPUThread());
  if (system.IsDualCoreMode() && !system.GetFifo().UseDeterministicGPUThread())
  {
    ERROR_LOG_FMT(BRAWLBACK, "gprb: dual core without the deterministic GPU thread (GPUDeterminismMode "
                             "\"{}\"): rollbacks can desync the GPU FIFO and hang. Use fake-completion.",
                  Config::Get(Config::MAIN_GPU_DETERMINISM_MODE));
  }
}

// 3-4 player matches, at the loop top of every pass (first runs and resimulations), before the
// frame's game logic: the ports whose input carries the gone marker (GekkoNet's input for a player
// who left, from the frame the remaining peers agreed on) get a neutral controller, and the game's
// gone flags (SESSION + GONE_FLAG_OFFSET, inside the region set) say which ports are gone in
// this frame. Every peer computes them from the same confirmed input, so they agree; a rollback
// that moves the agreed frame resimulates the frames in between with the corrected flags.
void WriteGoneFlags(Core::System& system, PadSlots* slots, s64 frame)
{
  std::array<u8, MAX_LOBBY_PLAYERS> gone{};
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if (!(s.match_ports & (1u << port)) || port == s.local_slot)
      continue;
    gone[port] = PeerData::TakeGoneMarker(slots->data() + port * INPUT_SIZE) ? 1 : 0;
    PeerView& p = s.peers[port];
    if (gone[port] && (p.gone_frame < 0 || frame < p.gone_frame))
    {
      p.gone_frame = frame;
      INFO_LOG_FMT(BRAWLBACK, "gprb: P{} gone from session frame {} (game frame {})", port + 1,
                   frame, frame + StartFrame());
    }
    else if (!gone[port] && p.gone_frame >= 0 && frame >= p.gone_frame)
    {
      // GekkoNet raised the agreed frame (a peer held more of its input): rolled back and
      // simulated again with its real input.
      INFO_LOG_FMT(BRAWLBACK, "gprb: P{} plays at session frame {} after all (was gone from {})",
                   port + 1, frame, p.gone_frame);
      p.gone_frame = -1;
    }
  }
  if (s.gone_addr)
    system.GetMemory().CopyToEmu(s.gone_addr, gone.data(), gone.size());
  s.gone_written = gone;
}

bool RunFrame(const Core::CPUThreadGuard& guard)
{
  s.cpu_where = "runframe";
  s.cpu_frame = s.current_frame;
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  auto& ct = system.GetCoreTiming();
  const Guest g(system.GetMemory());

  if (!IsSceneMelee(g))
  {
    EndRunning(system, "left the match scene");
    SetLoopDefault(ppc);
    return true;
  }

  PerformQueuedSave(system);

  // Diagnostics (PPR_GPRB_SYNC_GPU=1): in dual core, wait at every loop top until the video thread
  // has executed everything the CPU thread has sent, so the GPU thread never lags behind a frame.
  static const bool sync_gpu = std::getenv("PPR_GPRB_SYNC_GPU") != nullptr;
  if (sync_gpu && system.IsDualCoreMode())
    system.GetFifo().SyncGPU(Fifo::SyncGPUReason::Other, true);

  if (s.iteration == 0 && s.replay)
  {
    if (!ReplayNextUpdate(system))
    {
      EndRunning(system, "replay ended");
      SetLoopDefault(ppc);
      return true;
    }
  }
  else if (s.iteration == 0 && s.ops_prefetched)
  {
    s.ops_prefetched = false;  // StartRunning installed the start barrier's events
    PassLogWrite();
  }
  else if (s.iteration == 0)
  {
    ApplyPeerDrops();
    HardTimeSync(system);
    CaptureLocalInputs();
    if (s.mode == Mode::Network)
    {
      if (const double age = PadsLatestAgeMs(); age >= 0)
      {
        s.net_window.pad_age_sum += age;
        s.net_window.pad_age_max = std::max(s.net_window.pad_age_max, age);
        ++s.net_window.pad_age_n;
      }
    }
    if (!HandleGekkoFrame(system))
    {
      EndRunning(system, s.error.empty() ? "gekko session failed" : s.error);
      if (s.mode == Mode::Network && s.error == "peer disconnected")
      {
        s.phase = Phase::Ended;
        s.disconnected = true;
      }
      SetLoopDefault(ppc);
      return true;
    }
    if (s.ops.adv_count == 0)
    {
      // The peer is behind: wait on the host, polling GekkoNet, instead of spinning guest code.
      const auto wait_t0 = Clock::now();
      HostWait(
          system,
          [&] {
            if (!HandleGekkoFrame(system))
              return true;
            return s.ops.adv_count > 0;
          },
          std::chrono::milliseconds(2000));
      ct.ResetThrottleToNow();
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - wait_t0).count();
      ++s.net_window.stalls;
      s.net_window.stall_ms += ms;
      s.net_window.stall_ms_max = std::max(s.net_window.stall_ms_max, ms);
    }
    UpdateTimeSync(system);
    StartTelemetry();
    UpdatePingDisplay();
    PassLogWrite();
  }

  if (s.ops.adv_count == 0 || s.iteration >= s.ops.adv_count)
  {
    ct.SetRollbackResimulating(false);
    s.iteration = 0;
    Spin(ppc);
    return true;
  }

  const int i = s.iteration;
  s.resim_pass = s.ops.adv_count > 1 && i != s.ops.adv_count - 1;

  // Match end: the state at this loop top is the end of the previous frame.
  {
    const s64 prev_frame = static_cast<s64>(s.ops.adv_frame[i]) - 1;
    const bool game_set = IsGameSet(g);
    const bool quit = !game_set && IsQuitRequested(g);
    if (game_set || quit)
    {
      if (s.game_set_frame < 0 || prev_frame < s.game_set_frame)
      {
        s.game_set_frame = prev_frame;
        s.game_set_quit = quit;
      }
    }
    else if (s.game_set_frame >= 0 && prev_frame <= s.game_set_frame)
    {
      s.game_set_frame = -1;  // a rollback undid the game set
    }
    if (s.game_set_frame >= 0 && !s.resim_pass &&
        static_cast<s64>(s.ops.adv_frame[i]) >= s.game_set_frame + END_AFTER_GAME_SET)
    {
      EndRunning(system, s.game_set_quit ? "quit" : "game set");
      SetLoopDefault(ppc);
      return true;
    }
  }
  {
    // Sound bookkeeping: is this the first run of this frame?
    const s64 f = static_cast<s64>(s.ops.adv_frame[i]);
    const size_t slot = static_cast<size_t>(f) % s.snd_played.size();
    s.snd_frame = f;
    ++s.pass_serial;
    s.snd_first_run = f > s.snd_max_frame;
    if (s.snd_first_run)
    {
      s.snd_max_frame = f;
      s.snd_remaining.clear();
    }
    else
    {
      s.snd_remaining =
          s.snd_tag[slot] == f ? s.snd_played[slot] : std::vector<State::SndEntry>{};
    }
    s.snd_new.clear();
    s.snd_pass_open = true;
  }
  ct.SetRollbackResimulating(s.resim_pass);
  ct.SetRollbackSpeedAdjustment(s.speed_factor);
  if (!s.resim_pass)
  {
    Rollback::PresentStats::OnDisplayedFrameStart(s.ops.adv_count > 1);
    ++s.frames;
    NetStatsOnDisplayedFrame(system);
  }
  PadSlots slots = s.ops.slots[i];
  if (s.gone_enabled)
    WriteGoneFlags(system, &slots, static_cast<s64>(s.ops.adv_frame[i]));
  if (s.mode == Mode::SyncTest && s.st_opts.inject_input)
  {
    // The frame this pass produces (a load above may have rewound the counter).
    const u32 gf = g.U32(Addr::GAME_FRAME + 4).value_or(0) + 1;
    const auto take = [&](u32 frame, u32 ports) {
      const auto inj = PadsInjectedFor(frame);
      if (!inj)
        return false;
      for (u32 port = 0; port < 4; ++port)
      {
        if (ports & (1u << port))
          std::memcpy(slots.data() + port * INPUT_SIZE, inj->data() + port * INPUT_SIZE, INPUT_SIZE);
      }
      return true;
    };
    take(gf, s.st_opts.ports);
    const u32 mp = s.st_opts.mispredict_ports & s.st_opts.ports;
    if (mp && s.snd_first_run && s.st_opts.mispredict_every > 0 &&
        gf % static_cast<u32>(s.st_opts.mispredict_every) == 0 &&
        take(static_cast<u32>(static_cast<s64>(gf) + s.st_opts.mispredict_offset), mp))
    {
      ++s.mispredicted_passes;
    }
  }
  PadsSetCurrent(0xF, slots);
  PadsOnLoopTop(system);
  if (s.mode == Mode::SyncTest && std::getenv("PPR_GPRB_INPUT_LOG"))
  {
    u32 b0, b1;
    std::memcpy(&b0, s.ops.slots[i].data(), 4);
    std::memcpy(&b1, s.ops.slots[i].data() + INPUT_SIZE, 4);
    INFO_LOG_FMT(BRAWLBACK, "gprb input: frame {} iter {}/{} resim {} game frame {} p0 {:08x} p1 {:08x}",
                 s.ops.adv_frame[i], i, s.ops.adv_count, s.resim_pass,
                 g.U32(Addr::GAME_FRAME + 4).value_or(0), Common::swap32(b0), Common::swap32(b1));
  }
  // A load above may have rewound the frame counter: let an armed trace see the frame this pass
  // really produces.
  CpuTraceOnLoopTop(system);
  SetLoopDefault(ppc);
  s.cpu_where = "guest";
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Control API

std::optional<std::string> ArmSyncTest(const SyncTestOptions& options)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.phase == Phase::Running || s.phase == Phase::Barrier || s.phase == Phase::Countdown ||
      s.phase == Phase::StartBarrier)
    return "a session is running";
  if (options.distance < 1 || options.distance > MAX_ROLLBACK_FRAMES)
    return fmt::format("distance must be 1..{}", MAX_ROLLBACK_FRAMES);
  RegionSetSpec spec;
  if (options.region_set != "whole")
  {
    if (auto err = LoadRegionSet(options.region_set, &spec))
      return err;
  }
  CloseNetwork();
  s.mode = Mode::SyncTest;
  s.st_opts = options;
  s.phase = Phase::Armed;
  s.error.clear();
  return std::nullopt;
}

std::optional<std::string> Connect(const ConnectOptions& options)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.phase == Phase::Running || s.phase == Phase::Barrier || s.phase == Phase::Countdown ||
      s.phase == Phase::StartBarrier)
    return "a session is running";
  RegionSetSpec spec;
  if (options.region_set != "whole")
  {
    if (auto err = LoadRegionSet(options.region_set, &spec))
      return err;
  }
  // The players: a 1v1 (local_slot -1: the host P1, the joiner P2), or this player's port and
  // every other player's (2-4 players, gaps allowed).
  std::array<PeerView, MAX_LOBBY_PLAYERS> peers{};
  int local_slot = options.host ? 0 : 1;
  int host_slot = options.host ? 0 : -1;
  const bool ported = options.local_slot >= 0;
  if (!ported)
  {
    PeerView& p = peers[options.host ? 1 : 0];
    p.expected = true;
    if (options.host)
    {
      // With a known joiner address (matchmaking), send from the start, as Slippi connects from
      // both sides: it keeps the NAT mapping toward the joiner open.
      if (!options.remote_host.empty() && options.remote_port != 0)
      {
        if (const auto ip = sf::IpAddress::resolve(options.remote_host))
        {
          p.ip = *ip;
          p.port = options.remote_port;
        }
      }
    }
    else
    {
      const auto ip = sf::IpAddress::resolve(options.remote_host);
      if (!ip || options.remote_port == 0)
        return "joiner needs remote_host and remote_port";
      p.ip = *ip;
      p.port = options.remote_port;
      p.exact = true;
      p.confirmed = true;
      host_slot = 0;
    }
  }
  else
  {
    if (!PeerData::ValidPort(static_cast<u64>(options.local_slot)))
      return "local_slot must be 0-3";
    local_slot = options.local_slot;
    host_slot = options.host ? local_slot : -1;
    if (options.peers.empty() || options.peers.size() >= static_cast<size_t>(MAX_LOBBY_PLAYERS))
      return "a session has 2-4 players";
    for (const auto& c : options.peers)
    {
      if (c.slot < 0 || c.slot >= MAX_LOBBY_PLAYERS ||
          c.slot == local_slot || peers[c.slot].expected)
      {
        return fmt::format("bad peer port {}", c.slot);
      }
      PeerView& p = peers[c.slot];
      p.expected = true;
      if (!c.host.empty())
      {
        const auto ip = sf::IpAddress::resolve(c.host);
        if (!ip)
          return fmt::format("cannot resolve P{}'s address {}", c.slot + 1, c.host);
        p.ip = *ip;
        p.port = c.port;
      }
    }
    if (!options.host && options.host_slot >= 0)
    {
      if (options.host_slot >= MAX_LOBBY_PLAYERS || !peers[options.host_slot].expected)
        return "host_slot is not one of the peers";
      host_slot = options.host_slot;
    }
  }
  CloseNetwork();
  s.mode = Mode::Network;
  s.net_opts = options;
  s.peers = peers;
  s.local_slot = local_slot;
  s.host_slot = host_slot;
  s.ported = ported;
  s.have_seed = options.host;
  s.match_ports = 0;
  s.gone_enabled = false;
  s.gone_addr = 0;
  s.error.clear();
  s.at_s = s.applied = s.at_start = s.go = false;
  s.countdown_at.reset();
  s.did_frame0 = false;
  s.match_index = 0;
  s.setup = {};
  s.input_delay = DEFAULT_INPUT_DELAY;
  s.last_winner = 0xFF;
  s.last_pickers = 0;
  s.setup_error = 0;
  s.last_stage = NO_STAGE;
  s.stage_pool.clear();
  s.await_scene_exit = false;
  s.foreign_packets = 0;
  s.peer_left = false;
  s.peer_left_reason.clear();
  s.disconnected = false;
  s.desync_end = false;
  s.desynced = false;
  s.socket = std::make_unique<sf::UdpSocket>();
  s.socket->setBlocking(false);
  if (s.socket->bind(options.local_port) != sf::Socket::Status::Done)
  {
    s.socket.reset();
    return fmt::format("cannot bind UDP port {}", options.local_port);
  }
  if (options.host)
  {
    std::random_device rd;
    s.session_seed = rd();
  }
  INFO_LOG_FMT(BRAWLBACK, "gprb: session as {} on P{} with {} other player(s)",
               options.host ? "host" : "joiner", s.local_slot + 1, ActivePlayers() - 1);
  s.phase = Phase::Connecting;
  s.net_run = true;
  s.net_thread = std::thread(NetThread);
  return std::nullopt;
}

std::optional<std::string> SetSelections(const picojson::object& selections)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  s.local_selections = selections;
  return std::nullopt;
}

void SetLocalLock(const LockIn& lock_in)
{
  // The same port value bytes on both machines: the peer's are sanitised on receipt.
  LockIn lock = lock_in;
  PeerData::SanitizePortValues(lock.port_values);
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  const bool changed = lock.ready != s.local_lock.ready || lock.game != s.local_lock.game ||
                       lock.char_kind != s.local_lock.char_kind ||
                       lock.stage_pick != s.local_lock.stage_pick ||
                       lock.port_values != s.local_lock.port_values || lock.team != s.local_lock.team;
  s.local_lock = lock;
  if (changed)
  {
    INFO_LOG_FMT(BRAWLBACK,
                 "gprb lobby: local lock-in {} for game {} (char {:#x}/{}, stage {:#x}, tag {})",
                 lock.ready ? "ready" : "not ready", lock.game, lock.char_kind, lock.costume,
                 lock.stage_pick, (lock.port_values[0] & 1) ? "yes" : "no");
  }
  MaybeDecideSetup();
}

void SetGameResultCallback(std::function<void(const GameResult&)> callback)
{
  std::lock_guard lk(s_result_callback_mutex);
  s_result_callback = std::move(callback);
}

void SetStageDecider(std::function<StageDecision(u32 game)> decider)
{
  std::lock_guard lk(s_result_callback_mutex);
  s_stage_decider = std::move(decider);
}

void SetLocalExtra(const picojson::object& extra)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  s_local_extra = extra;
}

picojson::object GetPeerExtra()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  const int slot = FirstPeer();
  return slot >= 0 ? s.peers[slot].extra : picojson::object{};
}

LockIn GetPeerLock()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  const int slot = FirstPeer();
  return slot >= 0 ? s.peers[slot].lock : LockIn{};
}

std::optional<LockIn> GetPlayerLock(int port)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.mode != Mode::Network || port < 0 || port >= MAX_LOBBY_PLAYERS)
    return std::nullopt;
  if (port == s.local_slot)
    return s.local_lock;
  if (!s.peers[port].expected || s.peers[port].left)
    return std::nullopt;
  return s.peers[port].lock;
}

LockIn GetLocalLock()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  return s.local_lock;
}

Lobby GetLobby()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  Lobby l;
  l.active = s.mode == Mode::Network && s.phase != Phase::Idle && s.phase != Phase::Ended &&
             s.phase != Phase::Error;
  bool all_seen = true, all_ready = true;
  ForEachPeer([&](int, const PeerView& p) {
    if (p.left)
      return;
    all_seen = all_seen && p.seen;
    all_ready = all_ready && LockedFor(p.lock, NextGame());
  });
  l.connected = l.active && all_seen && !s.peer_left;
  l.in_match = InMatchPhase();
  l.disconnected = s.disconnected;
  l.desynced = s.desynced;
  l.local_port = s.mode == Mode::Network ? s.local_slot : -1;
  l.num_players = s.mode == Mode::Network ? ActivePlayers() : 0;
  l.game = NextGame();
  l.remote_ready = s.mode == Mode::Network && all_ready;
  l.setup_ready = s.setup.game != 0 && s.setup.game == l.game;
  if (l.setup_ready)
  {
    l.stage = s.setup.stage;
    l.asl = s.setup.asl;
    l.players = s.setup.players;
    l.teams = s.setup.teams;
  }
  l.last_winner = s.last_winner;
  l.stage_pickers = s.last_pickers;
  l.setup_error = s.setup_error;
  return l;
}

const std::vector<u16>& DefaultStages()
{
  // Page 0 of Switch00.rss with the random bit set, slot -> srStageKind through its slot table:
  // Battlefield, Final Destination, Delfino's Secret, Luigi's Mansion, Metal Cavern, Bowser's
  // Castle, Temple of Time, Frigate Husk, Yoshi's Island, Wario Land, Fountain of Dreams,
  // Smashville, Green Hill Zone, Dream Land, Pokemon Stadium 2.
  static const std::vector<u16> stages{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0C,
                                       0x0D, 0x1C, 0x1F, 0x21, 0x23, 0x2D, 0x2E};
  return stages;
}

bool InMatchPhase()
{
  return s.phase == Phase::Running || s.phase == Phase::Barrier || s.phase == Phase::Countdown ||
         s.phase == Phase::StartBarrier;
}

void Stop()
{
  {
    std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
    // Leaving tells the peer at once (Slippi's graceful ENet disconnect), so it does not wait for
    // the 7.2 s silence timeout. UDP: send it a few times.
    if (s.mode == Mode::Network && s.socket)
    {
      const std::string text = fmt::format(R"({{"t":"leave","v":1,"slot":{}}})", s.local_slot);
      std::vector<u8> pkt(text.size() + 1);
      pkt[0] = 'C';
      std::memcpy(pkt.data() + 1, text.data(), text.size());
      for (int i = 0; i < 3; ++i)
        SendToAll(pkt);
    }
    if (InMatchPhase())
    {
      // Ends on the CPU thread at the next loop top.
      s.end_reason = "stopped";
      s.mode = Mode::None;
    }
  }
  CloseNetwork();
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (!InMatchPhase())
  {
    s.mode = Mode::None;
    s.phase = Phase::Idle;
  }
}

picojson::value Status()
{
  std::unique_lock<std::recursive_timed_mutex> lk(s.mutex, std::chrono::milliseconds(1000));
  picojson::object o;
  o["cpu_where"] = picojson::value(std::string(s.cpu_where.load()));
  o["cpu_frame"] = picojson::value(static_cast<double>(s.cpu_frame.load()));
  if (!lk.owns_lock())
  {
    o["busy"] = picojson::value(true);
    o["phase"] = picojson::value(PhaseName(s.phase));
    o["current_frame"] = picojson::value(static_cast<double>(s.cpu_frame.load()));
    o["desync_log"] = picojson::value(picojson::array());
    return picojson::value(o);
  }
  o["mode"] = picojson::value(s.mode == Mode::SyncTest ? "synctest" :
                              s.mode == Mode::Network  ? "network" :
                                                         "none");
  o["phase"] = picojson::value(PhaseName(s.phase));
  o["error"] = picojson::value(s.error);
  o["disconnected"] = picojson::value(s.disconnected);
  o["desynced"] = picojson::value(s.desynced);
  o["peer_left"] = picojson::value(s.peer_left);
  o["role"] = picojson::value(s.mode == Mode::Network ? (IsHost() ? "host" : "joiner") : "");
  o["match_index"] = picojson::value(static_cast<double>(s.match_index));
  o["current_frame"] = picojson::value(static_cast<double>(s.current_frame));
  o["frames"] = picojson::value(static_cast<double>(s.frames));
  o["rollbacks"] = picojson::value(static_cast<double>(s.rollbacks));
  o["max_rollback_frames"] = picojson::value(static_cast<double>(s.max_rollback));
  o["frames_resimulated"] = picojson::value(static_cast<double>(s.frames_resimulated));
  o["stall_polls"] = picojson::value(static_cast<double>(s.stall_polls));
  o["desyncs_detected"] = picojson::value(static_cast<double>(s.desyncs));
  o["last_desync_frame"] = picojson::value(static_cast<double>(s.last_desync_frame));
  o["region_mismatches"] = picojson::value(static_cast<double>(s.region_mismatches));
  picojson::array log;
  for (const auto& l : s.region_mismatch_log)
    log.emplace_back(l);
  o["region_mismatch_log"] = picojson::value(log);
  picojson::array dlog;
  for (const auto& l : s.desync_log)
    dlog.emplace_back(l);
  o["desync_log"] = picojson::value(dlog);
  o["frames_ahead"] = picojson::value(static_cast<double>(s.frames_ahead));
  o["time_sync_speed"] = picojson::value(s.speed_factor);
  o["region_bytes"] = picojson::value(static_cast<double>(s.region_bytes));
  o["region_ranges"] = picojson::value(static_cast<double>(s.ranges.size()));
  o["end_reason"] = picojson::value(s.end_reason);
  o["end_game_frame"] = picojson::value(static_cast<double>(s.end_game_frame));
  o["game_set_frame"] = picojson::value(static_cast<double>(s.game_set_frame));
  o["sound_allocs"] = picojson::value(static_cast<double>(s.sound_allocs));
  o["resim_sound_allocs"] = picojson::value(static_cast<double>(s.resim_sound_allocs));
  o["suppressed_sound_allocs"] = picojson::value(static_cast<double>(s.suppressed_sound_allocs));
  o["sound_reattached"] = picojson::value(static_cast<double>(s.sound_reattached));
  o["sound_reattach_gone"] = picojson::value(static_cast<double>(s.sound_reattach_gone));
  o["sound_stopped"] = picojson::value(static_cast<double>(s.sound_stopped));
  o["sound_stop_gone"] = picojson::value(static_cast<double>(s.sound_stop_gone));
  o["sound_moved"] = picojson::value(static_cast<double>(s.sound_moved));
  o["io_waits"] = picojson::value(static_cast<double>(s.io_waits));
  o["io_wait_retraces"] = picojson::value(static_cast<double>(s.io_wait_retraces));
  o["io_wait_timeouts"] = picojson::value(static_cast<double>(s.io_wait_timeouts));
  o["sound_gone_why"] = picojson::value(picojson::array{
      picojson::value(static_cast<double>(s.snd_gone_why[0])),
      picojson::value(static_cast<double>(s.snd_gone_why[1])),
      picojson::value(static_cast<double>(s.snd_gone_why[2]))});
  o["await_scene_exit"] = picojson::value(s.await_scene_exit);
  o["mispredicted_passes"] = picojson::value(static_cast<double>(s.mispredicted_passes));
  {
    picojson::object skipped;
    for (const auto& [phys, n] : Rollback::RollbackManager::Get().PartialSkippedStats())
      skipped[fmt::format("{:08x}", phys | 0x80000000u)] = picojson::value(static_cast<double>(n));
    o["partial_granule_skips"] = picojson::value(skipped);
    o["partial_granules"] = picojson::value(
        static_cast<double>(Rollback::RollbackManager::Get().RegionPartialGranules()));
  }
  const auto t = Rollback::RollbackManager::Get().GetTimingStats();
  o["save_count"] = picojson::value(static_cast<double>(t.save_count));
  o["save_us_total"] = picojson::value(static_cast<double>(t.save_us_total));
  o["save_us_max"] = picojson::value(static_cast<double>(t.save_us_max));
  o["load_count"] = picojson::value(static_cast<double>(t.load_count));
  o["load_us_total"] = picojson::value(static_cast<double>(t.load_us_total));
  o["load_us_max"] = picojson::value(static_cast<double>(t.load_us_max));
  o["load_evict_waits"] = picojson::value(static_cast<double>(t.load_evict_waits));
  o["load_evict_wait_us_max"] = picojson::value(static_cast<double>(t.load_evict_wait_us_max));
  o["save_granules_total"] = picojson::value(static_cast<double>(t.save_granules_total));
  o["save_granules_max"] = picojson::value(static_cast<double>(t.save_granules_max));
  o["load_granules_total"] = picojson::value(static_cast<double>(t.load_granules_total));
  o["load_granules_max"] = picojson::value(static_cast<double>(t.load_granules_max));
  if (s.mode == Mode::Network)
  {
    // Every other player ("peers", by port), and the first of them as "peer" (a 1v1's peer).
    picojson::array peers;
    picojson::object first;
    ForEachPeer([&](int slot, const PeerView& q) {
      picojson::object p;
      p["slot"] = picojson::value(static_cast<double>(slot));
      p["seen"] = picojson::value(q.seen);
      p["name"] = picojson::value(q.name);
      p["selections"] = picojson::value(q.selections);
      p["at_s"] = picojson::value(q.at_s);
      p["rtt_ms"] = picojson::value(q.rtt_ms);
      p["setup_hash"] = picojson::value(fmt::format("{:016x}", q.setup_hash));
      p["left"] = picojson::value(q.left);
      p["left_reason"] = picojson::value(q.left_reason);
      p["confirmed"] = picojson::value(q.confirmed);
      p["address"] = picojson::value(q.ip ? fmt::format("{}:{}", q.ip->toString(), q.port) :
                                            std::string());
      p["handle"] = picojson::value(static_cast<double>(q.handle));
      p["gekko_dropped"] = picojson::value(q.gekko_dropped);
      p["gone_frame"] = picojson::value(static_cast<double>(q.gone_frame));
      p["lock"] = LockJson(q.lock);
      if (q.seen)
      {
        p["last_heard_ms"] = picojson::value(static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - q.last_heard)
                .count()));
      }
      if (first.empty())
        first = p;
      peers.emplace_back(p);
    });
    o["peer"] = picojson::value(first);
    o["peers"] = picojson::value(peers);
    o["local_slot"] = picojson::value(static_cast<double>(s.local_slot));
    o["host_slot"] = picojson::value(static_cast<double>(HostSlot()));
    o["players"] = picojson::value(static_cast<double>(ActivePlayers()));
    o["match_ports"] = picojson::value(static_cast<double>(s.match_ports));
    o["num_players"] = picojson::value(static_cast<double>(s.num_players));
    o["gone_flag_addr"] = picojson::value(static_cast<double>(s.gone_addr));
    picojson::array gone;
    for (const u8 g : s.gone_written)
      gone.emplace_back(static_cast<double>(g));
    o["gone_flags"] = picojson::value(gone);
    o["foreign_packets"] = picojson::value(static_cast<double>(s.foreign_packets));
    o["selections"] = picojson::value(s.local_selections);
    o["setup_hash"] = picojson::value(fmt::format("{:016x}", s.setup_hash));
    o["input_delay"] = picojson::value(static_cast<double>(s.input_delay));
    picojson::object lobby;
    lobby["game"] = picojson::value(static_cast<double>(NextGame()));
    lobby["lock"] = LockJson(s.local_lock);
    const int fp = FirstPeer();
    lobby["peer_lock"] = LockJson(fp >= 0 ? s.peers[fp].lock : LockIn{});
    lobby["setup"] = SetupJson(s.setup);
    lobby["last_winner"] = picojson::value(static_cast<double>(s.last_winner));
    lobby["stage_pickers"] = picojson::value(static_cast<double>(s.last_pickers));
    lobby["setup_error"] = picojson::value(static_cast<double>(s.setup_error));
    lobby["teams"] = picojson::value(s.net_opts.teams);
    lobby["last_stage"] = picojson::value(static_cast<double>(s.last_stage));
    picojson::array stages;
    for (const u16 st : AllowedStages())
      stages.emplace_back(static_cast<double>(st));
    lobby["stages"] = picojson::value(stages);
    lobby["stages_from_server"] = picojson::value(!s.net_opts.stages.empty());
    o["lobby"] = picojson::value(lobby);
    o["local_port"] = picojson::value(
        static_cast<double>(s.socket ? s.socket->getLocalPort() : 0));
  }
  return picojson::value(o);
}

picojson::value Checksums(s64 since)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  std::vector<FrameRecord> rows;
  for (const auto& r : s.history)
  {
    if (r.frame >= since && r.frame >= 0)
      rows.push_back(r);
  }
  std::sort(rows.begin(), rows.end(),
            [](const FrameRecord& a, const FrameRecord& b) { return a.frame < b.frame; });
  picojson::array out;
  for (const auto& r : rows)
  {
    picojson::array row;
    row.emplace_back(static_cast<double>(r.frame));
    row.emplace_back(static_cast<double>(r.checksum));
    for (u32 v : r.rng)
      row.emplace_back(static_cast<double>(v));
    row.emplace_back(static_cast<double>(r.fighters_crc));
    row.emplace_back(fmt::format("{:016x}", r.region_hash));
    row.emplace_back(static_cast<double>(r.game_set));
    out.emplace_back(row);
  }
  picojson::object o;
  o["rows"] = picojson::value(out);
  o["current_frame"] = picojson::value(static_cast<double>(s.current_frame));
  return picojson::value(o);
}

// ---------------------------------------------------------------------------------------------
// CPU-thread hooks


enum class MatchStart
{
  Done,
  Stopped,  // the emulation is stopping (the joiner was waiting for the host's setup)
  Error,
};

// The start of a match on this connection, before the match scene loads anything: the joiner
// takes the host's gmGlobalModeMelee init block (stage, rules, the clock-derived stage variant),
// both seed the three RNGs from the session seed and the match index, and the object serial
// counter starts from the same value. Runs at the first of: the loop top that sees the change to
// scMelee pending or the first one in scMelee, or the stage's constructor (OnStageCreate; some paths
// into the match construct the stage within the pass that switches the scene, and the
// constructor shuffles the fighters' start points with g_mtRand).
MatchStart ApplyMatchStart(Core::System& system, std::string_view when)
{
  const Guest g(system.GetMemory());
  const auto host_block_ready = [] {
    const PeerView* hp = HostPeer();
    return hp && hp->init_match == static_cast<s64>(s.match_index) && !hp->init_block.empty();
  };
  if (!IsHost())
  {
    const auto since = Clock::now();
    // (The host may leave meanwhile: the next host is then this player or another peer.)
    while (!IsHost() && !host_block_ready())
    {
      if (Clock::now() - since > std::chrono::seconds(60))
      {
        s.error = "the host's match setup never arrived";
        s.phase = Phase::Error;
        return MatchStart::Error;
      }
      if (!CpuRunning(system))
        return MatchStart::Stopped;
      s.mutex.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      s.mutex.lock();
    }
  }
  if (IsHost())
  {
    s.init_block = InitBlock(g);
    s.init_match = s.match_index;
  }
  else
  {
    const PeerView& host = *HostPeer();
    const auto mine = InitBlock(g);
    const auto mm = ModeMeleeAddr(g);
    // Only the stage variant may differ (PeerData::MergeInitBlock): the joiner never writes the
    // host's stage kind, rules or time limit into its game.
    bool rejected = false;
    const auto merged = PeerData::MergeInitBlock(mine, host.init_block, &rejected);
    if (rejected)
    {
      WARN_LOG_FMT(BRAWLBACK,
                   "gprb: the host's match init block differs beyond the stage variant; kept "
                   "ours ({} against the host's {})",
                   Hex(mine), Hex(host.init_block));
    }
    if (mm && merged != mine)
    {
      system.GetMemory().CopyToEmu(*mm + INIT_BLOCK_OFFSET, merged.data(), merged.size());
      INFO_LOG_FMT(BRAWLBACK, "gprb: joiner took the host's match init block ({} -> {})", Hex(mine),
                   Hex(merged));
    }
  }
  // Tests only (PPR_GPRB_TEST_TEAMS=t1,t2,t3,t4, a team per port or '-'): every machine makes the
  // match a team battle before it loads, as a team setup on the character select would. (The
  // online character select's teams are the game's, docs/nplayer/setup.md.)
  if (const char* teams = std::getenv("PPR_GPRB_TEST_TEAMS"); teams && *teams)
  {
    if (const auto mm = ModeMeleeAddr(g))
    {
      const u8 on = 1;
      system.GetMemory().CopyToEmu(*mm + MM_IS_TEAMS, &on, 1);
      std::string_view rest(teams);
      for (u32 port = 0; port < MAX_LOBBY_PLAYERS && !rest.empty(); ++port)
      {
        const size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        if (item.size() == 1 && item[0] >= '0' && item[0] <= '2')
        {
          const u8 team = static_cast<u8>(item[0] - '0');
          system.GetMemory().CopyToEmu(*mm + MM_PLAYERS + port * MM_PLAYER_SIZE + PI_TEAM, &team, 1);
        }
      }
      WARN_LOG_FMT(BRAWLBACK, "gprb: PPR_GPRB_TEST_TEAMS: a team battle ({})", teams);
    }
  }
  const auto seeds = MatchSeeds(s.session_seed, s.match_index);
  WriteU32(system, Addr::MTRAND_DEFAULT_SEED, seeds[0]);
  WriteU32(system, Addr::MTRAND_OTHER_SEED, seeds[1]);
  WriteU32(system, Addr::LIBC_RAND_NEXT, seeds[2]);
  WriteU32(system, Addr::OBJECT_SERIAL_COUNTER, SERIAL_COUNTER_START);
  s.did_frame0 = true;
  INFO_LOG_FMT(BRAWLBACK, "gprb: match {} RNG seeded ({:08x} {:08x} {:08x}){}{}", s.match_index,
               seeds[0], seeds[1], seeds[2], when.empty() ? "" : " ", when);
  // Tests (PPR_GPRB_TEST_RNG_SKEW): the joiner's g_mtRand moves on after the seeding, as a song
  // that P+ picks again on one machine only moves it (OnStageCreate).
  if (!IsHost() && std::getenv("PPR_GPRB_TEST_RNG_SKEW"))
  {
    WriteU32(system, Addr::MTRAND_DEFAULT_SEED, (seeds[0] * 0x5D588B65u + 0x269EC3u) & 0x7fffffff);
    WARN_LOG_FMT(BRAWLBACK, "gprb: PPR_GPRB_TEST_RNG_SKEW: joiner's g_mtRand moved on");
  }
  return MatchStart::Done;
}

bool OnLoopTop(const Core::CPUThreadGuard& guard)
{
  s.cpu_where = "looptop";
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();

  // Sounds of mispredicted runs are stopped here, between passes (guest calls that return here).
  if (s.gcall_active || (s.phase == Phase::Running && !s.snd_to_stop.empty()))
  {
    if (RunSoundStops(guard))
      return true;
  }

  if (s.mode == Mode::None)
  {
    if (s.phase == Phase::Running || s.phase == Phase::Barrier || s.phase == Phase::Countdown ||
        s.phase == Phase::StartBarrier)
    {
      EndRunning(system, s.end_reason.empty() ? "stopped" : s.end_reason);
      s.phase = Phase::Idle;
      SetLoopDefault(ppc);
      return true;
    }
    return false;
  }

  if (s.mode == Mode::Network && s.peer_left && InMatchPhase())
  {
    EndRunning(system, s.peer_left_reason);
    s.phase = Phase::Ended;
    s.disconnected = true;
    s.error = s.peer_left_reason;
    SetLoopDefault(ppc);
    return true;
  }

  // A desync ends the game with no winner (no game result), still connected: the game shows
  // DESYNC DETECTED and goes back to the character select for the next game.
  if (s.mode == Mode::Network && s.desync_end && s.phase == Phase::Running)
  {
    EndRunning(system, "desync");
    s.desync_end = false;
    s.desynced = true;
    SetLoopDefault(ppc);
    return true;
  }

  if (s.phase == Phase::Running)
  {
    if (RunIoWait(guard))
      return true;
    return RunFrame(guard);
  }

  const Guest g(system.GetMemory());

  if (s.phase == Phase::Countdown || s.phase == Phase::StartBarrier)
  {
    if (!IsSceneMelee(g))
    {
      EndRunning(system, "left the match scene during the countdown");
      return false;
    }
    const u32 fc = g.U32(Addr::GAME_FRAME + 4).value_or(0);
    PadsSetCurrent(0xF, NeutralSlots());
    PadsOnLoopTop(system);
    ApplyPeerDrops();
    if (s.mode == Mode::Network && s.phase == Phase::Countdown && !PumpGekkoInCountdown(system))
    {
      s.error = s.error.empty() ? "gekko session failed in the countdown" : s.error;
      EndRunning(system, s.error);
      s.phase = Phase::Error;
      SetLoopDefault(ppc);
      return true;
    }
    if (s.phase == Phase::Countdown && fc + 1 < StartFrame())
      return false;  // run the frame normally (the HLE hook's default)
    // No file read may be in flight when the base snapshot is taken (countdown preloads).
    if (s.phase == Phase::Countdown && RunIoWait(guard))
      return true;
    if (s.phase == Phase::Countdown && RunForceFinal(guard))
      return true;
    if (s.mode == Mode::SyncTest)
    {
      if (auto err = PrepareRegionSet(system, s.st_opts.region_set))
      {
        s.phase = Phase::Error;
        s.error = *err;
        return false;
      }
      if (!CreateGekko(true, static_cast<int>(std::popcount(s.st_opts.ports & 0xF))))
      {
        s.phase = Phase::Error;
        s.error = "gekko_create failed";
        return false;
      }
      StartRunning(system);
      return RunFrame(guard);
    }
    if (s.phase == Phase::Countdown)
    {
      if (auto err = PrepareRegionSet(system, s.net_opts.region_set))
      {
        s.phase = Phase::Error;
        s.error = *err;
        return false;
      }
      // Normally created when the countdown started (PumpGekkoInCountdown).
      if (!s.gekko && !CreateGekko(false, std::popcount(s.match_ports)))
      {
        s.phase = Phase::Error;
        s.error = "gekko_create failed";
        return false;
      }
      // The region set is in place now: the save of frame 0 the countdown's handshake returned.
      s.defer_initial_save = false;
      if (s.initial_save_deferred)
      {
        s.initial_save_deferred = false;
        InitialSave(system, s.deferred_initial_checksum);
        s.deferred_initial_checksum = nullptr;
      }
      s.at_start = true;
      s.barrier_since = Clock::now();
      s.phase = Phase::StartBarrier;
      SendState();  // the peer's barrier waits for at_start: not until the next 50 ms tick
      INFO_LOG_FMT(BRAWLBACK, "gprb: at the start barrier (game frame {}, GekkoNet synced {})",
                   fc + 1, s.gekko_started);
    }
    // StartBarrier: wait on the host for GekkoNet's handshake and the peer.
    s.cpu_where = "startbarrier";
    auto& ct = system.GetCoreTiming();
    for (;;)
    {
      if (!s.gekko_started)
      {
        // GekkoNet sends its sync requests from gekko_update_session (not from the network
        // poll), so update without adding local input until the session has started. The update
        // that first sees the peer synced may return frame 0's save and advance events (inputs
        // from the local delay): keep them for the first RunFrame.
        int count = 0;
        GekkoGameEvent** events = gekko_update_session(s.gekko, &count);
        if (!ProcessGekkoUpdate(system, events, count))
        {
          s.error = s.error.empty() ? "gekko session failed at the start barrier" : s.error;
          EndRunning(system, s.error);
          s.phase = Phase::Error;
          SetLoopDefault(ppc);
          return true;
        }
        if (s.ops.adv_count > 0)
        {
          s.prefetched_ops = s.ops;
          s.ops_prefetched = true;
        }
      }
      else
      {
        gekko_network_poll(s.gekko);
      }
      ApplyPeerDrops();
      // Every player still in the match is at the start barrier; the host waits for the slowest
      // round trip's half (a 1v1: the peer's).
      bool all_at_start = true;
      double rtt = 0;
      ForEachPeer([&](int, const PeerView& p) {
        if (p.left)
          return;
        all_at_start = all_at_start && p.at_start;
        rtt = std::max(rtt, std::clamp(p.rtt_ms, 0.0, PeerData::MAX_RTT_MS));
      });
      const bool host = IsHost();
      if (host && !s.go && all_at_start && s.gekko_started)
      {
        s.go = true;
        // Sent now: each joiner starts when "go" arrives, the host RTT/2 after sending it. On the
        // 50 ms state tick the joiner started up to 50 ms (3 frames) after the host, and time
        // sync then needed seconds to close that gap.
        SendState();
        s.start_at = Clock::now() + std::chrono::microseconds(static_cast<s64>(rtt * 500.0));
      }
      const PeerView* hp = HostPeer();
      const bool start = host ? (s.go && s.start_at && Clock::now() >= *s.start_at) :
                                (hp && hp->go && s.gekko_started);
      if (start)
      {
        INFO_LOG_FMT(BRAWLBACK, "gprb: start barrier passed after {:.0f} ms",
                     std::chrono::duration<double, std::milli>(Clock::now() - s.barrier_since)
                         .count());
        StartRunning(system);
        ct.ResetThrottleToNow();
        return RunFrame(guard);
      }
      if (s.peer_left)
        return OnLoopTop(guard);  // ends the session (peer gone)
      if (Clock::now() - s.barrier_since > std::chrono::seconds(60))
      {
        s.error = "start barrier timed out";
        EndRunning(system, s.error);
        s.phase = Phase::Error;
        SetLoopDefault(ppc);
        return true;
      }
      if (!CpuRunning(system))
      {
        Spin(ppc);
        return true;
      }
      s.mutex.unlock();
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      s.mutex.lock();
    }
  }

  if (s.mode == Mode::SyncTest && s.phase == Phase::Armed)
  {
    // Normally the first simulation frame; any countdown frame before start_frame also works
    // (e.g. after loading a savestate taken in the countdown).
    u32 fc;
    if (!IsSceneMelee(g))
      return false;
    IsSimStart(g, &fc);
    const u32 pc = g.U32(Addr::GAME_FRAME + 0x14).value_or(0);
    if (fc < 1 || pc <= fc || fc + 1 >= s.st_opts.start_frame)
      return false;
    s.phase = Phase::Countdown;
    PadsSetCurrent(0xF, NeutralSlots());
    PadsOnLoopTop(system);
    return false;
  }

  if (s.mode != Mode::Network)
    return false;

  if (s.phase == Phase::Connected || s.phase == Phase::Connecting)
  {
    // The match setup and seeds are applied at the first loop top that sees the scene change to
    // scMelee pending, or else the first one in scMelee: before the match scene starts loading.
    // (Some paths into the match, e.g. the online CSS's Versus setup, construct the stage within
    // the pass that switches the scene; the stage's constructor shuffles the fighters' start
    // points with g_mtRand and reads the stage variant from the init block.)
    const bool melee_pending = !IsSceneMelee(g) && IsNextSceneMelee(g) && !s.await_scene_exit;
    if (!IsSceneMelee(g) && !melee_pending)
    {
      s.did_frame0 = false;
      s.await_scene_exit = false;
      s.desynced = false;
      return false;
    }
    if (s.await_scene_exit)
      return false;
    if (!s.did_frame0 && s.phase == Phase::Connected && (IsHost() || s.have_seed))
    {
      switch (ApplyMatchStart(system, melee_pending ? "before the scene change" : ""))
      {
      case MatchStart::Error:
        return false;
      case MatchStart::Stopped:
        Spin(ppc);
        return true;
      case MatchStart::Done:
        break;
      }
    }
    if (melee_pending)
      return false;
    u32 fc;
    if (s.phase != Phase::Connected || !IsSimStart(g, &fc))
      return false;
    // The first frame of the match simulation: the barrier.
    s.setup_hash = SetupHash(g);
    s.task_order = TaskOrderNames(g);
    // The match's ports: a 1v1 is P1 and P2. With ports, the human ports of the game's own match
    // setup, which the peers compare (the setup hash): each must be this player or a peer of the
    // session, and every peer still in the session must have one.
    if (!s.ported)
    {
      s.match_ports = 0x3;
    }
    else
    {
      s.match_ports = HumanPorts(g);
      std::string problem;
      if (!(s.match_ports & (1u << s.local_slot)))
        problem = fmt::format("no player on this player's P{}", s.local_slot + 1);
      for (int port = 0; port < MAX_LOBBY_PLAYERS && problem.empty(); ++port)
      {
        const bool in_match = s.match_ports & (1u << port);
        if (port == s.local_slot)
          continue;
        const PeerView& p = s.peers[port];
        if (in_match && !p.expected)
          problem = fmt::format("P{} plays but is not in the session", port + 1);
        else if (!in_match && p.expected && !p.left)
          problem = fmt::format("P{} is in the session but has no fighter", port + 1);
      }
      if (std::popcount(s.match_ports) < 2)
        problem = "fewer than two players";
      if (!problem.empty())
      {
        s.error = fmt::format("the match's players differ from the session's: {}", problem);
        ERROR_LOG_FMT(BRAWLBACK, "gprb: {}", s.error);
        EndRunning(system, s.error);
        s.phase = Phase::Error;
        return false;
      }
    }
    // 3-4 players: a player who leaves is gone from an agreed frame on (WriteGoneFlags), and the
    // game reads the gone flags in its PPOM SESSION block.
    s.gone_enabled = std::popcount(s.match_ports) >= 3;
    s.gone_addr = 0;
    s.gone_written.fill(0);
    if (s.gone_enabled)
    {
      if (const u32 session = Online::GameBridge::SessionAddress())
        s.gone_addr = session + GONE_FLAG_OFFSET;
      else
        WARN_LOG_FMT(BRAWLBACK, "gprb: the game's PPOM block was not found: no gone flags");
    }
    if (IsHost())
      s.sync = ReadSyncBlock(system);
    s.at_s = true;
    s.applied = IsHost();
    s.barrier_host = HostSlot();
    s.barrier_since = Clock::now();
    s.phase = Phase::Barrier;
    SendState();
    INFO_LOG_FMT(BRAWLBACK, "gprb: at the barrier (P{} of ports {:#x}, setup {:016x}: {})",
                 s.local_slot + 1, s.match_ports, s.setup_hash, Hex(SetupKey(g)));
  }

  if (s.phase != Phase::Barrier)
    return false;

  // Barrier: wait on the host for the peer, without running guest code.
  auto& ct = system.GetCoreTiming();
  for (;;)
  {
    std::string differs;
    bool all_applied = true;
    ForEachPeer([&](int slot, const PeerView& p) {
      if (p.left)
        return;
      if (p.at_s && p.setup_hash != 0 && s.setup_hash != 0 && p.setup_hash != s.setup_hash &&
          differs.empty())
      {
        differs = s.ported ? fmt::format("match setup differs (local {:016x}, P{} {:016x})",
                                         s.setup_hash, slot + 1, p.setup_hash) :
                             fmt::format("match setup differs (local {:016x}, peer {:016x})",
                                         s.setup_hash, p.setup_hash);
      }
      all_applied = all_applied && p.at_s && p.applied;
    });
    PeerView* hp = HostPeer();
    if (differs.empty() && HostSlot() != s.barrier_host)
      differs = "the host left before the match started";
    if (differs.empty() && hp && !s.applied && hp->at_s && hp->sync.valid &&
        hp->match_ports != s.match_ports)
    {
      differs = fmt::format("the match's ports differ (local {:#x}, host {:#x})", s.match_ports,
                            hp->match_ports);
    }
    if (!differs.empty())
    {
      s.error = differs;
      ERROR_LOG_FMT(BRAWLBACK, "gprb: {}", s.error);
      EndRunning(system, s.error);
      s.phase = Phase::Error;
      SetLoopDefault(ppc);
      return true;
    }
    if (hp && !s.applied && hp->at_s && hp->sync.valid &&
        (!s.net_opts.sync_task_order || hp->task_order_valid))
    {
      WriteSyncBlock(system, hp->sync);
      int relinked = 0;
      if (s.net_opts.sync_task_order)
        relinked = ApplyTaskOrder(system, hp->task_order);
      INFO_LOG_FMT(BRAWLBACK, "gprb: joiner applied the host's sync block (task lists relinked: {})",
                   relinked);
      s.applied = true;
      SendState();
      const double rtt = std::clamp(hp->rtt_ms, 0.0, PeerData::MAX_RTT_MS);
      s.countdown_at = Clock::now() + std::chrono::microseconds(static_cast<s64>(rtt * 500.0));
    }
    // Everyone proceeds into the countdown once the joiners have copied the host's values: the
    // host when it hears so from all, a joiner RTT/2 after it told the host, so the countdowns
    // (which run without rollback) start about together and the peers reach the start barrier
    // together.
    const bool proceed =
        IsHost() ? all_applied : (s.applied && s.countdown_at && Clock::now() >= *s.countdown_at);
    if (proceed)
    {
      s.phase = Phase::Countdown;
      ct.ResetThrottleToNow();
      // GekkoNet's handshake runs during the countdown (PumpGekkoInCountdown).
      if (CreateGekko(false, std::popcount(s.match_ports)))
        s.defer_initial_save = true;
      else
        WARN_LOG_FMT(BRAWLBACK, "gprb: gekko_create failed; retried at the start barrier");
      PadsSetCurrent(0xF, NeutralSlots());
      PadsOnLoopTop(system);
      INFO_LOG_FMT(BRAWLBACK, "gprb: barrier passed, countdown until game frame {}", StartFrame());
      SetLoopDefault(ppc);
      return true;
    }
    if (s.peer_left)
      return OnLoopTop(guard);  // ends the session (peer gone)
    if (Clock::now() - s.barrier_since > std::chrono::seconds(60))
    {
      s.error = "barrier timed out";
      EndRunning(system, s.error);
      s.phase = Phase::Error;
      SetLoopDefault(ppc);
      return true;
    }
    if (!CpuRunning(system))
    {
      Spin(ppc);
      return true;
    }
    // GekkoNet's handshake runs in its network poll above. Never call gekko_update_session here:
    // the update that first sees the peer synced returns frame 0's save and advance events.
    s.mutex.unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    s.mutex.lock();
  }
}

bool OnFrameEnd(const Core::CPUThreadGuard& guard)
{
  if (s.phase != Phase::Running)
    return false;
  s.cpu_where = "frameend";
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  // The replaced instruction: stw r0,0x100(r23).
  PowerPC::MMU::HostWrite<u32>(guard, ppc.gpr[0], ppc.gpr[23] + 0x100);
  ppc.npc = FRAME_END + 4;
  FrameTraceOnFrameEnd(system, ppc.gpr[24], s.resim_pass);
  CpuTraceOnFrameEnd(system);
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  const int i = s.iteration;
  if (i >= 0 && i < s.ops.adv_count && s.ops.save_after[i])
  {
    s.queued_save.pending = true;
    s.queued_save.frame = s.ops.adv_frame[i];
    s.queued_save.checksum = s.ops.checksum_ptr[i];
  }
  CommitSoundPass();
  return true;
}

bool OnLoopEnd(const Core::CPUThreadGuard& guard)
{
  if (s.phase != Phase::Running)
    return false;
  auto& ppc = guard.GetSystem().GetPPCState();
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.spinning)
  {
    s.spinning = false;
  }
  else if (s.ops.adv_count > 0)
  {
    ++s.iteration;
    if (s.iteration >= s.ops.adv_count)
      s.iteration = 0;
  }
  ppc.npc = LOOP_TOP;
  return true;
}

void OnStageCreate(const Core::CPUThreadGuard& guard)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.mode != Mode::Network)
    return;
  INFO_LOG_FMT(BRAWLBACK, "gprb: stage constructed (match {}, seeded {})", s.match_index, s.did_frame0);
  if (s.phase != Phase::Connected || s.await_scene_exit || !(IsHost() || s.have_seed))
    return;
  auto& system = guard.GetSystem();
  if (!s.did_frame0)
  {
    ApplyMatchStart(system, "at the stage's construction");
    return;
  }
  // Seeded at the loop top already: seed the RNGs again here, so that the stage and everything
  // built after it start from the same RNG state on both machines, whatever ran in between. The
  // stage music is picked when the match scene starts, and P+ picks it again (with g_mtRand) when
  // the first pick read a stale tracklist, which depends on what this machine played before: on
  // one peer only (open issue 7, docs/gameplay-rollback-status.md). The object serial counter is
  // left alone: objects made since the loop top keep their serials.
  const Guest g(system.GetMemory());
  const std::array<u32, 3> before = {g.U32(Addr::MTRAND_DEFAULT_SEED).value_or(0),
                                     g.U32(Addr::MTRAND_OTHER_SEED).value_or(0),
                                     g.U32(Addr::LIBC_RAND_NEXT).value_or(0)};
  const auto seeds = MatchSeeds(s.session_seed, s.match_index);
  WriteU32(system, Addr::MTRAND_DEFAULT_SEED, seeds[0]);
  WriteU32(system, Addr::MTRAND_OTHER_SEED, seeds[1]);
  WriteU32(system, Addr::LIBC_RAND_NEXT, seeds[2]);
  INFO_LOG_FMT(BRAWLBACK, "gprb: match {} RNG seeded again at the stage's construction (was {:08x} "
               "{:08x} {:08x})", s.match_index, before[0], before[1], before[2]);
}

bool IsResimulationPass()
{
  return s.phase == Phase::Running && s.resim_pass;
}

bool IsRunning()
{
  return s.phase == Phase::Running;
}

namespace
{
// nw4r::snd::detail::BasicSound (Brawl's build): general handle +0x08, temporary general handle
// +0x0C, sound id +0x78, priority-list link +0xB8; vtable +0x18 Stop(int fade_frames).
// SoundArchivePlayer: its three SoundInstanceManagers (seq +0x38, strm +0x60, wave +0x88) keep the
// allocated sounds in a list {+0x4 count, +0x8 first node} (see detail_AllocWaveSound).
constexpr u32 SND_GENERAL_HANDLE = 0x08;
constexpr u32 SND_TEMP_HANDLE = 0x0C;
constexpr u32 SND_ID = 0x78;
constexpr u32 SND_PRIORITY_LINK = 0xB8;
constexpr u32 SND_VT_STOP = 0x18;
constexpr std::array<u32, 3> SND_MANAGERS{0x38, 0x60, 0x88};

u32 ReadGuest(const Core::CPUThreadGuard& guard, u32 addr)
{
  return PowerPC::MMU::HostIsRAMAddress(guard, addr) ? PowerPC::MMU::HostRead<u32>(guard, addr) : 0;
}

// The sound object is allocated (in one of the player's instance lists).
bool SoundIsAllocated(const Core::CPUThreadGuard& guard, u32 sound)
{
  if (!s.snd_player)
    return false;
  for (u32 off : SND_MANAGERS)
  {
    const u32 sentinel = s.snd_player + off + 8;
    u32 node = ReadGuest(guard, sentinel);
    for (int n = 0; node && node != sentinel && n < 256; ++n)
    {
      if (node - SND_PRIORITY_LINK == sound)
        return true;
      node = ReadGuest(guard, node);
    }
  }
  return false;
}

// `e` still describes a live sound: the same allocation of that pool object, still allocated, the
// same sound id.
bool SoundEntryAlive(const Core::CPUThreadGuard& guard, const State::SndEntry& e)
{
  const auto it = s.snd_gen.find(e.sound);
  if (it == s.snd_gen.end() || it->second != e.gen)
  {
    ++s.snd_gone_why[0];  // the pool object was allocated again since
    return false;
  }
  if (ReadGuest(guard, e.sound + SND_ID) != e.id)
  {
    ++s.snd_gone_why[1];
    return false;
  }
  if (!SoundIsAllocated(guard, e.sound))
  {
    ++s.snd_gone_why[2];  // freed (finished or stopped)
    return false;
  }
  return true;
}

bool DedupeSounds()
{
  return s.mode == Mode::SyncTest ? s.st_opts.dedupe_resim_sounds : s.net_opts.dedupe_resim_sounds;
}

// End of a pass (frame end): the frame's sound list becomes this run's; with dedupe, sounds the
// earlier run started and this one did not are queued to stop.
void CommitSoundPass()
{
  if (!s.snd_pass_open)
    return;
  s.snd_pass_open = false;
  const size_t slot = static_cast<size_t>(std::max<s64>(s.snd_frame, 0)) % s.snd_played.size();
  if (!s.snd_first_run && DedupeSounds())
  {
    for (const auto& e : s.snd_remaining)
      s.snd_to_stop.push_back(e);
  }
  s.snd_remaining.clear();
  s.snd_played[slot] = s.snd_new;
  s.snd_tag[slot] = s.snd_frame;
}

void SaveGuestRegs(const PowerPC::PowerPCState& ppc, State::GuestRegs* out)
{
  std::copy(std::begin(ppc.gpr), std::end(ppc.gpr), out->gpr.begin());
  std::copy(std::begin(ppc.ps), std::end(ppc.ps), out->ps.begin());
  std::copy(std::begin(ppc.cr.fields), std::end(ppc.cr.fields), out->cr.begin());
  out->fpscr = ppc.fpscr.Hex;
  out->xer_ca = ppc.xer_ca;
  out->xer_so_ov = ppc.xer_so_ov;
  out->xer_stringctrl = ppc.xer_stringctrl;
  out->lr = ppc.spr[SPR_LR];
  out->ctr = ppc.spr[SPR_CTR];
}

void RestoreGuestRegs(PowerPC::PowerPCState& ppc, const State::GuestRegs& in)
{
  std::copy(in.gpr.begin(), in.gpr.end(), std::begin(ppc.gpr));
  std::copy(in.ps.begin(), in.ps.end(), std::begin(ppc.ps));
  std::copy(in.cr.begin(), in.cr.end(), std::begin(ppc.cr.fields));
  const bool rounding_changed = ppc.fpscr.Hex != in.fpscr;
  ppc.fpscr.Hex = in.fpscr;
  if (rounding_changed)
    PowerPC::RoundingModeUpdated(ppc);
  ppc.xer_ca = in.xer_ca;
  ppc.xer_so_ov = in.xer_so_ov;
  ppc.xer_stringctrl = in.xer_stringctrl;
  ppc.spr[SPR_LR] = in.lr;
  ppc.spr[SPR_CTR] = in.ctr;
}

// Mid-match file loads (gfFileIOManager): a Zelda/Sheik transformation reads the other form's
// motion and model files into the fighter's resource heap, items preload Pokemon and Assist Trophy
// resources. The IO thread reads while the main thread waits for the retrace, so the number of
// game frames a load takes depended on emulated time, which region mode does not rewind (every
// resimulated pass spends some): peers (and the passes of one instance) saw a load complete on
// different frames, and a rollback restored the destination buffer under a read that was still
// in flight or already reported done. Now, at every loop top of the match, while a request is
// queued or being read, the main thread runs the manager's update (which the game loop calls once
// per pass; it hands queued requests to the IO thread and retires finished ones) and waits one
// more retrace, both as guest calls that return to the loop top, before the frame runs. A load requested in a frame is complete
// before the next frame, on every peer and in every pass, and no read is in flight at a save or
// a load; the IO manager's queues and requests are in the region set (gp-v15), so a rollback also
// rolls back which requests the game holds. Returns true while waiting (the hook must not do
// anything else).
constexpr u32 FILE_IO_MANAGER_PTR = 0x8059FFF4;  // g_gfFileIOManager (.sbss)
constexpr u32 FILE_IO_UPDATE = 0x80022F84;  // gfFileIOManager::update (mainLoop 0x8001725C, fn_80017618)
constexpr u32 VI_WAIT_FOR_RETRACE = 0x801E892C;
constexpr int IO_WAIT_MAX = 600;  // retraces (10 s of emulated time)

u32 FileIoPending(const Core::CPUThreadGuard& guard)
{
  // gfFileIOManager: +0x8 / +0xC the two request queues; a queue holds its utQueue at +0x18,
  // whose count is the low half of the first word.
  const u32 mgr = ReadGuest(guard, FILE_IO_MANAGER_PTR);
  if (!mgr)
    return 0;
  u32 n = 0;
  for (const u32 off : {0x8u, 0xCu})
  {
    const u32 q = ReadGuest(guard, mgr + off);
    const u32 uq = q ? ReadGuest(guard, q + 0x18) : 0;
    if (uq)
      n += ReadGuest(guard, uq) & 0xFFFF;
  }
  return n;
}

bool RunIoWait(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  if (FileIoPending(guard) != 0 && s.io_wait_n < IO_WAIT_MAX)
  {
    if (!s.io_wait_active)
    {
      SaveGuestRegs(ppc, &s.io_saved);
      s.io_wait_active = true;
      s.io_wait_update = true;
      s.io_wait_n = 0;
      ++s.io_waits;
    }
    ppc.spr[SPR_LR] = LOOP_TOP;
    if (s.io_wait_update)
    {
      ppc.gpr[3] = ReadGuest(guard, FILE_IO_MANAGER_PTR);
      ppc.npc = FILE_IO_UPDATE;
    }
    else
    {
      ppc.npc = VI_WAIT_FOR_RETRACE;
      ++s.io_wait_n;
      ++s.io_wait_retraces;
    }
    s.io_wait_update = !s.io_wait_update;
    s.cpu_where = "iowait";
    return true;
  }
  if (s.io_wait_active)
  {
    if (s.io_wait_n >= IO_WAIT_MAX)
    {
      ++s.io_wait_timeouts;
      WARN_LOG_FMT(BRAWLBACK, "gprb: file IO still busy after {} retraces, going on", s.io_wait_n);
    }
    RestoreGuestRegs(ppc, s.io_saved);
    s.io_wait_active = false;
    s.io_wait_n = 0;
  }
  return false;
}

// Diagnostics (PPR_GPRB_FORCE_FINAL=<port mask>): at the last countdown frame, before the base
// snapshot, give those ports their Final Smash as breaking a Smash Ball does: a guest call of
// ftManager::setFinal(entry id, false) per port, returning to the loop top. Both a rolled-back run
// and its ground truth (the same session machinery without rollback) do it at the same point, so a
// sync test can check that a Final Smash executes under rollback. Returns true while calling.
constexpr u32 FT_MANAGER = 0x80629A00;          // g_ftManager (System heap, every boot)
constexpr u32 FT_ENTRY_MANAGER = 0x80624780;    // {ftEntry* entries, count}
constexpr u32 FT_MANAGER_SET_FINAL = 0x8081828C;  // ftManager::setFinal(int, bool), sora_melee .text+0x10D878
constexpr u32 FT_ENTRY_SIZE = 0x244;

bool RunForceFinal(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  if (!s.force_final_inited)
  {
    const char* ff = std::getenv("PPR_GPRB_FORCE_FINAL");
    s.force_final_todo = ff ? static_cast<u32>(std::strtoul(ff, nullptr, 0)) & 0xF : 0;
    s.force_final_inited = true;
  }
  if (s.force_final_active && s.force_final_todo == 0)
  {
    RestoreGuestRegs(ppc, s.force_final_saved);
    s.force_final_active = false;
    return false;
  }
  while (s.force_final_todo)
  {
    const u32 port = static_cast<u32>(std::countr_zero(s.force_final_todo));
    s.force_final_todo &= s.force_final_todo - 1;
    // The REL must be where P+ loads it (stwu r1,-0x20(r1) at the function's entry).
    if (ReadGuest(guard, FT_MANAGER_SET_FINAL) != 0x9421FFE0)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb: force final: ftManager::setFinal not found");
      s.force_final_todo = 0;
      break;
    }
    const u32 entries = ReadGuest(guard, FT_ENTRY_MANAGER);
    const u32 count = std::min<u32>(ReadGuest(guard, FT_ENTRY_MANAGER + 4), 16);
    u32 entry_id = 0xFFFFFFFF;
    for (u32 i = 0; i < count; ++i)
    {
      const u32 e = entries + i * FT_ENTRY_SIZE;
      if (ReadGuest(guard, e + 0x28) != 0 && ReadGuest(guard, e + 0x58) == port)
        entry_id = ReadGuest(guard, e + 4);
    }
    if (entry_id == 0xFFFFFFFF)
      continue;
    if (!s.force_final_active)
    {
      SaveGuestRegs(ppc, &s.force_final_saved);
      s.force_final_active = true;
    }
    INFO_LOG_FMT(BRAWLBACK, "gprb: force final: port {} entry {:#x}", port, entry_id);
    ppc.gpr[3] = FT_MANAGER;
    ppc.gpr[4] = entry_id;
    ppc.gpr[5] = 0;
    ppc.spr[SPR_LR] = LOOP_TOP;
    ppc.npc = FT_MANAGER_SET_FINAL;
    return true;
  }
  if (s.force_final_active)
  {
    RestoreGuestRegs(ppc, s.force_final_saved);
    s.force_final_active = false;
  }
  return false;
}

// Loop top: stop the queued sounds of mispredicted runs, one guest call of BasicSound::Stop(0)
// each. The call returns to the loop top, where this runs again, restores the registers and goes
// on. Returns true while a call is being made (the hook must not do anything else).
bool RunSoundStops(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  if (s.gcall_active)
  {
    RestoreGuestRegs(ppc, s.gcall_saved);
    s.gcall_active = false;
  }
  while (!s.snd_to_stop.empty())
  {
    const State::SndEntry e = s.snd_to_stop.front();
    s.snd_to_stop.pop_front();
    if (!SoundEntryAlive(guard, e))
    {
      ++s.sound_stop_gone;
      continue;
    }
    // The handles the sound still points to were rolled back and refer to something else now:
    // cut those links, so that Stop does not detach the game's handles from their sounds.
    for (const u32 off : {SND_GENERAL_HANDLE, SND_TEMP_HANDLE})
    {
      const u32 h = ReadGuest(guard, e.sound + off);
      if (h && ReadGuest(guard, h) != e.sound)
        PowerPC::MMU::HostWrite<u32>(guard, 0, e.sound + off);
    }
    const u32 stop = ReadGuest(guard, ReadGuest(guard, e.sound) + SND_VT_STOP);
    if (!PowerPC::MMU::HostIsRAMAddress(guard, stop))
      continue;
    SaveGuestRegs(ppc, &s.gcall_saved);
    ppc.gpr[3] = e.sound;
    ppc.gpr[4] = 0;  // fade frames: stop now
    ppc.spr[SPR_LR] = LOOP_TOP;
    ppc.npc = stop;
    s.gcall_active = true;
    ++s.sound_stopped;
    return true;
  }
  return false;
}
}  // namespace

picojson::value SoundState(const Core::CPUThreadGuard& guard)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  picojson::object o;
  picojson::array sounds;
  u32 active = 0, orphans = 0;
  std::map<u32, u32> per_id;
  if (s.snd_player)
  {
    for (u32 off : SND_MANAGERS)
    {
      const u32 sentinel = s.snd_player + off + 8;
      u32 node = ReadGuest(guard, sentinel);
      for (int n = 0; node && node != sentinel && n < 256; ++n)
      {
        const u32 sound = node - SND_PRIORITY_LINK;
        const u32 id = ReadGuest(guard, sound + SND_ID);
        const u32 handle = ReadGuest(guard, sound + SND_GENERAL_HANDLE);
        const u32 temp = ReadGuest(guard, sound + SND_TEMP_HANDLE);
        // Owned: a handle that points back at it. An orphan plays on with nobody able to stop it.
        const bool owned = (handle && ReadGuest(guard, handle) == sound) ||
                           (temp && ReadGuest(guard, temp) == sound);
        ++active;
        ++per_id[id];
        if (!owned)
          ++orphans;
        picojson::object e;
        e["sound"] = picojson::value(static_cast<double>(sound));
        e["id"] = picojson::value(static_cast<double>(id));
        e["handle"] = picojson::value(static_cast<double>(handle));
        e["owned"] = picojson::value(owned);
        sounds.emplace_back(e);
        node = ReadGuest(guard, node);
      }
    }
  }
  u32 duplicates = 0;
  for (const auto& [id, n] : per_id)
    duplicates += n > 1 ? n - 1 : 0;
  o["player"] = picojson::value(static_cast<double>(s.snd_player));
  o["active"] = picojson::value(static_cast<double>(active));
  o["orphans"] = picojson::value(static_cast<double>(orphans));
  o["duplicate_ids"] = picojson::value(static_cast<double>(duplicates));
  o["sounds"] = picojson::value(sounds);
  return picojson::value(o);
}

SoundAllocDecision OnSoundAlloc(const Core::CPUThreadGuard& guard, u32 sound_id, u32 handle)
{
  if (s.phase != Phase::Running)
    return {};
  s.snd_player = guard.GetSystem().GetPPCState().gpr[27];  // detail_SetupSound's this
  ++s.sound_allocs;
  if (s.snd_first_run)
    return {};
  ++s.resim_sound_allocs;
  const bool suppress_all =
      s.mode == Mode::SyncTest ? s.st_opts.suppress_resim_sounds : s.net_opts.suppress_resim_sounds;
  if (suppress_all)
  {
    ++s.suppressed_sound_allocs;
    return {SoundAllocAction::NoChannel};
  }
  if (!DedupeSounds())
    return {};
  const auto it = std::find_if(s.snd_remaining.begin(), s.snd_remaining.end(),
                               [&](const State::SndEntry& e) { return e.id == sound_id; });
  if (it == s.snd_remaining.end())
    return {};  // new in this run (the corrected input changed what happens): play it
  const State::SndEntry e = *it;
  s.snd_remaining.erase(it);
  ++s.suppressed_sound_allocs;
  if (!SoundEntryAlive(guard, e))
  {
    // The earlier run's sound has already finished: do not play it a second time.
    ++s.sound_reattach_gone;
    return {SoundAllocAction::NoChannel};
  }
  const u32 back = ReadGuest(guard, e.sound + SND_GENERAL_HANDLE);
  if (back && back != handle && ReadGuest(guard, back) == e.sound)
  {
    // The earlier run's handle still holds it. Sounds started through sndSystem::playSE use a
    // free slot of sndSystem's own handle table (outside the set), so the resimulation asks with
    // the next free slot while the first slot still plays the sound: move the sound to the handle
    // this run asks with (the slot the game now keeps the id of), and free the old one.
    PowerPC::MMU::HostWrite<u32>(guard, 0, back);
    ++s.sound_moved;
  }
  // detail_AttachSound would detach the sound from its current general handle first, which is
  // this handle (or a rolled-back one); clear the link so that the attach only links the two.
  PowerPC::MMU::HostWrite<u32>(guard, 0, e.sound + SND_GENERAL_HANDLE);
  s.snd_reattach = e.sound;
  s.snd_reattach_gen = e.gen;
  ++s.sound_reattached;
  return {SoundAllocAction::Reattach, e.sound};
}

void OnSoundAttached(const Core::CPUThreadGuard&, u32 handle, u32 sound, u32 sound_id)
{
  if (s.phase != Phase::Running || !s.snd_pass_open)
    return;
  u64 gen;
  if (s.snd_reattach == sound)
  {
    gen = s.snd_reattach_gen;
    s.snd_reattach = 0;
  }
  else
  {
    gen = ++s.snd_gen[sound];  // a new allocation of this pool object
  }
  s.snd_new.push_back({sound_id, sound, handle, gen});
}

void ConfigureSamples(u32 every, std::vector<std::pair<u32, u32>> watch)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  s.sample_every = every;
  s.sample_watch = std::move(watch);
}

picojson::value Samples(const std::vector<s64>& frames_full)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  picojson::object o;
  picojson::array ranges, frames, digests, watch;
  for (const auto& r : s.sample_ranges)
    ranges.emplace_back(picojson::array{picojson::value(static_cast<double>(r.addr)),
                                        picojson::value(static_cast<double>(r.size))});
  for (const auto& [addr, size] : s.sample_watch)
    watch.emplace_back(picojson::array{picojson::value(static_cast<double>(addr)),
                                       picojson::value(static_cast<double>(size))});
  for (const auto& [f, chunks] : s.sample_chunks)
  {
    frames.emplace_back(static_cast<double>(f));
    digests.emplace_back(fmt::format("{:016x}", XXH3_64bits(chunks.data(), chunks.size() * 8)));
  }
  o["chunk_size"] = picojson::value(static_cast<double>(REGION_CHUNK));
  o["ranges"] = picojson::value(ranges);
  o["watch"] = picojson::value(watch);
  o["frames"] = picojson::value(frames);
  o["digests"] = picojson::value(digests);
  picojson::object full;
  for (s64 f : frames_full)
  {
    picojson::object e;
    if (const auto it = s.sample_chunks.find(f); it != s.sample_chunks.end())
    {
      std::string hex;
      hex.reserve(it->second.size() * 16);
      for (u64 h : it->second)
        hex += fmt::format("{:016x}", h);
      e["chunks"] = picojson::value(hex);
    }
    if (const auto it = s.sample_bytes.find(f); it != s.sample_bytes.end())
      e["watch"] = picojson::value(Hex(it->second));
    full[std::to_string(f)] = picojson::value(e);
  }
  o["full"] = picojson::value(full);
  return picojson::value(o);
}

void OnProbe(u32 pc, const std::array<u32, 7>& r)
{
  // Every phase of a match (the countdown and the load included): probes are diagnostics.
  if (s.phase == Phase::Idle)
    return;
  INFO_LOG_FMT(BRAWLBACK,
               "gprb probe: frame {} {} pass {} pc {:08x} r3 {:08x} r4 {:08x} r5 {:08x} r6 {:08x} "
               "r12 {:08x} ctr {:08x} lr {:08x}",
               s.snd_frame, s.snd_first_run ? "first" : "again", s.pass_serial, pc, r[0], r[1], r[2],
               r[3], r[4], r[5], r[6]);
}

void OnRngCall(u32 rng, const std::array<u32, 8>& callers, u32 state)
{
  // Every phase of a match (the load and the countdown included); PPR_GPRB_RNG_LOG is diagnostics.
  if (s.phase == Phase::Idle)
    return;
  INFO_LOG_FMT(BRAWLBACK,
               "gprb rng: frame {} {} pass {} rng {:08x} lr {:08x} state {:08x} up {:08x} {:08x} "
               "{:08x} {:08x} {:08x} {:08x} {:08x}",
               s.snd_frame, s.snd_first_run ? "first" : "again", s.pass_serial, rng, callers[0],
               state, callers[1], callers[2], callers[3], callers[4], callers[5], callers[6],
               callers[7]);
}

bool DrivesLoop()
{
  const Phase p = s.phase;
  return p == Phase::Running || p == Phase::Countdown || p == Phase::StartBarrier;
}
}  // namespace Gprb::Session
