// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GameplaySession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/UdpSocket.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "Core/Online/Timeouts.h"
#include <xxhash.h>

#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/Brawlback/include/brawlback-common/BrawlbackConstants.h"
#include "Core/Brawlback/include/gekkonet/GekkoLib/include/gekkonet.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/SoundStream.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Online/Chat.h"
#include "Core/Online/ChatText.h"
#include "Core/Online/GameBridge.h"
#include "Core/Online/GameSetup.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/FrameProfile.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/Rollback/PeerData.h"
#include "Core/Rollback/PresentStats.h"
#include "Core/Rollback/ReplayKeyframes.h"
#include "Core/Rollback/ReplayRecorder.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/Present.h"
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
// While a barrier waits, what each player still lacks is logged this often.
constexpr auto WAIT_LOG_INTERVAL = std::chrono::seconds(5);

enum class Mode
{
  None,
  SyncTest,
  Network,
  // A replay (docs/design/replays.md §6): a joiner whose host is the file. Armed: waiting for the
  // game to start the replay's match (the plugin does, from SESSION); then the barrier with the
  // file's values, the countdown, and the recorded inputs, every frame once, without rollback.
  Playback,
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
  Restore restore;     // a desynced game played again from the agreed state (SetDesyncRecovery)
};

// A desync where the game starts again (SetDesyncRecovery, docs/game-code.md desync recovery):
// each machine's checksums of the last RECOVERY_WINDOW confirmed frames, which the players
// exchange (the state message's "dsr") so that the host finds the last frame all of them agree
// on. Frames past the prediction window may still be predictions, so the list ends there.
constexpr s64 RECOVERY_WINDOW = 240;
struct RecoveryList
{
  u32 match = 0;  // the desynced match's index
  s64 from = 0;   // the session frame of crcs[0]
  std::vector<u32> crcs;  // 0: no record of that frame
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
  std::optional<RecoveryList> recovery;  // its checksums after a desync (the state's "dsr")
  u8 last_winner = 0xFF;
  picojson::object selections;
  std::string name;
  bool at_s = false;
  bool applied = false;
  bool at_start = false;
  bool go = false;
  u64 setup_hash = 0;
  // The bytes of its setup hash and what else the hash covers (its state's "setup_key",
  // "setup_start", "setup_cv" at the barrier; older builds send none): for the log when they differ.
  std::vector<u8> setup_key;
  std::optional<u32> setup_start;
  std::optional<u32> setup_cv;
  SyncBlock sync;
  u32 match_ports = 0;  // the host's match ports at the barrier (with its sync block)
  std::vector<std::vector<std::string>> task_order;
  bool task_order_valid = false;
  // Its held buttons when it reached the barrier (its state's "hold", with at_s), and the host's
  // countdown buttons for every port (the host's "holds", with its sync block). See CountdownSlots.
  u16 hold = 0;
  bool hold_valid = false;
  std::array<u16, MAX_LOBBY_PLAYERS> holds{};
  bool holds_valid = false;
  u32 session_seed = 0;
  u32 match_index = 0;
  // The host's gmGlobalModeMelee init block (stage, rules, stage variant) for match init_match.
  std::vector<u8> init_block;
  s64 init_match = -1;
  Clock::time_point last_heard{};
  double rtt_ms = -1;
  bool oversize_logged = false;  // a control message over MAX_CONTROL_SIZE came from it
  // Its PC is falling behind (its own check, UpdateSlowNotice): its frame rate, 0 if not; and
  // whether this match showed the notice.
  double slow_fps = 0;
  bool slow_shown = false;
  // Its state says it takes GekkoNet's messages bundled ('B', PeerData::AppendGekkoBundle);
  // builds before 2026-10-13 do not.
  bool gekko_bundles = false;
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
  // What a game restarted after a desync takes from the last frame every machine agreed on
  // (Restore): per port the stocks and damage (raw, as the checksum has them) and the match timer.
  std::array<u32, MAX_LOBBY_PLAYERS> stocks{};
  std::array<u32, MAX_LOBBY_PLAYERS> damage{};
  u32 frames_left = 0;
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
  // Control messages too large for one datagram go as 'F' pieces (PeerData::ControlDatagrams):
  // the next message id (random at the session's start, so a peer's restarted session cannot
  // mix its pieces with an older message's), and the others' pieces (network thread only).
  std::atomic<u32> control_id{0};
  PeerData::ControlReassembler reassembler;
  bool split_logged = false;  // our state message went as 'F' pieces (logged once)
  // The session ends (fewer than two players left): handled on the CPU thread for a match.
  bool peer_left = false;
  std::string peer_left_reason;
  // This player told the others it left: Stop, or the session gave up (Phase::Error), so they
  // do not wait for it at a barrier until their own timeout.
  bool leave_sent = false;
  // When the session began connecting, and the players (in-game ports as bits) it gave up on
  // because none of them was ever heard (Online::PEER_NEVER_HEARD_TIMEOUT_MS; Lobby::unheard).
  Clock::time_point connecting_since{};
  u8 unheard_ports = 0;
  // "Still waiting" lines logged at the current barrier (every WAIT_LOG_INTERVAL).
  int wait_logs = 0;
  bool state_size_logged = false;  // our state message was over MAX_CONTROL_SIZE (logged once)
  bool disconnected = false;  // reported to the game (Slippi's ONLINE_INPUTS result 3)
  // The two machines' game states differ (GekkoNet's checksums, or the peer's "desync"): the
  // CPU thread ends the match at its next loop top, as Slippi ends a game on a hard desync.
  bool desync_end = false;
  // The first frame whose checksums differ, ours or the one in the peer's "desync" (-1: not known,
  // an older peer's message): the frame of the desync's rng log (LogRngCalls).
  s64 desync_frame = -1;
  // The last match ended on a desync: reported to the game (LOCAL `desynced`, DESYNC DETECTED
  // and the end without "GAME!") until it has left the match scene.
  bool desynced = false;
  // The last match ended on a desync and the next game plays it again (SetDesyncRecovery): this
  // machine's checksums, sent to the others until that game starts, and the desynced game's
  // setup, which the next one repeats.
  struct Recovery
  {
    bool pending = false;
    RecoveryList list;
    MatchSetup setup;
  } recovery;
  std::thread net_thread;
  std::atomic<bool> net_run{false};
  std::mutex gekko_rx_mutex;
  std::deque<std::pair<int, std::vector<u8>>> gekko_rx;  // (sender's port, packet)
  std::vector<GekkoNetResult*> gekko_rx_ptrs;
  // Open GekkoBatch scopes and, per peer, the GekkoNet messages they hold (under s.mutex).
  int gekko_batch = 0;
  std::array<std::vector<u8>, MAX_LOBBY_PLAYERS> gekko_bundle;
  // The last state message sent, when, and how many more copies of it the 50 ms tick sends in a
  // running match (SendState).
  std::string state_sent;
  Clock::time_point state_sent_at{};
  int state_repeats = 0;
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
  // The pause screen's quit in a 3-4 player match (the game's SESSION quitPorts/quitStays, rolled
  // back with the match; 0: the PPOM block was not found). local_quit_frame: the first session
  // frame whose state shows this player's quit (-1: none); quit_left: the session ended because
  // of it (LOCAL.quitLeft), until the next match or session.
  u32 quit_addr = 0;
  s64 local_quit_frame = -1;
  bool quit_left = false;
  // SESSION during a match (any size; 0: not found), for a quit that ended it for everyone
  // (SESSION_QUIT_BY, SESSION_QUIT_RUNBACK), read when the session ends on it.
  u32 session_addr = 0;
  // Host: the stages not drawn yet (Slippi's stage_pool, refilled from the match's list when empty).
  std::vector<u16> stage_pool;

  // Local barrier view (broadcast).
  bool at_s = false;
  bool applied = false;
  bool at_start = false;
  bool go = false;
  u64 setup_hash = 0;
  std::vector<u8> setup_key;  // the bytes of setup_hash (SetupKey), for the replay
  u32 setup_start = 0;        // the start frame setup_hash covers (StartFrame)
  // Start-of-match hotkeys (CountdownSlots): this player's held buttons when it reached the
  // barrier, and every port's countdown buttons (host: fixed once every player's is in; joiner:
  // the host's, copied with its sync block; replay: the file's).
  u16 hold = 0;
  std::array<u16, MAX_LOBBY_PLAYERS> holds{};
  bool holds_valid = false;
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
  // Following a peer that cannot keep full speed (UpdatePeerFollow): whether it is on, this peer's
  // speed while it is, the gap, frame and time at the last time sync, and the peer's frame rate.
  bool following = false;
  double follow_speed = 1.0;
  double follow_prev_gap = 0;
  s64 follow_prev_frame = -1;
  u64 follow_prev_own_late = 0;
  int follow_release = 0;  // time syncs in a row that saw the peer at full speed
  Clock::time_point follow_t{};
  double peer_fps = 0;
  u64 own_late = 0;  // displayed frames late without a rollback before them (this PC's own speed)
  // "Falling behind" (UpdateSlowNotice): the last check of this PC's own speed, the checks in a
  // row that found it slow, its frame rate once it is (0: not; sent to the peers), and whether
  // this match showed the notice.
  s64 slow_check_frame = -1;
  u64 slow_check_own_late = 0;
  Clock::time_point slow_check_t{};
  int slow_checks = 0;
  double slow_fps = 0;
  bool slow_shown = false;
  // Slippi's frame advance (UpdateTimeSync): catch-up frames left in this batch, whether a batch
  // runs, and whether the frame running now / the previous displayed frame was one (its throttle
  // point is a frame early on purpose, not late).
  int frames_to_advance = 0;
  bool advancing = false;
  s64 last_advance_frame = -1;
  bool advanced_this_frame = false, prev_frame_advanced = false;
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
    u64 advances = 0;  // catch-up frames (UpdateTimeSync)
    u64 followed = 0;  // frames run at the speed of a peer that cannot keep full speed
    double peer_fps_sum = 0;  // the peer's frame rate over the followed frames
    double wall_ms = 0;  // wall-clock time of the window (its frames per second: the game's speed)
    double stall_ms = 0, stall_ms_max = 0;
    double ahead_sum = 0, ahead_min = 1e9, ahead_max = -1e9;
    double speed_min = 1e9, speed_max = -1e9;
    double pad_age_sum = 0, pad_age_max = 0;
    u64 pad_age_n = 0;
    Rollback::PresentStats::Cadence cadence;  // presents, and hitches on a 59.94 Hz screen
  };
  NetStats net_window, net_total;
  s64 net_window_first = -1;
  Clock::time_point net_window_t0{};
  bool prev_frame_rolled_back = false;
  Clock::time_point burst_t0{};
  bool burst_open = false;
  // Frame cost profile (Rollback::FrameProfile, docs/gameplay-rollback-status.md "Frame cost
  // profile"): where re-run and presented passes spend their wall-clock time. A pass runs from
  // the end of its loop top (RunFrame) to the next loop top.
  struct PassCost
  {
    u64 passes = 0;
    u64 guest_ns = 0;     // the pass: the game's frame
    u64 save_ns = 0;      // the snapshot after it, with its checksum (the next loop top)
    u64 loop_top_ns = 0;  // the loop top before it, without loads and saves (presented passes:
                          // GekkoNet, the network, waits for the peer, time sync)
    Rollback::FrameProfile::Counters c;  // during the pass
    u64 event_passes = 0;  // passes whose CoreTiming events were timed (c.event_ns, c.events)
  };
  struct FrameCost
  {
    PassCost resim, shown;
    u64 loads = 0, load_ns = 0;
  };
  FrameCost cost_window, cost_total;
  s64 cost_window_first = -1;
  bool cost_pass_open = false;
  bool cost_pass_resim = false;  // the pass open, or the last one closed
  bool cost_pass_events = false;  // the pass open times its CoreTiming events
  u64 cost_pass_serial = 0;
  Clock::time_point cost_pass_t0{};
  Clock::time_point cost_loop_top_t0{};
  u64 cost_loop_top_excl_ns = 0;  // loads and saves in the current loop top
  Rollback::FrameProfile::Counters cost_pass_c0;
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
  // The pass that runs now, for the audio clock's pause (CoreTimingManager::SetRollbackAudioWaits).
  s64 audio_pass_start = 0;
  bool audio_pass_resim = false;
  std::map<u32, u64> snd_gen;  // sound object -> allocation count
  u32 snd_reattach = 0;        // the sound the current detail_SetupSound re-attaches
  u64 snd_reattach_gen = 0;
  u32 snd_player = 0;          // SoundArchivePlayer (from detail_SetupSound)
  std::deque<SndEntry> snd_to_stop;
  u64 sound_reattached = 0, sound_reattach_gone = 0, sound_stopped = 0, sound_stop_gone = 0;
  std::array<u64, 3> snd_gone_why{};  // diagnostics: reallocated, other id, not allocated
  u64 sound_moved = 0;               // re-attached from another handle (playSE table slots)
  u64 sound_stops_ignored = 0;       // stops of sounds the resimulated timeline has not started
  // The mtRand calls of each recent frame's newest pass (OnRngCall), written to the log when a
  // desync is seen (LogRngCalls): which code drew how many numbers, so that the diverging
  // machine's own log names the call that one machine made and the other did not.
  struct RngCall
  {
    u32 rng = 0;    // the generator (g_mtRand, g_mtRandOther, ...)
    u32 state = 0;  // its seed before the call
    std::array<u32, 6> callers{};  // [0] is randi/randf's (mtRand::generate's LR), then up the stack
  };
  struct RngFrame
  {
    s64 frame = -1;
    u64 pass = 0;
    bool first_run = false;
    u32 count = 0;  // calls in the pass (more than the array holds: the rest are counted only)
    std::array<RngCall, 48> calls{};
  };
  std::array<RngFrame, 64> rng_frames{};
  bool rng_logged = false;  // the calls were logged for this match's first desync
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
  // Diagnostics (PPR_GPRB_FORCE_FINAL_AT): Final Smashes given in the match, before a session
  // frame's pass, in every pass of that frame or only in its first run.
  struct ForceFinalAt
  {
    s64 frame;
    u32 port;
    bool first_only;
  };
  std::vector<ForceFinalAt> force_final_at;
  bool force_final_at_active = false;  // a guest call for the coming pass is running
  bool force_final_at_reenter = false;  // RunFrame comes back for the same pass after the call
  GuestRegs force_final_at_saved;
  u64 pass_serial = 0;  // counts session passes (diagnostics)
  u64 mispredicted_passes = 0;  // misprediction sync test: first runs given a wrong input
  // Pass log (PPR_GPRB_PASS_LOG=path): every GekkoNet update's load and passes, for replays.
  FILE* pass_log = nullptr;
  int last_load_back = 0;
  bool last_initial_save = false;
  // Replay (SyncTestOptions::replay_path).
  FILE* replay = nullptr;
  bool replay_started = false;

  // Playback (Mode::Playback): the replay, the next frame to play, the first frame whose
  // checksum differed from the file's (-1: none), and how the recorded game ended.
  struct Playback
  {
    std::shared_ptr<const Replay::File> file;
    u32 start_frame = DEFAULT_START_FRAME;
    u32 match_index = 0;
    u32 seed = 0;
    std::vector<u8> init;
    std::vector<u8> setup_key;
    u32 ports = 0;
    int local_slot = 0;
    SyncBlock sync;
    std::vector<std::vector<std::string>> tasks;
    bool sync_task_order = true;
    std::vector<u8> session;
    std::array<u16, MAX_LOBBY_PLAYERS> holds{};  // the countdown's buttons (SETP "holds")
    std::string end_method;
    s64 final_frame = -1;
    bool play_anyway = false;
    // A room's game watched from the room CSS (ArmPlayback's `room_watch`, Online/RoomWatch.h):
    // the game leaves the room CSS for its match and comes back to it.
    bool room_watch = false;
    u32 serial = 0;  // bumped for every replay armed (the game starts each one once)
    size_t next = 0;
    s64 desync_frame = -1;
    std::string desync_text;
    u64 frames_checked = 0;
    bool started = false;   // its match started (the barrier passed)
    bool finished = false;  // its last frame ran
    // After its last frame, the end the live game showed: DISCONNECTED, DESYNC DETECTED, or the
    // end of a 3-4 player match this player quit (LOCAL).
    bool end_disconnected = false;
    bool end_desynced = false;
    bool end_quit_left = false;
    // StopPlayback with the game in (or on its way into) its match: the session ends at the next
    // loop top and the game leaves the match as after this player's quit of a 3-4 player match
    // (end_quit_left), straight to the CSS; the next replay arms once it is out (PlaybackStopping).
    bool stopped = false;
    std::string error;
    // Controls (PlaybackControl): paused at the loop top, frames to run before pausing again, a
    // seek's target (the frame to show; -1 none), the frames before which passes run unpresented
    // (a seek's fast-forward), the speed, and whether to hold at the last frame (the last replay
    // of what was asked) instead of ending the match.
    bool paused = false;
    int step = 0;
    s64 seek_to = -1;
    s64 ff_until = -1;
    double speed = 1.0;
    bool hold_at_end = false;
    bool at_end = false;
    // A highlight's frames (SetPlaybackClip): paused once frame clip_end has run (-1: none);
    // clip_ff while the fast-forward to its start runs.
    s64 clip_start = -1;
    s64 clip_end = -1;
    bool clip_done = false;
    bool clip_ff = false;
    // For the overlay: who played (INFO), the mode and game.
    std::array<std::string, MAX_LOBBY_PLAYERS> names{};
    std::array<u8, MAX_LOBBY_PLAYERS> chars{};
    std::string title;
    // Spectating (ArmPlayback's `live`, docs/design/spectating.md §6.2): the file grows
    // (`live_file` is `file`, writable under s.mutex: ExtendPlayback); the catch-up to the live
    // edge, its rate (frames run, since when), the wait for frames, the speed nudge that keeps the
    // buffer, and the overlay's banner.
    std::shared_ptr<Replay::File> live_file;
    bool live = false;
    bool catch_up_at_start = false;  // ArmPlayback's `catch_up`
    bool join_catch_up = false;      // ... and the barrier started that catch-up
    bool catch_up = false;
    // The catch-up a spectator starts with (not Go Live's) runs muted: the sound stream's volume
    // is 0 meanwhile and comes back from the user's settings after (UpdateSoundStream).
    bool joining = false;
    bool muted = false;
    s64 catch_frames = 0;
    Clock::time_point catch_since{};
    double catch_rate = 0;
    bool buffering = false;
    double nudge = 1.0;
    std::string banner;
    bool cut = false;  // the rest of the live game won't come
    // PlaybackControl calls (any of them cancels a spectated game's close countdown).
    u32 controls = 0;
  } pb;
  // Keyframes for seeking (ReplayKeyframes.h), from the replay's first frame on.
  std::unique_ptr<Replay::Keyframes> pb_keys = std::make_unique<Replay::Keyframes>();

  // Diagnostics: region-set samples (ConfigureSamples).
  u32 sample_every = 0;
  std::vector<std::pair<u32, u32>> sample_watch;
  std::map<s64, std::vector<u64>> sample_chunks;
  std::map<s64, std::vector<u8>> sample_bytes;
  std::vector<Range> sample_ranges;  // the region set the samples were taken over
};

State s;

// LinkHealth (GetLinkHealth): written on the CPU thread, read by the spectate uploader.
std::atomic<bool> s_health_running{false};
std::atomic<u64> s_health_late{0};
std::atomic<double> s_health_rollback{0};
std::atomic<double> s_health_ping{-1};
// The rollback depth's running average (CPU thread only): about 2 s at 60 frames a second.
double s_rollback_ema = 0;
constexpr double ROLLBACK_EMA_ALPHA = 1.0 / 120;

std::string NetStatsLine(const State::NetStats& w);
void CloseNetWindowClock();
void NetStatsFold(State::NetStats& t, const State::NetStats& w);
void LogPerfVerdict(const State::NetStats& t);
}  // namespace

bool InMatchPhase();
void EndReplay(Core::System& system, const std::string& reason, const GameResult* result);

namespace
{
// Sound bookkeeping (defined with OnSoundAlloc below).
bool RunSoundStops(const Core::CPUThreadGuard& guard);
void CommitSoundPass();
bool PauseResimAudio();
// File loads in a match (defined with RunSoundStops below).
bool RunIoWait(const Core::CPUThreadGuard& guard);
bool RunForceFinal(const Core::CPUThreadGuard& guard);
bool RunForceFinalAt(const Core::CPUThreadGuard& guard, s64 frame, bool first_run);
void SaveGuestRegs(const PowerPC::PowerPCState& ppc, State::GuestRegs* out);
void RestoreGuestRegs(PowerPC::PowerPCState& ppc, const State::GuestRegs& in);

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

// Brawl's screen setting (Options > Screen; at boot the game copies the Wii's aspect ratio, SYSCONF
// IPL.AR, into it): gmGlobalRecord (GameGlobal+0x24) menu data +0x810, byte +0x28 (0x9017BE78 on
// P+), 0xC0 with 16:9 and 0x00 with 4:3. The game frames its camera with it, and a screen KO places
// the fighter from the camera: peers with different settings desynced on every screen KO (prod
// 2026-10-12, docs/gameplay-rollback-status.md Phase 25). Every match is played with the value a
// fresh install has (16:9); how the picture is shown (Dolphin's aspect ratio, window, resolution)
// stays each player's.
constexpr u32 GG_RECORD = 0x24;
constexpr u32 RECORD_SCREEN = 0x810 + 0x28;
constexpr u8 SCREEN_BITS = 0xC0, SCREEN_WIDE = 0xC0;

void PinScreenSetting(Core::System& system)
{
  const Guest g(system.GetMemory());
  const auto gg = g.Ptr32(Addr::GAME_GLOBAL_PTR);
  const auto record = gg ? g.Ptr32(*gg + GG_RECORD) : std::nullopt;
  const u8* p = record ? g.Ptr(*record + RECORD_SCREEN, 1) : nullptr;
  if (!p)
  {
    WARN_LOG_FMT(BRAWLBACK, "gprb: no game record: the screen setting is left as it is");
    return;
  }
  const u8 was = *p;
  const u8 now = static_cast<u8>((was & ~SCREEN_BITS) | SCREEN_WIDE);
  if (now == was)
    return;
  system.GetMemory().CopyToEmu(*record + RECORD_SCREEN, &now, 1);
  INFO_LOG_FMT(BRAWLBACK, "gprb: screen setting {:02x} -> {:02x} (16:9) for the match", was, now);
}

constexpr u32 PAD_CONFIG = 0x805B7480;  // g_PadConfig (ipPadConfig, 0x1AC bytes)
constexpr u32 PAD_CONFIG_KEY_SIZE = 0xB9;

// The match's rules as the game applied them (OnlineMenu::applyRules, from SESSION's Rules):
// Brawl's set rule (GameGlobal+0x1C, the 16 bytes the plugin writes), and the rest of the match
// setup the rules reach: P+'s hazard switch (gmMeleeInitData+0x21 bit 0x20, gmGlobalModeMelee
// +0x29) and the item switch (gmMeleeInitData::m_itSwitch, +0x30..+0x40).
constexpr u32 GG_SET_RULE = 0x1C;
constexpr u32 SET_RULE_KEY_SIZE = 0x10;
// gmSetRule::m_handicap (P+'s STOCK CONTROL: 2 on) and gmPlayerInitData +0x26 (the handicap,
// u16; P+ reads it in tenths of a stock).
constexpr u32 SET_RULE_HANDICAP = 5;
constexpr u8 HANDICAP_STOCK_CONTROL = 2;
constexpr u32 MM_PLAYER_HANDICAP = 0x26;
constexpr u32 MM_HAZARD = 0x29, MM_HAZARD_OFF = 0x20;
constexpr u32 MM_ITEM_SWITCH = 0x30, ITEM_SWITCH_SIZE = 0x10;

// P+'s Code Menu (pf/menu3/dnet.cmnu, loaded to 0x804E0000): its gameplay settings (auto
// L-cancelling, the hitstun and shield modifiers, Special Modes...) are read straight from its
// lines by P+'s codes, so every machine must have the same. Online the plugin writes every value
// line (its defaults, the session's auto L-cancel; online_menu.cpp codeMenuLines). The lines are
// found by the same layout the plugin uses (fudgepop's: +0 size u16, +2 type, +6 text offset,
// +8 value, a selection's options pointer at +0x18). The per-player Character Select lines are
// left out: P+ keeps the fighter being played in them.
constexpr u32 CODE_MENU = 0x804E0000, CODE_MENU_SIZE = 0x2520;

void AppendCodeMenuValues(const Guest& g, std::vector<u8>& key)
{
  const u8* m = g.Ptr(CODE_MENU, CODE_MENU_SIZE);
  if (!m)
    return;
  for (u32 o = 0; o + 0x20 <= CODE_MENU_SIZE; o += 4)
  {
    const u8* l = m + o;
    const u32 size = u32(l[0]) << 8 | l[1];
    const u32 type = l[2], text = l[6];
    if ((size & 3) || size < 0x20 || size > 0x400 || o + size > CODE_MENU_SIZE)
      continue;
    if (type > 2 || text < 0x1C || text >= size)
      continue;
    const u32 options = u32(l[0x18]) << 24 | u32(l[0x19]) << 16 | u32(l[0x1A]) << 8 | l[0x1B];
    if (type == 0 ? (options < CODE_MENU || options >= CODE_MENU + CODE_MENU_SIZE) : text != 0x20)
      continue;
    const std::string_view t(reinterpret_cast<const char*>(l) + text, size - text);
    if (!std::isalpha(static_cast<unsigned char>(t[0])))
      continue;
    const size_t percent = t.find('%');
    if (percent == std::string_view::npos ||
        std::any_of(t.begin(), t.begin() + percent, [](char c) {
          return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F;
        }))
    {
      continue;
    }
    if (t.substr(0, percent).find("Character Select") != std::string_view::npos)
      continue;
    key.insert(key.end(), l + 8, l + 12);
  }
}

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
  // The rules as applied (docs/design/room-rules.md §1 #5): a machine that applied other rules
  // (an older plugin, a rule it could not write) refuses the match instead of playing it.
  if (const auto rule = g.Ptr32(*gg + GG_SET_RULE))
  {
    if (const u8* r = g.Ptr(*rule, SET_RULE_KEY_SIZE))
    {
      key.insert(key.end(), r, r + SET_RULE_KEY_SIZE);
      // Stock control (set rule +5 = 2, P+'s "on"): each player's picked count, the match
      // player's +0x26 halfword (tenths of a stock), which P+'s "Handicap = Stock Count" makes
      // that player's stocks when the fighter is made. Only then, so that every other game's key
      // is as before (replays compare it).
      if (r[SET_RULE_HANDICAP] == HANDICAP_STOCK_CONTROL)
      {
        for (u32 i = 0; i < 4; ++i)
        {
          const u8* pl = p + 0x98 + i * 0x5C;
          key.push_back(pl[MM_PLAYER_HANDICAP]);
          key.push_back(pl[MM_PLAYER_HANDICAP + 1]);
        }
      }
    }
  }
  key.push_back(p[MM_HAZARD] & MM_HAZARD_OFF);
  key.insert(key.end(), p + MM_ITEM_SWITCH, p + MM_ITEM_SWITCH + ITEM_SWITCH_SIZE);
  AppendCodeMenuValues(g, key);
  // A desynced game played again: the restored stocks, damage and time limit the game applies from
  // SESSION (the stocks and the time are in the setup above too). Only then, so that every other
  // game's key is as before (replays compare it).
  namespace GB = Online::GameBridge;
  const u32 session = GB::SessionAddress();
  const u8* restore = session ? g.Ptr(session + GB::SESSION_RESTORE, 1) : nullptr;
  if (restore && *restore)
  {
    for (u32 port = 0; port < MAX_LOBBY_PLAYERS; ++port)
    {
      const u32 pl = session + GB::SESSION_PLAYERS_OFF + port * GB::SESSION_PLAYER_SIZE;
      if (const u8* st = g.Ptr(pl + GB::SESSION_PLAYER_RESTORE_STOCKS, 1))
        key.push_back(*st);
      if (const u8* d = g.Ptr(pl + GB::SESSION_PLAYER_RESTORE_DAMAGE, 4))
        key.insert(key.end(), d, d + 4);
    }
    if (const u8* t = g.Ptr(session + GB::SESSION_RESTORE_FRAMES, 4))
      key.insert(key.end(), t, t + 4);
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

// How the countdown's input is made (CountdownSlots), in the setup hash: 1 = each player's
// buttons held at the barrier (2026-10-14; builds before ran it neutral and hash without it).
constexpr u8 COUNTDOWN_INPUT_VERSION = 1;

// The setup key, the session's start frame and how the countdown's input is made: peers that
// would start the rollback on different frames (builds with different start frames) or run the
// countdown with different input fail the barrier instead of desyncing.
u64 HashSetupKey(std::vector<u8> key, u32 start_frame, u8 countdown_version)
{
  if (key.empty())
    return 0;
  for (int shift = 24; shift >= 0; shift -= 8)
    key.push_back(static_cast<u8>(start_frame >> shift));
  key.push_back(countdown_version);
  return XXH3_64bits(key.data(), key.size());
}

u64 SetupHash(const Guest& g, u32 start_frame)
{
  return HashSetupKey(SetupKey(g), start_frame, COUNTDOWN_INPUT_VERSION);
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
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    rec->damage[port] = fighters[port][1];
    rec->stocks[port] = fighters[port][2];
  }
  rec->frames_left = RemainingFrames(g);
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

// A control message to every other player, `copies` times: one 'C' packet, or 'F' pieces when
// one datagram would be fragmented on the way (PeerData::ControlDatagrams). Returns the number
// of datagrams one copy took.
size_t SendControlToAll(std::string_view text, int copies = 1)
{
  size_t sent = 0;
  for (int i = 0; i < copies; ++i)
  {
    const auto datagrams = PeerData::ControlDatagrams(text, s.control_id.fetch_add(1));
    for (const auto& d : datagrams)
      SendToAll(d);
    sent = datagrams.size();
  }
  return sent;
}

// Tells the other players that this one has left the session (under s.mutex), with why when the
// session gave up (they log it). Slippi's graceful ENet disconnect: the others go on without it
// (3-4 players) or end the session at once, instead of waiting at a barrier for their timeout or
// for the 7.2 s silence. UDP: sent a few times.
void SendLeave(std::string_view why)
{
  if (s.mode != Mode::Network || !s.socket)
    return;
  picojson::object o;
  o["t"] = picojson::value("leave");
  o["v"] = picojson::value(1.0);
  o["slot"] = picojson::value(static_cast<double>(s.local_slot));
  if (!why.empty())
    o["why"] = picojson::value(PeerData::LeaveReason(why));
  const std::string text = picojson::value(o).serialize();
  SendControlToAll(text, 3);
  s.leave_sent = true;
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
void SendDesync(s64 frame)
{
  if (!s.socket)
    return;
  // "f": the first frame whose checksums differ (older builds ignore it).
  const std::string text =
      fmt::format(R"({{"t":"desync","v":1,"m":{},"f":{}}})", s.match_index, frame);
  SendControlToAll(text, 3);
}

// The messages GekkoSend holds for one peer while a GekkoBatch is open (a 'B' datagram,
// PeerData::AppendGekkoBundle), sent when it closes (under s.mutex).
void FlushGekkoBundle(int slot)
{
  auto& b = s.gekko_bundle[slot];
  if (b.empty())
    return;
  const auto msgs = PeerData::SplitGekkoBundle(b.data(), b.size());
  if (msgs && msgs->size() == 1)
  {
    // One message: as a 'G' packet, 2 bytes smaller.
    std::vector<u8> pkt((*msgs)[0].size() + 1);
    pkt[0] = 'G';
    std::memcpy(pkt.data() + 1, (*msgs)[0].data(), (*msgs)[0].size());
    SendRaw(slot, pkt);
  }
  else
  {
    SendRaw(slot, b);
  }
  b.clear();
}

void GekkoSend(GekkoNetAddress* addr, const char* data, int length)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  const int slot = GekkoAddrSlot(addr);
  if (s.gekko_batch > 0 && slot >= 0 && s.peers[slot].gekko_bundles)
  {
    const auto* msg = reinterpret_cast<const u8*>(data);
    auto& b = s.gekko_bundle[slot];
    if (PeerData::AppendGekkoBundle(b, msg, static_cast<size_t>(length)))
      return;
    FlushGekkoBundle(slot);
    if (PeerData::AppendGekkoBundle(b, msg, static_cast<size_t>(length)))
      return;
  }
  std::vector<u8> pkt(static_cast<size_t>(length) + 1);
  pkt[0] = 'G';
  std::memcpy(pkt.data() + 1, data, static_cast<size_t>(length));
  SendRaw(slot, pkt);
}

// While one is open, what GekkoNet sends to a peer that takes 'B' datagrams waits and goes out as
// one datagram when the last one closes. Open around the GekkoNet calls of one frame (the network
// poll, the local input and the session update, microseconds apart): the frame's inputs, the ack
// of the peer's inputs that poll received and the checksum the previous update queued then
// travel together, one datagram a frame instead of about four. Never open one across emulation:
// the messages would wait for it.
struct GekkoBatch
{
  GekkoBatch()
  {
    std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
    ++s.gekko_batch;
  }
  ~GekkoBatch()
  {
    std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
    if (--s.gekko_batch == 0)
    {
      for (int slot = 0; slot < MAX_LOBBY_PLAYERS; ++slot)
        FlushGekkoBundle(slot);
    }
  }
  GekkoBatch(const GekkoBatch&) = delete;
  GekkoBatch& operator=(const GekkoBatch&) = delete;
};

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
  if (l.stage_pick != NO_STAGE && l.stage_pick != STAGE_PENDING &&
      !PeerData::ValidStageKind(l.stage_pick))
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
  if (st.restore.valid)
  {
    // The agreed frame, its checksum, per port the stocks and the damage (the float's bits, so
    // every machine has the same value), and the timer.
    const Restore& r = st.restore;
    picojson::array stocks;
    std::vector<u8> damage;
    for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
    {
      stocks.emplace_back(static_cast<double>(r.stocks[port]));
      const u32 bits = std::bit_cast<u32>(r.damage[port]);
      for (int shift = 24; shift >= 0; shift -= 8)
        damage.push_back(static_cast<u8>(bits >> shift));
    }
    picojson::object rs;
    rs["f"] = picojson::value(static_cast<double>(r.frame));
    rs["c"] = picojson::value(static_cast<double>(r.checksum));
    rs["s"] = picojson::value(stocks);
    rs["d"] = picojson::value(Hex(damage));
    rs["t"] = picojson::value(static_cast<double>(r.frames_left));
    o["rs"] = picojson::value(rs);
  }
  return picojson::value(o);
}

// The host's restore values (SetupJson's "rs"): every value in the range the game can play, else
// none (the setup is then refused as a whole).
std::optional<Restore> RestoreFromJson(const picojson::value& v)
{
  if (!v.is<picojson::object>())
    return std::nullopt;
  const auto& o = v.get<picojson::object>();
  const auto field = [&](const char* k) -> const picojson::value* {
    const auto it = o.find(k);
    return it == o.end() ? nullptr : &it->second;
  };
  const auto frame = PeerData::JsonUInt(field("f"), 0x7FFFFFFF);
  const auto crc = PeerData::JsonUInt(field("c"), 0xFFFFFFFF);
  const auto left = PeerData::JsonUInt(field("t"), MAX_RESTORE_FRAMES);
  const picojson::value* stocks = field("s");
  const picojson::value* damage = field("d");
  if (!frame || !crc || !left || !stocks || !stocks->is<picojson::array>() ||
      stocks->get<picojson::array>().size() != MAX_LOBBY_PLAYERS || !damage ||
      !damage->is<std::string>())
  {
    return std::nullopt;
  }
  const auto bytes = PeerData::ParseHex(damage->get<std::string>(), 4 * MAX_LOBBY_PLAYERS);
  if (!bytes)
    return std::nullopt;
  Restore r;
  r.valid = true;
  r.frame = static_cast<s64>(*frame);
  r.checksum = static_cast<u32>(*crc);
  r.frames_left = static_cast<u32>(*left);
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    const auto n = PeerData::JsonUInt(&stocks->get<picojson::array>()[port], MAX_RESTORE_STOCKS);
    if (!n)
      return std::nullopt;
    r.stocks[port] = static_cast<u8>(*n);
    u32 bits = 0;
    for (int i = 0; i < 4; ++i)
      bits = (bits << 8) | (*bytes)[port * 4 + i];
    const float d = std::bit_cast<float>(bits);
    if (!(d >= 0.0f && d <= MAX_RESTORE_DAMAGE))
      return std::nullopt;
    r.damage[port] = d;
  }
  return r;
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
  if (const auto rs = o.find("rs"); rs != o.end())
  {
    const auto r = RestoreFromJson(rs->second);
    if (!r)
      return std::nullopt;
    st.restore = *r;
  }
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
std::atomic<bool> s_quit_loses{false};  // SetQuitLoses
std::atomic<bool> s_desync_recovery{false};  // SetDesyncRecovery
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

// The values a game restarted after a desync takes from frame record `rec` (Restore), for the
// players of `st`. Under s.mutex.
Restore RestoreOf(const FrameRecord& rec, const MatchSetup& st)
{
  Restore r;
  r.valid = true;
  r.frame = rec.frame;
  r.checksum = rec.checksum;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if (!st.players[port].present)
      continue;
    const s32 stocks = static_cast<s32>(rec.stocks[port]);
    r.stocks[port] = static_cast<u8>(std::clamp<s32>(stocks, 0, MAX_RESTORE_STOCKS));
    const float d = std::bit_cast<float>(rec.damage[port]);
    r.damage[port] = d >= 0.0f ? std::min(d, MAX_RESTORE_DAMAGE) : 0.0f;  // NaN: 0
  }
  r.frames_left = std::min(rec.frames_left, MAX_RESTORE_FRAMES);
  return r;
}

// Host, under s.mutex, every player locked in for `game` after a desync where the game starts
// again (s.recovery): the desynced game's setup again (players, characters, stage), from the last
// session frame whose checksum is the same on every machine, in a row from the first frame all
// their lists have (s.recovery.list and the others' "dsr"; a frame no machine recorded ends the
// row). No such frame: the game starts from the beginning. False when the restart does not apply
// (the players changed): the usual setup then.
bool DecideRestart(u32 game, const std::array<const LockIn*, MAX_LOBBY_PLAYERS>& locks)
{
  const MatchSetup& prev = s.recovery.setup;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if ((locks[port] != nullptr) != prev.players[port].present)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb lobby: game {}: the players are not the desynced game's; "
                              "a new game instead",
                   game);
      s.recovery.pending = false;
      return false;
    }
  }
  const RecoveryList& own = s.recovery.list;
  std::vector<const RecoveryList*> lists{&own};
  bool all = true;
  ForEachPeer([&](int, const PeerView& p) {
    if (p.left)
      return;
    if (p.recovery && p.recovery->match == own.match)
      lists.push_back(&*p.recovery);
    else
      all = false;
  });
  if (!all)
    return true;  // a player's checksums have not come yet (they come with every state message)
  s64 lo = 0, hi = std::numeric_limits<s64>::max();
  for (const RecoveryList* l : lists)
  {
    lo = std::max(lo, l->from);
    hi = std::min(hi, l->from + static_cast<s64>(l->crcs.size()) - 1);
  }
  s64 agreed = -1;
  for (s64 f = lo; f <= hi; ++f)
  {
    const u32 c = own.crcs[static_cast<size_t>(f - own.from)];
    const bool same = c != 0 && std::all_of(lists.begin(), lists.end(), [&](const RecoveryList* l) {
                        return l->crcs[static_cast<size_t>(f - l->from)] == c;
                      });
    if (!same)
      break;
    agreed = f;
  }
  MatchSetup st = prev;
  st.game = game;
  // No agreed frame: the game's own start, which for a game that was itself restarted is its
  // restored values (frame 0, checksum 0: no frame record to compare).
  if (st.restore.valid)
  {
    st.restore.frame = 0;
    st.restore.checksum = 0;
  }
  const FrameRecord* rec =
      agreed >= 0 ? &s.history[static_cast<size_t>(agreed) % s.history.size()] : nullptr;
  if (rec && rec->frame == agreed)
    st.restore = RestoreOf(*rec, st);
  s.setup = st;
  s.last_stage = st.stage;
  std::string what;
  for (int port = 0; port < MAX_LOBBY_PLAYERS && st.restore.valid; ++port)
  {
    if (st.players[port].present)
      what += fmt::format(", P{} {} stocks {:.2f}%", port + 1, st.restore.stocks[port], st.restore.damage[port]);
  }
  if (agreed >= 0)
  {
    INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} setup: the desynced game again from frame {} "
                            "(checksum {:08x}, frames {}-{} compared on {} machines){}, {} frames left",
                 game, agreed, st.restore.checksum, lo, hi, lists.size(), what, st.restore.frames_left);
  }
  else
  {
    WARN_LOG_FMT(BRAWLBACK, "gprb lobby: game {} setup: the desynced game again from its start{} "
                            "(no frame of {}-{} is the same on all {} machines)",
                 game, st.restore.valid ? " (its restored values)" : "", lo, hi, lists.size());
  }
  return true;
}

// Host, under s.mutex: once every player is locked in for the next game, decide its setup. The
// stage: the pick of the player who lost the last game (waited for), else random from the
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
  // After a desync where the game starts again: the same game, whatever the lock-ins say (the
  // characters stay locked on the CSS).
  if (s.recovery.pending && s.recovery.list.match + 1 == s.match_index && DecideRestart(game, locks))
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
  // The stage pick: the last game's loser's (s.last_pickers: a 1v1's loser, both after a draw;
  // the game sends the lower port's), no one else's. A picker locks in with the character first
  // (STAGE_PENDING) and goes to the stage select once everyone is locked in: wait for that pick.
  std::array<u16, Online::GameSetup::MAX_PORTS> picks{NO_STAGE, NO_STAGE, NO_STAGE, NO_STAGE};
  bool pick_pending = false;
  for (size_t i = 0; i < locks.size(); ++i)
  {
    if (!locks[i])
      continue;
    if (locks[i]->stage_pick == STAGE_PENDING)
      pick_pending = pick_pending || ((s.last_pickers >> i) & 1);
    else
      picks[i] = locks[i]->stage_pick;
  }
  const int pick_port = Online::GameSetup::StagePickPort(s.last_pickers, picks);
  if (!decided.applies && pick_pending && pick_port < 0)
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
  const LockIn* pick = pick_port >= 0 ? locks[pick_port] : nullptr;
  if (pick && s.net_opts.pick_from_stages)
  {
    // A room whose loser picks from the list: the game's stage select offers only those stages;
    // a pick that is not one of them (a game without that check) is drawn as without a pick.
    const auto& allowed = AllowedStages();
    if (std::find(allowed.begin(), allowed.end(), pick->stage_pick) == allowed.end())
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb lobby: P{}'s stage pick {:#x} is not on the room's list; drawn instead",
                   pick_port + 1, pick->stage_pick);
      pick = nullptr;
    }
  }
  if (decided.applies)
  {
    st.stage = decided.stage;
    st.asl = decided.asl;
  }
  else if (pick)
  {
    st.stage = pick->stage_pick;
    // The alternate stage the held buttons select only where the match plays them; else the
    // stage's normal layout, whatever was held.
    st.asl = s.net_opts.alternate_stages ? pick->asl : 0;
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

// In a running match the 50 ms tick sends the state message only when it changed (then the next
// STATE_REPEATS ticks send it again, as a lost copy was replaced within 50 ms before) or after
// STATE_KEEPALIVE: it holds the menus' and barriers' steps, which do not change in a match, and
// GekkoNet carries the match itself. Before 2026-10-13 it went every 50 ms throughout (a joiner's
// about 430 bytes: 71 kbit/s, nearly half of its upload).
constexpr int STATE_REPEATS = 2;
constexpr auto STATE_KEEPALIVE = std::chrono::milliseconds(500);

// `tick`: from the network thread's 50 ms tick (else: a step a peer waits for, sent at once).
void SendState(bool tick = false)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  picojson::object o;
  {
    const bool host = IsHost();
    o["t"] = picojson::value("st");
    o["v"] = picojson::value(1.0);
    // Takes GekkoNet's messages bundled ('B', GekkoBatch).
    o["gb"] = picojson::value(1.0);
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
    if (s.at_s)
      o["hold"] = picojson::value(static_cast<double>(s.hold));
    // The setup hash's bytes while at the barrier (about 0.9 KB), so that a peer whose hash
    // differs logs which fields do (bug report #40: only the hashes were known).
    if (s.at_s && s.phase == Phase::Barrier && !s.setup_key.empty())
    {
      o["setup_key"] = picojson::value(Hex(s.setup_key));
      o["setup_start"] = picojson::value(static_cast<double>(s.setup_start));
      o["setup_cv"] = picojson::value(static_cast<double>(COUNTDOWN_INPUT_VERSION));
    }
    o["lock"] = LockJson(s.local_lock);
    if (host && s.setup.game != 0)
      o["match_setup"] = SetupJson(s.setup);
    o["winner"] = picojson::value(static_cast<double>(s.last_winner));
    if (s.slow_fps > 0)
      o["slow"] = picojson::value(std::round(s.slow_fps * 10) / 10);
    if (!s_local_extra.empty())
      o["x"] = picojson::value(s_local_extra);
    if (s.recovery.pending)
    {
      // After a desync where the game starts again: our checksums of the last confirmed frames
      // (8 hex digits each), for the host to find the last frame every machine agrees on.
      const RecoveryList& l = s.recovery.list;
      std::string crcs;
      crcs.reserve(l.crcs.size() * 8);
      for (const u32 c : l.crcs)
        crcs += fmt::format("{:08x}", c);
      picojson::object dsr;
      dsr["m"] = picojson::value(static_cast<double>(l.match));
      dsr["from"] = picojson::value(static_cast<double>(l.from));
      dsr["crc"] = picojson::value(crcs);
      o["dsr"] = picojson::value(dsr);
    }
    if (host && !s.init_block.empty())
    {
      o["init"] = picojson::value(Hex(s.init_block));
      o["init_match"] = picojson::value(static_cast<double>(s.init_match));
    }
    // The sync block and task order (about 5.5 KB, 5 datagrams) only until every joiner has
    // applied them: the host goes on into the countdown only then (all_applied), so from the
    // countdown on nobody reads them (a joiner keeps the copy it has until the match ends).
    // Before 2026-10-13 the host sent them with every state message of the match, 20 a second:
    // about 650 kbit/s, nearly all of the host's upload.
    const bool past_barrier = s.phase == Phase::Countdown || s.phase == Phase::StartBarrier ||
                              s.phase == Phase::Running;
    if (host && s.at_s && s.sync.valid && !past_barrier)
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
      // Every port's countdown buttons, once every player's hold is in (joiners wait for them).
      if (s.holds_valid)
      {
        picojson::array holds;
        for (const u16 h : s.holds)
          holds.emplace_back(static_cast<double>(h));
        o["holds"] = picojson::value(holds);
      }
    }
  }
  std::string text = picojson::value(o).serialize();
  const auto now = Clock::now();
  if (text != s.state_sent)
    s.state_repeats = STATE_REPEATS;
  else if (tick && s.phase == Phase::Running && s.state_repeats <= 0 &&
           now - s.state_sent_at < STATE_KEEPALIVE)
    return;
  else if (tick && s.state_repeats > 0)
    --s.state_repeats;
  s.state_sent_at = now;
  if (text.size() > PeerData::MAX_CONTROL_SIZE && !s.state_size_logged)
  {
    // The others refuse a control message this large (PeerData::ParseControl).
    s.state_size_logged = true;
    const auto tasks = o.find("tasks");
    ERROR_LOG_FMT(BRAWLBACK,
                  "gprb: our state message is {} bytes, over the {} the other players accept "
                  "(task order {} bytes)",
                  text.size(), PeerData::MAX_CONTROL_SIZE,
                  tasks != o.end() ? tasks->second.serialize().size() : 0);
  }
  const size_t datagrams = SendControlToAll(text);
  s.state_sent = std::move(text);
  if (datagrams > 1 && !s.split_logged)
  {
    s.split_logged = true;
    INFO_LOG_FMT(BRAWLBACK, "gprb: our state message is {} bytes: sent as {} datagrams of at most {}",
                 s.state_sent.size(), datagrams, PeerData::MAX_DATAGRAM);
  }
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
  {
    picojson::object ev;
    ev["type"] = picojson::value("left");
    ev["port"] = picojson::value(static_cast<double>(slot + 1));
    ev["reason"] = picojson::value(reason);
    Replay::AddEvent(s.current_frame, ev);
  }
  if (active < 2 && !s.peer_left)
  {
    s.peer_left = true;
    s.peer_left_reason = reason;
  }
}

// The host's task order (a peer's control message, or a replay's setup): lists of names, sizes
// capped; a name too long reads "?" (ApplyTaskOrder then finds the sets differ).
std::optional<std::vector<std::vector<std::string>>> TaskListsFromJson(const picojson::value& v)
{
  if (!v.is<picojson::array>() || v.get<picojson::array>().size() > PeerData::MAX_TASK_LISTS)
    return std::nullopt;
  std::vector<std::vector<std::string>> lists;
  for (const auto& l : v.get<picojson::array>())
  {
    if (!l.is<picojson::array>() || l.get<picojson::array>().size() > PeerData::MAX_TASKS_PER_LIST)
      return std::nullopt;
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
  return lists;
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
    // "why": its session gave up (older builds send no reason).
    const auto why = o.find("why");
    OnPeerLeft(slot, why != o.end() && why->second.is<std::string>() ?
                         "peer gave up: " + PeerData::LeaveReason(why->second.get<std::string>()) :
                         std::string("peer left"));
    return;
  }
  if (const auto t = o.find("t"); t != o.end() && t->second.is<std::string>() &&
                                  t->second.get<std::string>() == "desync")
  {
    const auto m = o.find("m");
    if (s.phase == Phase::Running && !s.desync_end && m != o.end() && m->second.is<double>() &&
        m->second.get<double>() == static_cast<double>(s.match_index))
    {
      const auto f = o.find("f");
      if (f != o.end() && f->second.is<double>() && f->second.get<double>() >= 0 &&
          f->second.get<double>() < 1e9)
      {
        s.desync_frame = static_cast<s64>(f->second.get<double>());
      }
      s.desync_end = true;
      WARN_LOG_FMT(BRAWLBACK, "gprb: the peer found the game states differ from frame {}: the game ends",
                   s.desync_frame);
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
  // Its PC is falling behind (UpdateSlowNotice): its frame rate; absent when it is not, and from
  // builds before 2026-10-12.
  {
    const auto* x = get("slow");
    p.slow_fps = x && x->is<double>() && x->get<double>() > 0 && x->get<double>() < 60 ?
                     x->get<double>() :
                     0;
  }
  if (const auto* x = get("gb"); x && x->is<double>())
    p.gekko_bundles = x->get<double>() >= 1;
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
  // The barriers' steps, logged as each player reaches them (once per match each).
  const bool was_at_s = p.at_s, was_applied = p.applied, was_at_start = p.at_start;
  if (const auto* x = get("at_s"); x && x->is<bool>())
    p.at_s = x->get<bool>();
  if (const auto* x = get("applied"); x && x->is<bool>())
    p.applied = x->get<bool>();
  if (const auto* x = get("at_start"); x && x->is<bool>())
    p.at_start = x->get<bool>();
  if (p.at_s && !was_at_s)
    INFO_LOG_FMT(BRAWLBACK, "gprb: P{} is at the barrier", slot + 1);
  if (p.applied && !was_applied && slot != HostSlot())
    INFO_LOG_FMT(BRAWLBACK, "gprb: P{} applied the host's sync block", slot + 1);
  if (p.at_start && !was_at_start)
    INFO_LOG_FMT(BRAWLBACK, "gprb: P{} is at the start barrier", slot + 1);
  if (const auto* x = get("go"); x && x->is<bool>())
    p.go = x->get<bool>();
  if (const auto n = PeerData::JsonUInt(get("match"), 0xFFFFFFFF))
    p.match_index = static_cast<u32>(*n);
  // Its held buttons at the barrier (start-of-match hotkeys, CountdownSlots).
  if (const auto n = PeerData::JsonUInt(get("hold"), 0xFFFF);
      n && p.at_s && p.match_index == s.match_index)
  {
    p.hold = static_cast<u16>(*n);
    p.hold_valid = true;
  }
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
  // Kept until the next one: our barrier may fail after the peer has moved on from it.
  if (const auto* x = get("setup_key"); x && x->is<std::string>())
  {
    const std::string& hex = x->get<std::string>();
    if (const auto k = PeerData::ParseHex(hex, std::min<size_t>(hex.size() / 2, 4096)))
    {
      p.setup_key = *k;
      const auto sf = PeerData::JsonUInt(get("setup_start"), 0xFFFFFFFF);
      const auto cv = PeerData::JsonUInt(get("setup_cv"), 0xFF);
      p.setup_start = sf ? std::optional<u32>(static_cast<u32>(*sf)) : std::nullopt;
      p.setup_cv = cv ? std::optional<u32>(static_cast<u32>(*cv)) : std::nullopt;
    }
  }
  if (const auto* x = get("lock"); x && x->is<picojson::object>())
    p.lock = LockFromJson(x->get<picojson::object>());
  if (const auto n = PeerData::JsonUInt(get("winner"), 0xFF))
    p.last_winner = static_cast<u8>(*n);
  // Another module's state (GameSetup's strikes): only a small object is kept.
  if (const auto* x = get("x"); x && x->is<picojson::object>() && x->serialize().size() <= 1024)
    p.extra = x->get<picojson::object>();
  // Its checksums after a desync where the game starts again: at most RECOVERY_WINDOW of them, of
  // the match that just ended here (or ends here soon: its list may come before our own end).
  if (const auto* x = get("dsr"); x && x->is<picojson::object>())
  {
    const auto& d = x->get<picojson::object>();
    const auto field = [&](const char* k) -> const picojson::value* {
      const auto it = d.find(k);
      return it == d.end() ? nullptr : &it->second;
    };
    const auto m = PeerData::JsonUInt(field("m"), 0xFFFFFFFF);
    const auto first_frame = PeerData::JsonUInt(field("from"), 0x7FFFFFFF);
    const auto* crc = field("crc");
    if (m && first_frame && crc && crc->is<std::string>() && crc->get<std::string>().size() % 8 == 0 &&
        crc->get<std::string>().size() <= 8 * RECOVERY_WINDOW &&
        (*m == s.match_index || *m + 1 == s.match_index))
    {
      const std::string& hex = crc->get<std::string>();
      if (const auto bytes = PeerData::ParseHex(hex, hex.size() / 2))
      {
        RecoveryList l;
        l.match = static_cast<u32>(*m);
        l.from = static_cast<s64>(*first_frame);
        for (size_t i = 0; i + 4 <= bytes->size(); i += 4)
        {
          l.crcs.push_back(static_cast<u32>((*bytes)[i]) << 24 | static_cast<u32>((*bytes)[i + 1]) << 16 |
                           static_cast<u32>((*bytes)[i + 2]) << 8 | (*bytes)[i + 3]);
        }
        p.recovery = std::move(l);
      }
    }
  }
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
        if (const StageDecision d = DecideStage(st->game);
            d.applies && !d.wait && (d.stage != st->stage || d.asl != st->asl))
        {
          WARN_LOG_FMT(BRAWLBACK, "gprb lobby: game {}: the host set up stage {:#x} (alternate {:#x}), "
                                  "the stage strikes decided {:#x} ({:#x})",
                       st->game, st->stage, st->asl, d.stage, d.asl);
        }
        s.setup = *st;
        s.last_stage = st->stage;
        INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} setup from the host: stage {:#x}", st->game,
                     st->stage);
        if (st->restore.valid && st->restore.checksum == 0)
        {
          INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {}: the desynced game again from its start "
                                  "(the values it was restarted with)",
                       st->game);
        }
        else if (st->restore.valid)
        {
          // The host's restart of a desynced game: from a frame our own record has, with the
          // same checksum and values (every machine's checksums of it were the same).
          const Restore& r = st->restore;
          const FrameRecord& rec = s.history[static_cast<size_t>(r.frame) % s.history.size()];
          if (rec.frame != r.frame || rec.checksum != r.checksum || RestoreOf(rec, *st) != r)
          {
            WARN_LOG_FMT(BRAWLBACK, "gprb lobby: game {}: the host restarts it from frame {} "
                                    "(checksum {:08x}), which our record of it does not match "
                                    "(frame {}, checksum {:08x})",
                         st->game, r.frame, r.checksum, rec.frame, rec.checksum);
          }
          else
          {
            INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {}: the desynced game again from frame {} "
                                    "(checksum {:08x}, the same here)",
                         st->game, r.frame, r.checksum);
          }
        }
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
  if (const auto* x = get("tasks"))
  {
    if (auto lists = TaskListsFromJson(*x))
    {
      p.task_order = std::move(*lists);
      p.task_order_valid = true;
    }
  }
  // The host's countdown buttons, only with its sync block of this match (a late copy from the
  // last match must not stand in for them).
  if (const auto* x = get("holds"); x && x->is<picojson::array>() && p.at_s &&
                                     p.match_index == s.match_index)
  {
    const auto& a = x->get<picojson::array>();
    std::array<u16, MAX_LOBBY_PLAYERS> holds{};
    bool ok = a.size() == holds.size();
    for (size_t i = 0; ok && i < a.size(); ++i)
    {
      const auto n = PeerData::JsonUInt(&a[i], 0xFFFF);
      ok = n.has_value();
      holds[i] = static_cast<u16>(n.value_or(0));
    }
    if (ok)
    {
      p.holds = holds;
      p.holds_valid = true;
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
          if (slot < 0 && buf[0] != 'C' && buf[0] != 'F')
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
        else if (buf[0] == 'B' && known)
        {
          // Several GekkoNet messages in one datagram (GekkoBatch); a malformed one is dropped
          // whole, as GekkoNet drops a malformed message.
          if (auto msgs = PeerData::SplitGekkoBundle(buf.data(), received))
          {
            std::lock_guard lk(s.gekko_rx_mutex);
            for (auto& m : *msgs)
            {
              if (s.gekko_rx.size() < PeerData::MAX_QUEUED_GEKKO_PACKETS)
                s.gekko_rx.emplace_back(slot, std::move(m));
            }
          }
          else
          {
            std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
            ++s.foreign_packets;
          }
        }
        else if (buf[0] == 'C')
        {
          if (received - 1 <= PeerData::MAX_CONTROL_SIZE)
          {
            HandleControl(std::string_view(reinterpret_cast<const char*>(buf.data()) + 1,
                                           received - 1),
                          *from, from_port);
          }
          else if (known)
          {
            std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
            if (!s.peers[slot].oversize_logged)
            {
              s.peers[slot].oversize_logged = true;
              ERROR_LOG_FMT(BRAWLBACK, "gprb: refused a {} byte control message from P{} (limit {})",
                            received - 1, slot + 1, PeerData::MAX_CONTROL_SIZE);
            }
          }
        }
        else if (buf[0] == 'F')
        {
          // A piece of a control message (PeerData::ControlDatagrams); like a 'C' packet it may
          // come from an address that is not a peer's yet (HandleControl decides).
          const u64 source = (static_cast<u64>(from->toInteger()) << 16) | from_port;
          if (auto text = s.reassembler.Add(source, buf.data(), received))
            HandleControl(*text, *from, from_port);
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
      // A player never heard from at all: its own connect to another player failed, so it never
      // started a session. Everyone gives up (the leave below tells the others at once) rather
      // than waiting on the CSS for good.
      if (s.phase == Phase::Connecting &&
          now - s.connecting_since > std::chrono::milliseconds(Online::PEER_NEVER_HEARD_TIMEOUT_MS))
      {
        u8 unheard = 0;
        std::string list;
        ForEachPeer([&](int slot, const PeerView& p) {
          if (p.seen || p.left)
            return;
          unheard |= static_cast<u8>(1 << slot);
          list += fmt::format("{}P{}", list.empty() ? "" : ", ", slot + 1);
        });
        if (unheard != 0)
        {
          WARN_LOG_FMT(BRAWLBACK, "gprb: nothing from {} in {} ms since the session began",
                       list, Online::PEER_NEVER_HEARD_TIMEOUT_MS);
          s.unheard_ports = unheard;
          s.error = "never heard from " + list;
          s.phase = Phase::Error;
        }
      }
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
      SendState(true);
      // The session gave up (a barrier's check or timeout, a failed start): the others are told
      // at once, so they do not wait for this player until their own timeout. The game is told
      // as of a disconnect (LOCAL `disconnected`): it ends the match it has started loading and
      // goes back to the CSS, and a ranked set reports the opponent gone (both sides: no one
      // rated). Before 2026-10-12 it was not told and played the match on alone, unsynced, with
      // this player's pad on P1 (a joiner drove the host's fighter).
      std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
      if (s.mode == Mode::Network && s.phase == Phase::Error)
      {
        if (!s.leave_sent)
        {
          WARN_LOG_FMT(BRAWLBACK, "gprb: leaving the session ({})", s.error);
          SendLeave(s.error);
        }
        s.disconnected = true;
      }
    }
    // The pings' round trip picks the input delay and times the start; in a running match
    // GekkoNet measures its own (the ping line), so one a second is enough there.
    const bool running = [] {
      std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
      return s.phase == Phase::Running;
    }();
    if (now - last_ping >= std::chrono::milliseconds(running ? 1000 : 200))
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

// The controller that pressed START to lock in plays this game (Slippi-style: any port).
int LocalPad()
{
  return s.local_lock.local_pad < 4 ? s.local_lock.local_pad :
                                      std::clamp(s.net_opts.local_pad, 0, 3);
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
    // round trip measured now, to the furthest player still in the session, as GekkoNet will see
    // it in the game (GameRoundTrip). It stays for the whole game.
    double rtt = -1;
    ForEachPeer([&](int, const PeerView& p) {
      if (!p.left && p.rtt_ms >= 0)
        rtt = std::max(rtt, p.rtt_ms);
    });
    s.input_delay = s.net_opts.delay ? std::clamp(*s.net_opts.delay, 0, MAX_INPUT_DELAY) :
                                       AutoInputDelay(GameRoundTrip(rtt));
    gekko_set_local_delay(s.gekko, s.local_handles[0], static_cast<unsigned char>(s.input_delay));
    gekko_set_disconnect_timeout(s.gekko, Online::PeerSilenceTimeoutMs(s.input_delay));
    s.local_pad = LocalPad();
    INFO_LOG_FMT(BRAWLBACK,
                 "gprb: input delay {} ({}, round trip {:.0f} ms, in game {:.0f} ms), controller "
                 "port {}",
                 s.input_delay, s.net_opts.delay ? "set" : "auto", rtt, GameRoundTrip(rtt),
                 s.local_pad + 1);
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
  s.following = false;
  s.follow_speed = 1.0;
  s.follow_prev_gap = 0;
  s.follow_prev_frame = -1;
  s.follow_prev_own_late = 0;
  s.follow_release = 0;
  s.follow_t = {};
  s.peer_fps = 0;
  s.own_late = 0;
  s.slow_check_frame = -1;
  s.slow_check_own_late = 0;
  s.slow_check_t = {};
  s.slow_checks = 0;
  s.slow_fps = 0;
  s.slow_shown = false;
  ForEachPeer([](int, PeerView& p) { p.slow_shown = false; });
  s.frames_to_advance = 0;
  s.advancing = s.advanced_this_frame = s.prev_frame_advanced = false;
  s.last_advance_frame = -1;
  s.last_hard_sync_frame = -1;
  s.last_start_log_frame = -1;
  s.next_ping_display_frame = PING_DISPLAY_INTERVAL;
  s.rollbacks = s.max_rollback = s.frames_resimulated = s.stall_polls = s.desyncs = 0;
  s.net_window = s.net_total = {};
  s.net_window_first = -1;
  s.net_window_t0 = {};
  s.prev_frame_rolled_back = false;
  Core::System::GetInstance().GetCoreTiming().TakeMaxBehindSchedule();
  Rollback::PresentStats::TakeCadence();
  s.burst_open = false;
  s.cost_window = s.cost_total = {};
  s.cost_window_first = -1;
  s.cost_pass_open = false;
  s.cost_pass_resim = false;
  s.cost_pass_events = false;
  s.cost_pass_serial = 0;
  s.cost_loop_top_t0 = {};
  s.cost_loop_top_excl_ns = 0;
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
  s.local_quit_frame = -1;
  s.sound_allocs = s.resim_sound_allocs = s.suppressed_sound_allocs = 0;
  for (auto& v : s.snd_played)
    v.clear();
  s.snd_tag.fill(-1);
  s.snd_remaining.clear();
  s.snd_new.clear();
  s.snd_pass_open = false;
  s.snd_frame = s.snd_max_frame = -1;
  s.snd_first_run = true;
  s.audio_pass_resim = false;
  s.snd_gen.clear();
  s.snd_reattach = 0;
  s.snd_to_stop.clear();
  s.sound_reattached = s.sound_reattach_gone = s.sound_stopped = s.sound_stop_gone = 0;
  s.snd_gone_why.fill(0);
  s.sound_moved = 0;
  s.sound_stops_ignored = 0;
  for (auto& r : s.rng_frames)
    r.frame = -1;
  s.rng_logged = false;
  s.io_wait_active = false;
  s.io_wait_n = 0;
  s.io_waits = s.io_wait_retraces = s.io_wait_timeouts = 0;
  s.force_final_inited = false;  // the next match's countdown reads PPR_GPRB_FORCE_FINAL again
  s.force_final_at.clear();
  s.force_final_at_active = s.force_final_at_reenter = false;
  if (const char* v = std::getenv("PPR_GPRB_FORCE_FINAL_AT"))
  {
    // <session frame>:<port>[:first], comma separated.
    for (const auto& item : SplitString(v, ','))
    {
      const auto f = SplitString(item, ':');
      if (f.size() < 2)
        continue;
      s.force_final_at.push_back({std::strtoll(f[0].c_str(), nullptr, 0),
                                  static_cast<u32>(std::strtoul(f[1].c_str(), nullptr, 0)) & 3,
                                  f.size() > 2 && f[2] == "first"});
      INFO_LOG_FMT(BRAWLBACK, "gprb: force final at session frame {}, port {}{}",
                   s.force_final_at.back().frame, s.force_final_at.back().port,
                   s.force_final_at.back().first_only ? ", first run only" : "");
    }
  }
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

// A byte of the game's pause-screen quit state in SESSION (SESSION_QUIT_PORTS or
// SESSION_QUIT_STAYS; 3-4 player matches only, else 0).
u8 ReadQuitByte(const Guest& g, u32 session_off)
{
  if (!s.quit_addr)
    return 0;
  const u8* p = g.Ptr(s.quit_addr - Online::GameBridge::SESSION_QUIT_PORTS + session_off, 1);
  return p ? *p : 0;
}

// A byte of SESSION during the match (0 when the block was not found).
u8 ReadSessionByte(const Guest& g, u32 session_off)
{
  const u8* p = s.session_addr ? g.Ptr(s.session_addr + session_off, 1) : nullptr;
  return p ? *p : 0;
}

// The result of a game that ended with game set, from the state every peer ended on: per port of
// the match its stocks and damage (ReadFighterFields, by player number), its team and its place in
// the game's elimination order (SESSION `out`, rolled back with the match), the winner and who
// picks the next stage. A 1v1 keeps its rule: more stocks, then less damage (P+'s time-out rule),
// the loser picks (both after a draw). 3-4 players: Online::GameSetup::DecideOutcome. A player who
// left the match (its gone flag, or its pause-screen quit: both the same on every peer) is not
// placed.
GameResult ReadGameResult(Core::System& system)
{
  const Guest g = GuestOf(system);
  const u8 quit = ReadQuitByte(g, Online::GameBridge::SESSION_QUIT_PORTS);
  GameResult r;
  r.game = NextGame();
  r.stage = s.setup.stage;
  r.asl = s.setup.asl;
  r.frames = s.end_game_frame;
  const auto f = ReadFighterFields(g);
  const auto teams = MatchTeams(g);
  const u32 session = Online::GameBridge::SessionAddress();
  std::array<Online::GameSetup::PortEnd, Online::GameSetup::MAX_PORTS> ends{};
  bool team_battle = false;
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if (!(s.match_ports & (1u << port)) || (s.gone_enabled && s.gone_written[port]) ||
        ((quit >> port) & 1))
    {
      continue;
    }
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
void CostSessionEnd();

// The sound stream at 0 volume, or back to the user's settings (never their saved settings).
void PlaybackMute(Core::System& system, bool mute)
{
  if (s.pb.muted == mute)
    return;
  s.pb.muted = mute;
  if (mute)
  {
    if (SoundStream* stream = system.GetSoundStream())
      stream->SetVolume(0);
  }
  else
  {
    AudioCommon::UpdateSoundStream(system);
  }
}

void EndRunning(Core::System& system, const std::string& reason)
{
  CostSessionEnd();
  PassLogClose();
  DestroyGekko();
  Rollback::RollbackManager::Get().EndRegionMode();
  PadsClearCurrent();
  auto& ct = system.GetCoreTiming();
  ct.ResetThrottleToNow();  // drops a rollback burst still pending
  ct.SetRollbackResimulating(false);
  ct.SetRollbackAudioWaits(false);
  ct.SetRollbackSpeedAdjustment(1.0);
  system.GetFifo().SetRollbackSessionDeterminism(false);
  system.GetCoreTiming().SetPresentPacing(false);
  s.resim_pass = false;
  s.end_reason = reason;
  s_health_running.store(false, std::memory_order_relaxed);
  s.slow_fps = 0;  // the peers stop showing this PC as falling behind
  if (s.mode == Mode::Network)
  {
    s.net_window.cadence = Rollback::PresentStats::TakeCadence();
    if (s.net_window.frames)
      CloseNetWindowClock();
    NetStatsFold(s.net_total, s.net_window);
    s.net_window = {};
    if (s.net_total.frames)
    {
      INFO_LOG_FMT(BRAWLBACK, "gprb net: session: {}", NetStatsLine(s.net_total));
      LogPerfVerdict(s.net_total);
    }
  }
  s.end_game_frame = GuestOf(system).U32(Addr::GAME_FRAME + 4).value_or(0);
  INFO_LOG_FMT(BRAWLBACK, "gprb: session ended at game frame {} ({}), {} rollbacks, {} desyncs",
               s.end_game_frame, reason, s.rollbacks, s.desyncs);
  if (s.mode == Mode::SyncTest)
  {
    s.phase = Phase::Ended;
  }
  else if (s.mode == Mode::Playback)
  {
    // The replay's last frame ran: the game ends its match as it did for the player who recorded
    // it (GameBridge's LOCAL): GAME! and a quit by themselves, a disconnect with DISCONNECTED, a
    // desync with DESYNC DETECTED, a quit of a 3-4 player match as that player's own.
    s.phase = Phase::Ended;
    s.pb.finished = true;
    PlaybackMute(system, false);
    if (reason == "replay ended")
    {
      const std::string& m = s.pb.end_method;
      s.pb.end_desynced = m == "desync";
      s.pb.end_quit_left = m == "quit-left";
      s.pb.end_disconnected = !s.pb.end_desynced && !s.pb.end_quit_left && m != "game" &&
                              m != "quit" && m != "runback";
    }
    if (s.pb.stopped)
      s.pb.end_quit_left = true;
    s.await_scene_exit = true;
    INFO_LOG_FMT(BRAWLBACK, "replay: playback ended ({}; recorded end: {}), {} frames checked, {}",
                 reason, s.pb.end_method.empty() ? "incomplete" : s.pb.end_method,
                 s.pb.frames_checked,
                 s.pb.desync_frame < 0 ? std::string("no desync") :
                                         fmt::format("desynced at frame {}", s.pb.desync_frame));
    if (!s.pb.stopped)
      OSD::AddMessage("Replay finished", OSD::Duration::NORMAL);
  }
  else
  {
    // A pause-screen quit that ended the match for everyone (docs/game-code.md §11, the quit
    // per mode): who quit and whether with L+R+Y, from the state both peers ended on. Ranked
    // (SetQuitLoses): the quitter lost the game. Otherwise it does not count (a room's set plays
    // it again, Online/Ranked.cpp), and a runback sets the next game up as this one again.
    std::optional<MatchSetup> runback;
    std::optional<GameResult> replay_result;
    if (reason == "quit")
    {
      const Guest g = GuestOf(system);
      const u8 by = ReadSessionByte(g, Online::GameBridge::SESSION_QUIT_BY);
      if (by >= 1 && by <= MAX_LOBBY_PLAYERS && (s.match_ports & (1u << (by - 1))))
      {
        GameResult r = ReadGameResult(system);
        r.quitter = static_cast<u8>(by - 1);
        const bool rb = ReadSessionByte(g, Online::GameBridge::SESSION_QUIT_RUNBACK) != 0;
        r.end = rb ? GameEnd::Runback : GameEnd::Quit;
        if (s_quit_loses && std::popcount(s.match_ports) == 2)
        {
          r.end = GameEnd::Quit;
          r.winner = static_cast<u8>(std::countr_zero(s.match_ports & ~(1u << r.quitter)));
          r.pickers = static_cast<u8>(1u << r.quitter);
          s.last_winner = r.winner;
          s.last_pickers = r.pickers;
        }
        else
        {
          r.winner = 0xFF;
          r.pickers = 0;
          if (rb && s.setup.game != 0)
            runback = s.setup;
        }
        INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} quit by P{}{}{}", r.game, r.quitter + 1,
                     rb ? " with L+R+Y (runback)" : "",
                     r.winner < MAX_LOBBY_PLAYERS ? fmt::format(": P{} wins it", r.winner + 1) :
                     runback ? ": the same match again" : ": not counted");
        replay_result = r;
        ReportGameResult(r);
      }
    }
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
      replay_result = r;
      ReportGameResult(r);
    }
    // A desync where the game starts again (SetDesyncRecovery; docs/game-code.md, desync
    // recovery): this machine's checksums of its last confirmed frames, which the others get with
    // every state message until the next game starts, and the setup the next game repeats. The
    // game has no result: Online/Ranked.cpp plays its set game again.
    s.recovery.pending = false;
    if (reason == "desync" && s_desync_recovery)
    {
      RecoveryList& l = s.recovery.list;
      l = {};
      l.match = s.match_index;
      const s64 last = s.current_frame - MAX_ROLLBACK_FRAMES - 1;
      l.from = std::max<s64>(0, last - RECOVERY_WINDOW + 1);
      for (s64 f = l.from; f <= last; ++f)
      {
        const FrameRecord& rec = s.history[static_cast<size_t>(f) % s.history.size()];
        l.crcs.push_back(rec.frame == f ? rec.checksum : 0);
      }
      s.recovery.setup = s.setup;
      s.recovery.pending = true;
      INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} desynced: it starts again from the last frame "
                              "every machine agrees on (ours: frames {}-{})",
                   NextGame(), l.from, last);
      GameResult r;
      r.end = GameEnd::Desync;
      r.game = NextGame();
      r.stage = s.setup.stage;
      r.asl = s.setup.asl;
      r.frames = s.end_game_frame;
      ReportGameResult(r);
    }
    EndReplay(system, reason, replay_result ? &*replay_result : nullptr);
    // Ready for the next match on the same connection.
    bool seen = false;
    ForEachPeer([&](int, PeerView& p) {
      seen = seen || (p.seen && !p.left);
      p.at_s = p.applied = p.at_start = p.go = false;
      p.sync = {};
      p.task_order_valid = false;
      p.match_ports = 0;
      p.hold_valid = p.holds_valid = false;
    });
    s.phase = seen && !s.peer_left ? Phase::Connected : Phase::Ended;
    s.at_s = s.applied = s.at_start = s.go = false;
    s.holds_valid = false;
    s.did_frame0 = false;
    s.sync = {};
    s.start_at.reset();
    s.countdown_at.reset();
    ++s.match_index;
    s.await_scene_exit = true;
    if (runback)
    {
      // Every machine sets it up itself from the same setup (the host's copy is then the same and
      // is not taken again): the game goes from the match straight into this one.
      runback->game = NextGame();
      runback->restore = {};  // a runback of a restarted game starts from the beginning
      s.setup = *runback;
      s.last_stage = runback->stage;
      INFO_LOG_FMT(BRAWLBACK, "gprb lobby: game {} setup: the last game again (runback), stage {:#x}",
                   runback->game, runback->stage);
    }
  }
  s.match_ports = 0;
  s.gone_enabled = false;
  s.gone_addr = 0;
  s.quit_addr = 0;
  s.session_addr = 0;
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

void LogRngCalls(s64 frame, std::string_view why);

void PerformQueuedSave(Core::System& system)
{
  if (!s.queued_save.pending)
    return;
  s.queued_save.pending = false;
  if (s.mode == Mode::Playback)
  {
    // Nothing is rolled back: no snapshot. The frame's checksum against the file's (the
    // recorder's machine computed it after the same frame).
    const Guest g(system.GetMemory());
    FrameRecord rec;
    const s64 f = s.queued_save.frame;
    const u32 crc = FrameChecksum(g, &rec);
    RecordFrame(f, crc, rec);
    const auto& file = *s.pb.file;
    if (f >= file.first_frame && f - file.first_frame < static_cast<s64>(file.checksums.size()) &&
        (s.pb.final_frame < 0 || f <= s.pb.final_frame))
    {
      ++s.pb.frames_checked;
      const u32 want = file.checksums[static_cast<size_t>(f - file.first_frame)];
      if (want != crc && s.pb.desync_frame < 0)
      {
        s.pb.desync_frame = f;
        std::string parts;
        for (const auto& p : file.parts)
        {
          if (p.frame == f)
          {
            parts = fmt::format(" (rng {:08x} {:08x} {:08x} fighters {:08x} recorded)", p.rng[0],
                                p.rng[1], p.rng[2], p.fighters_crc);
          }
        }
        s.pb.desync_text = fmt::format(
            "frame {} (game frame {}): checksum {:08x}, recorded {:08x}; rng {:08x} {:08x} {:08x} "
            "fighters {:08x}{}",
            f, g.U32(Addr::GAME_FRAME + 4).value_or(0), crc, want, rec.rng[0], rec.rng[1],
            rec.rng[2], rec.fighters_crc, parts);
        WARN_LOG_FMT(BRAWLBACK, "replay: playback differs from the recording from {}",
                     s.pb.desync_text);
        LogRngCalls(f, "difference from the recording");
        const s64 seconds = std::max<s64>(f, 0) / 60;
        OSD::AddMessage(fmt::format("The replay desynced at {}:{:02} (frame {}). From here the game "
                                    "may differ from what was played.",
                                    seconds / 60, seconds % 60, f),
                        OSD::Duration::VERY_LONG, OSD::Color::YELLOW);
      }
    }
    return;
  }
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
  if (Replay::Active())
  {
    Replay::Parts parts;
    parts.rng = rec.rng;
    parts.fighters_crc = rec.fighters_crc;
    Replay::OnSave(system, s.queued_save.frame, crc, parts);
  }
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
constexpr double FRAME_US = 1e6 * 1001.0 / 60000.0;
constexpr double LATE_MS = 2.0;

std::string NetStatsLine(const State::NetStats& w)
{
  const double n = static_cast<double>(std::max<u64>(w.frames, 1));
  std::string line = fmt::format("{} frames", w.frames);
  if (w.wall_ms > 0)
    line += fmt::format(" in {:.1f} s ({:.1f}/s)", w.wall_ms / 1000, w.frames * 1000.0 / w.wall_ms);
  line += fmt::format(
      ", rollbacks {} ({:.1f}%), resimulated {} (avg depth {:.2f}, max {}), "
      "rollback cost avg {:.2f} ms max {:.2f} ms, late frames {} ({} after a rollback, max {:.1f} ms), "
      "waits for the peer {} ({:.0f} ms, max {:.1f} ms), frames ahead avg {:.2f} [{:.2f}, {:.2f}], "
      "speed [{:.4f}, {:.4f}], catch-up frames {}",
      w.rollbacks, 100.0 * w.rollbacks / n, w.resim,
      w.rollbacks ? static_cast<double>(w.resim) / w.rollbacks : 0.0, w.max_depth,
      w.bursts ? w.burst_ms / w.bursts : 0.0, w.burst_ms_max, w.late, w.late_after_rollback,
      w.late_ms_max, w.stalls,
      w.stall_ms, w.stall_ms_max, w.ahead_sum / n, w.frames ? w.ahead_min : 0.0,
      w.frames ? w.ahead_max : 0.0, w.frames ? w.speed_min : 1.0, w.frames ? w.speed_max : 1.0,
      w.advances);
  if (w.followed)
  {
    line += fmt::format(", frames at the peer's speed {} (the peer at {:.1f}/s)", w.followed,
                        w.peer_fps_sum / static_cast<double>(w.followed));
  }
  if (const auto& c = w.cadence; c.presents)
  {
    const double mean = c.interval_ms_sum / c.presents;
    const double sd = std::sqrt(std::max(0.0, c.interval_ms_sq / c.presents - mean * mean));
    line += fmt::format(", presents {} (interval sd {:.2f} ms, max {:.1f} ms), screen hitches {:.1f}",
                        c.presents, sd, c.interval_ms_max, c.hitches);
    if (c.holds)
    {
      line += fmt::format(", held {} (avg {:.2f} ms, max {:.1f} ms)", c.holds,
                          c.hold_ms_sum / c.holds, c.hold_ms_max);
    }
  }
  if (w.pad_age_n)
  {
    line += fmt::format(", pad sample age avg {:.1f} ms max {:.1f} ms", w.pad_age_sum / w.pad_age_n,
                        w.pad_age_max);
  }
  return line;
}

// The window's wall-clock time ends here and the next window's starts.
void CloseNetWindowClock()
{
  const auto now = Clock::now();
  if (s.net_window_t0 != Clock::time_point{})
    s.net_window.wall_ms = std::chrono::duration<double, std::milli>(now - s.net_window_t0).count();
  s.net_window_t0 = now;
}

void NetStatsFold(State::NetStats& t, const State::NetStats& w)
{
  t.frames += w.frames;
  t.rollbacks += w.rollbacks;
  t.resim += w.resim;
  t.max_depth = std::max(t.max_depth, w.max_depth);
  t.bursts += w.bursts;
  t.followed += w.followed;
  t.peer_fps_sum += w.peer_fps_sum;
  t.late += w.late;
  t.late_after_rollback += w.late_after_rollback;
  t.burst_ms += w.burst_ms;
  t.burst_ms_max = std::max(t.burst_ms_max, w.burst_ms_max);
  t.late_ms_max = std::max(t.late_ms_max, w.late_ms_max);
  t.stalls += w.stalls;
  t.advances += w.advances;
  t.wall_ms += w.wall_ms;
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
  t.cadence.holds += w.cadence.holds;
  t.cadence.hold_ms_sum += w.cadence.hold_ms_sum;
  t.cadence.hold_ms_max = std::max(t.cadence.hold_ms_max, w.cadence.hold_ms_max);
  t.cadence.interval_ms_sum += w.cadence.interval_ms_sum;
  t.cadence.interval_ms_sq += w.cadence.interval_ms_sq;
  t.cadence.interval_ms_max = std::max(t.cadence.interval_ms_max, w.cadence.interval_ms_max);
}

// At the end of a network session, one line on whether a PC held the match back, for the logs
// players send in. A frame late without a rollback before it is this PC's own speed (CPU or GPU);
// frames at the peer's speed are the peer's (UpdatePeerFollow).
void LogPerfVerdict(const State::NetStats& t)
{
  if (t.frames < NET_STATS_WINDOW)
    return;
  const double n = static_cast<double>(t.frames);
  const double fps = t.wall_ms > 0 ? t.frames * 1000.0 / t.wall_ms : 0.0;
  const double own_late = static_cast<double>(t.late - t.late_after_rollback) / n;
  if (own_late > 0.5)
  {
    WARN_LOG_FMT(BRAWLBACK,
                 "gprb perf: this PC could not keep full speed: {:.0f}% of frames ran late "
                 "without a rollback before them, {:.1f} frames a second; the match ran at this "
                 "PC's speed",
                 100 * own_late, fps);
  }
  else if (t.followed * 4 > t.frames)
  {
    WARN_LOG_FMT(
        BRAWLBACK,
        "gprb perf: the peer could not keep full speed: this PC ran at its speed on {:.0f}% "
        "of frames, the peer at {:.1f} frames a second",
        100 * t.followed / n, t.peer_fps_sum / static_cast<double>(t.followed));
  }
  else
  {
    INFO_LOG_FMT(BRAWLBACK,
                 "gprb perf: {:.1f} frames a second, {:.0f}% of frames late ({:.0f}% after a "
                 "rollback), at the peer's speed on {:.0f}%",
                 fps, 100 * t.late / n, 100 * t.late_after_rollback / n, 100 * t.followed / n);
  }
}

// ---- Frame cost profile (Rollback::FrameProfile) ----

double NsToMs(u64 ns)
{
  return static_cast<double>(ns) / 1e6;
}

u64 SinceNs(Clock::time_point t0, Clock::time_point t1)
{
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

void CostFold(State::PassCost& t, const State::PassCost& w)
{
  t.passes += w.passes;
  t.guest_ns += w.guest_ns;
  t.save_ns += w.save_ns;
  t.loop_top_ns += w.loop_top_ns;
  t.event_passes += w.event_passes;
  Rollback::FrameProfile::Accumulate(&t.c, w.c);
}

void CostFold(State::FrameCost& t, const State::FrameCost& w)
{
  CostFold(t.resim, w.resim);
  CostFold(t.shown, w.shown);
  t.loads += w.loads;
  t.load_ns += w.load_ns;
}

u64 CostWaitNs(const Rollback::FrameProfile::Counters& c)
{
  u64 n = 0;
  for (const u64 v : c.gpu_wait_ns)
    n += v;
  return n;
}

// The events of one category over all passes, estimated from the passes that timed them
// (CostPassStart: one in EVENT_SAMPLE_EVERY).
u64 CostEventNs(const State::PassCost& p, size_t cat)
{
  if (!p.event_passes)
    return 0;
  return static_cast<u64>(static_cast<double>(p.c.event_ns[cat]) * static_cast<double>(p.passes) /
                          static_cast<double>(p.event_passes));
}

u64 CostEventNs(const State::PassCost& p)
{
  u64 n = 0;
  for (size_t i = 0; i < Rollback::FrameProfile::EVENT_CATS; ++i)
    n += CostEventNs(p, i);
  return n;
}

// The pass's time in the game's own code: the JIT outside CoreTiming events, waits for the GPU
// thread and the throttle's sleep.
u64 CostGameNs(const State::PassCost& p)
{
  const u64 other = CostEventNs(p) + CostWaitNs(p.c) + p.c.throttle_ns;
  return p.guest_ns > other ? p.guest_ns - other : 0;
}

std::string PassCostText(const State::PassCost& p)
{
  using namespace Rollback::FrameProfile;
  const double n = static_cast<double>(std::max<u64>(p.passes, 1));
  const auto avg = [n](u64 ns) { return NsToMs(ns) / n; };
  const auto ev = [&](EventCat c) { return avg(CostEventNs(p, static_cast<size_t>(c))); };
  std::string waits;
  for (size_t i = 0; i < WAIT_REASONS; ++i)
  {
    if (p.c.gpu_wait_ns[i])
    {
      waits += fmt::format("{}{} {:.2f}", waits.empty() ? "" : ", ", WAIT_REASON_NAMES[i],
                           avg(p.c.gpu_wait_ns[i]));
    }
  }
  std::string line = fmt::format(
      "{} avg {:.2f} ms: game code {:.2f}, waiting for the GPU thread {:.2f}{}, events {:.2f} "
      "(gpu {:.2f}, vi {:.2f}, audio {:.2f}, ios {:.2f}, devices {:.2f})",
      p.passes, avg(p.guest_ns + p.save_ns + p.loop_top_ns), avg(CostGameNs(p)),
      avg(CostWaitNs(p.c)), waits.empty() ? "" : " (" + waits + ")", avg(CostEventNs(p)),
      ev(EventCat::Gpu), ev(EventCat::Vi), ev(EventCat::Audio), ev(EventCat::Ios),
      ev(EventCat::Devices));
  if (p.c.throttle_ns)
    line += fmt::format(", throttle sleep {:.2f}", avg(p.c.throttle_ns));
  line += fmt::format(", save {:.2f}, loop top {:.2f}; GPU thread busy {:.2f} (presenting {:.2f})",
                      avg(p.save_ns), avg(p.loop_top_ns), avg(p.c.gpu_busy_ns),
                      avg(p.c.gpu_present_ns));
  return line;
}

std::string FrameCostLine(const State::FrameCost& w)
{
  return fmt::format("re-run frames {}; loads {} avg {:.2f} ms; presented frames {}",
                     PassCostText(w.resim), w.loads,
                     w.loads ? NsToMs(w.load_ns) / static_cast<double>(w.loads) : 0.0,
                     PassCostText(w.shown));
}

picojson::value PassCostJson(const State::PassCost& p)
{
  using namespace Rollback::FrameProfile;
  const auto num = [](u64 v) { return picojson::value(static_cast<double>(v)); };
  picojson::object o;
  o["passes"] = num(p.passes);
  o["guest_ns"] = num(p.guest_ns);
  o["game_ns"] = num(CostGameNs(p));
  o["save_ns"] = num(p.save_ns);
  o["loop_top_ns"] = num(p.loop_top_ns);
  o["throttle_ns"] = num(p.c.throttle_ns);
  o["gpu_wait_ns"] = num(CostWaitNs(p.c));
  o["gpu_waits"] = num(p.c.gpu_waits);
  picojson::object waits;
  for (size_t i = 0; i < WAIT_REASONS; ++i)
    waits[std::string(WAIT_REASON_NAMES[i])] = num(p.c.gpu_wait_ns[i]);
  o["gpu_wait_ns_by_reason"] = picojson::value(waits);
  constexpr std::array<const char*, EVENT_CATS> EVENT_NAMES = {"gpu", "vi", "audio", "ios",
                                                               "devices"};
  // Estimated over all passes from those that timed their events.
  picojson::object events;
  for (size_t i = 0; i < EVENT_CATS; ++i)
    events[EVENT_NAMES[i]] = num(CostEventNs(p, i));
  o["event_ns"] = picojson::value(events);
  o["events"] = num(p.c.events);
  o["event_passes"] = num(p.event_passes);
  o["gpu_busy_ns"] = num(p.c.gpu_busy_ns);
  o["gpu_present_ns"] = num(p.c.gpu_present_ns);
  return picojson::value(o);
}

picojson::value FrameCostJson()
{
  State::FrameCost t = s.cost_total;
  CostFold(t, s.cost_window);
  picojson::object o;
  o["resim"] = PassCostJson(t.resim);
  o["shown"] = PassCostJson(t.shown);
  o["loads"] = picojson::value(static_cast<double>(t.loads));
  o["load_ns"] = picojson::value(static_cast<double>(t.load_ns));
  return picojson::value(o);
}

// RunFrame's entry: the pass that ran since the last loop top ends.
void CostPassEnd()
{
  const auto now = Clock::now();
  s.cost_loop_top_t0 = now;
  s.cost_loop_top_excl_ns = 0;
  if (!s.cost_pass_open)
    return;
  s.cost_pass_open = false;
  Rollback::FrameProfile::SetTimeEvents(false);
  auto& b = s.cost_pass_resim ? s.cost_window.resim : s.cost_window.shown;
  ++b.passes;
  if (s.cost_pass_events)
    ++b.event_passes;
  b.guest_ns += SinceNs(s.cost_pass_t0, now);
  Rollback::FrameProfile::Accumulate(
      &b.c, Rollback::FrameProfile::Delta(Rollback::FrameProfile::Snapshot(), s.cost_pass_c0));
}

// The snapshot after the pass that just ended.
void CostAddSave(u64 ns)
{
  (s.cost_pass_resim ? s.cost_window.resim : s.cost_window.shown).save_ns += ns;
  s.cost_loop_top_excl_ns += ns;
}

void CostAddLoad(u64 ns)
{
  ++s.cost_window.loads;
  s.cost_window.load_ns += ns;
  s.cost_loop_top_excl_ns += ns;
}

// RunFrame hands the CPU to the game: a pass starts. Every NET_STATS_WINDOW presented passes,
// a log line (not for replays: their seeks are not rollbacks).
void CostPassStart(bool resim)
{
  if (!resim && s.mode != Mode::Playback && s.cost_window.shown.passes >= NET_STATS_WINDOW)
  {
    INFO_LOG_FMT(BRAWLBACK, "gprb frame cost: frames {}-{}: {}", s.cost_window_first,
                 s.current_frame - 1, FrameCostLine(s.cost_window));
    CostFold(s.cost_total, s.cost_window);
    s.cost_window = {};
    s.cost_window_first = -1;
  }
  if (s.cost_window_first < 0)
    s.cost_window_first = s.current_frame;
  const auto now = Clock::now();
  auto& b = resim ? s.cost_window.resim : s.cost_window.shown;
  if (s.cost_loop_top_t0 != Clock::time_point{})
  {
    const u64 loop_top = SinceNs(s.cost_loop_top_t0, now);
    b.loop_top_ns += loop_top > s.cost_loop_top_excl_ns ? loop_top - s.cost_loop_top_excl_ns : 0;
  }
  // One pass in EVENT_SAMPLE_EVERY times its CoreTiming events (a prime, so that it does not
  // follow a pattern of re-run and presented passes).
  constexpr u64 EVENT_SAMPLE_EVERY = 7;
  s.cost_pass_events =
      Rollback::FrameProfile::Enabled() && ++s.cost_pass_serial % EVENT_SAMPLE_EVERY == 0;
  Rollback::FrameProfile::SetTimeEvents(s.cost_pass_events);
  s.cost_pass_open = true;
  s.cost_pass_resim = resim;
  s.cost_pass_t0 = now;
  s.cost_pass_c0 = Rollback::FrameProfile::Snapshot();
}

// The session ends: its totals.
void CostSessionEnd()
{
  Rollback::FrameProfile::SetEnabled(false);
  s.cost_pass_open = false;
  CostFold(s.cost_total, s.cost_window);
  s.cost_window = {};
  if (s.mode != Mode::Playback && (s.cost_total.shown.passes || s.cost_total.resim.passes))
    INFO_LOG_FMT(BRAWLBACK, "gprb frame cost: session: {}", FrameCostLine(s.cost_total));
}

// Rolls back: RollbackManager::LoadFrame, timed for the profile.
void LoadForRollback(Core::System& system, int frames_back)
{
  const auto t0 = Clock::now();
  Rollback::RollbackManager::Get().LoadFrame(system, frames_back);
  CostAddLoad(SinceNs(t0, Clock::now()));
}

// Network sessions: every NET_STATS_WINDOW displayed frames, one log line of what the netcode did,
// with the link's numbers from GekkoNet.
void NetStatsOnDisplayedFrame(Core::System& system)
{
  if (s.mode != Mode::Network)
    return;
  auto& w = s.net_window;
  if (s.net_window_first < 0)
  {
    s.net_window_first = s.current_frame;
    if (s.net_window_t0 == Clock::time_point{})
      s.net_window_t0 = Clock::now();
  }
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
    // A catch-up frame's throttle point is a frame early on purpose.
    const bool advanced = std::exchange(s.prev_frame_advanced, s.advanced_this_frame);
    s.advanced_this_frame = false;
    const double ms = std::chrono::duration<double, std::milli>(
                          system.GetCoreTiming().TakeMaxBehindSchedule())
                          .count() -
                      (advanced ? FRAME_US / 1000 : 0.0);
    if (ms > LATE_MS)
    {
      s_health_late.fetch_add(1, std::memory_order_relaxed);
      ++w.late;
      if (s.prev_frame_rolled_back)
        ++w.late_after_rollback;
      else
        ++s.own_late;
      w.late_ms_max = std::max(w.late_ms_max, ms);
    }
    s.prev_frame_rolled_back = s.ops.adv_count > 1;
  }
  {
    const double depth = s.ops.adv_count > 1 ? std::max(s.last_load_back, 0) : 0;
    s_rollback_ema += ROLLBACK_EMA_ALPHA * (depth - s_rollback_ema);
    s_health_rollback.store(s_rollback_ema, std::memory_order_relaxed);
    if (w.frames % 60 == 0)
    {
      if (GekkoNetworkStats st{}; WorstLink(&st) && st.avg_ping > 0)
        s_health_ping.store(st.avg_ping, std::memory_order_relaxed);
    }
  }
  w.ahead_sum += s.frames_ahead;
  w.ahead_min = std::min<double>(w.ahead_min, s.frames_ahead);
  w.ahead_max = std::max<double>(w.ahead_max, s.frames_ahead);
  w.speed_min = std::min(w.speed_min, s.speed_factor);
  w.speed_max = std::max(w.speed_max, s.speed_factor);
  if (s.following)
  {
    ++w.followed;
    w.peer_fps_sum += s.peer_fps;
  }
  if (w.frames < NET_STATS_WINDOW)
    return;
  w.cadence = Rollback::PresentStats::TakeCadence();
  CloseNetWindowClock();

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
  int count = 0;
  GekkoGameEvent** events;
  {
    // This frame's messages to each peer in one datagram (GekkoBatch).
    GekkoBatch batch;
    if (s.mode == Mode::Network)
      gekko_network_poll(s.gekko);
    for (int h : s.local_handles)
      gekko_add_local_input(s.gekko, h, s.local_inputs.data() + s.handle_port[h] * INPUT_SIZE);
    events = gekko_update_session(s.gekko, &count);
  }
  return ProcessGekkoUpdate(system, events, count);
}

// The mtRand calls of `frame` and the 3 frames before it, from each frame's newest pass (State::
// rng_frames), to the log: once per match, at its first desync. Both machines log them, so a
// player's log shows which code drew how many numbers where the game states parted; a replay of
// the game played back with PPR_GPRB_RNG_LOG shows what a fresh machine draws there.
void LogRngCalls(s64 frame, std::string_view why)
{
  if (s.rng_logged)
    return;
  s.rng_logged = true;
  const auto gen = [](u32 rng) -> std::string {
    if (rng == Addr::MTRAND_DEFAULT_SEED - 4)
      return "mtRand";
    if (rng == Addr::MTRAND_OTHER_SEED - 4)
      return "mtRandOther";
    return fmt::format("{:08x}", rng);
  };
  WARN_LOG_FMT(BRAWLBACK, "gprb rng calls before the {} at frame {} (each frame's newest pass):", why,
               frame);
  for (s64 f = std::max<s64>(frame - 3, 0); f <= frame; ++f)
  {
    const State::RngFrame& rf = s.rng_frames[static_cast<size_t>(f) % s.rng_frames.size()];
    if (rf.frame != f)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb rng frame {}: not kept", f);
      continue;
    }
    WARN_LOG_FMT(BRAWLBACK, "gprb rng frame {}: {} calls ({} pass {})", f, rf.count,
                 rf.first_run ? "first" : "again", rf.pass);
    const u32 n = std::min<u32>(rf.count, static_cast<u32>(rf.calls.size()));
    for (u32 i = 0; i < n; ++i)
    {
      const State::RngCall& c = rf.calls[i];
      WARN_LOG_FMT(BRAWLBACK,
                   "gprb rng frame {} #{}: {} state {:08x} lr {:08x} < {:08x} < {:08x} < {:08x} < "
                   "{:08x} < {:08x}",
                   f, i, gen(c.rng), c.state, c.callers[0], c.callers[1], c.callers[2],
                   c.callers[3], c.callers[4], c.callers[5]);
    }
    if (rf.count > n)
      WARN_LOG_FMT(BRAWLBACK, "gprb rng frame {}: {} more calls not kept", f, rf.count - n);
  }
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
      {
        picojson::object ev;
        ev["type"] = picojson::value("desync");
        ev["local"] = picojson::value(static_cast<double>(sev[i]->data.desynced.local_checksum));
        ev["remote"] = picojson::value(static_cast<double>(sev[i]->data.desynced.remote_checksum));
        Replay::AddEvent(sev[i]->data.desynced.frame, ev);
      }
      LogRngCalls(sev[i]->data.desynced.frame, "desync");
      if (s.mode == Mode::Network && !s.desync_end)
      {
        // A desync does not heal: the machines play different games from here on, and one
        // of them may reach game set while the other plays on and waits for inputs that never
        // come. End the game on both (the peer detects it too; the message makes sure).
        s.desync_frame = sev[i]->data.desynced.frame;
        s.desync_end = true;
        WARN_LOG_FMT(BRAWLBACK, "gprb: the game states differ from frame {}: the game ends",
                     sev[i]->data.desynced.frame);
        SendDesync(sev[i]->data.desynced.frame);
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
    // last frame run (s.current_frame, the previous update's last advance). Not the frame before
    // this update's last advance: while GekkoNet cannot advance (a player's input is missing), a
    // rollback brings only the resimulated frames, up to the last frame run, and no new frame
    // (3-4 players: one peer's input corrects a prediction while another's is still missing).
    // Counted from the last advance, that load went one frame short and the game ran a frame
    // ahead of the session from then on (2026-10-10, a 3-player room: "desync at frame 1572",
    // the peer's checksum of frame 1573 was ours of 1572).
    const int frames_back = static_cast<int>(s.current_frame - load_frame);
    auto& rbm = Rollback::RollbackManager::Get();
    if (frames_back >= 1 && frames_back <= MAX_ROLLBACK_FRAMES && rbm.m_ring_count >= 2 &&
        frames_back < rbm.m_ring_count)
    {
      BeginBurst(system);
      LoadForRollback(system, frames_back);
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

// Online time sync, as Slippi's (CEXISlippi::shouldAdvanceOnlineFrame). The gap to the peers is
// GekkoNet's frames ahead, which is Slippi's CalcTimeOffsetUs in frames (positive: ahead). Every
// TIME_SYNC_INTERVAL frames:
// - the speed: a peer more than 8 ms ahead runs up to 0.5% slower, a peer behind up to 1% faster,
//   all of it from 3 frames on (the fast PC is slowed down less, so its performance comes first);
// - after HARD_SYNC_UNTIL, a peer more than a frame and 10 ms behind runs up to 3 catch-up frames,
//   one every ADVANCE_EVERY frames. Slippi's game loops twice in such a frame; here the frame
//   skips its wait (CoreTimingManager::AdvanceThrottle), which comes to the same.
// After HARD_SYNC_UNTIL the peer that is ahead never waits: Slippi took that out (2023) because a
// halted frame can drop an input, and a speed nudge on the fast PC does little once it is behind
// its own schedule (until 0.1.47 it was up to 2% slower and nothing caught up: a peer that fell
// behind stayed behind, and the other rolled back deeper for the rest of the game).
constexpr u32 TIME_SYNC_INTERVAL = 30;
constexpr double SYNC_T1_US = 10000;  // Slippi's t1
constexpr double MAX_SLOW_DOWN = 0.005, MAX_SPEED_UP = 0.01;
constexpr double SPEED_WINDOW_FRAMES = 3;
constexpr int MAX_ADVANCE_FRAMES = 3;
constexpr u32 ADVANCE_EVERY = 5;
constexpr s64 HARD_SYNC_UNTIL = 120;

// Following a peer that cannot keep full speed. Slippi's sync leaves the gap to the peer behind,
// which closes it with its spare time; a PC without any never does, and this peer runs on ahead,
// rolling back deeper on every frame. Prod (2026-10-12, a 9800X3D against an i3-7100U laptop that
// missed its schedule on every frame, even without rollbacks, at 110 ms): the fast PC 4-6 frames
// ahead for whole 10 s windows, at its 0.995 speed floor, rolling back 6 frames deep at 14-16 ms a
// frame, so it ran at 54 frames a second with 190 screen hitches per window, while the laptop ran
// at its own 55-58 with almost no rollbacks. The match can only run at the slow PC's speed either
// way; following it, the fast PC runs at it evenly instead of in bursts of rollbacks.
//
// Every TIME_SYNC_INTERVAL frames after HARD_SYNC_UNTIL, the peer's frame rate is the frames this
// peer ran minus the change of the gap, over the wall time. When the gap is more than FOLLOW_ENGAGE
// frames over its target and not closing, and this PC kept its own schedule on most of the
// interval's frames (a PC that cannot keep full speed never slows itself down further), this
// peer's speed follows the peer's frame rate, slower by FOLLOW_CLOSE_RATE per frame over the
// target (faster below it), at most FOLLOW_MAX_STEP per interval. Following ends when the peer runs
// at full speed again with the gap at its target, FOLLOW_RELEASE time syncs in a row; a gap below
// it alone does not end it (a harness peer at 56-58 frames a second otherwise had it end and start
// again every few seconds, the gap swinging between 0 and 3-5 frames), nor one estimate of the
// peer's frame rate (GekkoNet averages the gap over 26 frames, so the estimate trails a change of
// this peer's speed, by up to 2-3 frames a second).
//
// The target gap: the peer behind rolls back about (one-way latency - this peer's input delay -
// gap) frames, so a gap of that much, and the link's jitter on top, spares the slow PC its
// rollbacks (19 ms each on the laptop; without the jitter a harness peer still rolled back on 2.6%
// of its frames, at 20-27 ms each); more only deepens this peer's, which has time to spare.
constexpr double FOLLOW_ENGAGE = 1.0;
constexpr double FOLLOW_CLOSING = 0.25;  // the gap closes this much per interval: left to the peer
constexpr double FOLLOW_CLOSE_RATE = 0.02;
constexpr double FOLLOW_MAX_STEP = 0.05;
// A harness peer at 43 frames a second (72%) kept a floor of 0.8 at GekkoNet's prediction window.
constexpr double FOLLOW_MIN = 0.5;
constexpr double FOLLOW_TARGET_MIN = 0.5, FOLLOW_TARGET_MAX = 2.5;
constexpr int FOLLOW_RELEASE = 4;

double FollowTargetGap()
{
  GekkoNetworkStats st{};
  if (!WorstLink(&st) || st.avg_ping <= 0)
    return 1.0;
  // GekkoNet's ping is the round trip in game (a frame of polling included, GameRoundTrip).
  const double one_way_frames = (st.avg_ping / 2.0 + st.jitter) / (FRAME_US / 1000.0);
  return std::clamp(one_way_frames - s.input_delay, FOLLOW_TARGET_MIN, FOLLOW_TARGET_MAX);
}

void UpdatePeerFollow()
{
  // Test (PPR_GPRB_TEST_NO_FOLLOW=1): Slippi's sync alone, for A/B runs.
  static const bool off = std::getenv("PPR_GPRB_TEST_NO_FOLLOW") != nullptr;
  if (off)
    return;
  const auto now = Clock::now();
  const double gap = s.frames_ahead;
  const double change = gap - s.follow_prev_gap;
  const double frames = static_cast<double>(s.current_frame - s.follow_prev_frame);
  const double own_late = static_cast<double>(s.own_late - s.follow_prev_own_late);
  const double dt = s.follow_t == Clock::time_point{} ?
                        0.0 :
                        std::chrono::duration<double>(now - s.follow_t).count();
  s.follow_prev_gap = gap;
  s.follow_prev_frame = s.current_frame;
  s.follow_prev_own_late = s.own_late;
  s.follow_t = now;
  if (dt <= 0 || frames <= 0 || s.current_frame <= HARD_SYNC_UNTIL)
    return;
  const double peer_fps = std::max(0.0, (frames - change) / dt);
  s.peer_fps = s.peer_fps > 0 ? (s.peer_fps + peer_fps) / 2 : peer_fps;

  const double target = FollowTargetGap();
  const double over = gap - target;
  if (!s.following)
  {
    if (over <= FOLLOW_ENGAGE || change < -FOLLOW_CLOSING || own_late * 2 > frames)
      return;
    s.following = true;
    s.follow_speed = 1.0;
    s.follow_release = 0;
    INFO_LOG_FMT(
        BRAWLBACK,
        "gprb: following the peer's speed from frame {}: {:.2f} frames ahead (target {:.2f}) and "
        "not closing, the peer at {:.1f} frames a second",
        s.current_frame, gap, target, s.peer_fps);
  }
  const double peer_speed = s.peer_fps * FRAME_US / 1e6;
  const double want = peer_speed - std::clamp(over, -1.0, 4.0) * FOLLOW_CLOSE_RATE;
  s.follow_speed = std::clamp(
      std::clamp(want, s.follow_speed - FOLLOW_MAX_STEP, s.follow_speed + FOLLOW_MAX_STEP),
      FOLLOW_MIN, 1.0);
  s.follow_release =
      peer_speed >= 1.0 && s.follow_speed >= 1.0 && over <= 0 ? s.follow_release + 1 : 0;
  if (s.follow_release >= FOLLOW_RELEASE)
  {
    s.following = false;
    INFO_LOG_FMT(BRAWLBACK,
                 "gprb: following the peer's speed ends at frame {}: {:.2f} frames ahead (target "
                 "{:.2f}), the peer at {:.1f} frames a second",
                 s.current_frame, gap, target, s.peer_fps);
  }
}

void UpdateTimeSync(Core::System& system)
{
  if (s.mode != Mode::Network || s.current_frame < 0)
    return;
  const u32 frame = static_cast<u32>(s.current_frame);
  if (frame % TIME_SYNC_INTERVAL == 0 && frame != s.last_timesync_frame)
  {
    s.last_timesync_frame = frame;
    const double offset_us = s.frames_ahead * FRAME_US;
    double deviation = 0;
    if (offset_us > -250 && offset_us < 8000)
      deviation = 0;
    else if (offset_us < 0)
      deviation = std::min(-offset_us / (SPEED_WINDOW_FRAMES * FRAME_US), 1.0) * MAX_SPEED_UP;
    else
      deviation = -std::min(offset_us / (SPEED_WINDOW_FRAMES * FRAME_US), 1.0) * MAX_SLOW_DOWN;
    UpdatePeerFollow();
    s.speed_factor = s.following ? s.follow_speed : 1.0 + deviation;

    if (offset_us < -(FRAME_US + SYNC_T1_US) && !s.advancing)
    {
      s.advancing = true;
      const int max_frames = s.current_frame > HARD_SYNC_UNTIL ? MAX_ADVANCE_FRAMES : 0;
      s.frames_to_advance =
          std::min(static_cast<int>((-offset_us - SYNC_T1_US) / FRAME_US) + 1, max_frames);
    }
  }
  if (s.frames_to_advance > 0)
  {
    // (A pass whose wait for the peer timed out comes here again with the same frame.)
    if (frame % ADVANCE_EVERY != 0 || s.current_frame == s.last_advance_frame)
      return;
    s.last_advance_frame = s.current_frame;
    --s.frames_to_advance;
    system.GetCoreTiming().AdvanceThrottle(std::chrono::duration_cast<DT>(
        std::chrono::duration<double, std::micro>(FRAME_US)));
    s.advanced_this_frame = true;
    ++s.net_window.advances;
    return;
  }
  s.advancing = false;
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

// "Falling behind": a PC that cannot keep full speed slows the whole match down (UpdatePeerFollow),
// and players blamed the netcode or the other player (prod 2026-10-12). Every SLOW_CHECK_FRAMES
// after HARD_SYNC_UNTIL this PC checks its own speed: more than half of the frames late without a
// rollback before them (LogPerfVerdict's test) and under SLOW_FPS frames a second. Two checks in a
// row (10 s; one hitch, e.g. a shader compiling, is not enough) and it is falling behind: its
// state messages carry its frame rate ("slow"), and every screen shows it once per match, on the
// screen (OSD, which hidden chat does not hide) and in chat: "Your PC can't keep up. Expect
// slowdowns." here, "<name>'s PC can't keep up. Expect slowdowns." on the others'; the log has its
// frame rate. A peer's notice is shown no earlier than this
// PC could have found one, so a late state message of the previous match shows nothing.
constexpr s64 SLOW_CHECK_FRAMES = 300;
constexpr int SLOW_CHECKS = 2;
constexpr double SLOW_FPS = 58.0;

void ShowSlowNotice(const std::string& who, double fps)
{
  const std::string text = fmt::format("{} can't keep up. Expect slowdowns.", who);
  WARN_LOG_FMT(BRAWLBACK, "gprb: notice: {} ({:.1f} frames a second)", text, fps);
  OSD::AddMessage(text, OSD::Duration::VERY_LONG, OSD::Color::YELLOW);
  Online::Chat::AddNotice(text);
}

void UpdateSlowNotice()
{
  if (s.mode != Mode::Network || s.current_frame <= HARD_SYNC_UNTIL)
    return;
  const auto now = Clock::now();
  if (s.slow_check_frame < 0)
  {
    s.slow_check_frame = s.current_frame;
    s.slow_check_own_late = s.own_late;
    s.slow_check_t = now;
  }
  else if (s.current_frame - s.slow_check_frame >= SLOW_CHECK_FRAMES)
  {
    const double frames = static_cast<double>(s.current_frame - s.slow_check_frame);
    const double late = static_cast<double>(s.own_late - s.slow_check_own_late);
    const double dt = std::chrono::duration<double>(now - s.slow_check_t).count();
    const double fps = dt > 0 ? frames / dt : 0.0;
    s.slow_check_frame = s.current_frame;
    s.slow_check_own_late = s.own_late;
    s.slow_check_t = now;
    s.slow_checks = late * 2 > frames && fps > 0 && fps < SLOW_FPS ? s.slow_checks + 1 : 0;
    s.slow_fps = s.slow_checks >= SLOW_CHECKS ? fps : 0.0;
    if (s.slow_fps > 0 && !s.slow_shown)
    {
      s.slow_shown = true;
      ShowSlowNotice("Your PC", s.slow_fps);
    }
  }
  if (s.current_frame <= HARD_SYNC_UNTIL + SLOW_CHECK_FRAMES * SLOW_CHECKS)
    return;
  ForEachPeer([](int, PeerView& p) {
    if (p.left || p.slow_fps <= 0 || p.slow_shown)
      return;
    p.slow_shown = true;
    const auto name = Online::ChatText::Clean(p.name, Online::ChatText::MAX_NAME_CODE_POINTS);
    ShowSlowNotice(name ? *name + "'s PC" : std::string("The other player's PC"), p.slow_fps);
  });
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

// Wakes a playback waiting at its loop top (PlaybackWait): frames came, a control, a stop.
std::condition_variable_any s_pb_wake;

// A playback that stands still at its loop top (paused, held at the end, buffering a live game)
// sleeps until what it waits for may have changed: ExtendPlayback, PlaybackControl and
// StopPlayback wake it, and `max` bounds it so a Dolphin pause or stop is seen. HostWait polls
// every 100 us, which is a spin: a spectator buffering a live game kept a core busy (bug report
// #17, 2026-10-15). Called with s.mutex held once, as HostWait.
template <typename Pred>
bool PlaybackWait(Core::System& system, Pred pred, std::chrono::milliseconds max)
{
  const auto deadline = Clock::now() + max;
  while (!pred())
  {
    if (!CpuRunning(system) || Clock::now() >= deadline)
      return false;
    s_pb_wake.wait_until(s.mutex, deadline);
  }
  return true;
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
// HARD_SYNC_UNTIL session frames, every HARD_SYNC_INTERVAL, a peer more than 10 ms ahead of the
// other waits on the host for the frames it is ahead, rounded up (at most HARD_SYNC_MAX_FRAMES).
// The speed nudge alone (UpdateTimeSync) takes seconds to close a gap of a few frames, and the
// peer that is ahead rolls back on nearly every frame meanwhile. Later, the peer behind catches up
// instead (UpdateTimeSync's catch-up frames).
constexpr s64 HARD_SYNC_INTERVAL = 30;
constexpr int HARD_SYNC_MAX_FRAMES = 5;

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
  const double offset_us = ahead * FRAME_US;
  if (offset_us <= SYNC_T1_US)
    return;
  const int frames =
      std::min(static_cast<int>((offset_us - SYNC_T1_US) / FRAME_US) + 1, HARD_SYNC_MAX_FRAMES);
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
  return s.mode == Mode::SyncTest ? s.st_opts.start_frame :
         s.mode == Mode::Playback ? s.pb.start_frame :
                                    s.net_opts.start_frame;
}

// Start-of-match hotkeys (Samus's ice mode: the side taunt held on entry; PM 3.6): the game reads
// them while the fighters enter, game frames ~2-122, all before start_frame, when no input of the
// match can be taken (see DEFAULT_START_FRAME). So each player's digital buttons are sampled once,
// when its machine reaches the barrier (the match appears after the load), and every machine
// gives each port its player's sample for the whole countdown: the hold must already be down
// when the match appears. Only the digital buttons (gfPadStatus +0x0/+0x4), without START; the
// sticks and analog triggers stay neutral (an analog shield or a tilted stick in the countdown
// would only show an entry's wobble, and a stick at rest is never exactly centred).
// gfPadButtons (as the game reads them; D-pad left 0x1, right 0x2, down 0x4, up 0x8): the D-pad,
// Z, R, L, A, B, X, Y; not START (0x1000) nor 0x80.
constexpr u16 HOLD_BUTTONS = 0x0F7F;

u16 SampleHold()
{
  const PadSlots latest = PadsLatestRaw();
  const u8* pad = latest.data() + LocalPad() * INPUT_SIZE;
  if (pad[0x38] != 0)  // gfPadError: no controller (or not ready)
    return 0;
  return static_cast<u16>((pad[2] << 8 | pad[3]) & HOLD_BUTTONS);
}

// Countdown: every port reads as a connected GameCube controller holding its player's sampled
// buttons (else neutral; ports nobody plays as unplugged), identically on every machine, and
// nothing is rolled back.
PadSlots CountdownSlots()
{
  PadSlots slots{};
  const u32 ports = s.mode == Mode::SyncTest ? s.st_opts.ports : s.match_ports ? s.match_ports : 0x3u;
  for (u32 port = 0; port < 4; ++port)
  {
    u8* pad = slots.data() + port * INPUT_SIZE;
    if (!(ports & (1u << port)))
    {
      pad[0x38] = 0xFF;  // gfPadError NO_CONTROLLER
      continue;
    }
    const u16 hold = s.holds_valid ? static_cast<u16>(s.holds[port] & HOLD_BUTTONS) : 0;
    for (u32 off : {0x0u, 0x4u})  // current buttons, and its copy (both big-endian u32)
    {
      pad[off + 2] = static_cast<u8>(hold >> 8);
      pad[off + 3] = static_cast<u8>(hold);
    }
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
      LoadForRollback(system, load_back);
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

// Playback's keyframes hold what a rollback restores: the region set, without its excluded
// bytes (the game<->Dolphin mailbox and LOCAL, which Dolphin writes, and the set's own excludes).
void PreparePlaybackKeyframes(Core::System& system)
{
  s.pb_keys->Clear();
  RegionSetSpec spec;
  if (auto err = LoadRegionSet(s.net_opts.region_set, &spec))
  {
    WARN_LOG_FMT(BRAWLBACK, "replay: no seeking ({})", *err);
    return;
  }
  const Guest g(system.GetMemory());
  std::vector<std::pair<u32, u32>> ranges;
  for (const auto& r : ResolveRegionSet(g, spec))
    ranges.emplace_back(r.addr, r.size);
  std::vector<std::pair<u32, u32>> holes;
  for (const auto& r : spec.exclude)
    holes.emplace_back(r.addr, r.size);
  if (const auto& mb = Rollback::RollbackManager::Get().m_mailbox_region)
    holes.emplace_back(mb->phys_start | 0x80000000u, mb->phys_end - mb->phys_start);
  for (const auto& [hole, hole_size] : holes)
  {
    std::vector<std::pair<u32, u32>> out;
    for (const auto& [addr, size] : ranges)
    {
      const u64 a = addr, e = u64(addr) + size, h = hole, he = u64(hole) + hole_size;
      if (he <= a || h >= e)
      {
        out.emplace_back(addr, size);
        continue;
      }
      if (h > a)
        out.emplace_back(addr, static_cast<u32>(h - a));
      if (he < e)
        out.emplace_back(static_cast<u32>(he), static_cast<u32>(e - he));
    }
    ranges = std::move(out);
  }
  u64 bytes = 0;
  for (const auto& [a, size] : ranges)
    bytes += size;
  INFO_LOG_FMT(BRAWLBACK, "replay: keyframes hold {} ranges, {} bytes", ranges.size(), bytes);
  s.pb_keys->Reset(std::move(ranges));
}

void StartRunning(Core::System& system)
{
  // Before the first frame (and its base snapshot): GPUDeterminismMode "auto" turns the
  // deterministic GPU thread on for the session; the menus run without it.
  system.GetFifo().SetRollbackSessionDeterminism(true);
  system.GetCoreTiming().SetPresentPacing(true);
  ResetRunStats();
  // PPR_GPRB_NO_FRAME_COST=1: no frame cost profile (to measure what it costs).
  static const bool no_frame_cost = std::getenv("PPR_GPRB_NO_FRAME_COST") != nullptr;
  Rollback::FrameProfile::SetEnabled(!no_frame_cost);
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
  if (s.mode == Mode::Network)
  {
    s_rollback_ema = 0;
    s_health_rollback.store(0, std::memory_order_relaxed);
    s_health_ping.store(-1, std::memory_order_relaxed);
    s_health_running.store(true, std::memory_order_relaxed);
  }
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

// ---- Playback controls (docs/design/replays.md §6.4) ----

// A keyframe every KEYFRAME_EVERY frames (5 s; Slippi's are 15 s apart), and while a seek runs to
// its target, every DENSE_EVERY frames of the last KEYFRAME_EVERY before it, so stepping back
// from there is quick.
constexpr s64 KEYFRAME_EVERY = 300;
constexpr s64 DENSE_EVERY = 60;

// Spectating (docs/design/spectating.md §6.2): the live edge is the newest frame less 3 s. Live
// play keeps 2-4 s of frames by nudging the speed 2 %; out of frames, it holds ("Buffering") until
// 2 s are there again. Catching up to the edge shows one frame in 15, so the game visibly runs.
constexpr s64 LIVE_BUFFER = 180;
constexpr s64 LIVE_LOW = 120;
constexpr s64 LIVE_HIGH = 240;
constexpr s64 LIVE_NEAR = 60;
constexpr s64 CATCH_UP_SHOW_EVERY = 15;
// A highlight's fast-forward (SetPlaybackClip) shows one frame in 30: about 20 a second at the
// ~650 frames a second it runs.
constexpr s64 CLIP_SHOW_EVERY = 30;

// The frame on screen (the last one run), and the next one.
s64 PlaybackShown()
{
  return static_cast<s64>(s.pb.file->first_frame + s.pb.next) - 1;
}

// The file's last frame (a live one's grows).
s64 PlaybackLast()
{
  return static_cast<s64>(s.pb.file->first_frame + s.pb.file->pads.size()) - 1;
}

// Whether the whole game is there: always for a replay, once its ENDG came for a live one.
bool PlaybackComplete()
{
  return !s.pb.live || s.pb.cut || !s.pb.file->endg.empty();
}

s64 LiveEdge()
{
  const s64 last = PlaybackLast();
  return PlaybackComplete() ? last :
                              std::max<s64>(last - LIVE_BUFFER, s.pb.file->first_frame);
}

// Whether a live playback waits for frames before frame `f`: out of frames it holds until
// LIVE_LOW are there (or the game's end came).
bool LiveWaiting(s64 f)
{
  auto& pb = s.pb;
  if (!pb.live || PlaybackComplete())
  {
    pb.buffering = false;
    return false;
  }
  const s64 last = PlaybackLast();
  if (f > last)
    pb.buffering = true;
  else if (pb.buffering && last - f >= LIVE_LOW)
    pb.buffering = false;
  return pb.buffering;
}

// At a live playback's loop top, before frame `f`: the catch-up follows the moving edge and ends
// there (or when paused), fast-forward stops there, and live play nudges its speed to keep the
// buffer.
void LiveLoopTop(s64 f)
{
  auto& pb = s.pb;
  const s64 edge = LiveEdge();
  if (pb.catch_up)
  {
    if (f >= edge || pb.paused)
    {
      pb.catch_up = false;
      pb.ff_until = -1;
      if (f >= edge)
      {
        INFO_LOG_FMT(BRAWLBACK, "replay: live at frame {} ({:.1f}x while catching up)", f,
                     pb.catch_rate);
        OSD::AddMessage("Live", OSD::Duration::SHORT);
      }
    }
    else
    {
      pb.ff_until = edge;
      const auto now = Clock::now();
      if (pb.catch_frames++ == 0)
        pb.catch_since = now;
      const double secs = std::chrono::duration<double>(now - pb.catch_since).count();
      if (secs >= 1.0)
        pb.catch_rate = static_cast<double>(pb.catch_frames) / (secs * 60.0);
    }
  }
  if (pb.speed > 1.0 && f >= edge && !PlaybackComplete())
  {
    pb.speed = 1.0;
    OSD::AddMessage("Live", OSD::Duration::SHORT);
  }
  pb.nudge = 1.0;
  if (!PlaybackComplete() && !pb.paused && !pb.catch_up && f >= edge - LIVE_NEAR)
  {
    const s64 buffer = PlaybackLast() - f;
    pb.nudge = buffer > LIVE_HIGH ? 1.02 : buffer < LIVE_LOW ? 0.98 : 1.0;
  }
}

// Starts a seek to s.pb.seek_to (under s.mutex, at the loop top): the newest keyframe at or before
// it is put back, unless the target is ahead with no keyframe in between, and the frames up to the
// target run unpresented. Paused, the target frame is run and shown, then it pauses again.
void PlaybackSeek(Core::System& system)
{
  auto& pb = s.pb;
  const auto& file = *pb.file;
  const s64 first = file.first_frame;
  const s64 last = first + static_cast<s64>(file.pads.size()) - 1;
  const s64 target = std::clamp(pb.seek_to, first, last);
  pb.seek_to = -1;
  pb.at_end = false;
  const s64 next = first + static_cast<s64>(pb.next);
  const auto key = s.pb_keys->AtOrBefore(target);
  if (target >= next && (!key || *key <= next))
  {
    pb.ff_until = target;  // straight on
  }
  else if (key && s.pb_keys->Restore(system, *key))
  {
    pb.next = static_cast<size_t>(*key - first);
    pb.ff_until = target;
    // What the session keeps about frames that ran: as if they had not yet.
    s.current_frame = *key - 1;
    s.game_set_frame = -1;
    s.game_set_quit = false;
    s.snd_max_frame = *key - 1;
    for (auto& v : s.snd_played)
      v.clear();
    s.snd_tag.fill(-1);
    s.snd_remaining.clear();
    s.snd_to_stop.clear();
    s.queued_save = {};
    INFO_LOG_FMT(BRAWLBACK, "replay: seek to frame {}: keyframe {}, {} frames to run", target, *key,
                 target - *key);
  }
  else
  {
    WARN_LOG_FMT(BRAWLBACK, "replay: cannot seek to frame {} (no keyframe at or before it)", target);
    return;
  }
  if (pb.paused)
    pb.step = std::max(pb.step, 1);
}

enum class PlaybackStep
{
  Run,   // s.ops holds the next frame
  Wait,  // paused: spin this pass (the emulation is stopping or pausing)
  End,   // the last frame has run
};

PlaybackStep PlaybackLoopTopControls(Core::System& system)
{
  auto& pb = s.pb;
  const auto& file = *pb.file;
  const s64 first = file.first_frame;
  for (;;)
  {
    // Stopped while paused, held at the end or waiting for frames: OnLoopTop ends it.
    if (pb.stopped)
      return PlaybackStep::Wait;
    if (pb.seek_to >= 0)
      PlaybackSeek(system);
    // A live file grows while this waits.
    const s64 last = PlaybackLast();
    const s64 f = first + static_cast<s64>(pb.next);
    if (pb.live)
      LiveLoopTop(f);
    // Muted while the spectator's first catch-up runs (the user, 2026-10-14).
    if (!pb.catch_up)
      pb.joining = false;
    const bool ff = f < pb.ff_until;
    if (!ff)
      pb.clip_ff = false;
    PlaybackMute(system, (pb.catch_up && pb.joining) || pb.clip_ff);
    const bool waiting = !ff && LiveWaiting(f);
    // A highlight's end: paused there (the launcher plays the next one, or it stays).
    if (pb.clip_end >= 0 && !ff && f > pb.clip_end && !pb.paused)
    {
      pb.clip_end = -1;
      pb.clip_done = true;
      pb.paused = true;
      pb.step = 0;
    }
    if (f > last && !ff && !waiting)
    {
      if (pb.live && pb.cut && pb.file->endg.empty() && !pb.at_end)
        OSD::AddMessage("The rest of this game wasn't streamed.", OSD::Duration::NORMAL, OSD::Color::YELLOW);
      // The last frame has run: the match ends as it did for its player, unless this is the last
      // replay asked for, which holds there (paused) so that it can still be sought back.
      if (!pb.hold_at_end)
        return PlaybackStep::End;
      if (!pb.at_end)
      {
        pb.at_end = true;
        pb.paused = true;
        pb.step = 0;
        OSD::AddMessage("Replay finished", OSD::Duration::NORMAL);
      }
      if (!pb.paused)
        return PlaybackStep::End;
    }
    else if (!waiting && (!pb.paused || pb.step > 0 || ff))
    {
      break;
    }
    // Paused at the loop top (or a live playback waiting for frames): the game stands still
    // between two frames (emulated time too). The video thread shows the last frame again about
    // 30 times a second, so the overlay draws and a screenshot or a capture of the window still
    // gets the paused frame. Commands (PlaybackControl) and frames arriving (ExtendPlayback) wake
    // it.
    s.cpu_where = waiting ? "replay buffering" : "replay paused";
    AsyncRequests::GetInstance()->PushEvent([] {
      if (g_presenter)
        g_presenter->RedrawLastFrame();
    });
    // The event wakes the GPU thread, which then sleeps again only once GpuMaySleep is called. That
    // is a CoreTiming event (1000 a second of emulated time), and emulated time stands still here:
    // without this the GPU thread spun until the playback went on (bug report #17).
    system.GetFifo().GpuMaySleep();
    const bool woke = PlaybackWait(
        system,
        [] {
          const s64 next = static_cast<s64>(s.pb.file->first_frame + s.pb.next);
          return s.pb.stopped || s.pb.seek_to >= 0 ||
                 (!LiveWaiting(next) && (!s.pb.paused || s.pb.step > 0));
        },
        std::chrono::milliseconds(33));
    if (!woke && !CpuRunning(system))
      return PlaybackStep::Wait;  // Dolphin pauses or stops: let it, and come back here
    system.GetCoreTiming().ResetThrottleToNow();
  }
  const s64 f = first + static_cast<s64>(pb.next);
  const bool ff = f < pb.ff_until;
  // Keyframes: the state before frame f.
  if ((f - first) % KEYFRAME_EVERY == 0 ||
      (ff && pb.ff_until - f <= KEYFRAME_EVERY && (f - first) % DENSE_EVERY == 0))
  {
    if (!s.pb_keys->Has(f))
      s.pb_keys->Capture(system, f);
  }
  PadSlots slots = file.pads[pb.next];
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if ((pb.ports >> port) & 1)
    {
      PeerData::SanitizePad(slots.data() + port * INPUT_SIZE);
    }
    else
    {
      std::fill(slots.begin() + port * INPUT_SIZE, slots.begin() + (port + 1) * INPUT_SIZE, u8(0));
      slots[port * INPUT_SIZE + 0x38] = 0xFF;  // gfPadError NO_CONTROLLER
    }
  }
  s.ops.adv_count = 1;
  s.ops.adv_frame[0] = static_cast<int>(f);
  s.ops.save_after[0] = true;
  s.ops.slots[0] = slots;
  s.current_frame = f;
  ++pb.next;
  if (!ff && pb.step > 0)
    --pb.step;
  if (!ff && pb.ff_until >= 0 && f >= pb.ff_until)
    pb.ff_until = -1;
  return PlaybackStep::Run;
}

bool RunFrame(const Core::CPUThreadGuard& guard)
{
  CostPassEnd();
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

  {
    const auto save_t0 = Clock::now();
    PerformQueuedSave(system);
    CostAddSave(SinceNs(save_t0, Clock::now()));
  }

  // Diagnostics (PPR_GPRB_SYNC_GPU=1): in dual core, wait at every loop top until the video thread
  // has executed everything the CPU thread has sent, so the GPU thread never lags behind a frame.
  static const bool sync_gpu = std::getenv("PPR_GPRB_SYNC_GPU") != nullptr;
  if (sync_gpu && system.IsDualCoreMode())
    system.GetFifo().SyncGPU(Fifo::SyncGPUReason::Other, true);

  if (s.force_final_at_reenter)
  {
    // Back from PPR_GPRB_FORCE_FINAL_AT's guest call: the same pass, already fetched.
  }
  else if (s.iteration == 0 && s.replay)
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
  else if (s.iteration == 0 && s.mode == Mode::Playback)
  {
    // The next recorded frame: one advance, saved (for its checksum), never a load. Pauses, steps
    // and seeks happen here, at the loop top, where a keyframe can be taken or put back.
    s.ops.Clear();
    switch (PlaybackLoopTopControls(system))
    {
    case PlaybackStep::Wait:
      Spin(ppc);
      return true;
    case PlaybackStep::End:
      s.pb.finished = true;
      EndRunning(system, "replay ended");
      SetLoopDefault(ppc);
      return true;
    case PlaybackStep::Run:
      break;
    }
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
    UpdateSlowNotice();
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
  // A replay's seek runs the frames before its target unpresented (as a rollback's resimulation).
  // A live playback running ahead (catching up, a seek, a jump) shows a frame in
  // CATCH_UP_SHOW_EVERY, so the game visibly moves instead of freezing until its target.
  // A highlight's fast-forward shows a frame in CLIP_SHOW_EVERY the same way.
  s.resim_pass = s.mode == Mode::Playback ?
                     static_cast<s64>(s.ops.adv_frame[i]) < s.pb.ff_until &&
                         !(s.pb.live && s.ops.adv_frame[i] % CATCH_UP_SHOW_EVERY == 0) &&
                         !(s.pb.clip_ff && s.ops.adv_frame[i] % CLIP_SHOW_EVERY == 0) :
                     s.ops.adv_count > 1 && i != s.ops.adv_count - 1;

  if (s.force_final_at_active)
  {
    RestoreGuestRegs(ppc, s.force_final_at_saved);
    s.force_final_at_active = false;
  }
  else if (!s.force_final_at.empty() &&
           RunForceFinalAt(guard, static_cast<s64>(s.ops.adv_frame[i]),
                           static_cast<s64>(s.ops.adv_frame[i]) > s.snd_max_frame))
  {
    s.force_final_at_reenter = true;
    return true;
  }
  s.force_final_at_reenter = false;

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
    // This player quit a 3-4 player match with the pause screen's quit (the game took its fighter
    // out; docs/game-code.md). Once that frame is final (as a game set's), this machine alone
    // leaves the session: the others hear the leave and play on without it, as after any player
    // who leaves, and the game ends its match (LOCAL.quitLeft). A quit that left one player or
    // team in (quitStays), or a game set by then, ends the session for everyone instead.
    if (s.quit_addr && s.mode == Mode::Network && s.local_slot >= 0 &&
        s.local_slot < MAX_LOBBY_PLAYERS)
    {
      const u8 mine = static_cast<u8>(1u << s.local_slot);
      const u8 leaving = ReadQuitByte(g, Online::GameBridge::SESSION_QUIT_PORTS) &
                         ~ReadQuitByte(g, Online::GameBridge::SESSION_QUIT_STAYS);
      if (leaving & mine)
      {
        if (s.local_quit_frame < 0 || prev_frame < s.local_quit_frame)
          s.local_quit_frame = prev_frame;
      }
      else if (s.local_quit_frame >= 0 && prev_frame <= s.local_quit_frame)
      {
        s.local_quit_frame = -1;  // a rollback undid the quit
      }
      if (s.local_quit_frame >= 0 && s.game_set_frame < 0 && !s.resim_pass &&
          static_cast<s64>(s.ops.adv_frame[i]) >= s.local_quit_frame + END_AFTER_GAME_SET)
      {
        INFO_LOG_FMT(BRAWLBACK, "gprb: P{} quit the match at session frame {}: leaving the session",
                     s.local_slot + 1, s.local_quit_frame);
        SendLeave("");
        EndRunning(system, "quit left");
        s.phase = Phase::Ended;
        s.quit_left = true;
        SetLoopDefault(ppc);
        return true;
      }
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
    // This pass's mtRand calls replace the frame's earlier pass's.
    State::RngFrame& rf = s.rng_frames[static_cast<size_t>(f) % s.rng_frames.size()];
    rf.frame = f;
    rf.pass = s.pass_serial;
    rf.first_run = s.snd_first_run;
    rf.count = 0;
  }
  ct.SetRollbackResimulating(s.resim_pass);
  ct.SetRollbackAudioWaits(PauseResimAudio());
  {
    // The audio clock waits as long as the resimulated frames took when they were presented: a
    // field per pass when it starts (so the pause lines up with the passes), corrected once the
    // pass's length is known if it was not a field long (a lag frame is nearly two, the frames
    // that catch up after it shorter). Smaller differences are not frame lengths: the first pass
    // after a load is about 0.5% short and the presented pass after it as much longer, which a
    // pause by measured lengths would turn into a drift of the audio against the picture.
    const s64 now = static_cast<s64>(ct.GetTicks());
    const s64 field = static_cast<s64>(system.GetVideoInterface().GetTicksPerField());
    if (s.audio_pass_resim)
    {
      const s64 length = now - s.audio_pass_start;
      if (std::abs(length - field) > field / 10)
        ct.AddRollbackAudioPause(length - field);
    }
    if (s.resim_pass)
      ct.AddRollbackAudioPause(field);
    s.audio_pass_start = now;
    s.audio_pass_resim = s.resim_pass;
  }
  ct.SetRollbackSpeedAdjustment(s.mode == Mode::Playback ? s.pb.speed * s.pb.nudge : s.speed_factor);
  if (!s.resim_pass)
  {
    Rollback::PresentStats::OnDisplayedFrameStart(s.ops.adv_count > 1);
    ++s.frames;
    NetStatsOnDisplayedFrame(system);
  }
  // The replay: this pass's input (a later pass of the frame overwrites it), once per update the
  // frames no rollback can reach any more.
  if (i == 0)
    Replay::Advance(s.current_frame);
  Replay::OnPass(static_cast<s64>(s.ops.adv_frame[i]), s.ops.slots[i]);
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
  // Test (PPR_GPRB_TEST_FRAME_COST_US): every pass, a rollback's too, costs this much more CPU
  // time, as on a slower PC, so that a harness peer can be one that cannot keep full speed.
  static const long test_frame_cost_us = [] {
    const char* v = std::getenv("PPR_GPRB_TEST_FRAME_COST_US");
    return v ? std::atol(v) : 0L;
  }();
  if (test_frame_cost_us > 0)
  {
    const auto until = Clock::now() + std::chrono::microseconds(test_frame_cost_us);
    s.mutex.unlock();
    while (Clock::now() < until)
    {
    }
    s.mutex.lock();
  }
  SetLoopDefault(ppc);
  s.cpu_where = "guest";
  CostPassStart(s.resim_pass);
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
  // The host's init block is tagged with its match index, which starts at 0 again: one kept
  // from the last session (a set that ended in its first game) read as this session's match 0
  // to a joiner that reached the match first, and its stage variant failed the barrier.
  s.init_block.clear();
  s.init_match = -1;
  s.setup = {};
  s.input_delay = DEFAULT_INPUT_DELAY;
  s.last_winner = 0xFF;
  s.last_pickers = options.initial_pickers & 0x0F;
  s.setup_error = 0;
  s.last_stage = NO_STAGE;
  s.stage_pool.clear();
  s.await_scene_exit = false;
  s.foreign_packets = 0;
  s.control_id = std::random_device{}();
  s.reassembler.Clear();
  s.split_logged = false;
  s.state_sent.clear();
  s.state_repeats = 0;
  for (auto& b : s.gekko_bundle)
    b.clear();
  s.peer_left = false;
  s.peer_left_reason.clear();
  s.leave_sent = false;
  s.wait_logs = 0;
  s.state_size_logged = false;
  s.disconnected = false;
  s.desync_end = false;
  s.desync_frame = -1;
  s.desynced = false;
  s.recovery = {};
  s.quit_left = false;
  s.unheard_ports = 0;
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
  s.connecting_since = Clock::now();
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

void SetQuitLoses(bool loses)
{
  s_quit_loses = loses;
}

void SetDesyncRecovery(bool on)
{
  s_desync_recovery = on;
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

picojson::object GetPlayerExtra(int port)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.mode != Mode::Network || port < 0 || port >= MAX_LOBBY_PLAYERS || port == s.local_slot ||
      !s.peers[port].expected || s.peers[port].left)
  {
    return {};
  }
  return s.peers[port].extra;
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
  l.quit_left = s.quit_left;
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
    l.restore = s.setup.restore;
  }
  l.last_winner = s.last_winner;
  l.stage_pickers = s.last_pickers;
  l.setup_error = s.setup_error;
  l.after_match = s.await_scene_exit;
  l.unheard = s.unheard_ports;
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

// A replay stopped in its match whose game has not left it yet (StopPlayback).
bool PlaybackStoppingLocked()
{
  return s.mode == Mode::Playback && s.pb.stopped && (InMatchPhase() || s.await_scene_exit);
}

void Stop()
{
  {
    std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
    // A replay is no online connection: the game's CLEANUP_CONNECTION (sent whenever it is on the
    // menus, e.g. on its way from the boot to the CSS a replay starts from) and the online
    // backend's stops leave it. StopPlayback ends it.
    if (s.mode == Mode::Playback)
      return;
    // Leaving tells the peer at once, so it does not wait for the 7.2 s silence timeout.
    SendLeave("");
    s.quit_left = false;
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

// How the recorded game ended (ENDG): its end method and final frame.
void ReadPlaybackEnd(const std::string& endg, std::string* method, s64* final_frame)
{
  picojson::value e;
  if (endg.empty() || !picojson::parse(e, endg).empty() || !e.is<picojson::object>())
    return;
  const auto& eo = e.get<picojson::object>();
  if (const auto it = eo.find("endMethod"); it != eo.end() && it->second.is<std::string>())
    *method = it->second.get<std::string>().substr(0, 32);
  if (const auto it = eo.find("finalFrame"); it != eo.end())
  {
    if (const auto f = PeerData::JsonUInt(&it->second, Replay::MAX_FRAMES))
      *final_frame = static_cast<s64>(*f);
  }
}

std::optional<std::string> ArmPlayback(std::shared_ptr<const Replay::File> file, bool play_anyway,
                                       bool hold_at_end, bool live, bool catch_up, bool room_watch)
{
  if (!file)
    return "no replay";
  // Everything here comes from a file a stranger may have made: the setup is checked as the
  // host's control message is (PeerData.h), and only what a joiner takes from its host is used.
  picojson::value v;
  if (!PeerData::JsonSafeToParse(file->setup, 12) || !picojson::parse(v, file->setup).empty() ||
      !v.is<picojson::object>())
  {
    return "the replay has no match setup";
  }
  const auto& o = v.get<picojson::object>();
  const auto get = [&](const char* k) -> const picojson::value* {
    const auto it = o.find(k);
    return it == o.end() ? nullptr : &it->second;
  };
  State::Playback pb;
  pb.play_anyway = play_anyway;
  pb.hold_at_end = hold_at_end;
  // Who played and what, for the overlay (display only: cleaned and cut to length).
  {
    picojson::value info;
    if (PeerData::JsonSafeToParse(file->info, 12) && picojson::parse(info, file->info).empty() &&
        info.is<picojson::object>())
    {
      const auto& io = info.get<picojson::object>();
      const auto str = [&](const picojson::object& o, const char* k) {
        const auto it = o.find(k);
        return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string();
      };
      if (const auto it = io.find("players"); it != io.end() && it->second.is<picojson::array>())
      {
        for (const auto& p : it->second.get<picojson::array>())
        {
          if (!p.is<picojson::object>())
            continue;
          const auto& po = p.get<picojson::object>();
          const auto port = po.find("port");
          const auto n = PeerData::JsonUInt(port != po.end() ? &port->second : nullptr, MAX_LOBBY_PLAYERS);
          if (!n || *n < 1)
            continue;
          const auto name = Online::ChatText::Clean(str(po, "displayName"), 16);
          pb.names[*n - 1] = name ? *name : fmt::format("P{}", *n);
          const auto ch = po.find("characterId");
          pb.chars[*n - 1] =
              static_cast<u8>(PeerData::JsonUInt(ch != po.end() ? &ch->second : nullptr, 0xFF).value_or(0xFF));
        }
      }
      const std::string mode = str(io, "mode");
      const auto game = io.find("gameIndex");
      const auto g = PeerData::JsonUInt(game != io.end() ? &game->second : nullptr, 255);
      const char* label = mode == "ranked"   ? "Ranked" :
                          mode == "unranked" ? "Unranked" :
                          mode == "direct"   ? "Direct" :
                          mode == "room"     ? "Room" :
                          mode == "teams"    ? "Teams" :
                                               "Replay";
      pb.title = g && *g ? fmt::format("{} - Game {}", label, *g) : std::string(label);
    }
  }
  const auto sim = PeerData::JsonUInt(get("sim"), 0xFFFFFFFF);
  if (!sim || (!Replay::SimVersionPlays(*sim) && !play_anyway))
  {
    return fmt::format("the replay was recorded with another version of the game's netcode ({}, "
                       "this one plays {})",
                       sim ? std::to_string(*sim) : std::string("unknown"), Replay::SIM_VERSION);
  }
  const auto start = PeerData::JsonUInt(get("start_frame"), 600);
  const auto index = PeerData::JsonUInt(get("match_index"), 0xFFFFFFFF);
  const auto seed = PeerData::JsonUInt(get("seed"), 0xFFFFFFFF);
  const auto ports = PeerData::JsonUInt(get("ports"), 0xF);
  const auto local = PeerData::JsonUInt(get("local_slot"), MAX_LOBBY_PLAYERS - 1);
  if (!start || *start < 60 || !index || !seed || !ports || std::popcount(*ports) < 2 || !local ||
      !((*ports >> *local) & 1))
  {
    return "the replay's match setup is damaged";
  }
  pb.start_frame = static_cast<u32>(*start);
  pb.match_index = static_cast<u32>(*index);
  pb.seed = static_cast<u32>(*seed);
  pb.ports = static_cast<u32>(*ports);
  pb.local_slot = static_cast<int>(*local);
  const auto hex = [&](const char* k, size_t min, size_t max) -> std::optional<std::vector<u8>> {
    const auto* x = get(k);
    if (!x || !x->is<std::string>())
      return std::nullopt;
    const size_t n = x->get<std::string>().size() / 2;
    if (n < min || n > max)
      return std::nullopt;
    return PeerData::ParseHex(x->get<std::string>(), n);
  };
  const auto init = hex("init", PeerData::INIT_BLOCK_SIZE, PeerData::INIT_BLOCK_SIZE);
  const auto key = hex("setup_key", 1, 16 * 1024);
  const auto session = hex("session", 0x220, 0x400);
  const auto sync = get("sync") ? SyncBlockFromJson(*get("sync")) : std::nullopt;
  const auto tasks = get("tasks") ? TaskListsFromJson(*get("tasks")) : std::nullopt;
  if (!init || !key || !session || !sync || !tasks)
    return "the replay's match setup is damaged";
  pb.init = *init;
  pb.setup_key = *key;
  pb.session = *session;
  pb.sync = *sync;
  pb.tasks = *tasks;
  if (const auto* x = get("sync_task_order"); x && x->is<bool>())
    pb.sync_task_order = x->get<bool>();
  // The countdown's buttons (SIM_VERSION 2); a SIM_VERSION 1 file has none: its countdown ran
  // neutral.
  if (const auto* x = get("holds"); x && x->is<picojson::array>())
  {
    const auto& a = x->get<picojson::array>();
    if (a.size() != pb.holds.size())
      return "the replay's match setup is damaged";
    for (size_t i = 0; i < a.size(); ++i)
    {
      const auto n = PeerData::JsonUInt(&a[i], 0xFFFF);
      if (!n)
        return "the replay's match setup is damaged";
      pb.holds[i] = static_cast<u16>(*n & HOLD_BUTTONS);
    }
  }
  // A live game may be armed from its setup alone: its match loads while its first frames come,
  // and the barrier waits for them (PlaybackLoopTop).
  if (file->pads.empty() && !live)
    return "the replay has no frames";
  if (!file->pads.empty() && file->ports != pb.ports)
    return "the replay's frames do not match its players";
  ReadPlaybackEnd(file->endg, &pb.end_method, &pb.final_frame);
  pb.live = live;
  pb.catch_up_at_start = live && catch_up;
  pb.room_watch = room_watch;
  if (live)
  {
    // Its own copy, which ExtendPlayback grows; it starts by waiting for a buffer's frames.
    pb.live_file = std::make_shared<Replay::File>(*file);
    pb.file = pb.live_file;
    pb.buffering = true;
  }
  else
  {
    pb.file = std::move(file);
  }

  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.mode == Mode::Network && s.phase != Phase::Idle && s.phase != Phase::Ended &&
      s.phase != Phase::Error)
  {
    return "an online session is running";
  }
  if (s.mode == Mode::Playback && InMatchPhase())
    return "a replay is playing";
  if (PlaybackStoppingLocked())
    return "the last replay's match is still ending";
  CloseNetwork();
  pb.serial = s.pb.serial + 1;
  s.pb = std::move(pb);
  s.mode = Mode::Playback;
  s.phase = Phase::Armed;
  s.error.clear();
  s.did_frame0 = false;
  s.match_index = s.pb.match_index;
  s.session_seed = s.pb.seed;
  s.end_reason.clear();
  INFO_LOG_FMT(BRAWLBACK, "replay: armed ({} frames from frame {}, ports {:#x}, P{}'s file, end {}{}{})",
               s.pb.file->pads.size(), s.pb.file->first_frame, s.pb.ports, s.pb.local_slot + 1,
               s.pb.end_method.empty() ? "incomplete" : s.pb.end_method,
               s.pb.play_anyway ? ", played anyway" : "", s.pb.live ? ", live" : "");
  return std::nullopt;
}

// What a live file read again has past `have` (under s.mutex).
void ExtendLiveFile(Replay::File& have, const Replay::File& file)
{
  auto& pb = s.pb;
  const size_t n = have.pads.size();
  if (file.pads.size() > n)
  {
    have.pads.insert(have.pads.end(), file.pads.begin() + n, file.pads.end());
    have.checksums.insert(have.checksums.end(), file.checksums.begin() + n, file.checksums.end());
    for (const auto& p : file.parts)
    {
      if (have.parts.empty() || p.frame > have.parts.back().frame)
        have.parts.push_back(p);
    }
  }
  for (size_t i = have.events.size(); i < file.events.size(); ++i)
    have.events.push_back(file.events[i]);
  if (have.endg.empty() && !file.endg.empty())
  {
    have.endg = file.endg;
    ReadPlaybackEnd(have.endg, &pb.end_method, &pb.final_frame);
    INFO_LOG_FMT(BRAWLBACK, "replay: the live game ended ({}, {} frames)",
                 pb.end_method.empty() ? "no end method" : pb.end_method, have.pads.size());
  }
}

void ExtendPlayback(std::shared_ptr<const Replay::File> file, bool hold_at_end, bool cut)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  auto& pb = s.pb;
  if (s.mode != Mode::Playback || !pb.live || !pb.live_file)
    return;
  Replay::File& have = *pb.live_file;
  // A game armed from its setup alone: its first frames say where its frames start.
  if (file && have.pads.empty() && !file->pads.empty() && file->uid == have.uid &&
      file->ports == pb.ports)
  {
    have.first_frame = file->first_frame;
    have.ports = file->ports;
  }
  if (file && (file->uid != have.uid || file->first_frame != have.first_frame ||
               file->ports != have.ports || file->checksums.size() != file->pads.size()))
  {
    WARN_LOG_FMT(BRAWLBACK, "replay: the live file changed under playback; not followed");
    file = nullptr;
  }
  if (file)
    ExtendLiveFile(have, *file);
  if (cut && !pb.cut)
  {
    pb.cut = true;
    INFO_LOG_FMT(BRAWLBACK, "replay: the live game was cut after {} frames", have.pads.size());
  }
  if (pb.hold_at_end != hold_at_end)
  {
    pb.hold_at_end = hold_at_end;
    // Held at the end for the set's next game, which has come: the match ends and it plays.
    if (!hold_at_end && pb.at_end)
      pb.paused = false;
  }
  s_pb_wake.notify_all();
}

void SetPlaybackBanner(std::string text)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  s.pb.banner = std::move(text);
}

bool SetPlaybackClip(s64 start, s64 end)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  auto& pb = s.pb;
  if (s.mode != Mode::Playback || !pb.file || pb.stopped || pb.live)
    return false;
  const s64 first = pb.file->first_frame;
  start = std::clamp<s64>(start, first, Replay::MAX_FRAMES);
  pb.clip_start = start;
  pb.clip_end = std::max(end, start);
  pb.clip_done = false;
  pb.clip_ff = true;
  pb.paused = false;
  pb.step = 0;
  pb.seek_to = start;
  ++pb.controls;
  INFO_LOG_FMT(BRAWLBACK, "replay: clip {}-{}", start, pb.clip_end);
  s_pb_wake.notify_all();
  return true;
}

void PlaybackControl(PlaybackCommand command, double arg)
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  auto& pb = s.pb;
  if (s.mode != Mode::Playback || !pb.file || (s.phase != Phase::Running && s.phase != Phase::Countdown))
    return;
  const s64 shown = s.phase == Phase::Running ? PlaybackShown() : static_cast<s64>(pb.file->first_frame);
  static constexpr std::array<double, 6> SPEEDS{0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
  ++pb.controls;
  // Any other control stops a catch-up to the live edge where it is (GoLive starts it again).
  if (command != PlaybackCommand::GoLive && pb.catch_up)
  {
    pb.catch_up = false;
    pb.ff_until = -1;
  }
  switch (command)
  {
  case PlaybackCommand::GoLive:
    if (!pb.live)
      break;
    pb.paused = false;
    pb.step = 0;
    pb.speed = 1.0;
    if (PlaybackComplete())
    {
      pb.seek_to = PlaybackLast();
      break;
    }
    // A seek to the edge (from a keyframe when one is nearer), which the catch-up then follows.
    pb.seek_to = LiveEdge();
    pb.catch_up = true;
    pb.catch_frames = 0;
    pb.catch_rate = 0;
    break;
  case PlaybackCommand::TogglePause:
    pb.paused = !pb.paused;
    pb.step = 0;
    break;
  case PlaybackCommand::Pause:
    pb.paused = true;
    pb.step = 0;
    break;
  case PlaybackCommand::Play:
    pb.paused = false;
    break;
  case PlaybackCommand::StepForward:
    pb.paused = true;
    ++pb.step;
    break;
  case PlaybackCommand::StepBack:
    pb.paused = true;
    pb.step = 0;
    pb.seek_to = std::max<s64>((pb.seek_to >= 0 ? pb.seek_to : shown) - 1, pb.file->first_frame);
    break;
  case PlaybackCommand::Jump:
    if (std::isfinite(arg))
    {
      pb.seek_to = std::max<s64>((pb.seek_to >= 0 ? pb.seek_to : shown) +
                                     static_cast<s64>(std::clamp(arg, -3600.0, 3600.0) * 60),
                                 pb.file->first_frame);
    }
    break;
  case PlaybackCommand::Seek:
    if (std::isfinite(arg))
      pb.seek_to = static_cast<s64>(std::clamp(arg, 0.0, static_cast<double>(Replay::MAX_FRAMES)));
    break;
  case PlaybackCommand::Speed:
    if (std::isfinite(arg))
      pb.speed = std::clamp(arg, 0.25, 2.0);
    break;
  case PlaybackCommand::Faster:
  case PlaybackCommand::Slower:
  {
    size_t i = 0;
    while (i + 1 < SPEEDS.size() && SPEEDS[i] < pb.speed - 1e-6)
      ++i;
    if (command == PlaybackCommand::Faster && i + 1 < SPEEDS.size() && SPEEDS[i] <= pb.speed + 1e-6)
      ++i;
    else if (command == PlaybackCommand::Slower && i > 0)
      --i;
    pb.speed = SPEEDS[i];
    OSD::AddMessage(fmt::format("Speed {}x", pb.speed), OSD::Duration::SHORT);
    break;
  }
  }
  // A live playback seeks no further than the live edge (past it there is nothing to play yet but
  // the buffer): a seek or jump that gets there is Go Live, which then follows the moving edge.
  if ((command == PlaybackCommand::Seek || command == PlaybackCommand::Jump) && pb.live &&
      !PlaybackComplete() && pb.seek_to >= LiveEdge() - LIVE_NEAR)
  {
    pb.seek_to = LiveEdge();
    pb.paused = false;
    pb.step = 0;
    pb.speed = 1.0;
    pb.catch_up = true;
    pb.catch_frames = 0;
    pb.catch_rate = 0;
  }
  s_pb_wake.notify_all();
}

PlaybackInfo GetPlaybackInfo()
{
  PlaybackInfo info;
  std::unique_lock<std::recursive_timed_mutex> lk(s.mutex, std::chrono::milliseconds(5));
  if (!lk.owns_lock() || s.mode != Mode::Playback || !s.pb.file || !s.pb.started ||
      (s.phase != Phase::Running && s.phase != Phase::Countdown))
  {
    return info;
  }
  const auto& file = *s.pb.file;
  info.active = true;
  info.first = file.first_frame;
  info.last = file.first_frame + static_cast<s64>(file.pads.size()) - 1;
  info.shown = s.phase == Phase::Running ? std::max<s64>(PlaybackShown(), info.first) : info.first;
  info.paused = s.pb.paused;
  info.at_end = s.pb.at_end;
  info.seeking = s.pb.seek_to >= 0 || s.pb.ff_until >= 0;
  info.speed = s.pb.speed;
  info.desync_frame = s.pb.desync_frame;
  info.title = s.pb.title;
  info.names = s.pb.names;
  info.chars = s.pb.chars;
  info.keyframes = s.pb_keys->Count();
  info.live = s.pb.live;
  if (s.pb.live)
  {
    info.ended = PlaybackComplete();
    info.live_edge = LiveEdge();
    info.buffering = s.pb.buffering;
    info.catching_up = s.pb.catch_up;
    info.catch_rate = s.pb.catch_rate;
    info.at_live = !info.ended && !s.pb.paused && !s.pb.catch_up && s.pb.seek_to < 0 &&
                   info.shown >= info.live_edge - LIVE_NEAR;
  }
  info.banner = s.pb.banner;
  return info;
}

void StopPlayback()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  if (s.mode != Mode::Playback)
    return;
  // The game must leave a match it is in, or on its way into, before another replay can start
  // (spectating, the user 2026-10-14: another game picked while one played was armed into the
  // match still running, never passed its barrier, and the first game's match stayed on screen).
  // Left to itself (no session, nothing DISCONNECTED or quit) the game plays such a match on.
  if (InMatchPhase())
  {
    // Ends on the CPU thread at the next loop top (OnLoopTop).
    s.pb.stopped = true;
    s_pb_wake.notify_all();
    return;
  }
  if (s.phase == Phase::Armed && s.did_frame0)
  {
    // Seeded, its match on the way: nothing runs yet; the game quits it once in it.
    s.pb.stopped = true;
    s.pb.end_quit_left = true;
    s.phase = Phase::Ended;
    s.await_scene_exit = true;
    INFO_LOG_FMT(BRAWLBACK, "replay: stopped on the way into its match");
    return;
  }
  s.mode = Mode::None;
  s.phase = Phase::Idle;
}

bool PlaybackStopping()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  return PlaybackStoppingLocked();
}

PlaybackView GetPlaybackView()
{
  std::lock_guard<std::recursive_timed_mutex> lk(s.mutex);
  PlaybackView v;
  if (s.mode != Mode::Playback)
    return v;
  v.active = true;
  v.serial = s.pb.serial;
  v.session = s.pb.session;
  v.local_port = s.pb.local_slot;
  v.in_match = InMatchPhase();
  v.disconnected = s.pb.end_disconnected;
  v.desynced = s.pb.end_desynced;
  v.quit_left = s.pb.end_quit_left;
  v.room_watch = s.pb.room_watch;
  v.starting = s.phase == Phase::Armed && !s.pb.stopped && !s.await_scene_exit;
  v.stopping = PlaybackStoppingLocked();
  return v;
}

picojson::object PlaybackStatus()
{
  std::unique_lock<std::recursive_timed_mutex> lk(s.mutex, std::chrono::milliseconds(1000));
  picojson::object o;
  if (!lk.owns_lock())
  {
    o["busy"] = picojson::value(true);
    return o;
  }
  const bool playback = s.mode == Mode::Playback;
  o["active"] = picojson::value(playback);
  o["phase"] = picojson::value(PhaseName(s.phase));
  o["serial"] = picojson::value(static_cast<double>(s.pb.serial));
  o["started"] = picojson::value(s.pb.started);
  o["finished"] = picojson::value(s.pb.finished);
  o["error"] = picojson::value(s.pb.error);
  o["frame"] = picojson::value(static_cast<double>(playback ? s.current_frame : -1));
  if (s.pb.file)
  {
    o["first_frame"] = picojson::value(static_cast<double>(s.pb.file->first_frame));
    // (first_frame - 1 while a live game armed from its setup has no frames yet)
    o["last_frame"] = picojson::value(static_cast<double>(PlaybackLast()));
  }
  o["final_frame"] = picojson::value(static_cast<double>(s.pb.final_frame));
  o["frames_checked"] = picojson::value(static_cast<double>(s.pb.frames_checked));
  o["desync_frame"] = picojson::value(static_cast<double>(s.pb.desync_frame));
  o["desync"] = picojson::value(s.pb.desync_text);
  o["end_method"] = picojson::value(s.pb.end_method);
  o["play_anyway"] = picojson::value(s.pb.play_anyway);
  o["paused"] = picojson::value(s.pb.paused);
  o["speed"] = picojson::value(s.pb.speed);
  o["at_end"] = picojson::value(s.pb.at_end);
  o["seeking"] = picojson::value(s.pb.seek_to >= 0 || s.pb.ff_until >= 0);
  o["clip_start"] = picojson::value(static_cast<double>(s.pb.clip_start));
  o["clip_end"] = picojson::value(static_cast<double>(s.pb.clip_end));
  o["clip_done"] = picojson::value(s.pb.clip_done);
  o["shown"] = picojson::value(static_cast<double>(
      playback && s.pb.file && s.phase == Phase::Running ? PlaybackShown() : -1));
  o["keyframes"] = picojson::value(static_cast<double>(s.pb_keys->Count()));
  o["keyframe_bytes"] = picojson::value(static_cast<double>(s.pb_keys->Bytes()));
  o["live"] = picojson::value(s.pb.live);
  o["controls"] = picojson::value(static_cast<double>(s.pb.controls));
  if (playback && s.pb.live && s.pb.file)
  {
    o["complete"] = picojson::value(PlaybackComplete());
    o["live_edge"] = picojson::value(static_cast<double>(LiveEdge()));
    o["buffering"] = picojson::value(s.pb.buffering);
    o["catching_up"] = picojson::value(s.pb.catch_up);
    o["join_catch_up"] = picojson::value(s.pb.join_catch_up);
    o["catch_rate"] = picojson::value(s.pb.catch_rate);
  }
  return o;
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
                              s.mode == Mode::Playback ? "playback" :
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
  o["following_peer"] = picojson::value(s.following);
  o["peer_fps"] = picojson::value(s.peer_fps);
  o["region_bytes"] = picojson::value(static_cast<double>(s.region_bytes));
  o["region_ranges"] = picojson::value(static_cast<double>(s.ranges.size()));
  o["end_reason"] = picojson::value(s.end_reason);
  o["end_game_frame"] = picojson::value(static_cast<double>(s.end_game_frame));
  o["game_set_frame"] = picojson::value(static_cast<double>(s.game_set_frame));
  o["local_quit_frame"] = picojson::value(static_cast<double>(s.local_quit_frame));
  o["quit_left"] = picojson::value(s.quit_left);
  // A desync where the game starts again: our checksums wait for the restart's setup, and the
  // restart's values once it is set up (the setup's game, "restore").
  o["recovery_pending"] = picojson::value(s.recovery.pending);
  if (s.setup.restore.valid)
  {
    const Restore& r = s.setup.restore;
    picojson::object ro;
    ro["game"] = picojson::value(static_cast<double>(s.setup.game));
    ro["frame"] = picojson::value(static_cast<double>(r.frame));
    ro["checksum"] = picojson::value(static_cast<double>(r.checksum));
    picojson::array stocks, damage;
    for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
    {
      stocks.emplace_back(static_cast<double>(r.stocks[port]));
      damage.emplace_back(static_cast<double>(r.damage[port]));
    }
    ro["stocks"] = picojson::value(stocks);
    ro["damage"] = picojson::value(damage);
    ro["frames_left"] = picojson::value(static_cast<double>(r.frames_left));
    o["restore"] = picojson::value(ro);
  }
  o["sound_allocs"] = picojson::value(static_cast<double>(s.sound_allocs));
  o["resim_sound_allocs"] = picojson::value(static_cast<double>(s.resim_sound_allocs));
  o["suppressed_sound_allocs"] = picojson::value(static_cast<double>(s.suppressed_sound_allocs));
  o["sound_reattached"] = picojson::value(static_cast<double>(s.sound_reattached));
  o["sound_reattach_gone"] = picojson::value(static_cast<double>(s.sound_reattach_gone));
  o["sound_stopped"] = picojson::value(static_cast<double>(s.sound_stopped));
  o["sound_stop_gone"] = picojson::value(static_cast<double>(s.sound_stop_gone));
  o["sound_moved"] = picojson::value(static_cast<double>(s.sound_moved));
  o["sound_stops_ignored"] = picojson::value(static_cast<double>(s.sound_stops_ignored));
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
  o["frame_cost"] = FrameCostJson();
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
    lobby["pick_from_stages"] = picojson::value(s.net_opts.pick_from_stages);
    lobby["alternate_stages"] = picojson::value(s.net_opts.alternate_stages);
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

// What every other player has told us, for the logs while a barrier waits (under s.mutex).
std::string WaitText()
{
  std::string out;
  const auto now = Clock::now();
  const int host = HostSlot();
  ForEachPeer([&](int slot, const PeerView& p) {
    if (!out.empty())
      out += "; ";
    out += fmt::format("P{} ({}{})", slot + 1, p.name, slot == host ? ", host" : "");
    if (p.left)
    {
      out += fmt::format(" left: {}", p.left_reason);
      return;
    }
    if (!p.seen)
    {
      out += " never heard";
      return;
    }
    out += fmt::format(" heard {:.0f} ms ago, at_s {}, applied {}, at_start {}, go {}, "
                       "setup {:016x}, hold {}",
                       std::chrono::duration<double, std::milli>(now - p.last_heard).count(),
                       p.at_s, p.applied, p.at_start, p.go, p.setup_hash, p.hold_valid);
    if (slot == host)
    {
      out += fmt::format(", init for match {}, sync {}, ports {:#x}, task order {}, holds {}",
                         p.init_match, p.sync.valid, p.match_ports, p.task_order_valid,
                         p.holds_valid);
    }
  });
  return out;
}

// Every WAIT_LOG_INTERVAL of a barrier's wait: what it still waits for (under s.mutex).
void LogWait(std::string_view where, Clock::time_point since)
{
  const auto waited = Clock::now() - since;
  if (waited < WAIT_LOG_INTERVAL * (s.wait_logs + 1))
    return;
  ++s.wait_logs;
  WARN_LOG_FMT(BRAWLBACK, "gprb: {:.0f} s {} (P{}{}, match {}); {}",
               std::chrono::duration<double>(waited).count(), where, s.local_slot + 1,
               IsHost() ? ", host" : fmt::format(", applied {}", s.applied), s.match_index,
               WaitText());
}

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
    s.wait_logs = 0;
    // (The host may leave meanwhile: the next host is then this player or another peer.)
    while (!IsHost() && !host_block_ready())
    {
      LogWait("waiting for the host's match setup", since);
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
    // Tests (PPR_GPRB_TEST_HOST_START_DELAY_MS=<ms>): the host reaches the match start that much
    // later, so its joiner gets there first and must wait for this match's init block (a slower
    // host's machine on 2026-10-12 took the last session's).
    if (const char* delay = std::getenv("PPR_GPRB_TEST_HOST_START_DELAY_MS"); delay && *delay)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb: PPR_GPRB_TEST_HOST_START_DELAY_MS: {} ms", delay);
      s.mutex.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
      s.mutex.lock();
    }
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
  PinScreenSetting(system);
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

// The replay of this game (ReplayRecorder.h), at the barrier once every joiner has applied the
// host's values: everything a fresh boot needs to rebuild the match as this machine played it
// (SETP; playback applies it as a joiner applies its host's, GameplayPlayback), and the metadata.
void BeginReplay(Core::System& system)
{
  if (s.mode != Mode::Network || !Replay::Enabled())
    return;
  const Guest g(system.GetMemory());
  const PeerView* hp = HostPeer();
  const bool host = IsHost() || !hp;
  const SyncBlock& sync = host ? s.sync : hp->sync;
  const auto& tasks = host ? s.task_order : hp->task_order;
  const std::vector<u8>& init = host ? s.init_block : hp->init_block;

  picojson::object o;
  o["v"] = picojson::value(1.0);
  o["sim"] = picojson::value(static_cast<double>(Replay::SIM_VERSION));
  o["start_frame"] = picojson::value(static_cast<double>(StartFrame()));
  o["match_index"] = picojson::value(static_cast<double>(s.match_index));
  o["seed"] = picojson::value(static_cast<double>(s.session_seed));
  o["init"] = picojson::value(Hex(init));
  o["setup_key"] = picojson::value(Hex(s.setup_key));
  o["setup_hash"] = picojson::value(fmt::format("{:016x}", s.setup_hash));
  o["ports"] = picojson::value(static_cast<double>(s.match_ports));
  o["local_slot"] = picojson::value(static_cast<double>(s.local_slot));
  o["host_slot"] = picojson::value(static_cast<double>(HostSlot()));
  o["sync"] = SyncBlockJson(sync);
  picojson::array lists;
  for (const auto& l : tasks)
  {
    picojson::array names;
    for (const auto& n : l)
      names.emplace_back(n);
    lists.emplace_back(names);
  }
  o["tasks"] = picojson::value(lists);
  o["sync_task_order"] = picojson::value(s.net_opts.sync_task_order);
  if (s.setup.game != 0)
    o["match_setup"] = SetupJson(s.setup);
  o["gone"] = picojson::value(s.gone_enabled);
  // The countdown's buttons per port (CountdownSlots; SIM_VERSION 2).
  {
    picojson::array holds;
    for (const u16 h : s.holds)
      holds.emplace_back(static_cast<double>(s.holds_valid ? h : 0));
    o["holds"] = picojson::value(holds);
  }
  // SESSION as the game built this match from it (the same on every machine).
  std::vector<u8> session;
  if (const u32 sa = Online::GameBridge::SessionAddress())
  {
    const u32 size = Online::GameBridge::SessionSize();
    if (const u8* p = size ? g.Ptr(sa, size) : nullptr)
      session.assign(p, p + size);
  }
  o["session"] = picojson::value(Hex(session));

  Replay::BeginInfo info;
  info.meta = s.net_opts.replay_meta;
  info.local_port = s.local_slot;
  info.host = IsHost();
  info.match_ports = s.match_ports;
  info.session_game = NextGame();
  info.game = session.size() > 6 && session[6] ? session[6] : NextGame();
  info.start_frame = StartFrame();
  info.input_delay = s.input_delay;
  info.region_set = s.net_opts.region_set;
  // Who plays what, from the match setup the game built (the same on every machine).
  if (const auto mm = ModeMeleeAddr(g))
  {
    if (const u8* p = g.Ptr(*mm, Addr::MODE_MELEE_SIZE))
    {
      info.teams = p[MM_IS_TEAMS] != 0;
      info.stage = static_cast<u16>(p[0x1A] << 8 | p[0x1B]);
      for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
      {
        const u8* pl = p + MM_PLAYERS + port * MM_PLAYER_SIZE;
        info.char_kind[port] = pl[0];
        info.costume[port] = pl[5];
        info.team[port] = info.teams ? pl[PI_TEAM] : NO_TEAM;
      }
    }
  }
  if (s.setup.game != 0)
  {
    info.stage = s.setup.stage;
    info.asl = s.setup.asl;
  }
  // The game's id, the same in every player's file of it. A room's game number is each machine's
  // own count of the room's games (Rooms.cpp), which a player who joined after the room's first
  // game counts differently; the session's own game is the same on every machine. (mm pairs the
  // players' uploads by match too: builds up to 0.1.72 put the room's count here.)
  {
    std::vector<u8> id(info.meta.match_id.begin(), info.meta.match_id.end());
    const u32 id_game = info.meta.room ? info.session_game : info.game;
    for (u32 v : {id_game, s.session_seed, s.match_index})
    {
      for (int shift = 0; shift < 32; shift += 8)
        id.push_back(static_cast<u8>(v >> shift));
    }
    id.insert(id.end(), s.setup_key.begin(), s.setup_key.end());
    const XXH128_hash_t h = XXH3_128bits(id.data(), id.size());
    for (int k = 0; k < 8; ++k)
    {
      info.uid[k] = static_cast<u8>(h.high64 >> (56 - 8 * k));
      info.uid[8 + k] = static_cast<u8>(h.low64 >> (56 - 8 * k));
    }
  }
  info.setup_json = picojson::value(o).serialize();
  Replay::Begin(system, info);
}

// The end of this game's replay (EndRunning): how and with what result it ended.
void EndReplay(Core::System& system, const std::string& reason, const GameResult* result)
{
  if (!Replay::Active())
    return;
  Replay::EndInfo e;
  e.method = reason == "game set"                                     ? "game" :
             reason == "quit"                                         ? "quit" :
             reason == "quit left"                                    ? "quit-left" :
             reason == "desync"                                       ? "desync" :
             reason == "stopped" || reason == "left the match scene"  ? "stopped" :
             reason.find("peer") != std::string::npos                 ? "disconnect" :
                                                                        "error";
  if (result && result->end == GameEnd::Runback)
    e.method = "runback";
  e.game_set_frame = s.game_set_frame;
  e.rollbacks = s.rollbacks;
  e.max_rollback = s.max_rollback;
  e.desyncs = s.desyncs;
  const GameResult r = result ? *result : ReadGameResult(system);
  e.winner = result ? r.winner : 0xFF;
  e.quitter = r.quitter;
  e.present = r.present;
  e.stocks = r.stocks;
  e.damage = r.damage;
  e.place = r.place;
  Replay::End(e);
}

// ---- Playback (docs/design/replays.md §6): a joiner whose host is the replay ----

// P+'s CSS slot table (0x39 slots: docs/brawl-memory-map.md, harness brawl.py CSS_TO_CHAR_KIND):
// per slot +0 its gmCharacterKind and +8 its costume list; the team colours (red, blue, green).
constexpr u32 CSS_SLOT_TABLE = 0x80585B00, CSS_SLOT_SIZE = 0x10, CSS_SLOT_COSTUMES = 0x08,
              CSS_SLOTS = 0x39;
constexpr u32 TEAM_COLOURS = 0x805A21F0;
constexpr u32 PI_COLOR_FILE = 0x05, PI_COLOR_NO = 0x06;  // m_colorFileNo, m_colorNo
// The setup key (SetupKey): the init block, then per player character, state, stocks, costume
// (m_colorFileNo) and, in a team battle, the team.
constexpr size_t KEY_PLAYERS = 0x20, KEY_TEAMS_PLAYER = 5, KEY_COSTUME = 3;

std::vector<PeerData::CostumeEntry> CostumeList(const Guest& g, u32 list)
{
  std::vector<PeerData::CostumeEntry> out;
  const u8* l = g.Ptr(list, 2 * PeerData::MAX_COSTUME_LIST);
  for (size_t i = 0; l && i < PeerData::MAX_COSTUME_LIST && l[2 * i] != PeerData::COSTUME_LIST_END; ++i)
    out.push_back({l[2 * i], l[2 * i + 1]});
  return out;
}

// A team battle recorded before 2026-10-12 (PeerData::LegacyTeamCostumes): its players played
// their team's costume, the rebuilt match the recorded SESSION's (the costume they locked in). The
// match gets the recorded costumes before it loads, when the old rule gives every one of them from
// this game's own costume lists; else it is left as built and the barrier's setup check decides.
void RestoreLegacyTeamCostumes(Core::System& system, const Guest& g)
{
  namespace GB = Online::GameBridge;
  const auto& key = s.pb.setup_key;
  const auto& se = s.pb.session;
  const auto mm = ModeMeleeAddr(g);
  const u8* p = mm ? g.Ptr(*mm, Addr::MODE_MELEE_SIZE) : nullptr;
  if (!p || p[MM_IS_TEAMS] == 0 || key.size() < KEY_PLAYERS + MAX_LOBBY_PLAYERS * KEY_TEAMS_PLAYER ||
      key[MM_IS_TEAMS - INIT_BLOCK_OFFSET] == 0 ||
      se.size() < GB::SESSION_PLAYERS_OFF + MAX_LOBBY_PLAYERS * GB::SESSION_PLAYER_SIZE)
  {
    return;
  }
  const u8* colours = g.Ptr(TEAM_COLOURS, 3);
  const u8* slots = g.Ptr(CSS_SLOT_TABLE, CSS_SLOTS * CSS_SLOT_SIZE);
  if (!colours || !slots)
    return;
  std::array<PeerData::LegacyCostumePlayer, MAX_LOBBY_PLAYERS> players;
  bool differs = false;
  for (u32 port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    const u8* pl = p + MM_PLAYERS + port * MM_PLAYER_SIZE;
    const u8* rec = key.data() + KEY_PLAYERS + port * KEY_TEAMS_PLAYER;
    if (pl[PI_STATE] != 0)
      continue;
    // Only the costume may differ: the character, state, stocks and team are the recording's.
    if (rec[0] != pl[0] || rec[1] != pl[PI_STATE] || rec[2] != pl[4] || rec[4] != pl[PI_TEAM])
      return;
    const u32 sp = GB::SESSION_PLAYERS_OFF + port * GB::SESSION_PLAYER_SIZE;
    auto& lp = players[port];
    lp.present = true;
    lp.costume = se[sp + 2];  // SessionPlayer::costume
    lp.team = pl[PI_TEAM];
    lp.recorded_file = rec[KEY_COSTUME];
    differs |= lp.recorded_file != pl[PI_COLOR_FILE];
    for (u32 slot = 0; slot < CSS_SLOTS; ++slot)
    {
      const u8* e = slots + slot * CSS_SLOT_SIZE;
      if (e[0] != pl[0])
        continue;
      const auto list = g.Ptr32(CSS_SLOT_TABLE + slot * CSS_SLOT_SIZE + CSS_SLOT_COSTUMES);
      if (list)
        lp.lists.push_back(CostumeList(g, *list));
    }
  }
  if (!differs)
    return;
  const auto picks = PeerData::LegacyTeamCostumes(players, {colours[0], colours[1], colours[2]});
  if (!picks)
  {
    WARN_LOG_FMT(BRAWLBACK, "replay: the recorded team costumes are not the pre-2026-10-12 team "
                            "costumes; kept the rebuilt ones");
    return;
  }
  for (u32 port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    const auto& pick = (*picks)[port];
    const u8* pl = p + MM_PLAYERS + port * MM_PLAYER_SIZE;
    if (!pick || (pl[PI_COLOR_FILE] == pick->file && pl[PI_COLOR_NO] == pick->color_no))
      continue;
    INFO_LOG_FMT(BRAWLBACK, "replay: P{} plays its team's costume as recorded ({} file {}, was {} file {})",
                 port + 1, pick->color_no, pick->file, pl[PI_COLOR_NO], pl[PI_COLOR_FILE]);
    const u32 at = *mm + MM_PLAYERS + port * MM_PLAYER_SIZE;
    system.GetMemory().CopyToEmu(at + PI_COLOR_FILE, &pick->file, 1);
    system.GetMemory().CopyToEmu(at + PI_COLOR_NO, &pick->color_no, 1);
  }
}

// The match start (ApplyMatchStart's joiner side): the recorded host's stage variant, the costumes
// of a team battle recorded before 2026-10-12, the seeds of the recorded session seed and match
// index, the serial counter.
void ApplyPlaybackStart(Core::System& system, std::string_view when)
{
  const Guest g(system.GetMemory());
  const auto mine = InitBlock(g);
  bool rejected = false;
  const auto merged = PeerData::MergeInitBlock(mine, s.pb.init, &rejected);
  if (rejected)
  {
    WARN_LOG_FMT(BRAWLBACK, "replay: the recorded match init block differs beyond the stage variant; "
                            "kept ours ({} against {})",
                 Hex(mine), Hex(s.pb.init));
  }
  if (const auto mm = ModeMeleeAddr(g); mm && merged != mine)
    system.GetMemory().CopyToEmu(*mm + INIT_BLOCK_OFFSET, merged.data(), merged.size());
  RestoreLegacyTeamCostumes(system, g);
  PinScreenSetting(system);
  const auto seeds = MatchSeeds(s.pb.seed, s.pb.match_index);
  WriteU32(system, Addr::MTRAND_DEFAULT_SEED, seeds[0]);
  WriteU32(system, Addr::MTRAND_OTHER_SEED, seeds[1]);
  WriteU32(system, Addr::LIBC_RAND_NEXT, seeds[2]);
  WriteU32(system, Addr::OBJECT_SERIAL_COUNTER, SERIAL_COUNTER_START);
  s.did_frame0 = true;
  INFO_LOG_FMT(BRAWLBACK, "replay: match RNG seeded ({:08x} {:08x} {:08x}){}{}", seeds[0], seeds[1],
               seeds[2], when.empty() ? "" : " ", when);
}

bool PlaybackLoopTop(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  const Guest g(system.GetMemory());
  if (s.phase != Phase::Armed)
  {
    if (!IsSceneMelee(g) && !IsNextSceneMelee(g))
      s.await_scene_exit = false;
    return false;
  }
  const bool melee_pending = !IsSceneMelee(g) && IsNextSceneMelee(g) && !s.await_scene_exit;
  if (!IsSceneMelee(g) && !melee_pending)
  {
    s.did_frame0 = false;
    s.await_scene_exit = false;
    return false;
  }
  if (s.await_scene_exit)
    return false;
  if (!s.did_frame0)
    ApplyPlaybackStart(system, melee_pending ? "before the scene change" : "");
  if (melee_pending)
    return false;
  u32 fc;
  if (!IsSimStart(g, &fc))
    return false;

  // A live game armed from its setup alone (spectating): its stage loaded while the players'
  // first frames were on their way, and its countdown starts once they are here. The stream comes
  // at the speed of play, so by the countdown's end the live buffer's 2 s are too, and GO does not
  // stop for "Buffering".
  if (s.pb.live && s.pb.file->pads.empty())
  {
    if (s.pb.cut || s.pb.stopped)
    {
      // Cut before any frames came: the game leaves the match as after a stop on the way in.
      if (s.pb.cut)
      {
        INFO_LOG_FMT(BRAWLBACK, "replay: the live game was cut before its first frames");
        OSD::AddMessage("This game wasn't streamed.", OSD::Duration::NORMAL, OSD::Color::YELLOW);
      }
      s.pb.stopped = true;
      s.pb.finished = true;
      s.pb.end_quit_left = true;
      s.phase = Phase::Ended;
      s.await_scene_exit = true;
      return false;
    }
    s.cpu_where = "replay waiting for frames";
    OSD::AddTypedMessage(OSD::MessageType::SpectateWaiting, "Game found, waiting for data...",
                         1000, OSD::Color::CYAN);
    AsyncRequests::GetInstance()->PushEvent([] {
      if (g_presenter)
        g_presenter->RedrawLastFrame();
    });
    system.GetFifo().GpuMaySleep();
    PlaybackWait(
        system,
        [] {
          return s.phase != Phase::Armed || s.pb.stopped || s.pb.cut || !s.pb.file->pads.empty();
        },
        std::chrono::milliseconds(33));
    if (s.phase != Phase::Armed)
      return false;
    Spin(ppc);
    return true;
  }

  // The barrier: the match must be the recorded one (the setup the live peers compared), then the
  // recorded host's sync block and task order, as a joiner applies its host's.
  const auto fail = [&](std::string why) {
    s.pb.error = std::move(why);
    s.error = s.pb.error;
    ERROR_LOG_FMT(BRAWLBACK, "replay: {}", s.pb.error);
    OSD::AddMessage(fmt::format("This replay cannot be played: {}", s.pb.error),
                    OSD::Duration::VERY_LONG, OSD::Color::RED);
    s.phase = Phase::Error;
    s.await_scene_exit = true;
    // The game ends the match it started (as after a disconnect) instead of playing it on.
    s.pb.end_disconnected = true;
    return false;
  };
  const auto key = SetupKey(g);
  if (key != s.pb.setup_key)
  {
    WARN_LOG_FMT(BRAWLBACK, "replay: the match's setup differs from the recording's:\n  here {}\n  file {}",
                 Hex(key), Hex(s.pb.setup_key));
    WARN_LOG_FMT(BRAWLBACK, "replay: it differs in (file as the peer): {}",
                 fmt::join(PeerData::SetupKeyDiff(key, s.pb.setup_key), "; "));
    if (!s.pb.play_anyway)
      return fail("its match could not be rebuilt here (the setup differs)");
  }
  const u32 human = HumanPorts(g);
  if (human != s.pb.ports)
    return fail(fmt::format("its players could not be rebuilt here (ports {:#x}, recorded {:#x})",
                            human, s.pb.ports));
  s.match_ports = s.pb.ports;
  s.local_slot = s.pb.local_slot;
  s.peers = {};
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
    s.peers[port].expected = port != s.local_slot && ((s.match_ports >> port) & 1);
  s.gone_enabled = std::popcount(s.match_ports) >= 3;
  s.gone_addr = s.quit_addr = 0;
  s.session_addr = Online::GameBridge::SessionAddress();
  s.gone_written.fill(0);
  if (s.gone_enabled && s.session_addr)
  {
    s.gone_addr = s.session_addr + GONE_FLAG_OFFSET;
    s.quit_addr = s.session_addr + Online::GameBridge::SESSION_QUIT_PORTS;
  }
  WriteSyncBlock(system, s.pb.sync);
  s.holds = s.pb.holds;
  s.holds_valid = true;
  int relinked = 0;
  if (s.pb.sync_task_order)
  {
    relinked = ApplyTaskOrder(system, s.pb.tasks);
    if (relinked < 0)
      WARN_LOG_FMT(BRAWLBACK, "replay: the recorded task lists differ from this game's; not relinked");
  }
  INFO_LOG_FMT(BRAWLBACK, "replay: barrier passed (ports {:#x}, task lists relinked: {}), "
                          "countdown until game frame {}",
               s.match_ports, relinked, StartFrame());
  s.pb.started = true;
  // A spectator who joined a game well under way catches up to the live edge at once; one there
  // from its start watches it from the start (docs/design/spectating.md §6.2).
  if (s.pb.live && s.pb.catch_up_at_start && s.pb.file->pads.size() > LIVE_JOIN_CATCH_UP_FRAMES)
  {
    s.pb.join_catch_up = true;
    s.pb.catch_up = true;
    s.pb.joining = true;
    s.pb.catch_frames = 0;
  }
  s.phase = Phase::Countdown;
  system.GetCoreTiming().ResetThrottleToNow();
  PadsSetCurrent(0xF, CountdownSlots());
  PadsOnLoopTop(system);
  SetLoopDefault(ppc);
  return true;
}

// The barrier's setup hashes differ: which fields, from the key the peer sent with its hash (bug
// report #40, where only the two hashes were logged and the other machine's logs were not to be
// had). Both machines log it, each against the other's key.
static void LogSetupDifference(int slot, const PeerView& p)
{
  if (p.setup_key.empty())
  {
    ERROR_LOG_FMT(BRAWLBACK, "gprb: P{} sent no setup key (an older build): our setup {}", slot + 1,
                  Hex(s.setup_key));
    return;
  }
  const u64 theirs = HashSetupKey(p.setup_key, p.setup_start.value_or(0),
                                  static_cast<u8>(p.setup_cv.value_or(0)));
  if (theirs != p.setup_hash)
  {
    ERROR_LOG_FMT(BRAWLBACK, "gprb: P{}'s setup key is not the one of its hash {:016x} (an older "
                  "message): ours {}, P{}'s {}",
                  slot + 1, p.setup_hash, Hex(s.setup_key), slot + 1, Hex(p.setup_key));
    return;
  }
  const auto lines = PeerData::SetupKeyDiff(s.setup_key, p.setup_key);
  if (lines.empty())
  {
    ERROR_LOG_FMT(BRAWLBACK,
                  "gprb: P{}'s setup is the same as ours, but its hash covers start frame {} and "
                  "countdown input v{} (ours {}, v{})",
                  slot + 1, p.setup_start.value_or(0), p.setup_cv.value_or(0), s.setup_start,
                  COUNTDOWN_INPUT_VERSION);
    return;
  }
  ERROR_LOG_FMT(BRAWLBACK, "gprb: P{}'s setup differs from ours in: {}", slot + 1,
                fmt::join(lines, "; "));
  INFO_LOG_FMT(BRAWLBACK, "gprb: P{}'s setup: {}", slot + 1, Hex(p.setup_key));
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

  // A replay stopped in its match (StopPlayback): the session ends here; the game then leaves the
  // match (end_quit_left).
  if (s.mode == Mode::Playback && s.pb.stopped && InMatchPhase())
  {
    EndRunning(system, "stopped");
    SetLoopDefault(ppc);
    return true;
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
    // The peer found it first (our own detection logged the calls already): the frame it named,
    // or, from an older peer, the newest frames.
    if (s.desync_frame >= 0)
      LogRngCalls(s.desync_frame, "desync");
    else
      LogRngCalls(s.snd_max_frame, "desync the peer found (frame not known; the newest frames)");
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
    PadsSetCurrent(0xF, CountdownSlots());
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
    if (s.mode == Mode::Playback)
    {
      // The recorded frames from here: no rollback, no GekkoNet, nobody to wait for. The region
      // set is only resolved, for the keyframes of seeking.
      PreparePlaybackKeyframes(system);
      StartRunning(system);
      system.GetCoreTiming().ResetThrottleToNow();
      return RunFrame(guard);
    }
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
      s.wait_logs = 0;
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
      LogWait("at the start barrier", s.barrier_since);
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
    s.holds_valid = false;  // neutral countdown
    s.phase = Phase::Countdown;
    PadsSetCurrent(0xF, CountdownSlots());
    PadsOnLoopTop(system);
    return false;
  }

  if (s.mode == Mode::Playback)
    return PlaybackLoopTop(guard);

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
    s.setup_hash = SetupHash(g, StartFrame());
    s.setup_key = SetupKey(g);
    s.setup_start = StartFrame();
    // Tests (PPR_GPRB_TEST_SETUP_DIFF=<port 1-4>): that player's key has its last byte changed (a
    // Code Menu value), as a machine that built another setup would have.
    if (const char* diff = std::getenv("PPR_GPRB_TEST_SETUP_DIFF");
        diff && std::atoi(diff) == s.local_slot + 1 && !s.setup_key.empty())
    {
      s.setup_key.back() ^= 0xFF;
      s.setup_hash = HashSetupKey(s.setup_key, s.setup_start, COUNTDOWN_INPUT_VERSION);
      WARN_LOG_FMT(BRAWLBACK, "gprb: PPR_GPRB_TEST_SETUP_DIFF: our setup key's last byte changed");
    }
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
    s.quit_addr = 0;
    s.session_addr = Online::GameBridge::SessionAddress();
    s.quit_left = false;
    s.gone_written.fill(0);
    if (s.gone_enabled)
    {
      if (const u32 session = Online::GameBridge::SessionAddress())
      {
        s.gone_addr = session + GONE_FLAG_OFFSET;
        s.quit_addr = session + Online::GameBridge::SESSION_QUIT_PORTS;
      }
      else
        WARN_LOG_FMT(BRAWLBACK, "gprb: the game's PPOM block was not found: no gone flags");
    }
    if (IsHost())
      s.sync = ReadSyncBlock(system);
    s.hold = SampleHold();
    s.holds_valid = false;
    s.at_s = true;
    s.applied = IsHost();
    s.barrier_host = HostSlot();
    s.barrier_since = Clock::now();
    s.wait_logs = 0;
    // A desynced game played again has started: its checksums are no longer needed.
    s.recovery.pending = false;
    ForEachPeer([](int, PeerView& p) { p.recovery.reset(); });
    s.phase = Phase::Barrier;
    SendState();
    INFO_LOG_FMT(BRAWLBACK,
                 "gprb: at the barrier (P{} of ports {:#x}, holding {:#06x}, setup {:016x}: {})",
                 s.local_slot + 1, s.match_ports, s.hold, s.setup_hash, Hex(SetupKey(g)));
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
        LogSetupDifference(slot, p);
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
    // Tests (PPR_GPRB_TEST_GIVE_UP=<port 1-4>): that player's barrier gives up as a failed check
    // would, so the others' reaction can be seen: once every other player is at the barrier too,
    // as a setup check can only fail then (the others wait there when it does).
    bool all_at_s = true;
    ForEachPeer([&](int, const PeerView& p) { all_at_s = all_at_s && (p.left || p.at_s); });
    if (const char* give_up = std::getenv("PPR_GPRB_TEST_GIVE_UP");
        differs.empty() && give_up && std::atoi(give_up) == s.local_slot + 1 && all_at_s)
    {
      differs = "PPR_GPRB_TEST_GIVE_UP";
    }
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
    // Host: every port's countdown buttons, fixed once every player still in the session has
    // sent its hold (a player who left before: neutral). The joiners take these with the sync
    // block, so every machine runs the countdown with the same ones whoever leaves meanwhile.
    if (IsHost() && !s.holds_valid)
    {
      bool all_holds = true;
      std::array<u16, MAX_LOBBY_PLAYERS> holds{};
      holds[s.local_slot] = s.hold;
      ForEachPeer([&](int slot, const PeerView& p) {
        if (p.left)
          return;
        all_holds = all_holds && p.at_s && p.hold_valid;
        holds[slot] = p.hold;
      });
      if (all_holds)
      {
        for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
          s.holds[port] = (s.match_ports >> port) & 1 ? holds[port] & HOLD_BUTTONS : 0;
        s.holds_valid = true;
        INFO_LOG_FMT(BRAWLBACK, "gprb: countdown buttons {:#06x} {:#06x} {:#06x} {:#06x}",
                     s.holds[0], s.holds[1], s.holds[2], s.holds[3]);
        SendState();
      }
    }
    if (hp && !s.applied && hp->at_s && hp->sync.valid && hp->holds_valid &&
        (!s.net_opts.sync_task_order || hp->task_order_valid))
    {
      s.holds = hp->holds;
      for (u16& h : s.holds)
        h &= HOLD_BUTTONS;
      s.holds_valid = true;
      INFO_LOG_FMT(BRAWLBACK,
                   "gprb: countdown buttons {:#06x} {:#06x} {:#06x} {:#06x} (the host's)",
                   s.holds[0], s.holds[1], s.holds[2], s.holds[3]);
      WriteSyncBlock(system, hp->sync);
      int relinked = 0;
      if (s.net_opts.sync_task_order)
        relinked = ApplyTaskOrder(system, hp->task_order);
      INFO_LOG_FMT(BRAWLBACK, "gprb: joiner applied the host's sync block (task lists relinked: {})",
                   relinked);
      s.applied = true;
      SendState();
      double rtt = 0;
      ForEachPeer([&](int, const PeerView& p) {
        if (!p.left)
          rtt = std::max(rtt, std::clamp(p.rtt_ms, 0.0, PeerData::MAX_RTT_MS));
      });
      s.countdown_at = Clock::now() + std::chrono::microseconds(static_cast<s64>(rtt * 500.0));
    }
    // Everyone proceeds into the countdown once every joiner has copied the host's values: the
    // host when it hears so from all; a joiner once it has heard so from every other joiner too
    // (state goes to every peer), and not before the slowest round trip's half after its own
    // apply (the last joiner to apply: the others hear of it about then). So the countdowns
    // (which run without rollback) start about together and the peers reach the start barrier
    // together. Before, a joiner went on RTT/2 after its own apply: with 3-4 players, one that
    // reached the match first ran its countdown ahead of the others and stood at the start
    // barrier for as long (1-2 s in the harness's 4-player sessions).
    if (s.peer_left)
      return OnLoopTop(guard);  // ends the session (everyone else left)
    const bool proceed =
        IsHost() ? all_applied :
                   (s.applied && all_applied && s.countdown_at && Clock::now() >= *s.countdown_at);
    if (proceed)
    {
      s.phase = Phase::Countdown;
      BeginReplay(system);
      ct.ResetThrottleToNow();
      // GekkoNet's handshake runs during the countdown (PumpGekkoInCountdown).
      if (CreateGekko(false, std::popcount(s.match_ports)))
        s.defer_initial_save = true;
      else
        WARN_LOG_FMT(BRAWLBACK, "gprb: gekko_create failed; retried at the start barrier");
      PadsSetCurrent(0xF, CountdownSlots());
      PadsOnLoopTop(system);
      INFO_LOG_FMT(BRAWLBACK, "gprb: barrier passed, countdown until game frame {}", StartFrame());
      SetLoopDefault(ppc);
      return true;
    }
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
    LogWait("at the barrier", s.barrier_since);
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
  if (s.mode == Mode::Playback)
  {
    // As below, with the recorded session's seeds.
    if (s.phase != Phase::Armed || s.await_scene_exit)
      return;
    auto& system = guard.GetSystem();
    if (!s.did_frame0)
    {
      ApplyPlaybackStart(system, "at the stage's construction");
      return;
    }
    const auto seeds = MatchSeeds(s.pb.seed, s.pb.match_index);
    WriteU32(system, Addr::MTRAND_DEFAULT_SEED, seeds[0]);
    WriteU32(system, Addr::MTRAND_OTHER_SEED, seeds[1]);
    WriteU32(system, Addr::LIBC_RAND_NEXT, seeds[2]);
    INFO_LOG_FMT(BRAWLBACK, "replay: match RNG seeded again at the stage's construction");
    return;
  }
  if (s.mode != Mode::Network)
    return;
  auto& system = guard.GetSystem();
  // The online waiting room's stage (the player waits there while matchmaking connects): not
  // the match's. Its init block must not be taken as the match's, nor its RNG seeded.
  if (IsWaitingRoom(Guest(system.GetMemory())))
  {
    INFO_LOG_FMT(BRAWLBACK, "gprb: the waiting room's stage constructed (not a match)");
    return;
  }
  INFO_LOG_FMT(BRAWLBACK, "gprb: stage constructed (match {}, seeded {})", s.match_index, s.did_frame0);
  if (s.phase != Phase::Connected || s.await_scene_exit || !(IsHost() || s.have_seed))
    return;
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

LinkHealth GetLinkHealth()
{
  LinkHealth h;
  h.running = s_health_running.load(std::memory_order_relaxed);
  h.late_frames = s_health_late.load(std::memory_order_relaxed);
  h.rollback_avg = s_health_rollback.load(std::memory_order_relaxed);
  h.ping_ms = s_health_ping.load(std::memory_order_relaxed);
  return h;
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
// A mispredicted run's sound fades out over this many sound frames (game frames) when it is
// stopped: Stop(0) cut it off mid-waveform, which clicks.
constexpr u32 SND_STOP_FADE_FRAMES = 3;
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

// The sound system is outside the set, so it is not rolled back: a resimulated pass pauses it
// (CoreTimingManager::SetRollbackAudioWaits). PPR_GPRB_RESIM_AUDIO=advance lets it run through the
// pass as before (its samples are dropped, so every rollback cut the re-run's span out of every
// playing sound and the music).
bool PauseResimAudio()
{
  static const bool advance = [] {
    const char* env = std::getenv("PPR_GPRB_RESIM_AUDIO");
    return env && std::strcmp(env, "advance") == 0;
  }();
  return !advance;
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

// Diagnostics (PPR_GPRB_FORCE_FINAL_AT=<session frame>:<port>[:first],...): before a pass of that
// session frame (after its load, with the previous frame saved), give the port its Final Smash as
// breaking a Smash Ball does (ftManager::setFinal, as RunForceFinal). With "first", only in the
// frame's first run: a rollback then discards it, as a Smash Ball broken by a mispredicted run.
// The call returns to the loop top, and RunFrame comes back for the same pass. Returns true while
// calling.
bool RunForceFinalAt(const Core::CPUThreadGuard& guard, s64 frame, bool first_run)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  for (const auto& e : s.force_final_at)
  {
    if (e.frame != frame || (e.first_only && !first_run))
      continue;
    if (ReadGuest(guard, FT_MANAGER_SET_FINAL) != 0x9421FFE0)
    {
      WARN_LOG_FMT(BRAWLBACK, "gprb: force final at: ftManager::setFinal not found");
      return false;
    }
    const u32 entries = ReadGuest(guard, FT_ENTRY_MANAGER);
    const u32 count = std::min<u32>(ReadGuest(guard, FT_ENTRY_MANAGER + 4), 16);
    u32 entry_id = 0xFFFFFFFF;
    for (u32 i = 0; i < count; ++i)
    {
      const u32 en = entries + i * FT_ENTRY_SIZE;
      if (ReadGuest(guard, en + 0x28) != 0 && ReadGuest(guard, en + 0x58) == e.port)
        entry_id = ReadGuest(guard, en + 4);
    }
    if (entry_id == 0xFFFFFFFF)
      return false;
    INFO_LOG_FMT(BRAWLBACK, "gprb: force final at session frame {} ({} run): port {} entry {:#x}",
                 frame, first_run ? "first" : "a later", e.port, entry_id);
    SaveGuestRegs(ppc, &s.force_final_at_saved);
    s.force_final_at_active = true;
    ppc.gpr[3] = FT_MANAGER;
    ppc.gpr[4] = entry_id;
    ppc.gpr[5] = 0;
    ppc.spr[SPR_LR] = LOOP_TOP;
    ppc.npc = FT_MANAGER_SET_FINAL;
    return true;
  }
  return false;
}

// Loop top: stop the queued sounds of mispredicted runs, one guest call of
// BasicSound::Stop(SND_STOP_FADE_FRAMES) each. The call returns to the loop top, where this runs
// again, restores the registers and goes on. Returns true while a call is being made (the hook
// must not do anything else).
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
    ppc.gpr[4] = SND_STOP_FADE_FRAMES;
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

bool OnSoundStop(const Core::CPUThreadGuard&, u32 sound)
{
  // Not for the bookkeeping's own stops (RunSoundStops, between passes).
  if (s.phase != Phase::Running || !s.snd_pass_open || s.snd_first_run || s.gcall_active ||
      !DedupeSounds())
  {
    return false;
  }
  const auto it = s.snd_gen.find(sound);
  if (it == s.snd_gen.end())
    return false;
  const auto same = [&](const State::SndEntry& e) { return e.sound == sound && e.gen == it->second; };
  bool ahead = std::any_of(s.snd_remaining.begin(), s.snd_remaining.end(), same);
  for (size_t slot = 0; !ahead && slot < s.snd_played.size(); ++slot)
  {
    if (s.snd_tag[slot] > s.snd_frame && s.snd_tag[slot] <= s.snd_max_frame)
      ahead = std::any_of(s.snd_played[slot].begin(), s.snd_played[slot].end(), same);
  }
  if (ahead)
    ++s.sound_stops_ignored;
  return ahead;
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
  if (s.phase == Phase::Idle)
    return;
  if (s.phase == Phase::Running && s.snd_pass_open && s.snd_frame >= 0)
  {
    State::RngFrame& rf = s.rng_frames[static_cast<size_t>(s.snd_frame) % s.rng_frames.size()];
    if (rf.frame == s.snd_frame)
    {
      if (rf.count < rf.calls.size())
        rf.calls[rf.count] = {rng, state,
                              {callers[0], callers[1], callers[2], callers[3], callers[4], callers[5]}};
      ++rf.count;
    }
  }
  // PPR_GPRB_RNG_LOG (diagnostics): every call of every phase of a match (the load and the
  // countdown included).
  static const bool verbose = std::getenv("PPR_GPRB_RNG_LOG") != nullptr;
  if (!verbose)
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
