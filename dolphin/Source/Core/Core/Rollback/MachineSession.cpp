// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/MachineSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#include <enet/enet.h>
#if defined(_M_X86_64)
#include <xmmintrin.h>
#endif
#include <fmt/format.h>
#include <mbedtls/sha256.h>
#include <zstd.h>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/Brawlback/include/brawlback-common/BrawlbackConstants.h"
#include "Core/Brawlback/include/gekkonet/GekkoLib/include/gekkonet.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/WII_IPC.h"
#include "Common/ChunkFile.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/CommandProcessor.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/SDIO/SDCardStorage.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/Harness/Harness.h"
#include "Core/NetPlayProto.h"
#include "Core/Online/Chat.h"
#include "Core/Online/ModsRoom.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/FrameProfile.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/MachineSDCard.h"
#include "Core/Rollback/PresentStats.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoState.h"

namespace HLE_Misc
{
bool IsBootScene(const Core::CPUThreadGuard& guard, u32 app_ptr);
}

namespace Machine
{
Boundary SessionBoundary();
namespace
{
using Clock = std::chrono::steady_clock;
// A rollback's load and re-runs per frame back (State::burst_ms_per_frame): the estimate before
// the session has measured one (a re-run frame of a REX room took about 3.4 ms, its load 1.2,
// docs/design/brawl-mods.md 6.4), and the weight of each new rollback in the average.
constexpr double BURST_MS_PER_FRAME_START = 5.0;
constexpr double BURST_AVERAGE_WEIGHT = 0.25;

// The Brawl game loop (main.dol; the same in every Brawl build).
constexpr u32 BRAWL_LOOP_TOP = 0x800171b4;      // li r25,1
constexpr u32 BRAWL_FRAME_END = 0x80017504;     // stw r0,0x100(r23)
constexpr u32 BRAWL_LOOP_END = 0x80017508;      // b loop top
constexpr u32 BRAWL_LOOP_TOP_OPCODE = 0x3B200001;

// ENet channels of the session's links.
constexpr u8 CHANNEL_GEKKO = 0;    // GekkoNet's packets (unsequenced)
constexpr u8 CHANNEL_CONTROL = 1;  // small JSON messages (reliable)
constexpr u8 CHANNEL_IMAGE = 2;    // a machine image for a joiner (reliable, in chunks)
constexpr u8 CHANNEL_SD = 3;       // the session's SD card: chunk requests and chunks (reliable)
constexpr u8 CHANNEL_COUNT = 4;
// A CHANNEL_SD packet: one of these, then a chunk request or a chunk, or (a joiner before it asks
// to join) a request for the session card's manifest and file list, and those.
constexpr u8 SD_REQUEST = 0;
constexpr u8 SD_CHUNK = 1;
constexpr u8 SD_MANIFEST_REQUEST = 2;
constexpr u8 SD_MANIFEST = 3;
// A slice of a larger CHANNEL_SD packet ([SD_PIECE][last: 0 or 1][bytes]): PaceSD sends a chunk (64
// KiB) or a manifest as slices of SD_PIECE_BYTES, each at the pace, so a game packet waits behind
// at most one slice on a slow uplink (a whole chunk is 64 ms at 8 Mbit/s). The channel is
// sequenced: a sender's slices arrive in order, and its packets are never interleaved.
constexpr u8 SD_PIECE = 4;
constexpr size_t SD_PIECE_BYTES = 4096;
constexpr size_t MAX_SD_PACKET = 2 * SDCard::CHUNK_SIZE;
// A manifest (17 bytes a chunk of a card up to 64 GiB, compressed) and its file list.
constexpr size_t MAX_SD_MANIFEST_PACKET = 64u << 20;
// The SD card's upload to a joiner (PaceSD): a rate that grows while nothing queues on the way and
// shrinks as soon as the joiner's link or the game's links to the other players show a queue
// (their round trip above their lowest), so a download never costs the game its link.
constexpr double SD_RATE_START = 256.0 * 1024;  // bytes a second
constexpr double SD_RATE_MIN = 32.0 * 1024;
constexpr double SD_RATE_MAX = 64.0 * 1024 * 1024;
constexpr auto SD_PACE_STEP = std::chrono::milliseconds(100);
// The queueing (ms over a link's lowest round trip) the upload aims to leave on the way, a little
// over what ENet's own timing shows with nothing queued (3-4 ms, the upload sent in slices).
// Three times it halves the rate at once.
constexpr double SD_TARGET_DELAY_MS = 6;
// A link whose round trip changes by more than SD_JITTER_MS from one send to the next beyond a
// trend (on average, OnAck) is jittery until it is back under SD_JITTER_CLEAR_MS, and its queue
// is the lowest of its LinkQueueMs over SD_JITTER_WINDOW (SteadyQueueMs). Through netsim: 2-4 ms
// on a clean link (9 at most), 8-14 with normal jitter of 10 ms each way, 30-45 with pareto 25.
constexpr double SD_JITTER_MS = 8;
constexpr double SD_JITTER_CLEAR_MS = 5;
constexpr auto SD_JITTER_WINDOW = std::chrono::seconds(3);
// On a link without jitter, the window of round trips whose lowest is its delay now for PaceSD.
constexpr auto SD_CLEAN_DELAY_WINDOW = std::chrono::milliseconds(100);
// After slowing down, the delays it looks at need this long to show the change.
constexpr auto SD_SLOWDOWN_HOLD = std::chrono::milliseconds(800);
// The window of round trips whose lowest is a link's delay now.
constexpr auto SD_DELAY_WINDOW = std::chrono::milliseconds(500);
// What a hello says this build speaks: 2 since the SD card is synced before the join and its
// chunks are hashed with BLAKE2b (a build before says nothing: 1), 3 since a Brawl session's
// checksums leave out the disc revision (BRAWL_DISC_REVISION), 4 since a machine image carries
// what only drawing uses (image version 4). Players of different versions refuse each other's
// joins.
constexpr int PROTOCOL_VERSION = 4;
// The disc revision a Brawl disc leaves in RAM: the only bytes a Rev 1 and a Rev 2 disc leave
// different (both booted to the same frame and diffed, docs/design/disc-images.md): the disc ID's
// revision in low memory, in the DVD library's copy and in the game's, and the operand of
// main.dol's revision check. A Brawl session's checksums read them as zero, so Rev 1 and Rev 2
// players share a room (a joiner takes the room's with the machine image).
const std::vector<Rollback::MemoryRegion> BRAWL_DISC_REVISION = {
    Rollback::MemoryRegion::FromVirt(0x80000007, 1, "disc ID revision"),
    Rollback::MemoryRegion::FromVirt(0x8001BC9F, 1, "main.dol revision check"),
    Rollback::MemoryRegion::FromVirt(0x8049493F, 1, "DVD library disc ID revision"),
    Rollback::MemoryRegion::FromVirt(0x804E4585, 1, "game's disc ID revision"),
};
// The chunks the game has read that a machine image lists for the joiner to fetch first.
constexpr size_t MAX_SD_HOT_CHUNKS = 32768;
constexpr size_t MAX_CONTROL_SIZE = 4096;
// GekkoNet's largest packet is a disconnect claim: a dropped player's whole input queue (128
// inputs of INPUT_SIZE, 6 KiB; ENet fragments it).
constexpr size_t MAX_GEKKO_PACKET = 16384;
constexpr int MAX_ADVANCE = MAX_ROLLBACK_FRAMES + 2;
constexpr size_t HISTORY_SIZE = 1 << 14;
// An epoch (a change of who plays) starts this many frames after the frame whose input first
// carries its request: more than GekkoNet's prediction window, so every machine has run that
// frame with its real input before it can get to the epoch, and all of them stop at the same one.
constexpr s64 EPOCH_LEAD = MAX_ROLLBACK_FRAMES + 3;
constexpr int NO_DELAY_REQUEST = -100;  // State::requested_delay
// How long the players wait at an epoch for the image to reach a joiner and load there.
constexpr auto EPOCH_TIMEOUT = std::chrono::seconds(120);
// A joiner gives up after this long without being admitted.
constexpr auto JOIN_TIMEOUT = std::chrono::seconds(300);
// Machine images: sent in chunks of IMAGE_CHUNK; a received one is at most MAX_IMAGE_PACKED
// compressed and MAX_IMAGE_RAW decompressed (MEM1 + MEM2 + device state + the NAND).
constexpr size_t IMAGE_CHUNK = 256 * 1024;
constexpr size_t MAX_IMAGE_PACKED = 256u << 20;
constexpr size_t MAX_IMAGE_RAW = 768u << 20;
constexpr u32 IMAGE_MAGIC = 0x424D4931;  // "BMI1"
// 4: what only drawing uses (ImageParts::copies: EFB copies, TEV colours), after the SD card's
// chunks.
constexpr u32 IMAGE_VERSION = 4;
// The reliable data ENet keeps unacknowledged on a link. Its own is 64 KB (unless the hosts set
// bandwidths), so a machine image or SD card chunks went at 64 KB per round trip: 1.3 MB/s at
// 50 ms, a 22.5 MB image in 23 s with the whole room waiting (prod 2026-10-14), and a joiner's
// chunk waiting behind the chunks asked for before it. ENet uses a peer's window only to send, so
// each machine sets its own and a build without this still plays with one that has it.
// The window is this machine's congestion window for the peer (PaceSendWindow), at most
// SEND_WINDOW: ENet has no congestion control of reliable data of its own, and 1 MiB at once
// overflows a home uplink's buffer (prod 2026-10-14, the user's REX room: IMPY#5's 18.2 MB image
// stalled 13 s at 10 %, then ENet gave up on the link at 70 %; "iposbltngs could not join").
constexpr enet_uint32 SEND_WINDOW = 1u << 20;
constexpr enet_uint32 SEND_WINDOW_START = 64 * 1024;  // ENet's own window, safe on any link
constexpr enet_uint32 SEND_WINDOW_MIN = 32 * 1024;
constexpr enet_uint32 SEND_WINDOW_STEP = 16 * 1024;  // added each round trip after a loss
// Resent in a round trip, taken for congestion with no queue: with 5 % random loss each way ENet
// resends about 10 % (lost acknowledgements too), and a rate limit overrun loses far more.
constexpr enet_uint32 SEND_LOSS_PERCENT = 25;
// GekkoNet drops a peer it has not heard from for this long.
constexpr unsigned int DISCONNECT_TIMEOUT_MS = 5000;

enum class Phase
{
  Idle,       // nothing armed
  Armed,      // armed, the game has not reached its first boundary
  Connecting, // at the first boundary, waiting for every peer
  Starting,   // GekkoNet runs its handshake / the start barrier
  Running,
  Epoch,      // at an epoch's frame: the players agree on it, a joiner gets the machine
  Joining,    // a joiner: waits to be admitted and for the machine image
  Stopped,
  Error,
};

const char* PhaseName(Phase p)
{
  switch (p)
  {
  case Phase::Idle: return "idle";
  case Phase::Armed: return "armed";
  case Phase::Connecting: return "connecting";
  case Phase::Starting: return "starting";
  case Phase::Running: return "running";
  case Phase::Epoch: return "epoch";
  case Phase::Joining: return "joining";
  case Phase::Stopped: return "stopped";
  case Phase::Error: return "error";
  }
  return "?";
}

struct PeerLink
{
  int slot = -1;
  std::string host;
  u16 port = 0;
  ENetPeer* peer = nullptr;  // the connection kept for this slot
  bool hello = false;        // its hello arrived on `peer`
  int proto = 1;             // its PROTOCOL_VERSION (a hello without one: 1)
  // ENet's round trip on this link, a sample every 10 ms (net thread), for PaceSD.
  std::deque<std::pair<Clock::time_point, enet_uint32>> rtt_window;
  // Its game has an SD card, and the session card's root it plays (RootHex; "" until it has
  // built or synced the card).
  bool sd_card = false;
  std::string sd_root;
  int started = -1;          // the epoch whose GekkoNet session it started (start barrier)
  int go = -1;               // the epoch whose start barrier it said go for
  bool left = false;
  bool late = false;         // not a player when the session started (it joins)
  bool join_asked = false;   // it asked to join (a joiner)
  Controller join_controller = Controller::GameCube;
  int epoch_at = -1;         // the epoch whose frame it reached, with its machine's checksum
  u32 epoch_sum = 0;
  // A player's digest of the machine image at that epoch (for a joiner: it loads an image only if
  // every player's digest matches it).
  std::string epoch_digest;
  bool dropped = false;  // GekkoNet was told (DropLeftPeers)
  Clock::time_point last_connect{};
  ENetPeer* connecting = nullptr;  // this side's latest connection attempt
  double rtt_ms = -1;
  double min_rtt_ms = -1;  // the lowest rtt_ms of this connection (PickInputDelay)
  // A room member (SetRoom): who is in this slot, and the addresses its hello may come from.
  std::string code;
  std::string name;
  std::vector<enet_uint32> ips;
  // While a room member has no link (LogNoLink): since when, this side's connection attempts, its
  // hellos on a connection this side does not keep (only the one the lower slot started is kept),
  // and the lines logged.
  Clock::time_point link_wanted{};
  int attempts = 0;
  int unkept_hellos = 0;
  int no_link_logs = 0;
  // A joiner: a member already in the room when it came (a player, or an earlier joiner), whose
  // link it waits for (JoinNoLinkStep), and whether it told its player.
  bool no_link_watch = false;
  bool no_link_shown = false;
  // A joiner: how much of its machine image it has (tenths), for the image of `image_epoch`.
  int image_epoch = -1;
  int image_tenths = -1;
  // Someone new has this player's slot in the room: `left` is the previous player's until
  // GekkoNet unplugged it.
  bool reseated = false;
  // The congestion window of what this machine sends `peer` reliably (PaceSendWindow).
  struct
  {
    ENetPeer* peer = nullptr;  // the connection it is for
    enet_uint32 window = SEND_WINDOW_START;
    bool slow_start = true;
    // In this round trip: reliable data waited for the window; what ENet sent and resent.
    bool limited = false;
    enet_uint32 sent = 0, lost = 0;
    enet_uint32 seen_sent = 0, seen_lost = 0;  // ENet's counters when last seen
    enet_uint32 min_rtt = 0xffffffff;  // the lowest of ENet's mean round trip (ms)
    Clock::time_point step{};
    enet_uint32 step_enet = 0;  // ENet's clock at `step`
    // From the peer's acknowledgements, read before ENet reads them (OnAck): the latest send
    // (ENet's clock) an acknowledgement echoed and that send's round trip (RACK), the highest round
    // trip of each of the last four half seconds (the retransmission timeout's floor), the link's
    // jitter (SteadyQueueMs) and this round trip's resends the peer showed were not needed.
    bool rack_valid = false;
    enet_uint32 rack_sent = 0, rack_rtt = 0;
    std::array<enet_uint32, 4> rtt_high{};
    enet_uint32 rtt_high_at = 0;  // ENet's clock when rtt_high[0] began
    // The jitter (ms), as RTP's interarrival jitter (RFC 3550) less a trend: the mean change of
    // the round trip from one send to one at least JITTER_SPACING_MS later, past the mean change
    // (`trend`: a queue filling or draining moves it steadily).
    double jitter = 0, trend = 0;
    bool jittery = false;  // past SD_JITTER_MS, until back under SD_JITTER_CLEAR_MS
    s32 jitter_rtt = -1;
    enet_uint32 jitter_sent = 0;
    enet_uint32 spurious = 0;
    // The last cut, while the losses that made it may still prove spurious: the window and slow
    // start before it (undo_window 0: none), its losses not shown spurious yet, and until when.
    enet_uint32 undo_window = 0, undo_lost = 0;
    bool undo_slow_start = false;
    Clock::time_point undo_until{};
    enet_uint32 undone = 0, rack_resent = 0;  // diagnostics: cuts undone, RACK's resends
    // LinkQueueMs every 100 ms over the last SD_JITTER_WINDOW (NetThread; SteadyQueueMs).
    std::deque<std::pair<Clock::time_point, double>> queue_readings;
  } cc;
};

struct PendingOps
{
  int count = 0;
  std::array<s32, MAX_ADVANCE> frame{};
  std::array<bool, MAX_ADVANCE> save_after{};
  std::array<unsigned int*, MAX_ADVANCE> checksum{};
  std::array<std::array<Input, MAX_PLAYERS>, MAX_ADVANCE> inputs{};
  void Clear()
  {
    count = 0;
    save_after.fill(false);
    checksum.fill(nullptr);
  }
};

struct HistoryEntry
{
  s64 frame = -1;
  std::array<Input, MAX_PLAYERS> inputs{};
  u32 checksum = 0;
  u64 ticks = 0;  // the emulated time this frame's last pass started at
};

struct State
{
  std::recursive_timed_mutex mutex;
  Options opts;
  std::atomic<Phase> phase{Phase::Idle};
  std::string error;
  bool env_checked = false;
  // The player's controller is a GameCube controller adapter (their own SI device setting).
  bool local_adapter = false;
  std::array<Controller, MAX_PLAYERS> controller{};
  u32 wiimote_captures = 0;
  std::optional<WiimoteOverride> wiimote_override;
  // The local controller (CPU thread): a switch the launcher asked for (a Controller, or -1), and
  // the controller picked (opts.auto_pad) every PICK_EVERY captures. `pick` is copied out under
  // `pick_mutex`. A real Wii Remote drives `real_wiimote`, an emulated Wii Remote with its
  // controls (the session reads and sends an emulated Wii Remote's state).
  std::atomic<int> requested_controller{-1};
  u32 pick_countdown = 0;
  s64 pick_frame = -1;  // the frame the countdown last counted
  std::mutex pick_mutex;
  LocalControllerInfo pick;
  bool use_real_wiimote = false;
  bool picked = false;  // `pick` is the CPU thread's (it captured once)
  std::unique_ptr<WiimoteEmu::Wiimote> real_wiimote;
  std::string real_wiimote_device;
  // A GameCube controller's analog input, every INPUT_LOG_EVERY frames in the log (CPU thread):
  // how often the C-stick was pushed, in the controller's reads while waiting for a frame and in
  // the samples GekkoNet took (one a frame), and the longest time between two samples.
  struct
  {
    s64 frame = -1;
    Clock::time_point at{};
    u32 frames = 0, raw_pushes = 0, sampled_pushes = 0;
    bool raw_pushed = false, sampled_pushed = false;
    double max_gap_ms = 0;
    int peak = 0;
  } input_log;

  // Network (net thread, under `net_mutex`).
  std::mutex net_mutex;
  ENetHost* host = nullptr;
  std::thread net_thread;
  std::atomic<bool> net_run{false};
  std::array<PeerLink, MAX_PLAYERS> peers;  // by slot (this player's unused)

  // At exit without Stop (a session never started, an error, the process ending): a joinable
  // thread's destructor calls std::terminate ("libc++abi: terminating", a crash report on every
  // exit on macOS). Joining here could wait on statics already gone: the thread is told to stop
  // and left to the process's end.
  ~State()
  {
    net_run = false;
    if (net_thread.joinable())
      net_thread.detach();
    // Its destructor unregisters from Dolphin's config, which may be gone by now: left to the
    // process's end.
    (void)real_wiimote.release();
    // Its thread waits for requests: left to the process's end as well.
    (void)sd_server.release();
  }
  std::deque<std::pair<int, std::vector<u8>>> gekko_rx;  // (slot, packet)
  std::vector<GekkoNetResult*> gekko_rx_ptrs;
  std::vector<std::pair<int, std::vector<u8>>> gekko_tx;  // (slot, packet)
  // The decider's epoch_go (the epoch's players), from the net thread.
  struct EpochGo
  {
    int epoch = -1;
    s64 frame = -1;
    u32 players = 0;
    std::array<Controller, MAX_PLAYERS> controllers{};
    // Two slots that swap players at this epoch ({-1, -1}: none). `players` and `controllers`
    // are already as after the swap.
    std::pair<int, int> move{-1, -1};
  };
  std::optional<EpochGo> epoch_go;
  bool join_refused = false;
  std::string join_refused_why;
  // A room session (Options::room): the room's members arrived (SetRoom).
  bool room_known = false;
  // The network chosen in the launcher whose members are reached at their LAN address, from the
  // room's first state (SetRoom): none when it is the home LAN (Config::GetChosenNetwork).
  Config::ChosenNetwork room_network;
  // The endpoint probe to mm (ProbeEndpoint), on the session's socket.
  struct Probe
  {
    std::string host;
    u16 port = 0;
    std::string token;
    std::string state;
    ENetPeer* peer = nullptr;
    Clock::time_point last{};
    int attempts = 0;
  } probe;
  // A machine image on its way to this joiner (net thread): header, then the compressed bytes.
  struct ImageRx
  {
    int from = -1;
    int epoch = -1;
    s64 frame = -1;
    u32 sum = 0;
    u64 packed_size = 0, raw_size = 0;
    std::vector<u8> data;
    bool complete = false;
    Clock::time_point started{};
  } image_rx;
  // A machine image this player sends (net thread: to `image_to`, in chunks).
  std::vector<u8> image_tx;
  int image_to = -1;
  size_t image_tx_sent = 0;

  // The session's SD card (docs/design/brawl-mods.md §6.5), under `net_mutex`: this machine's card
  // when it plays the session's (the starter's own, or a joiner's from its image on), the
  // manifest's wire form (in machine images) and canonical form (in their digest), the server
  // answering the other players' requests for its chunks, and what goes out on CHANNEL_SD.
  // A joiner asks the player that sent its image first, and the others for chunks that player
  // has not got (`sd_tried`: the slots that said so, as bits; `sd_asked`: whom it asked last).
  std::shared_ptr<SDCard::SessionCard> sd_card;
  std::vector<u8> sd_manifest_wire, sd_manifest_canon;
  std::unique_ptr<SDCard::CardServer> sd_server;
  std::deque<std::pair<int, std::vector<u8>>> sd_tx;
  std::map<u32, u8> sd_tried;
  std::map<u32, int> sd_asked;
  int sd_source = -1;
  // The SD card's upload to each joiner, by its slot (PaceSD).
  struct SDPace
  {
    std::deque<std::vector<u8>> queue;  // CHANNEL_SD packets waiting for the pace
    size_t front_sent = 0;              // bytes of the front packet sent as slices
    double rate = SD_RATE_START;
    double tokens = 0;
    bool limited = false;  // something waited for the rate since the last step
    Clock::time_point filled{}, stepped{}, slowed{}, last_send{}, started{};
    // The rate a queue last showed at (0: none yet): fast up to most of it, then a slow probe.
    double ceiling = 0;
    u64 sent = 0;
    u32 slowdowns = 0;
  };
  std::array<SDPace, MAX_PLAYERS> sd_pace;
  // CHANNEL_SD slices from each player, until the last of a packet (net thread).
  std::array<std::vector<u8>, MAX_PLAYERS> sd_rx_pieces;
  // What each player sent this joiner of the session's card lately (when, bytes).
  std::array<std::deque<std::pair<Clock::time_point, u64>>, MAX_PLAYERS> sd_from;
  // The session card's root (RootHex, for hellos) and file list (wire form, sent with the
  // manifest), while this machine plays the session's card.
  std::string sd_root;
  std::vector<u8> sd_files_wire;
  // A joiner's own card (CPU thread), compared with the session's card before the join.
  std::string sd_path;

  // A joiner's sync of the session's SD card before it asks to join (SDSyncStep; CPU thread,
  // under `mutex`, except as marked).
  struct SDSync
  {
    enum class Step
    {
      Idle,         // nothing done yet
      Checking,     // its own card hashing, the manifest asked for
      Asking,       // its player decides (the launcher's dialog)
      Downloading,  // the chunks it has not got coming in
      Ready,        // it has the whole card (or the session has none)
      Failed,
    } step = Step::Idle;
    // Its own card's hashes (a thread of their own; `own` under `own_mutex`).
    std::thread hash_thread;
    std::atomic<bool> hash_cancel{false};
    std::atomic<bool> hash_finished{false};
    std::atomic<u64> hash_done{0}, hash_total{0};
    std::mutex own_mutex;
    std::optional<SDCard::ImageHashes> own;
    // The manifest asked for (under `net_mutex`: the net thread fills it in).
    int from = -1;
    Clock::time_point asked{};
    bool received = false;
    std::vector<u8> packet;
    double packet_ms = 0;
    // The session's card, compared with this one.
    std::shared_ptr<SDCard::SessionCard> card;
    std::vector<u8> files_wire;
    u64 missing_bytes = 0;
    u32 missing_files = 0;
    std::vector<std::string> names;  // the differing files, largest first (some)
    double rate = 0;                 // bytes a second the link gave the manifest
    std::atomic<int> choice{static_cast<int>(SDSyncChoice::Ask)};
    bool chose = false;
    int tenths_shown = -1;
    Clock::time_point download_since{};
    // The download's pace: bytes done at times (the last few seconds), and the rate from them.
    std::deque<std::pair<Clock::time_point, u64>> progress;
    double rate_now = 0;
  } sd_sync;

  // GekkoNet and the frame loop (CPU thread).
  GekkoSession* gekko = nullptr;
  GekkoNetAdapter adapter{};
  bool gekko_started = false;
  bool barrier_done = false;
  std::optional<Clock::time_point> start_at;
  Clock::time_point last_barrier_send{};
  std::array<int, MAX_PLAYERS> handle_slot{-1, -1, -1, -1};
  std::array<int, MAX_PLAYERS> slot_handle{-1, -1, -1, -1};
  int local_handle = -1;
  // A bit per slot that plays (the net thread reads it: a hello's `playing`, a joiner's request).
  std::atomic<u32> players{0};
  // Players GekkoNet unplugged since this epoch started (DropLeftPeers): their slot is free for a
  // joiner although they are still in `players` until the next epoch.
  std::atomic<u32> gone{0};
  PendingOps ops;
  int iteration = 0;
  bool resim = false;
  // The boundary this session runs at (Auto until the first boundary resolves it).
  Boundary boundary = Boundary::Auto;
  // The VI-field boundary: a pass runs from BeginPass to the next field; vi_frame_ended says the
  // frame-end half of a boundary ran and its frame-start half has to be taken again (it gave the
  // CPU back while it waited).
  bool pass_open = false;
  bool vi_frame_ended = false;
  struct
  {
    bool pending = false;
    s64 frame = 0;
    unsigned int* checksum = nullptr;
  } queued_save;
  std::array<Input, MAX_PLAYERS> latched{};  // the inputs of the frame running now
  Input local_input{};
  // The local controller played the boot (LatchBootInput): the session starts with it plugged in.
  bool boot_local = false;
  s64 current_frame = -1;
  // When UpdatePingDisplay shows the ping next (a second after GekkoNet starts: it has no round
  // trip before its first health messages, and would say "Ping: 0").
  Clock::time_point next_ping_display{};
  // GekkoNet's sessions start at frame 0; a session after an epoch continues the global frame
  // count from the epoch's frame.
  s64 frame_base = 0;
  int epoch = 0;
  // The next epoch, from a request in some player's input (PlanEpoch).
  struct Plan
  {
    bool active = false;
    s64 request_frame = -1;  // the frame whose input first carried the request
    s64 frame = -1;          // the epoch's frame (request_frame + EPOCH_LEAD): nobody runs it yet
    int from = -1;           // the player who asked (decides the epoch)
    int admit = -1;          // the slot it admits
    Controller controller = Controller::GameCube;
    std::pair<int, int> move{-1, -1};  // or the two slots that swap players
  } plan;
  // This player asks to admit a joiner (its input carries the request until it is planned).
  std::optional<std::pair<int, Controller>> local_request;
  // This player (the room's host) asks two slots to swap players (RequestMove; its input carries
  // it until it is planned), and the move it asked for that took effect (for the room).
  std::optional<std::pair<int, int>> local_move;
  std::optional<std::pair<int, int>> move_done;
  // At an epoch.
  Clock::time_point epoch_since{};
  u32 epoch_sum = 0;
  std::string epoch_digest;  // of this machine's image at the epoch (when it admits a joiner)
  bool image_built = false;
  double image_ms = 0;
  u64 image_packed = 0, image_raw = 0;
  int image_tenths_shown = -1;  // the joiner's progress last shown in chat
  // A joiner.
  bool join_sent = false;
  bool join_loaded = false;
  int join_tenths_shown = -1;  // -1: the image's header not shown yet; then tenths received
  Clock::time_point join_since{};
  double speed = 1.0;
  s64 last_timesync_frame = -1;

  // Stats.
  u64 rollbacks = 0, max_rollback = 0, frames_resimulated = 0, stall_polls = 0, skipped_loads = 0;
  u64 desyncs = 0, saves = 0;
  s64 last_desync_frame = -1;
  // The rollback just loaded (the pass after it must start when that frame's last run started).
  std::optional<std::pair<int, int>> loaded;  // frames back, the ring's count
  // A rollback's time from its load to the start of the pass after its re-runs, per frame it
  // went back (an average): the boundary sleep ends that much early when one is pending.
  Clock::time_point burst_start{};
  int burst_frames = 0;
  double burst_ms_per_frame = BURST_MS_PER_FRAME_START;
  u64 load_mismatches = 0;
  Clock::time_point desync_notice_at{};  // the last desync the players were told of
  float frames_ahead = 0;
  int input_delay = 0;  // this player's, in the current GekkoNet session
  // Input latency trace (PPR_MACHINE_INPUT_LAT_LOG): per frame, when its first run began, when
  // the local controller behind its local input was read, and the game's first reads of it.
  struct LatFrame
  {
    s64 frame = -1;
    Clock::time_point begin{}, first_si{}, first_bt{};
    int si_n = 0, bt_n = 0;
    s64 read_frame = -1;
    Clock::time_point read{};
  };
  std::array<LatFrame, 64> lat{};
  Clock::time_point local_read_at{};
  // Late input (off; PPR_MACHINE_LATE_INPUT=1: on). The game reads a frame's inputs at its own
  // SI polls and Wii Remote reports, and the throttle's one sleep a frame often comes between the
  // frame's start and those reads, so the local controller read at the boundary was up to a field
  // old when the game took it. Now: the local controller is read when the game first reads the
  // local player's device in a frame (LateLocalInput), for the frame `delay` later, and sent at
  // once (GekkoNet's delay is one less, so the boundary's own add, the fallback, gives the same
  // frame); and a remote player's input that arrives before the game first read their device in
  // the frame it is for replaces the prediction there, without a rollback (LateRemoteInput).
  bool late_input = false;
  std::array<u8, MAX_PLAYERS> consumed{};  // this frame's first run: 1 GC pad read, 2 Wii Remote
  struct LateStats
  {
    u64 early = 0, fallback = 0, dup = 0, skipped = 0, accepted = 0, refused = 0, missed = 0,
        unread = 0, outside = 0;
  } late{}, late_window{};
  // A controller read while no frame's pass is open (none is expected): it took the inputs of the
  // frame that ended without marking them read, so that frame takes no late input at its end.
  bool read_outside_pass = false;
  s64 late_window_from = -1;
  std::vector<Clock::time_point> gekko_tx_at;  // when each gekko_tx packet was queued
  // PPR_MACHINE_DIRECT_SEND=1 (prototype): the CPU thread sends a frame's GekkoNet packets itself
  // with its early local input (FlushGekkoNow), the net thread only those older than
  // DIRECT_SEND_HOLD (a frame without an early input: its boundary's acks), and it waits on the
  // socket instead of sleeping, so packets that come are taken at once.
  bool direct_send = false;
  double tx_wait_sum = 0, tx_wait_max = 0;
  u64 tx_n = 0;
  double loop_gap_max = 0;
  // The player's setting changed while the session runs (SetInputDelay, any thread): a profile
  // (AUTO_DELAY...) or 0-9, NO_DELAY_REQUEST for none.
  std::atomic<int> requested_delay{NO_DELAY_REQUEST};
  std::vector<HistoryEntry> history = std::vector<HistoryEntry>(HISTORY_SIZE);
  // The NAND journal mark of each snapshot in RollbackManager's ring, oldest first: a load drops
  // the newer ones, and the journal forgets changes older than the oldest snapshot.
  std::deque<u64> journal_marks;
  u64 journal_errors = 0;
  std::mutex log_mutex;
  std::vector<std::string> log;  // under log_mutex
};

State s;

const char* s_gekko_addr[MAX_PLAYERS] = {"p0", "p1", "p2", "p3"};

void AfterSave(Core::System& system);
void AfterLoad(Core::System& system, int frames_back);
void PaceLoad(Clock::duration took);
void PaceSave(Clock::duration took);

void AddLog(std::string line)
{
  INFO_LOG_FMT(BRAWLBACK, "machine: {}", line);
  // The network thread logs too (peers connecting and leaving).
  std::lock_guard lk(s.log_mutex);
  if (s.log.size() >= 64)
    s.log.erase(s.log.begin());
  s.log.push_back(std::move(line));
}

// A grey line in the chat window (only here) and the log. Not with `net_mutex` held.
void ShowNotice(const std::string& text)
{
  AddLog("notice: " + text);
  Online::Chat::AddNotice(text);
}

// An SD card difference this big is rarely a few changed files: more likely another release of the
// mod (the user's, 2026-10-19; bug reports #2 and #4, a REX card 280 MB different). Said with the
// size wherever it shows; the launcher says it too (mods.messages.ts sdBigDifference).
constexpr u64 SD_BIG_DIFFERENCE_BYTES = 50ull << 20;

// " Too big? ..." after a notice about a difference of `bytes`, or "" for a small one. `joiner`:
// said to the player whose card differs, else to the others about them.
std::string SDBigDifferenceNote(u64 bytes, bool joiner)
{
  if (bytes <= SD_BIG_DIFFERENCE_BYTES)
    return "";
  return joiner ? " Too big? You might have an entirely different version than the host." :
                  " Too big? They might have an entirely different version than the host.";
}

void BroadcastControl(const picojson::object& msg);

// The pace diagnostic's counters (PPR_MACHINE_PACE_LOG, PaceFrame).
struct PaceStats
{
  Clock::time_point since{}, last_frame{};
  u32 frames = 0, resim = 0, rollbacks = 0, waits = 0, late25 = 0, late50 = 0;
  double wait_ms = 0, late_ms = 0, speed_sum = 0, ahead_sum = 0;
  float ahead_min = 1e9f, ahead_max = -1e9f;
  // Where a rollback's time goes: its loads (the wait for the video thread, the device states,
  // the granules restored), its re-run passes (each from its start to the next pass's: the frame
  // and its save), and every pass's save.
  double load_ms = 0, load_max_ms = 0, load_sync_ms = 0, load_dostate_ms = 0, load_jit_ms = 0;
  double resim_ms = 0, resim_max_ms = 0, save_ms = 0, save_max_ms = 0;
  u64 load_granules = 0, load_changed = 0, load_jit_dropped = 0;
  u32 saves = 0;
  // The frame profile (Rollback::FrameProfile) of re-run passes and of shown passes (a shown
  // pass's includes the rollback load before the re-runs that follow it, and the throttle).
  Rollback::FrameProfile::Counters resim_c{}, shown_c{};
};
PaceStats s_pace;
// The pass that started last (kept across the pace log's seconds).
Clock::time_point s_pace_pass_start{};
bool s_pace_pass_resim = false;
Rollback::FrameProfile::Counters s_pace_pass_c0{};

void Fail(std::string why)
{
  ERROR_LOG_FMT(BRAWLBACK, "machine: {}", why);
  s.error = std::move(why);
  s.phase = Phase::Error;
  OSD::AddMessage("Session ended: " + s.error, 10000, OSD::Color::RED);
  // The others stop too, instead of waiting for frames that never come.
  std::lock_guard lk(s.net_mutex);
  if (s.host)
  {
    picojson::object msg;
    msg["type"] = picojson::value("leave");
    msg["reason"] = picojson::value(s.error);
    BroadcastControl(msg);
    enet_host_flush(s.host);
  }
}

// ------------------------------------------------------------------------------ network thread

void SendControl(int slot, const picojson::object& msg)
{
  // net_mutex held
  PeerLink& p = s.peers[slot];
  if (!p.peer)
    return;
  const std::string text = picojson::value(msg).serialize();
  ENetPacket* packet =
      enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE);
  enet_peer_send(p.peer, CHANNEL_CONTROL, packet);
}

void BroadcastControl(const picojson::object& msg)
{
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (slot != s.opts.local_slot && s.peers[slot].slot >= 0 && s.peers[slot].hello)
      SendControl(slot, msg);
  }
}

// ENetPeer::data: the peer's slot + 1 (0: not known yet), with OUTGOING for a connection this
// side started; PROBE for the endpoint probe's connection to mm.
constexpr intptr_t OUTGOING = 0x100;
constexpr intptr_t PROBE = 0x200;

bool Plays(int slot)
{
  return ((s.players.load() & ~s.gone.load()) >> slot) & 1;
}

bool LocalPlays()
{
  return Plays(s.opts.local_slot);
}

// A joiner: whether a peer's digest of the image at `epoch` must be in (and match) before the
// image loads. Every connected player's. Another joiner's (its hello said it does not play) only
// if it sent one: it is not at the epoch, and two joiners waiting for each other's digest held
// the decider at the epoch for good (2026-10-14, REX: one joiner had the image, the other asked).
bool MustVouch(const PeerLink& p, int epoch)
{
  return p.slot >= 0 && p.hello && !p.left && (!p.late || p.epoch_at == epoch);
}

// A player in the chat window's notices: its room name, or its slot.
std::string NameOf(int slot)
{
  const PeerLink& p = s.peers[slot];
  return p.name.empty() ? fmt::format("Player {}", slot + 1) : p.name;
}

// A hello is taken from a peer's own addresses only (its slot is otherwise just a number anyone
// could send). A room peer with no address yet has none.
bool AddressAllowed(const PeerLink& p, const ENetAddress& from)
{
  if (p.ips.empty())
    return !s.opts.room;
  return std::find(p.ips.begin(), p.ips.end(), from.host) != p.ips.end();
}

std::vector<enet_uint32> ResolveIps(std::initializer_list<const std::string*> hosts)
{
  std::vector<enet_uint32> ips;
  for (const std::string* h : hosts)
  {
    ENetAddress a{};
    if (!h->empty() && enet_address_set_host(&a, h->c_str()) == 0)
      ips.push_back(a.host);
  }
  return ips;
}

int SlotOf(ENetPeer* peer)
{
  return static_cast<int>((reinterpret_cast<intptr_t>(peer->data) & 0xFF) - 1);
}

// Both sides connect to each other (needed through NATs); of two connections for the same pair,
// both keep the one the lower slot started, so they agree without another round trip.
void OnHello(ENetPeer* peer, const picojson::object& msg)
{
  const auto it = msg.find("slot");
  if (it == msg.end() || !it->second.is<double>())
    return;
  const double v = it->second.get<double>();
  if (v < 0 || v >= MAX_PLAYERS)
    return;
  const int slot = static_cast<int>(v);
  if (slot == s.opts.local_slot || s.peers[slot].slot != slot)
  {
    enet_peer_disconnect(peer, 0);
    return;
  }
  PeerLink& p = s.peers[slot];
  if (!AddressAllowed(p, peer->address))
  {
    WARN_LOG_FMT(BRAWLBACK, "machine: a hello for slot {} from an address that is not its own",
                 slot + 1);
    enet_peer_disconnect(peer, 0);
    return;
  }
  const bool lower_is_me = s.opts.local_slot < slot;
  const bool this_is_mine = (reinterpret_cast<intptr_t>(peer->data) & OUTGOING) != 0;
  const bool keep = lower_is_me ? this_is_mine : !this_is_mine;
  if (!keep)
  {
    ++p.unkept_hellos;
    return;  // the other connection is the one both keep; this one idles until it drops
  }
  if (p.peer && p.peer != peer)
    enet_peer_disconnect_later(p.peer, 0);
  if (p.peer != peer)
    p.min_rtt_ms = -1;
  p.peer = peer;
  p.hello = true;
  {
    const auto pr = msg.find("proto");
    p.proto = pr != msg.end() && pr->second.is<double>() ? static_cast<int>(pr->second.get<double>()) : 1;
    const auto card = msg.find("sd_card");
    p.sd_card = card != msg.end() && card->second.is<bool>() && card->second.get<bool>();
    const auto root = msg.find("sd");
    p.sd_root = root != msg.end() && root->second.is<std::string>() &&
                        root->second.get<std::string>().size() == 64 ?
                    root->second.get<std::string>() :
                    "";
  }
  // Whether it plays in its session now: one that does not is a joiner (a room's joiner learns
  // who the session's players are this way).
  if (const auto pl = msg.find("playing"); pl != msg.end() && pl->second.is<bool>())
  {
    if (!pl->second.get<bool>())
      p.late = true;
    else if (s.opts.join && !LocalPlays())
      p.late = false;
  }
  if (p.late && !Plays(slot))
  {
    // A joiner (again): what an earlier visit left does not count.
    p.reseated = false;
    p.left = false;
    p.dropped = false;
    p.join_asked = false;
  }
  AddLog(fmt::format("peer {} connected ({}:{})", slot, p.host, p.port));
}

void HandleControl(ENetPeer* peer, const u8* data, size_t size)
{
  if (size > MAX_CONTROL_SIZE)
    return;
  picojson::value v;
  const std::string err = picojson::parse(v, std::string(reinterpret_cast<const char*>(data), size));
  if (!err.empty() || !v.is<picojson::object>())
    return;
  const auto& msg = v.get<picojson::object>();
  const auto type_it = msg.find("type");
  if (type_it == msg.end() || !type_it->second.is<std::string>())
    return;
  const std::string& type = type_it->second.get<std::string>();
  if (type == "hello")
  {
    OnHello(peer, msg);
    return;
  }
  const int slot = SlotOf(peer);
  if (slot < 0 || slot >= MAX_PLAYERS || s.peers[slot].peer != peer)
    return;
  PeerLink& p = s.peers[slot];
  auto num = [&](const char* key, double def) {
    const auto it = msg.find(key);
    return it != msg.end() && it->second.is<double>() ? it->second.get<double>() : def;
  };
  const int epoch = static_cast<int>(num("epoch", -1));
  if (type == "sd_sync")
  {
    // A joiner whose SD card differs gets the session's from the players: everyone hears it (a
    // lag while it does has a name).
    const auto state = msg.find("state");
    const std::string st = state != msg.end() && state->second.is<std::string>() ?
                               state->second.get<std::string>() :
                               "";
    if (st == "start")
    {
      const double bytes = std::max(0.0, num("bytes", 0));
      ShowNotice(fmt::format("{}'s SD card does not match. Sending them {:.0f} MB.{}", NameOf(slot),
                             std::ceil(bytes / 1048576.0),
                             SDBigDifferenceNote(static_cast<u64>(bytes), false)));
    }
    else if (st == "done")
    {
      ShowNotice(fmt::format("{} has the SD card now.", NameOf(slot)));
    }
  }
  else if (type == "sd")
  {
    const auto root = msg.find("root");
    if (root != msg.end() && root->second.is<std::string>() &&
        root->second.get<std::string>().size() == 64)
    {
      p.sd_root = root->second.get<std::string>();
    }
  }
  else if (type == "started")
    p.started = epoch;
  else if (type == "go")
    p.go = epoch;
  else if (type == "leave")
  {
    p.left = true;
    // Why its session ended, in this log too: a joiner whose machine image did not load says why
    // only to the players (its own log never reaches them; prod 2026-10-14, ANGE#858 joining a
    // REX TE room: four "could not join" right after the image arrived, no reason on the host).
    const auto r = msg.find("reason");
    if (r != msg.end() && r->second.is<std::string>() && !r->second.get<std::string>().empty())
      AddLog(fmt::format("player {} left: {}", slot + 1, r->second.get<std::string>().substr(0, 160)));
  }
  else if (type == "join")
  {
    // A joiner asks to play in its slot (only a peer this session expects to join: `late`).
    if (!p.late || !LocalPlays())
      return;
    const auto c = msg.find("controller");
    p.join_controller = c != msg.end() && c->second.is<std::string>() &&
                                c->second.get<std::string>() == "wiimote" ?
                            Controller::WiiRemote :
                            Controller::GameCube;
    if (p.proto != PROTOCOL_VERSION)
    {
      picojson::object no;
      no["type"] = picojson::value("join_refused");
      no["why"] = picojson::value("this room's players have another version of Brawl Online: "
                                  "everyone needs the latest");
      SendControl(slot, no);
      AddLog(fmt::format("player {} has another version of Brawl Online ({}): refused", slot + 1,
                         p.proto));
      return;
    }
    // The same session clock, or its machine could never run as ours.
    if (num("rtc", 0) != static_cast<double>(s.opts.rtc))
    {
      picojson::object no;
      no["type"] = picojson::value("join_refused");
      no["why"] = picojson::value("the session's clock differs");
      SendControl(slot, no);
      return;
    }
    p.join_asked = true;
    AddLog(fmt::format("player {} asks to join", slot + 1));
  }
  else if (type == "epoch_at")
  {
    p.epoch_at = epoch;
    p.epoch_sum = static_cast<u32>(num("sum", 0));
    const auto d = msg.find("digest");
    p.epoch_digest.clear();
    if (d != msg.end() && d->second.is<std::string>() && d->second.get<std::string>().size() == 64)
      p.epoch_digest = d->second.get<std::string>();
  }
  else if (type == "epoch_go")
  {
    State::EpochGo go;
    go.epoch = epoch;
    go.frame = static_cast<s64>(num("frame", -1));
    const double players = num("players", 0);
    if (go.epoch < 0 || go.frame < 0 || players < 1 || players > 15)
      return;
    go.players = static_cast<u32>(players);
    const auto cit = msg.find("controllers");
    if (cit != msg.end() && cit->second.is<picojson::array>())
    {
      const auto& arr = cit->second.get<picojson::array>();
      for (size_t i = 0; i < arr.size() && i < MAX_PLAYERS; ++i)
      {
        go.controllers[i] = arr[i].is<std::string>() && arr[i].get<std::string>() == "wiimote" ?
                                Controller::WiiRemote :
                                Controller::GameCube;
      }
    }
    const auto mit = msg.find("move");
    if (mit != msg.end() && mit->second.is<picojson::array>())
    {
      const auto& arr = mit->second.get<picojson::array>();
      if (arr.size() == 2 && arr[0].is<double>() && arr[1].is<double>())
      {
        const double a = arr[0].get<double>(), b = arr[1].get<double>();
        if (a >= 0 && a < MAX_PLAYERS && b >= 0 && b < MAX_PLAYERS && a != b)
          go.move = {static_cast<int>(a), static_cast<int>(b)};
      }
    }
    s.epoch_go = go;
  }
  else if (type == "image_progress")
  {
    // A joiner's progress with its machine image, for the players' chat notices.
    const double tenths = num("tenths", -1);
    if (epoch >= 0 && tenths >= 0 && tenths <= 10)
    {
      p.image_epoch = epoch;
      p.image_tenths = static_cast<int>(tenths);
    }
  }
  else if (type == "join_refused")
  {
    s.join_refused = true;
    const auto w = msg.find("why");
    s.join_refused_why = w != msg.end() && w->second.is<std::string>() ?
                             w->second.get<std::string>().substr(0, 160) :
                             "refused";
  }
}

// Net thread: a packet of a machine image for this joiner. The first packet of an image is its
// header; the rest is the compressed image, in order (the channel is reliable and sequenced).
void HandleImagePacket(int slot, const u8* data, size_t size)
{
  State::ImageRx& rx = s.image_rx;
  if (!s.opts.join || s.join_loaded || rx.complete)
    return;
  if (rx.from < 0)
  {
    // Header: magic, epoch, frame, sum, packed size, raw size (little endian).
    if (size != 4 + 4 + 8 + 4 + 8 + 8)
      return;
    u32 magic, epoch, sum;
    s64 frame;
    u64 packed, raw;
    std::memcpy(&magic, data, 4);
    std::memcpy(&epoch, data + 4, 4);
    std::memcpy(&frame, data + 8, 8);
    std::memcpy(&sum, data + 16, 4);
    std::memcpy(&packed, data + 20, 8);
    std::memcpy(&raw, data + 28, 8);
    if (magic != IMAGE_MAGIC || frame < 0 || packed == 0 || packed > MAX_IMAGE_PACKED ||
        raw == 0 || raw > MAX_IMAGE_RAW || epoch > 1000000)
    {
      WARN_LOG_FMT(BRAWLBACK, "machine: player {} sent an image header that is not valid", slot + 1);
      return;
    }
    rx.from = slot;
    rx.epoch = static_cast<int>(epoch);
    rx.frame = frame;
    rx.sum = sum;
    rx.packed_size = packed;
    rx.raw_size = raw;
    rx.data.clear();
    rx.data.reserve(static_cast<size_t>(packed));
    rx.started = Clock::now();
    AddLog(fmt::format("machine image coming from player {}: {:.1f} MB", slot + 1, packed / 1048576.0));
    return;
  }
  if (slot != rx.from)
    return;
  if (rx.data.size() + size > rx.packed_size)
  {
    WARN_LOG_FMT(BRAWLBACK, "machine: player {} sent more image than it announced", slot + 1);
    rx = {};
    return;
  }
  rx.data.insert(rx.data.end(), data, data + size);
  if (rx.data.size() == rx.packed_size)
  {
    rx.complete = true;
    AddLog("machine image received");
  }
}

// The session's SD card ------------------------------------------------------------------------

// Asks the players for chunks of the session's card (`net_mutex` held): the player that sent
// this joiner its machine image first, then any other, never one that said it has not got the
// chunk. A chunk no connected player is left to ask for is asked for again when its read retries.
// What player `slot` sent this joiner of the card in the last few seconds, bytes a second
// (`net_mutex` held).
double SourceRateLocked(int slot, Clock::time_point now)
{
  constexpr auto WINDOW = std::chrono::seconds(3);
  auto& from = s.sd_from[slot];
  while (!from.empty() && now - from.front().first > WINDOW)
    from.pop_front();
  u64 bytes = 0;
  for (const auto& [t, b] : from)
    bytes += b;
  return bytes / std::chrono::duration<double>(WINDOW).count();
}

// The chunks a player may have asked of it and not sent yet: what it sends in about three
// seconds (at least 8, at most 128).
size_t SourceCapacityLocked(int slot, Clock::time_point now)
{
  const double chunks = SourceRateLocked(slot, now) * 3 / SDCard::CHUNK_SIZE;
  return static_cast<size_t>(std::clamp(chunks, 8.0, 128.0));
}

// A player that can send the session card's chunks to this joiner (`net_mutex` held).
bool SDSourceUsableLocked(int slot)
{
  if (slot < 0 || slot >= MAX_PLAYERS || slot == s.opts.local_slot)
    return false;
  const PeerLink& p = s.peers[slot];
  return p.peer && p.hello && !p.left && !p.late;
}

// Asks the players for chunks of the session's card (`net_mutex` held), spread over every player
// that has the card (each paces its own upload, PaceSD): each chunk goes to the one with the
// least asked of it for what it sends, not again to the one asked last (a retry), never to one
// that said it has not got the chunk. A chunk no connected player is left to ask for is asked for
// again when its read retries.
void RequestChunksLocked(std::span<const u32> chunks)
{
  const auto now = Clock::now();
  std::array<std::vector<u32>, MAX_PLAYERS> by_slot;
  std::array<size_t, MAX_PLAYERS> outstanding{};
  for (const auto& [chunk, slot] : s.sd_asked)
  {
    if (slot >= 0 && slot < MAX_PLAYERS)
      ++outstanding[slot];
  }
  std::array<double, MAX_PLAYERS> rate{};
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    rate[slot] = SourceRateLocked(slot, now);
  for (const u32 c : chunks)
  {
    const auto tried_it = s.sd_tried.find(c);
    const u8 tried = tried_it != s.sd_tried.end() ? tried_it->second : 0;
    const auto asked_it = s.sd_asked.find(c);
    const int asked_before = asked_it != s.sd_asked.end() ? asked_it->second : -1;
    auto usable = [&](int slot) { return SDSourceUsableLocked(slot) && !(tried & (1u << slot)); };
    int target = -1;
    double best = 0;
    for (const bool avoid_previous : {true, false})
    {
      for (int slot = 0; slot < MAX_PLAYERS; ++slot)
      {
        if (!usable(slot) || (avoid_previous && slot == asked_before))
          continue;
        // The image's sender first among equals (it may be the only one with the card).
        const double load = (outstanding[slot] + 1) / std::max(rate[slot], 64.0 * 1024) -
                            (slot == s.sd_source ? 1e-9 : 0);
        if (target < 0 || load < best)
        {
          target = slot;
          best = load;
        }
      }
      if (target >= 0)
        break;
    }
    if (target < 0)
      continue;
    if (asked_before >= 0 && asked_before < MAX_PLAYERS && outstanding[asked_before] > 0)
      --outstanding[asked_before];
    ++outstanding[target];
    s.sd_asked[c] = target;
    by_slot[target].push_back(c);
  }
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    const std::vector<u32>& list = by_slot[slot];
    for (size_t i = 0; i < list.size(); i += SDCard::MAX_REQUEST_CHUNKS)
    {
      const size_t n = std::min<size_t>(SDCard::MAX_REQUEST_CHUNKS, list.size() - i);
      sf::Packet packet;
      packet << SD_REQUEST;
      SDCard::WriteChunkRequest(std::span<const u32>(list.data() + i, n), packet);
      const u8* data = static_cast<const u8*>(packet.getData());
      s.sd_tx.emplace_back(slot, std::vector<u8>(data, data + packet.getDataSize()));
    }
  }
}

// Net thread (`net_mutex` held): a CHANNEL_SD packet from a player of the session. A request goes
// to this machine's server (only chunks of the session's card that hold data, and only while
// they match their hash); a chunk to this joiner's card, which checks it against the manifest.
void HandleSDPacket(int slot, const u8* data, size_t size)
{
  if (size < 1)
    return;
  if (data[0] == SD_PIECE)
  {
    // A slice: the packet whole once its last slice is in (checked like any packet then).
    std::vector<u8>& buffer = s.sd_rx_pieces[slot];
    if (size < 2 || buffer.size() + (size - 2) > MAX_SD_MANIFEST_PACKET)
    {
      buffer.clear();
      return;
    }
    buffer.insert(buffer.end(), data + 2, data + size);
    if (data[1] == 0)
      return;
    std::vector<u8> whole = std::move(buffer);
    buffer.clear();
    if (!whole.empty() && whole[0] != SD_PIECE)
      HandleSDPacket(slot, whole.data(), whole.size());
    return;
  }
  if (data[0] == SD_MANIFEST_REQUEST)
  {
    // A joiner before its join: the session card's manifest and file list (a player only).
    if (s.sd_manifest_wire.empty() || !LocalPlays())
      return;
    std::vector<u8> out;
    out.reserve(1 + 4 + s.sd_manifest_wire.size() + s.sd_files_wire.size());
    out.push_back(SD_MANIFEST);
    const u32 n = static_cast<u32>(s.sd_manifest_wire.size());
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<u8>(n >> (8 * i)));
    out.insert(out.end(), s.sd_manifest_wire.begin(), s.sd_manifest_wire.end());
    out.insert(out.end(), s.sd_files_wire.begin(), s.sd_files_wire.end());
    s.sd_tx.emplace_back(slot, std::move(out));
    AddLog(fmt::format("SD card: sent player {} the card's list ({:.1f} MB)", slot + 1,
                       (s.sd_manifest_wire.size() + s.sd_files_wire.size()) / 1048576.0));
    return;
  }
  if (data[0] == SD_MANIFEST)
  {
    State::SDSync& y = s.sd_sync;
    if (slot != y.from || y.received)
      return;
    y.packet.assign(data + 1, data + size);
    y.packet_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - y.asked).count();
    y.received = true;
    return;
  }
  if (size > MAX_SD_PACKET)
    return;
  sf::Packet packet;
  packet.append(data + 1, size - 1);
  if (data[0] == SD_REQUEST)
  {
    if (s.sd_server)
      s.sd_server->OnRequest(static_cast<u8>(slot), packet);
    return;
  }
  if (data[0] != SD_CHUNK || !s.sd_card)
    return;
  const std::optional<SDCard::ChunkData> chunk = SDCard::ReadChunkData(packet);
  if (!chunk)
  {
    WARN_LOG_FMT(BRAWLBACK, "machine: player {} sent an SD card chunk that is not valid", slot + 1);
    return;
  }
  const auto asked = s.sd_asked.find(chunk->index);
  if (!chunk->ok)
  {
    // Only the player asked can say it has not got the chunk; another player may have it.
    if (asked == s.sd_asked.end() || asked->second != slot)
      return;
    s.sd_asked.erase(asked);
    s.sd_tried[chunk->index] |= static_cast<u8>(1u << slot);
    const u32 index = chunk->index;
    RequestChunksLocked(std::span<const u32>(&index, 1));
    if (!s.sd_asked.contains(index))
    {
      // Nobody connected has it: the reads that need it fail.
      AddLog(fmt::format("SD card: no player has chunk {} of the session's card", index));
      s.sd_card->Deliver(*chunk);
    }
    return;
  }
  if (asked != s.sd_asked.end())
    s.sd_asked.erase(asked);
  s.sd_tried.erase(chunk->index);
  s.sd_from[slot].emplace_back(Clock::now(), chunk->data.size());
  s.sd_card->Deliver(*chunk);
}

// This machine plays the session's card `card` (`net_mutex` held): its manifest for the machine
// images this player builds, and a server for the other players' requests. Returns the previous
// server, which the caller destroys without the lock (its thread may be waiting for it).
std::unique_ptr<SDCard::CardServer>
SetSessionCardLocked(std::shared_ptr<SDCard::SessionCard> card)
{
  std::unique_ptr<SDCard::CardServer> old = std::move(s.sd_server);
  s.sd_card = card;
  const std::shared_ptr<const SDCard::Manifest>& manifest = card->GetManifest();
  sf::Packet wire;
  SDCard::WriteManifest(*manifest, wire);
  const u8* data = static_cast<const u8*>(wire.getData());
  s.sd_manifest_wire.assign(data, data + wire.getDataSize());
  s.sd_manifest_canon = SDCard::CanonicalManifest(*manifest);
  s.sd_root = SDCard::RootHex(SDCard::ManifestRoot(*manifest));
  if (s.host)
  {
    picojson::object m;
    m["type"] = picojson::value("sd");
    m["root"] = picojson::value(s.sd_root);
    BroadcastControl(m);
  }
  std::weak_ptr<SDCard::SessionCard> weak = card;
  s.sd_server = std::make_unique<SDCard::CardServer>(
      manifest,
      [weak](u32 index, std::span<u8> chunk) {
        const auto c = weak.lock();
        return c && c->ReadForPeer(index, chunk);
      },
      [](sf::Packet& packet) { packet << SD_CHUNK; },
      [](u8 slot, sf::Packet&& packet) {
        if (slot >= MAX_PLAYERS)
          return;
        const u8* bytes = static_cast<const u8*>(packet.getData());
        std::lock_guard lk(s.net_mutex);
        s.sd_tx.emplace_back(slot, std::vector<u8>(bytes, bytes + packet.getDataSize()));
      });
  return old;
}

// The end of a session's card (Arm, Stop): this machine stops serving it and forgets it.
void ResetSD()
{
  std::unique_ptr<SDCard::CardServer> server;
  std::shared_ptr<SDCard::SessionCard> card;
  {
    std::lock_guard lk(s.net_mutex);
    server = std::move(s.sd_server);
    card = std::move(s.sd_card);
    s.sd_manifest_wire.clear();
    s.sd_manifest_canon.clear();
    s.sd_root.clear();
    s.sd_files_wire.clear();
    s.sd_tx.clear();
    for (State::SDPace& pace : s.sd_pace)
      pace = {};
    for (auto& pieces : s.sd_rx_pieces)
      pieces.clear();
    for (auto& from : s.sd_from)
      from.clear();

    s.sd_tried.clear();
    s.sd_asked.clear();
    s.sd_source = -1;
  }
  server.reset();
  if (card)
    card->Detach();
  s.sd_path.clear();
  State::SDSync& y = s.sd_sync;
  y.hash_cancel = true;
  if (y.hash_thread.joinable())
    y.hash_thread.join();
  if (y.card)
    y.card->Detach();
  {
    std::lock_guard lk(s.net_mutex);
    y.from = -1;
    y.received = false;
    y.packet.clear();
  }
  y.step = State::SDSync::Step::Idle;
  y.hash_cancel = false;
  y.hash_finished = false;
  y.hash_done = 0;
  y.hash_total = 0;
  y.own.reset();
  y.card.reset();
  y.files_wire.clear();
  y.missing_bytes = 0;
  y.missing_files = 0;
  y.names.clear();
  y.rate = 0;
  y.choice = static_cast<int>(s.opts.sd_sync);
  y.chose = false;
  y.tenths_shown = -1;
}

// A joiner's card reaches the session's card through the players.
class SessionCardRemote final : public SDCard::SessionCard::Remote
{
public:
  void Request(std::span<const u32> chunks) override
  {
    std::lock_guard lk(s.net_mutex);
    RequestChunksLocked(chunks);
  }
  // A read waiting for chunks on the CPU thread: GekkoNet keeps exchanging packets, so the other
  // players wait for this one rather than drop it.
  void KeepAlive() override
  {
    if (Core::IsCPUThread() && s.gekko)
      gekko_network_poll(s.gekko);
  }
  size_t Capacity() override
  {
    std::lock_guard lk(s.net_mutex);
    const auto now = Clock::now();
    size_t total = 0;
    for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    {
      if (SDSourceUsableLocked(slot))
        total += SourceCapacityLocked(slot, now);
    }
    return total;
  }
};

// Net thread: this machine's LAN address, as Slippi's matchmaking finds it (a UDP "connect"
// towards mm makes the OS pick the interface; nothing is sent).
std::string LanHost(const ENetAddress& towards)
{
  ENetSocket sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (sock == ENET_SOCKET_NULL)
    return {};
  ENetAddress local{};
  const bool ok =
      enet_socket_connect(sock, &towards) == 0 && enet_socket_get_address(sock, &local) == 0;
  enet_socket_destroy(sock);
  if (!ok || local.host == 0)
    return {};
  char text[64]{};
  if (enet_address_get_host_ip(&local, text, sizeof(text)) != 0)
    return {};
  return text;
}

// Net thread: the endpoint probe (docs/design/brawl-mods.md §5): a connection of its own to mm
// from the session's socket, one `mods-endpoint` message, closed by mm after its answer.
void StepProbe(Clock::time_point now)
{
  State::Probe& pr = s.probe;
  if (pr.state != "probing" || pr.peer || now - pr.last < std::chrono::seconds(2))
    return;
  pr.last = now;
  if (++pr.attempts > 15)
  {
    pr.state = "failed: the server did not answer";
    AddLog("endpoint probe: the server did not answer");
    return;
  }
  ENetAddress addr{};
  if (enet_address_set_host(&addr, pr.host.c_str()) != 0)
    return;
  addr.port = pr.port;
  pr.peer = enet_host_connect(s.host, &addr, CHANNEL_COUNT, 0);
  if (pr.peer)
  {
    pr.peer->data = reinterpret_cast<void*>(PROBE);
    enet_peer_timeout(pr.peer, 0, 3000, 5000);
  }
}

void SendProbe(ENetPeer* peer)
{
  picojson::object m;
  m["type"] = picojson::value("mods-endpoint");
  m["token"] = picojson::value(s.probe.token);
  // A Force LAN IP or the network chosen in the launcher (Config::GetChosenNetwork) replaces it.
  const std::string chosen = Config::GetChosenNetwork().address;
  const std::string lan = !chosen.empty() ? chosen : LanHost(peer->address);
  if (!lan.empty() && s.host)
  {
    picojson::object l;
    l["host"] = picojson::value(lan);
    l["port"] = picojson::value(static_cast<double>(s.host->address.port));
    m["lan"] = picojson::value(l);
  }
  const std::string text = picojson::value(m).serialize();
  enet_peer_send(peer, 0, enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE));
}

void OnProbeAnswer(ENetPeer* peer, const u8* data, size_t size)
{
  picojson::value v;
  if (size > MAX_CONTROL_SIZE ||
      !picojson::parse(v, std::string(reinterpret_cast<const char*>(data), size)).empty() ||
      !v.is<picojson::object>())
  {
    return;
  }
  const auto& o = v.get<picojson::object>();
  const auto t = o.find("type");
  if (t == o.end() || !t->second.is<std::string>() ||
      t->second.get<std::string>() != "mods-endpoint-resp")
  {
    return;
  }
  const auto e = o.find("error");
  if (e != o.end() && e->second.is<std::string>())
  {
    s.probe.state = "failed: " + e->second.get<std::string>().substr(0, 120);
    AddLog("endpoint probe refused: " + e->second.get<std::string>().substr(0, 120));
  }
  else
  {
    s.probe.state = "done";
    AddLog(fmt::format("endpoint probe answered (attempt {})", s.probe.attempts));
  }
  enet_peer_disconnect(peer, 0);
}

// The reliable data queued for `peer` that ENet has not sent yet (new and retransmitted), counted
// up to `limit`.
size_t QueuedReliable(ENetPeer* peer, size_t limit)
{
  size_t n = 0;
  for (ENetListIterator it = enet_list_begin(&peer->outgoingSendReliableCommands);
       it != enet_list_end(&peer->outgoingSendReliableCommands) && n < limit;
       it = enet_list_next(it))
  {
    n += reinterpret_cast<ENetOutgoingCommand*>(it)->fragmentLength;
  }
  return n;
}

// ENet's clock in ms, which wraps: how far `a` is after `b`.
s32 EnetAfter(enet_uint32 a, enet_uint32 b)
{
  return static_cast<s32>(a - b);
}

// The highest round trip of each half second of a peer's acknowledgements (OnAck), [0] the
// current one, moved on to `now`.
void RollRttHigh(decltype(PeerLink::cc)& cc, enet_uint32 now)
{
  constexpr enet_uint32 SPAN = 500;
  if (EnetAfter(now, cc.rtt_high_at) >= static_cast<s32>(SPAN * cc.rtt_high.size()))
  {
    cc.rtt_high.fill(0);
    cc.rtt_high_at = now;
  }
  while (EnetAfter(now, cc.rtt_high_at) >= static_cast<s32>(SPAN))
  {
    std::copy_backward(cc.rtt_high.begin(), cc.rtt_high.end() - 1, cc.rtt_high.end());
    cc.rtt_high[0] = 0;
    cc.rtt_high_at += SPAN;
  }
}

// The highest round trip of the last two seconds (ms; 0 with none).
enet_uint32 RecentHighRtt(decltype(PeerLink::cc)& cc, enet_uint32 now)
{
  RollRttHigh(cc, now);
  return *std::max_element(cc.rtt_high.begin(), cc.rtt_high.end());
}

// Net thread: when ENet resends what it sent `peer` reliably. ENet resends a reliable packet when
// it has waited its round-trip timeout, which doubles at each try, and drops the whole link once
// one try's timeout passes 10 s or the 6th (or later) waits 5 s (enet_peer_timeout), however many
// later packets the peer acknowledged meanwhile; and on a sequenced channel nothing after that
// packet reaches the game. Over a link that drops what exceeds its rate with little buffer (an
// ISP's rate limit), one packet lost a few times over held a machine image for seconds, then
// ended the join (netsim at 16 Mbit/s with a 10 ms queue: the image all sent and acknowledged but
// for its holes, the joiner below 10 %, the link gone after 15 s). And on a jittery link (Wi-Fi)
// ENet's first timeout, its mean round trip plus four deviations, was far too short: it updates
// the deviation with each acknowledgement, and a run of them arriving together (a transfer's,
// held up behind one slow packet) shrinks it to a few ms, so the next slow packet timed out with
// everything sent after it: through netsim at 80 ms with pareto jitter of 25 ms, a third to a
// half of what a round trip sent was resent though the joiner had it, and each of those round
// trips halved the congestion window (PaceSendWindow).
// Now, as TCP's RACK (RFC 8985): a packet is lost once the peer acknowledged one sent after it
// (the way keeps their order but for a little reordering) and it has been out that one's round
// trip plus a quarter of the link's lowest; then it is resent at once. Otherwise its first
// timeout is at least the highest round trip of the last two seconds plus a quarter, so jitter
// alone seldom fires it; and a resend waits at most two round trips plus four deviations (or that
// floor) while the peer answers. A resend counts as a loss for the congestion window unless the
// peer's acknowledgement shows it was not needed (OnAck). A peer that stopped answering keeps
// ENet's timeouts.
void ResendLost(PeerLink& p)
{
  ENetPeer* e = p.peer;
  auto& cc = p.cc;
  const enet_uint32 now = enet_time_get();
  if (EnetAfter(now, e->lastReceiveTime) > 1000)
    return;
  const enet_uint32 high = RecentHighRtt(cc, now);
  const enet_uint32 floor = high + high / 4;
  const enet_uint32 limit = std::max<enet_uint32>(
      {2 * e->roundTripTime + 4 * e->roundTripTimeVariance, floor, 30});
  const enet_uint32 reorder =
      std::max<enet_uint32>(cc.min_rtt == 0xffffffff ? 0 : cc.min_rtt / 4, 2);
  bool any = false;
  enet_uint32 next = 0;
  // Oldest sent first (ENet moves a command to the end when it sends it again).
  for (ENetListIterator it = enet_list_begin(&e->sentReliableCommands);
       it != enet_list_end(&e->sentReliableCommands); it = enet_list_next(it))
  {
    auto* c = reinterpret_cast<ENetOutgoingCommand*>(it);
    const s32 age = EnetAfter(now, c->sentTime);
    if (cc.rack_valid && EnetAfter(cc.rack_sent, c->sentTime) > 0 &&
        age >= static_cast<s32>(cc.rack_rtt + reorder))
    {
      if (c->roundTripTimeout > static_cast<enet_uint32>(age))
      {
        c->roundTripTimeout = static_cast<enet_uint32>(std::max(age, 1));
        ++cc.rack_resent;
      }
    }
    else if (c->sendAttempts <= 1)
    {
      c->roundTripTimeout = std::max(c->roundTripTimeout, floor);
    }
    else if (c->roundTripTimeout > limit && EnetAfter(e->lastReceiveTime, c->sentTime + limit) > 0)
    {
      c->roundTripTimeout = limit;
    }
    // ENet looks at its timeouts once its clock reaches nextTimeout.
    const enet_uint32 due = c->sentTime + c->roundTripTimeout;
    if (!any || EnetAfter(next, due) > 0)
      next = due;
    any = true;
  }
  if (any)
    e->nextTimeout = next;
}

// Net thread, from ENet's intercept (InterceptAcks): the peer acknowledged the reliable command
// `seq` on `channel`, echoing the low 16 bits of the time it was sent (ENet's clock: the send
// that reached it). Its round trip feeds RACK and the timeout's floor (ResendLost). And if that
// command had timed out, the echo tells whether its resend was needed (Eifel's detection, RFC
// 3522, ENet's echoed time as the timestamp): an acknowledgement of an earlier send than its last,
// or of a command that timed out and waits to go again, shows the packet was only late. Such a
// resend does not count as a loss: it is taken off its round trip's losses or, once that round
// trip has cut the window, off the cut's, and a cut none of whose losses were real is undone.
void OnAck(PeerLink& p, enet_uint32 service_time, u8 channel, u16 seq, u16 echo)
{
  ENetPeer* e = p.peer;
  auto& cc = p.cc;
  // As ENet's enet_protocol_handle_acknowledge.
  enet_uint32 sent = echo | (service_time & 0xFFFF0000);
  if ((sent & 0x8000) > (service_time & 0x8000))
    sent -= 0x10000;
  const s32 rtt = EnetAfter(service_time, sent);
  if (rtt < 0)
    return;
  RollRttHigh(cc, service_time);
  cc.rtt_high[0] = std::max(cc.rtt_high[0], static_cast<enet_uint32>(rtt));
  // Sends close together (one datagram's acknowledgements, a burst held up behind one slow packet)
  // have about the same round trip whatever the jitter.
  constexpr s32 JITTER_SPACING_MS = 30;
  if (cc.jitter_rtt < 0 || EnetAfter(sent, cc.jitter_sent) >= JITTER_SPACING_MS)
  {
    if (cc.jitter_rtt >= 0)
    {
      const double change = rtt - cc.jitter_rtt;
      cc.trend += (change - cc.trend) / 16;
      cc.jitter += (std::abs(change - cc.trend) - cc.jitter) / 16;
      cc.jittery = cc.jitter > (cc.jittery ? SD_JITTER_CLEAR_MS : SD_JITTER_MS);
    }
    cc.jitter_rtt = rtt;
    cc.jitter_sent = sent;
  }
  if (!cc.rack_valid || EnetAfter(sent, cc.rack_sent) > 0)
  {
    cc.rack_valid = true;
    cc.rack_sent = sent;
    cc.rack_rtt = static_cast<enet_uint32>(rtt);
  }
  auto find = [&](ENetList* list, bool sent_list) -> ENetOutgoingCommand* {
    for (ENetListIterator it = enet_list_begin(list); it != enet_list_end(list);
         it = enet_list_next(it))
    {
      auto* c = reinterpret_cast<ENetOutgoingCommand*>(it);
      if (c->reliableSequenceNumber == seq && c->command.header.channelID == channel &&
          (sent_list || (c->sendAttempts >= 1 &&
                         (c->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE))))
      {
        return c;
      }
    }
    return nullptr;
  };
  bool waiting = false;
  ENetOutgoingCommand* c = find(&e->sentReliableCommands, true);
  if (!c)
  {
    waiting = true;
    c = find(&e->outgoingSendReliableCommands, false);
    if (!c)
      c = find(&e->outgoingCommands, false);
  }
  if (!c || (!waiting && (c->sendAttempts <= 1 || sent == c->sentTime)))
    return;
  // When it timed out: when it went again, or (still waiting) half its doubled timeout after it
  // last went.
  const enet_uint32 timed_out = waiting ? c->sentTime + c->roundTripTimeout / 2 : c->sentTime;
  if (EnetAfter(timed_out, cc.step_enet) >= 0)
  {
    ++cc.spurious;
  }
  else if (cc.undo_window != 0 && cc.undo_lost > 0 && --cc.undo_lost == 0)
  {
    cc.window = std::max(cc.window, cc.undo_window);
    cc.slow_start = cc.undo_slow_start;
    cc.undo_window = 0;
    ++cc.undone;
    e->windowSize = cc.window;
  }
}

// Tests (the harness): PPR_TEST_P2P_BLOCK=<connect code>[,<connect code>...], as for Unranked
// (Online/Matchmaking.cpp): every datagram from those room members is dropped, as between two
// NATs that cannot reach each other. Set on both players of the pair. Net thread.
bool TestBlocked(const ENetAddress& from)
{
  static const std::string blocked = [] {
    const char* e = std::getenv("PPR_TEST_P2P_BLOCK");
    return e && *e ? fmt::format(",{},", e) : std::string();
  }();
  if (blocked.empty())
    return false;
  for (const PeerLink& p : s.peers)
  {
    if (p.slot >= 0 && !p.code.empty() && p.port == from.port &&
        std::find(p.ips.begin(), p.ips.end(), from.host) != p.ips.end() &&
        blocked.find(fmt::format(",{},", p.code)) != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

// Net thread, ENet's intercept: each datagram the session's host receives, before ENet reads it.
// It reads the acknowledgements in it (OnAck) and leaves the datagram to ENet as ever.
int ENET_CALLBACK InterceptAcks(ENetHost* host, ENetEvent*)
{
  if (TestBlocked(host->receivedAddress))
    return 1;
  const u8* d = host->receivedData;
  const size_t size = host->receivedDataLength;
  auto u16_at = [&](size_t at) { return static_cast<u16>((d[at] << 8) | d[at + 1]); };
  if (size < sizeof(enet_uint16))
    return 0;
  u16 peer_id = u16_at(0);
  const u16 flags = peer_id & ENET_PROTOCOL_HEADER_FLAG_MASK;
  peer_id &= ~(ENET_PROTOCOL_HEADER_FLAG_MASK | ENET_PROTOCOL_HEADER_SESSION_MASK);
  if ((flags & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED) || host->checksum ||
      peer_id >= host->peerCount)
  {
    return 0;
  }
  ENetPeer* e = &host->peers[peer_id];
  if (e->state != ENET_PEER_STATE_CONNECTED || e->address.host != host->receivedAddress.host ||
      e->address.port != host->receivedAddress.port)
  {
    return 0;
  }
  PeerLink* link = nullptr;
  for (PeerLink& p : s.peers)
  {
    if (p.peer == e && p.cc.peer == e)
      link = &p;
  }
  if (!link)
    return 0;
  size_t at = (flags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ?
                  sizeof(ENetProtocolHeader) :
                  offsetof(ENetProtocolHeader, sentTime);
  while (at + sizeof(ENetProtocolCommandHeader) <= size)
  {
    const u8 command = d[at] & ENET_PROTOCOL_COMMAND_MASK;
    if (command >= ENET_PROTOCOL_COMMAND_COUNT)
      break;
    const size_t n = enet_protocol_command_size(command);
    if (n == 0 || at + n > size)
      break;
    size_t data = 0;
    switch (command)
    {
    case ENET_PROTOCOL_COMMAND_ACKNOWLEDGE:
      OnAck(*link, host->serviceTime, d[at + offsetof(ENetProtocolCommandHeader, channelID)],
            u16_at(at + offsetof(ENetProtocolAcknowledge, receivedReliableSequenceNumber)),
            u16_at(at + offsetof(ENetProtocolAcknowledge, receivedSentTime)));
      break;
    case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
      data = u16_at(at + offsetof(ENetProtocolSendReliable, dataLength));
      break;
    case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
      data = u16_at(at + offsetof(ENetProtocolSendUnreliable, dataLength));
      break;
    case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
      data = u16_at(at + offsetof(ENetProtocolSendUnsequenced, dataLength));
      break;
    case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
    case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
      data = u16_at(at + offsetof(ENetProtocolSendFragment, dataLength));
      break;
    default:
      break;
    }
    at += n + data;
  }
  return 0;
}

// Net thread: the congestion window of what this machine sends a peer reliably, about as TCP's.
// From SEND_WINDOW_START it doubles each round trip that used it, until a round trip that shows
// congestion halves it (down to SEND_WINDOW_MIN); after that it grows by SEND_WINDOW_STEP a round
// trip, up to SEND_WINDOW. Congestion is a loss (ENet resending a packet) while the round trip is
// well above the link's lowest (a buffer on the way filling up), or SEND_LOSS_PERCENT of what was
// sent lost (a rate limit that drops with little buffer). A loss alone is not: halving on any
// loss, as TCP does, held an image to 1.2 MB/s with 1 % random loss (Wi-Fi), against 20 MB/s with
// the window fixed. "Well above" is the lowest of ENet's round trip over the last round trip
// (rtt_window), not its mean now, which jitter alone keeps above the lowest; and a resend the
// peer's acknowledgement shows was not needed is no loss (OnAck), nor is a cut whose losses all
// were such (it is undone).
// It only grows in a round trip whose reliable data had to wait for it, so it does not climb while
// idle and then burst. ENet sets a peer's window when it connects and again when any peer's
// bandwidth limits are recalculated (every new connection), so it is set back each time.
void PaceSendWindow(PeerLink& p, Clock::time_point now)
{
  ENetPeer* e = p.peer;
  if (!e || e->state != ENET_PEER_STATE_CONNECTED)
    return;
  // GekkoNet's packets go unsequenced (unreliable), and ENet drops unreliable packets itself
  // whenever a round trip comes in more than twice its deviation above the last ones (its packet
  // throttle, enet_peer_throttle): on Wi-Fi, or with a machine image or SD card chunks on the
  // link, it threw away a player's inputs for up to two seconds and both games froze until they
  // got through (bug reports #2 and #4, 2026-10-14: in every freeze both machines waited for the
  // other's inputs at once). GekkoNet resends its inputs itself and the session sends only what
  // it must, so the throttle stays open. PPR_MACHINE_ENET_THROTTLE=1 keeps ENet's, for comparisons.
  static const bool enet_throttle = std::getenv("PPR_MACHINE_ENET_THROTTLE") != nullptr;
  if (!enet_throttle)
    e->packetThrottle = e->packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;
  // PPR_MACHINE_FIXED_WINDOW=1 keeps the old fixed window, for comparisons.
  static const bool fixed = std::getenv("PPR_MACHINE_FIXED_WINDOW") != nullptr;
  if (fixed)
  {
    e->windowSize = std::max(e->windowSize, SEND_WINDOW);
    return;
  }
  auto& cc = p.cc;
  if (cc.peer != e)
  {
    cc = {};
    cc.peer = e;
    cc.seen_lost = e->packetsLost;
    cc.seen_sent = e->packetsSent;
    cc.step = now;
    cc.step_enet = enet_time_get();
  }
  ResendLost(p);
  // ENet counts what it sends and resends (packetsSent, packetsLost: commands) and zeroes both
  // every 10 s (ENET_PEER_PACKET_LOSS_INTERVAL).
  auto delta = [](enet_uint32 cur, enet_uint32* seen) {
    const enet_uint32 d = cur >= *seen ? cur - *seen : cur;
    *seen = cur;
    return d;
  };
  cc.lost += delta(e->packetsLost, &cc.seen_lost);
  cc.sent += delta(e->packetsSent, &cc.seen_sent);
  cc.limited |= !enet_list_empty(&e->outgoingSendReliableCommands);
  cc.min_rtt = std::min(cc.min_rtt, e->roundTripTime);
  const auto rtt = std::chrono::milliseconds(std::max<enet_uint32>(e->roundTripTime, 10));
  if (now - cc.step >= rtt)
  {
    // A queue building up on the way: the round trip well above the link's lowest all this round
    // trip.
    enet_uint32 recent = e->roundTripTime;
    for (auto it = p.rtt_window.rbegin(); it != p.rtt_window.rend() && now - it->first <= rtt; ++it)
      recent = std::min(recent, it->second);
    const bool queued = recent > cc.min_rtt + std::max<enet_uint32>(5, cc.min_rtt / 4);
    // The resends not shown needless yet.
    const enet_uint32 lost = cc.lost - std::min(cc.lost, cc.spurious);
    const bool congested = lost >= 4 && lost * 100 >= cc.sent * SEND_LOSS_PERCENT;
    const enet_uint32 before = cc.window;
    if (cc.undo_window != 0 && now > cc.undo_until)
      cc.undo_window = 0;
    if ((lost > 0 && queued) || congested)
    {
      // Undone if all of its losses (and those of any cut right after it) prove spurious.
      if (cc.undo_window == 0)
      {
        cc.undo_window = cc.window;
        cc.undo_slow_start = cc.slow_start;
        cc.undo_lost = 0;
      }
      cc.undo_lost += lost;
      cc.undo_until = now + std::max<Clock::duration>(4 * rtt, std::chrono::seconds(1));
      cc.window = std::max(SEND_WINDOW_MIN, cc.window / 2);
      cc.slow_start = false;
    }
    else if (cc.limited)
    {
      cc.window =
          std::min(SEND_WINDOW, cc.slow_start ? cc.window * 2 : cc.window + SEND_WINDOW_STEP);
    }
    // Diagnostics (PPR_MACHINE_CC_LOG=1): a line each round trip that sent anything.
    static const bool log = std::getenv("PPR_MACHINE_CC_LOG") != nullptr;
    if (log && cc.sent > 0)
    {
      INFO_LOG_FMT(BRAWLBACK,
                   "machine: cc slot {}: window {} -> {} KB, sent {} lost {} (needless {}), rtt {} "
                   "(low {}, recent low {}, var {}, high {}), in flight {} KB, throttle {}, RACK "
                   "resends {}, cuts undone {}{}{}",
                   p.slot, before / 1024, cc.window / 1024, cc.sent, cc.lost, cc.spurious,
                   e->roundTripTime, cc.min_rtt, recent, e->roundTripTimeVariance,
                   RecentHighRtt(cc, enet_time_get()), e->reliableDataInTransit / 1024,
                   e->packetThrottle, cc.rack_resent, cc.undone, queued ? ", queued" : "",
                   cc.limited ? ", limited" : "");
    }
    cc.step = now;
    cc.step_enet = enet_time_get();
    cc.limited = false;
    cc.lost = cc.sent = cc.spurious = 0;
  }
  e->windowSize = cc.window;
}

// The lowest of a window of round trips (ms), the old ones dropped.
template <typename T>
T RecentMin(std::deque<std::pair<Clock::time_point, T>>& samples, Clock::time_point now)
{
  while (!samples.empty() && now - samples.front().first > SD_DELAY_WINDOW)
    samples.pop_front();
  T low = samples.empty() ? T{} : samples.front().second;
  for (const auto& [t, v] : samples)
    low = std::min(low, v);
  return low;
}

// How much a link's round trip is over its lowest now (ms; net thread): the lowest of the last
// SD_DELAY_WINDOW of ENet's round trip against the lowest this connection has had. A queue on
// the way (a home uplink filling) raises it; jitter alone does not (the window's lowest stays low).
double LinkQueueMs(PeerLink& p, Clock::time_point now)
{
  if (!p.peer || p.cc.min_rtt == 0xffffffff || p.rtt_window.empty())
    return 0;
  return static_cast<double>(RecentMin(p.rtt_window, now)) - static_cast<double>(p.cc.min_rtt);
}

// A link's queue for PaceSD (ms): on a link without jitter, its lowest round trip of the last
// SD_CLEAN_DELAY_WINDOW over the connection's lowest; on a jittery one the lowest of its
// LinkQueueMs over the last SD_JITTER_WINDOW. LinkQueueMs sets the lowest of the last half second
// of ENet's mean round trip against the lowest the connection ever had, and jitter alone keeps
// the first well above the second, by an amount that wanders by tens of ms over a second or two:
// through netsim at 80 ms with pareto jitter of 25 ms, readings of 3 to 73 ms with the upload
// at its least, so PaceSD halved its rate at most steps and held it at 32 KB/s (the SD card's 2.3 MB
// list took 60 s, and the joiner gives up then). Jitter shows as round trips that change a lot
// from one send to the next (cc.jitter); a queue filling or draining moves them steadily, which
// does not count (SteadyQueueMs would hide it).
double SteadyQueueMs(PeerLink& p, Clock::time_point now)
{
  const double q = LinkQueueMs(p, now);
  auto& r = p.cc.queue_readings;
  while (!r.empty() && now - r.front().first > SD_JITTER_WINDOW)
    r.pop_front();
  if (!p.peer || p.cc.min_rtt == 0xffffffff)
    return q;
  if (!p.cc.jittery)
  {
    // Without jitter, the lowest of the last SD_CLEAN_DELAY_WINDOW: the half second's showed a
    // queue that had just begun half a second late, and the rate went a fifth past the uplink
    // meanwhile (netsim, an 8 Mbit/s uplink with a 200 ms buffer: game packets 195 ms late). The
    // congestion window used to hold that back by halving on its own needless resends.
    enet_uint32 low = 0xffffffff;
    for (auto it = p.rtt_window.rbegin();
         it != p.rtt_window.rend() && now - it->first <= SD_CLEAN_DELAY_WINDOW; ++it)
    {
      low = std::min(low, it->second);
    }
    if (low == 0xffffffff)
      return q;
    return std::max(0.0, static_cast<double>(low) - static_cast<double>(p.cc.min_rtt));
  }
  double low = q;
  for (const auto& [at, v] : r)
    low = std::min(low, v);
  return low;
}

// The SD card's upload to the joiner in `slot` (net thread, `net_mutex` held): its packets go out
// at a rate, a token bucket's, that grows while the way is clear and shrinks as soon as a queue
// shows: on the joiner's link or on the game's links to the other players (SteadyQueueMs: ENet's
// round trip, which the net thread measures apart from the game's frames, kept fresh by the
// chunks' acknowledgements and, on a game link, a ping every 50 ms). It keeps the upload a little under what the
// links can take (as a "background" transfer such as BitTorrent's uTP or an OS update does), so
// the players' game never waits for it. PPR_MACHINE_SD_PACE=0 sends at once (for comparisons);
// PPR_MACHINE_SD_PACE_LOG=1 logs each step.
void PaceSD(PeerLink& p, State::SDPace& pace, Clock::time_point now)
{
  static const bool off = [] {
    const char* e = std::getenv("PPR_MACHINE_SD_PACE");
    return e && *e == '0';
  }();
  static const bool log = std::getenv("PPR_MACHINE_SD_PACE_LOG") != nullptr;
  if (!p.peer || !p.hello || p.peer->state != ENET_PEER_STATE_CONNECTED)
  {
    pace = {};
    return;
  }
  auto send_raw = [&](const u8* data, size_t size) {
    enet_peer_send(p.peer, CHANNEL_SD, enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE));
    if (pace.sent == 0)
      pace.started = now;
    pace.sent += size;
    pace.last_send = now;
  };
  // The front packet's next slice (or all of a small one); true when the packet is done.
  auto send_next = [&]() {
    const std::vector<u8>& data = pace.queue.front();
    if (data.size() <= SD_PIECE_BYTES)
    {
      send_raw(data.data(), data.size());
    }
    else
    {
      const size_t n = std::min(SD_PIECE_BYTES, data.size() - pace.front_sent);
      std::vector<u8> piece(2 + n);
      piece[0] = SD_PIECE;
      piece[1] = pace.front_sent + n == data.size() ? 1 : 0;
      std::memcpy(piece.data() + 2, data.data() + pace.front_sent, n);
      send_raw(piece.data(), piece.size());
      pace.front_sent += n;
      if (pace.front_sent < data.size())
        return false;
    }
    pace.front_sent = 0;
    pace.queue.pop_front();
    return true;
  };
  if (off)
  {
    while (!pace.queue.empty())
    {
      send_raw(pace.queue.front().data(), pace.queue.front().size());
      pace.queue.pop_front();
    }
    return;
  }

  if (pace.filled == Clock::time_point{})
    pace.filled = pace.stepped = now;
  const double dt = std::chrono::duration<double>(now - pace.filled).count();
  pace.filled = now;
  // At most 50 ms of the rate ahead; a packet may take the bucket below zero (the manifest is one
  // of megabytes), and nothing else goes until it is paid back.
  pace.tokens = std::min(pace.tokens + pace.rate * dt, pace.rate * 0.05);
  while (!pace.queue.empty())
  {
    const size_t left = pace.queue.front().size() - pace.front_sent;
    const size_t size = std::min(left, SD_PIECE_BYTES);
    // Requests and refusals are small: they go at once.
    const bool small = pace.queue.front().size() <= 1024;
    // What waits in ENet's own queue (behind its window) would not be paced: a slice or two.
    // Only the rate's wait counts for its growth (`limited`): one that grew while the congestion
    // window held the upload back went far past what the way takes, and once the window opened
    // (it no longer halves on jitter, PaceSendWindow) the upload burst at it (netsim, an 8 Mbit/s
    // uplink: the rate at 1.5 MB/s, its 200 ms buffer full, game packets dropped).
    if (!small && (pace.tokens < 0 || QueuedReliable(p.peer, 4 * SD_PIECE_BYTES) >= 2 * SD_PIECE_BYTES))
    {
      pace.limited |= pace.tokens < 0;
      break;
    }
    if (!small)
      pace.tokens -= size;
    send_next();
  }

  if (now - pace.stepped < SD_PACE_STEP)
    return;
  const bool active = pace.limited || !pace.queue.empty() || now - pace.last_send < SD_PACE_STEP;
  if (!active)
  {
    if (pace.sent >= (1u << 20) && now - pace.last_send > std::chrono::seconds(5))
    {
      const double secs = std::chrono::duration<double>(pace.last_send - pace.started).count();
      AddLog(fmt::format("SD card: sent player {} {:.1f} MB in {:.1f} s ({:.2f} MB/s, slowed down "
                         "{} times)",
                         p.slot + 1, pace.sent / 1048576.0, secs,
                         secs > 0 ? pace.sent / 1048576.0 / secs : 0.0, pace.slowdowns));
      pace = {};
    }
    pace.stepped = now;
    return;
  }
  // The queue on the joiner's link, and the worst on the game's links to the other players.
  const double own = SteadyQueueMs(p, now);
  double game = -1;
  int game_slot = -1;
  for (PeerLink& g : s.peers)
  {
    if (g.slot < 0 || g.slot == p.slot || !g.peer || !g.hello || !Plays(g.slot))
      continue;
    const double q = SteadyQueueMs(g, now);
    if (q > game)
    {
      game = q;
      game_slot = g.slot;
    }
  }
  // The joiner's link crosses this machine's own uplink one way (the joiner sends back only
  // acknowledgements and requests): its queue is this upload's to keep at the target. A game
  // link's round trip also carries the other player's uplink (their own upload, which they pace),
  // so it only stops this one when it queues past three times the target.
  // PPR_MACHINE_SD_TARGET_MS=N: another target, for measurements.
  static const double target = [] {
    const char* e = std::getenv("PPR_MACHINE_SD_TARGET_MS");
    return e && std::atof(e) > 0 ? std::atof(e) : SD_TARGET_DELAY_MS;
  }();
  const double queued = game > 3 * target ? std::max(own, game) : own;
  const double before = pace.rate;
  // LEDBAT's rule (RFC 6817, as BitTorrent's uTP): the rate moves in proportion to how far the
  // queue is from the target, up while under it (only while the rate is what holds the upload
  // back), down while over it. The delays show a change a few hundred ms late, so a fast climb
  // would overshoot into a queue the game feels and halve, again and again: it climbs fast only
  // to 85 % of the rate a queue last showed at, then probes 0.2 % a step (about 8 s to that rate).
  const double off_target = std::clamp((target - queued) / target, -2.0, 1.0);
  if (queued > 3 * target)
  {
    if (now - pace.slowed >= SD_SLOWDOWN_HOLD)
    {
      pace.ceiling = pace.rate;
      pace.rate *= 0.5;
      pace.slowed = now;
      ++pace.slowdowns;
    }
  }
  else if (off_target < 0)
  {
    pace.rate *= 1 + 0.1 * off_target;
  }
  else if (pace.limited)
  {
    const bool probing = pace.ceiling > 0 && pace.rate >= 0.85 * pace.ceiling;
    pace.rate += off_target * std::max(pace.rate * (probing ? 0.002 : 0.04), probing ? 2048.0 : 16384.0);
    // Past the old ceiling with no queue: the way has room after all.
    pace.ceiling = std::max(pace.ceiling, pace.ceiling > 0 ? pace.rate : 0.0);
  }
  pace.rate = std::clamp(pace.rate, SD_RATE_MIN, SD_RATE_MAX);
  if (log)
  {
    INFO_LOG_FMT(BRAWLBACK,
                 "machine: SD pace to player {}: {:.0f} -> {:.0f} KB/s, queued {:.0f} ms (own link "
                 "{:.0f}, half a second {:.0f}, jitter {:.0f}; game link {} {:.0f}), {} packets "
                 "waiting, sent {:.1f} MB",
                 p.slot + 1, before / 1024, pace.rate / 1024, queued, own, LinkQueueMs(p, now),
                 p.cc.jitter, game_slot + 1, game, pace.queue.size(), pace.sent / 1048576.0);
  }
  pace.stepped = now;
  pace.limited = false;
}

constexpr auto DIRECT_SEND_HOLD = std::chrono::milliseconds(2);

// Under net_mutex: GekkoNet's queued packets to ENet (sent with the host's next flush or service).
void SendQueuedGekko(Clock::time_point now)
{
  for (const auto& at : s.gekko_tx_at)
  {
    const double w = std::chrono::duration<double, std::milli>(now - at).count();
    s.tx_wait_sum += w;
    s.tx_wait_max = std::max(s.tx_wait_max, w);
    ++s.tx_n;
  }
  s.gekko_tx_at.clear();
  for (auto& [slot, data] : s.gekko_tx)
  {
    PeerLink& p = s.peers[slot];
    if (!p.peer || !p.hello)
      continue;
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_UNSEQUENCED);
    enet_peer_send(p.peer, CHANNEL_GEKKO, packet);
  }
  s.gekko_tx.clear();
}

// Net thread, under net_mutex: a room member still without a link 10 s and 30 s after it came,
// once each, with how far this side got, so a joiner that never connects says why in its log
// (bug report #133: three REX joins with no "peer connected" and nothing else).
void LogNoLink(PeerLink& p, Clock::time_point now)
{
  if (p.slot < 0 || p.hello || p.link_wanted == Clock::time_point{} || p.no_link_logs >= 2)
    return;
  const auto after = std::chrono::seconds(p.no_link_logs == 0 ? 10 : 30);
  if (now - p.link_wanted < after)
    return;
  ++p.no_link_logs;
  const char* latest = !p.connecting                                  ? "none under way" :
                       p.connecting->state == ENET_PEER_STATE_CONNECTING ? "no answer" :
                       p.connecting->state == ENET_PEER_STATE_CONNECTED  ? "connected, no hello" :
                                                                           "answered, not connected";
  AddLog(fmt::format("no link to player {} ({}) at {}:{} after {} s: {} attempts from here "
                     "(latest: {}), {} hellos on the other connection (only the lower slot's is kept)",
                     p.slot + 1, p.code, p.host, p.port, after.count(), p.attempts, latest,
                     p.unkept_hellos));
}

void NetThread()
{
  Common::SetCurrentThreadName("Machine session");
  while (s.net_run.load())
  {
    std::unique_lock lk(s.net_mutex);
    if (!s.host)
      break;
    const auto now = Clock::now();
    for (PeerLink& p : s.peers)
    {
      PaceSendWindow(p, now);
      if (p.peer && p.peer->state == ENET_PEER_STATE_CONNECTED &&
          (p.rtt_window.empty() || now - p.rtt_window.back().first >= std::chrono::milliseconds(10)))
      {
        p.rtt_window.emplace_back(now, p.peer->roundTripTime);
        if (p.rtt_window.size() > 400)
          p.rtt_window.pop_front();
      }
      // SteadyQueueMs's readings.
      if (p.peer && p.peer->state == ENET_PEER_STATE_CONNECTED && p.cc.peer == p.peer &&
          (p.cc.queue_readings.empty() ||
           now - p.cc.queue_readings.back().first >= std::chrono::milliseconds(100)))
      {
        p.cc.queue_readings.emplace_back(now, LinkQueueMs(p, now));
        if (p.cc.queue_readings.size() > 64)
          p.cc.queue_readings.pop_front();
      }
    }
    // Every 10 s, each link as ENet measures it: what a bug report needs to tell a bad link
    // (round trips jumping, loss) from a machine that stalled.
    static Clock::time_point last_link_log;
    if (now - last_link_log >= std::chrono::seconds(10))
    {
      last_link_log = now;
      for (const PeerLink& p : s.peers)
      {
        const ENetPeer* e = p.peer;
        if (!e || e->state != ENET_PEER_STATE_CONNECTED || !p.hello)
          continue;
        INFO_LOG_FMT(BRAWLBACK,
                     "machine: link to player {}: rtt {} ms (lowest {}, deviation {}, highest "
                     "deviation {}), loss {:.1f}%, throttle {}/{}, window {} KB",
                     p.slot + 1, e->roundTripTime, e->lowestRoundTripTime,
                     e->roundTripTimeVariance, e->highestRoundTripTimeVariance,
                     e->packetLoss * 100.0 / static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE),
                     e->packetThrottle, static_cast<int>(ENET_PEER_PACKET_THROTTLE_SCALE),
                     e->windowSize / 1024);
      }
    }
    // Outgoing: GekkoNet's packets queued by the CPU thread.
    if (!s.direct_send ||
        (!s.gekko_tx_at.empty() && now - s.gekko_tx_at.front() >= DIRECT_SEND_HOLD))
    {
      SendQueuedGekko(now);
    }
    // A machine image for a joiner: a few chunks at a time, so ENet's queue stays short.
    if (!s.image_tx.empty() && s.image_to >= 0)
    {
      PeerLink& p = s.peers[s.image_to];
      if (!p.peer || !p.hello)
      {
        s.image_tx.clear();
      }
      else
      {
        // Diagnostics (PPR_MACHINE_IMAGE_KBPS=N): the image goes out at N KiB/s, as over a slow
        // link, so a join can take longer than the connect limits.
        static const double image_kbps = [] {
          const char* e = std::getenv("PPR_MACHINE_IMAGE_KBPS");
          return e ? std::atof(e) : 0.0;
        }();
        static Clock::time_point image_since;
        if (s.image_tx_sent == 0)
          image_since = Clock::now();
        const double image_allowed =
            image_kbps > 0 ?
                IMAGE_CHUNK + image_kbps * 1024 *
                                  std::chrono::duration<double>(Clock::now() - image_since).count() :
                1e18;
        // Only another chunk once ENet has sent most of the last: what it sends goes at the pace
        // of the peer's congestion window (PaceSendWindow), and what waits in its queue holds up
        // the SD card's chunks behind it.
        for (int n = 0; n < 8 && s.image_tx_sent < s.image_tx.size() &&
                        s.image_tx_sent < image_allowed &&
                        QueuedReliable(p.peer, IMAGE_CHUNK) < IMAGE_CHUNK;
             ++n)
        {
          const size_t len = std::min(IMAGE_CHUNK, s.image_tx.size() - s.image_tx_sent);
          enet_peer_send(p.peer, CHANNEL_IMAGE,
                         enet_packet_create(s.image_tx.data() + s.image_tx_sent, len,
                                            ENET_PACKET_FLAG_RELIABLE));
          s.image_tx_sent += len;
        }
        if (s.image_tx_sent >= s.image_tx.size())
        {
          s.image_tx.clear();
          s.image_tx.shrink_to_fit();
        }
      }
    }
    // The SD card's chunk requests and chunks, each player's at its pace (PaceSD).
    for (auto& [slot, data] : s.sd_tx)
    {
      if (slot >= 0 && slot < MAX_PLAYERS && s.peers[slot].peer && s.peers[slot].hello)
        s.sd_pace[slot].queue.push_back(std::move(data));
    }
    s.sd_tx.clear();
    for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    {
      if (s.peers[slot].slot == slot)
        PaceSD(s.peers[slot], s.sd_pace[slot], now);
    }
    // Keep connecting to every peer without a link (both sides do).
    StepProbe(now);
    for (PeerLink& p : s.peers)
    {
      if (p.slot < 0 || p.hello || p.host.empty() || now - p.last_connect < std::chrono::seconds(1))
        continue;
      p.last_connect = now;
      // One attempt at a time: a peer that is not there yet (a joiner that has not started)
      // would otherwise collect an attempt a second until the host has no peers left.
      if (p.connecting && p.connecting != p.peer && p.connecting->state != ENET_PEER_STATE_CONNECTED)
      {
        p.connecting->data = nullptr;
        enet_peer_reset(p.connecting);
      }
      p.connecting = nullptr;
      ENetAddress addr{};
      if (enet_address_set_host(&addr, p.host.c_str()) != 0)
        continue;
      addr.port = p.port;
      ENetPeer* peer = enet_host_connect(s.host, &addr, CHANNEL_COUNT, 0);
      if (peer)
      {
        peer->data = reinterpret_cast<void*>(static_cast<intptr_t>(p.slot + 1) | OUTGOING);
        p.connecting = peer;
        ++p.attempts;
      }
    }
    for (PeerLink& p : s.peers)
      LogNoLink(p, now);
    ENetEvent ev;
    int r = enet_host_service(s.host, &ev, 0);
    while (r > 0)
    {
      switch (ev.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
      {
        if (ev.peer == s.probe.peer)
        {
          SendProbe(ev.peer);
          break;
        }
        // ENet hands out its peers again without clearing `data`: a connection this side did not
        // start learns its slot from its hello, whatever an earlier one left there.
        bool outgoing = false;
        for (const PeerLink& p : s.peers)
          outgoing |= p.connecting == ev.peer;
        if (!outgoing)
          ev.peer->data = nullptr;
        enet_peer_timeout(ev.peer, 0, 5000, 10000);
        // A round trip every 50 ms on a link with nothing reliable to send (a game link: GekkoNet's
        // packets are unsequenced), so PaceSD sees a queue building on it within a few hundred ms.
        enet_peer_ping_interval(ev.peer, 50);
        // The peer's side of the same (PaceSendWindow): ENet sends this to the peer, which then
        // never lowers its throttle towards this machine either (an older build included).
        if (!std::getenv("PPR_MACHINE_ENET_THROTTLE"))
        {
          enet_peer_throttle_configure(ev.peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                       ENET_PEER_PACKET_THROTTLE_SCALE, 0);
        }
        picojson::object hello;
        hello["type"] = picojson::value("hello");
        hello["slot"] = picojson::value(static_cast<double>(s.opts.local_slot));
        hello["name"] = picojson::value(s.opts.name);
        hello["playing"] = picojson::value(LocalPlays());
        hello["proto"] = picojson::value(static_cast<double>(PROTOCOL_VERSION));
        hello["sd_card"] = picojson::value(Config::Get(Config::MAIN_WII_SD_CARD));
        hello["sd"] = picojson::value(s.sd_root);
        const std::string text = picojson::value(hello).serialize();
        enet_peer_send(ev.peer, CHANNEL_CONTROL,
                       enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE));
        break;
      }
      case ENET_EVENT_TYPE_RECEIVE:
      {
        if (ev.peer == s.probe.peer)
          OnProbeAnswer(ev.peer, ev.packet->data, ev.packet->dataLength);
        else if (ev.channelID == CHANNEL_CONTROL)
        {
          // An incoming connection learns its slot from the hello.
          if (ev.peer->data == nullptr)
          {
            picojson::value v;
            const std::string text(reinterpret_cast<const char*>(ev.packet->data),
                                   std::min<size_t>(ev.packet->dataLength, MAX_CONTROL_SIZE));
            if (picojson::parse(v, text).empty() && v.is<picojson::object>())
            {
              const auto& o = v.get<picojson::object>();
              const auto it = o.find("slot");
              if (it != o.end() && it->second.is<double>())
              {
                const double sl = it->second.get<double>();
                if (sl >= 0 && sl < MAX_PLAYERS)
                  ev.peer->data = reinterpret_cast<void*>(static_cast<intptr_t>(sl) + 1);
              }
            }
          }
          HandleControl(ev.peer, ev.packet->data, ev.packet->dataLength);
        }
        else if (ev.channelID == CHANNEL_IMAGE && ev.packet->dataLength <= IMAGE_CHUNK)
        {
          const int slot = SlotOf(ev.peer);
          if (slot >= 0 && slot < MAX_PLAYERS && s.peers[slot].peer == ev.peer)
            HandleImagePacket(slot, ev.packet->data, ev.packet->dataLength);
        }
        else if (ev.channelID == CHANNEL_SD && ev.packet->dataLength <= MAX_SD_MANIFEST_PACKET)
        {
          const int slot = SlotOf(ev.peer);
          if (slot >= 0 && slot < MAX_PLAYERS && s.peers[slot].peer == ev.peer &&
              s.peers[slot].hello)
          {
            HandleSDPacket(slot, ev.packet->data, ev.packet->dataLength);
          }
        }
        else if (ev.channelID == CHANNEL_GEKKO && ev.packet->dataLength <= MAX_GEKKO_PACKET)
        {
          const int slot = SlotOf(ev.peer);
          if (slot >= 0 && slot < MAX_PLAYERS && s.peers[slot].peer == ev.peer)
          {
            s.gekko_rx.emplace_back(slot, std::vector<u8>(ev.packet->data,
                                                          ev.packet->data + ev.packet->dataLength));
            if (s.gekko_rx.size() > 4096)
              s.gekko_rx.pop_front();
          }
        }
        enet_packet_destroy(ev.packet);
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT:
      {
        if (ev.peer == s.probe.peer)
        {
          s.probe.peer = nullptr;
          ev.peer->data = nullptr;
          break;
        }
        for (PeerLink& p : s.peers)
        {
          if (p.connecting == ev.peer)
            p.connecting = nullptr;
          if (p.peer == ev.peer)
          {
            p.peer = nullptr;
            p.hello = false;
            if (s.phase == Phase::Running || s.phase == Phase::Starting ||
                s.phase == Phase::Epoch)
              p.left = true;
            AddLog(fmt::format("peer {} disconnected", p.slot));
          }
        }
        ev.peer->data = nullptr;
        break;
      }
      default:
        break;
      }
      r = enet_host_check_events(s.host, &ev);
    }
    for (PeerLink& p : s.peers)
    {
      if (!p.peer)
        continue;
      p.rtt_ms = p.peer->roundTripTime;
      if (p.min_rtt_ms < 0 || p.rtt_ms < p.min_rtt_ms)
        p.min_rtt_ms = p.rtt_ms;
    }
    ENetSocket sock = s.host->socket;
    lk.unlock();
    if (s.direct_send)
    {
      // Wakes when a packet comes, else after 1 ms (ENet's resends, pings, the queue's hold).
      enet_uint32 cond = ENET_SOCKET_WAIT_RECEIVE;
      enet_socket_wait(sock, &cond, 1);
    }
    else
    {
      // ~1 ms: GekkoNet's packets go out at most this late.
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    {
      static Clock::time_point last{};
      const auto t = Clock::now();
      if (last != Clock::time_point{})
      {
        const double gap = std::chrono::duration<double, std::milli>(t - last).count();
        std::lock_guard lk2(s.net_mutex);
        s.loop_gap_max = std::max(s.loop_gap_max, gap);
      }
      last = t;
    }
  }
}

bool StartNetwork()
{
  std::lock_guard lk(s.net_mutex);
  if (s.opts.synctest > 0)
    return true;
  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  addr.port = s.opts.local_port;
  s.host = enet_host_create(&addr, MAX_PLAYERS * 4, CHANNEL_COUNT, 0, 0);
  if (!s.host)
  {
    s.error = fmt::format("could not bind UDP port {}", s.opts.local_port);
    return false;
  }
  s.host->intercept = InterceptAcks;
  s.net_run = true;
  s.net_thread = std::thread(NetThread);
  return true;
}

void StopNetwork()
{
  s.net_run = false;
  if (s.net_thread.joinable())
    s.net_thread.join();
  std::lock_guard lk(s.net_mutex);
  s.probe.peer = nullptr;
  if (s.host)
  {
    for (PeerLink& p : s.peers)
    {
      if (p.peer)
        enet_peer_disconnect_now(p.peer, 0);
      p.peer = nullptr;
      p.hello = false;
    }
    enet_host_flush(s.host);
    enet_host_destroy(s.host);
    s.host = nullptr;
  }
  s.gekko_rx.clear();
  s.gekko_tx.clear();
}

// ------------------------------------------------------------------------------ GekkoNet adapter

void GekkoSend(GekkoNetAddress* addr, const char* data, int length)
{
  if (!addr || !addr->data || addr->size != 2 || length <= 0)
    return;
  const char* a = static_cast<const char*>(addr->data);
  const int slot = a[1] - '0';
  if (slot < 0 || slot >= MAX_PLAYERS)
    return;
  std::lock_guard lk(s.net_mutex);
  s.gekko_tx.emplace_back(slot, std::vector<u8>(data, data + length));
  s.gekko_tx_at.push_back(Clock::now());
}

GekkoNetResult** GekkoReceive(int* length)
{
  std::lock_guard lk(s.net_mutex);
  s.gekko_rx_ptrs.clear();
  for (auto& [slot, data] : s.gekko_rx)
  {
    // GekkoNet frees every pointer it is handed with free_data (GekkoFree).
    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = 2;
    result->addr.data = std::malloc(2);
    std::memcpy(result->addr.data, s_gekko_addr[slot], 2);
    result->data_len = static_cast<unsigned int>(data.size());
    result->data = std::malloc(data.size());
    std::memcpy(result->data, data.data(), data.size());
    s.gekko_rx_ptrs.push_back(result);
  }
  s.gekko_rx.clear();
  *length = static_cast<int>(s.gekko_rx_ptrs.size());
  return s.gekko_rx_ptrs.data();
}

void GekkoFree(void* p)
{
  std::free(p);
}

void DestroyGekko()
{
  if (s.gekko)
    gekko_destroy(&s.gekko);
  s.gekko = nullptr;
  s.gekko_started = false;
  s.handle_slot.fill(-1);
  s.slot_handle.fill(-1);
  s.local_handle = -1;
}

// This player's input delay for the GekkoNet session starting now: the set value, else from the
// round trip to the furthest player in it, by the player's profile as P+ picks it
// (Gprb::Session::ProfileInputDelay, without P+'s agreement between players); none
// measured (no other player yet): P+'s default. Each connection's lowest round trip: a join's
// machine image has just been sent when an epoch starts, and the inputs waiting behind it are still
// in ENet's mean round trip.
int PickInputDelay(double* rtt_ms)
{
  *rtt_ms = -1;
  if (s.opts.delay >= 0)
    return s.opts.delay;
  {
    std::lock_guard lk(s.net_mutex);
    for (const PeerLink& p : s.peers)
    {
      if (p.slot >= 0 && ((s.players >> p.slot) & 1) && !p.left && p.min_rtt_ms >= 0)
        *rtt_ms = std::max(*rtt_ms, p.min_rtt_ms);
    }
  }
  return Gprb::Session::ProfileInputDelay(AutoDelayLevel(s.opts.delay),
                                          Gprb::Session::GameRoundTrip(*rtt_ms));
}

// A Wii Remote at rest, no extension (the struct has no assignment).
void ResetWiimoteState(WiimoteEmu::DesiredWiimoteState* state)
{
  state->buttons = {};
  state->acceleration = WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION;
  state->camera_points = WiimoteEmu::DesiredWiimoteState::DEFAULT_CAMERA;
  state->motion_plus.reset();
  state->extension.data = std::monostate{};
}

void EncodeWiimote(const WiimoteEmu::DesiredWiimoteState& state, Input* in)
{
  const WiimoteEmu::SerializedWiimoteState ser = WiimoteEmu::SerializeDesiredState(state);
  const u8 len = std::min<u8>(ser.length, 31);
  (*in)[0] |= INPUT_WIIMOTE_CONNECTED;
  (*in)[9] = len;
  std::memcpy(in->data() + 10, ser.data.data(), len);
}

// A controller of the kind plugged in (a Wii Remote connected) and at rest.
Input RestingInput(Controller kind)
{
  Input in{};
  if (kind == Controller::WiiRemote)
  {
    WiimoteEmu::DesiredWiimoteState state;
    ResetWiimoteState(&state);
    EncodeWiimote(state, &in);
    return in;
  }
  GCPadStatus pad{};
  pad.stickX = GCPadStatus::MAIN_STICK_CENTER_X;
  pad.stickY = GCPadStatus::MAIN_STICK_CENTER_Y;
  pad.substickX = GCPadStatus::C_STICK_CENTER_X;
  pad.substickY = GCPadStatus::C_STICK_CENTER_Y;
  EncodePad(pad, &in);
  return in;
}

// GekkoNet's local delay for an input delay: one less with late input (the local input is read in
// the frame before the one whose boundary would have read it).
int GekkoDelayFor(int delay)
{
  return s.late_input ? std::max(delay - 1, 0) : delay;
}

bool CreateGekko()
{
  DestroyGekko();
  const bool stress = s.opts.synctest > 0;
  if (!gekko_create(&s.gekko, stress ? GekkoStressSession : GekkoGameSession))
    return false;
  int n = 0;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    n += (s.players >> slot) & 1;
  GekkoConfig cfg{};
  cfg.num_players = static_cast<unsigned char>(n);
  cfg.max_spectators = 0;
  cfg.input_prediction_window = MAX_ROLLBACK_FRAMES;
  cfg.input_size = INPUT_SIZE;
  cfg.state_size = sizeof(u32);
  cfg.limited_saving = false;
  cfg.desync_detection = true;
  cfg.check_distance = stress ? static_cast<unsigned int>(s.opts.synctest) : 0;
  gekko_start(s.gekko, &cfg);
  s.next_ping_display = Clock::now() + std::chrono::seconds(1);
  if (!stress)
  {
    s.adapter.send_data = GekkoSend;
    s.adapter.receive_data = GekkoReceive;
    s.adapter.free_data = GekkoFree;
    gekko_net_adapter_set(s.gekko, &s.adapter);
  }
  // Handles follow the slots in order, the same on every machine.
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (!((s.players >> slot) & 1))
      continue;
    int h;
    if (slot == s.opts.local_slot)
    {
      h = gekko_add_actor(s.gekko, GekkoLocalPlayer, nullptr);
      s.local_handle = h;
    }
    else
    {
      GekkoNetAddress addr{const_cast<char*>(s_gekko_addr[slot]), 2};
      h = gekko_add_actor(s.gekko, GekkoRemotePlayer, &addr);
    }
    if (h < 0 || h >= MAX_PLAYERS)
    {
      DestroyGekko();
      return false;
    }
    s.handle_slot[h] = slot;
    s.slot_handle[slot] = h;
  }
  if (!stress)
  {
    double rtt = -1;
    s.input_delay = PickInputDelay(&rtt);
    {
      const char* env = std::getenv("PPR_MACHINE_LATE_INPUT");
      s.late_input = env && env[0] == '1';  // off unless PPR_MACHINE_LATE_INPUT=1 (the boundary sleep took its place)
      const char* direct = std::getenv("PPR_MACHINE_DIRECT_SEND");
      s.direct_send = s.late_input && direct && direct[0] == '1';
      s.consumed = {};
      AddLog(fmt::format("late input {}", s.late_input ? "on" : "off"));
      gekko_set_advantage_delay_bias(s.gekko, s.late_input ? 1 : 0);
    }
    gekko_set_local_delay(s.gekko, s.local_handle,
                          static_cast<unsigned char>(GekkoDelayFor(s.input_delay)));
    AddLog(
        s.opts.delay >= 0 ? fmt::format("input delay {} (set)", s.input_delay) :
        rtt < 0           ? fmt::format("input delay {} (auto, no round trip yet)", s.input_delay) :
                  fmt::format("input delay {} (auto, round trip {:.0f} ms)", s.input_delay, rtt));
    gekko_set_disconnect_timeout(s.gekko, DISCONNECT_TIMEOUT_MS);
    static const Input unplugged{};
    gekko_set_disconnected_input(s.gekko, unplugged.data());
    // After an epoch, the frames this player's delay holds back repeat its input of the frame
    // before (as every machine has it), instead of nothing: its controller stays plugged in.
    if (s.frame_base > 0)
    {
      const HistoryEntry& last =
          s.history[static_cast<size_t>(s.frame_base - 1) % HISTORY_SIZE];
      if (last.frame == s.frame_base - 1)
      {
        Input fill = last.inputs[s.opts.local_slot];
        fill[INPUT_REQUEST] = 0;
        fill[INPUT_MOVE] = 0;
        gekko_set_local_delay_input(s.gekko, s.local_handle, fill.data());
      }
    }
    // After a boot this player played: its controller at rest, so it is not unplugged for the
    // first frames (a Wii Remote would disconnect and connect again).
    else if (s.boot_local)
    {
      const Input fill = RestingInput(s.controller[s.opts.local_slot]);
      gekko_set_local_delay_input(s.gekko, s.local_handle, fill.data());
    }
  }
  else
  {
    s.gekko_started = true;
  }
  return true;
}

// ------------------------------------------------------------------------------ frame loop

// ------------------------------------------------------------------------------ local controller

// How often (in captures, frames) the local controller is picked again: one plugged in or
// unplugged is taken within half a second.
constexpr u32 PICK_EVERY = 30;

// Dolphin's "Wii Remote with MotionPlus Pointing" profile (Data/Sys/Profiles/Wiimote), pointing
// with the real Wii Remote's own IR camera (the sensor bar) instead of its MotionPlus: every
// button, the accelerometer, a MotionPlus and the extension plugged into it as they are.
constexpr std::pair<const char*, const char*> REAL_WIIMOTE_PROFILE[] = {
    {"Device", "Bluetooth/0/Wii Remote"},
    {"Buttons/A", "`A`"},
    {"Buttons/B", "`B`"},
    {"Buttons/1", "`1`"},
    {"Buttons/2", "`2`"},
    {"Buttons/-", "`-`"},
    {"Buttons/+", "`+`"},
    {"Buttons/Home", "`HOME`"},
    {"D-Pad/Up", "`Up`"},
    {"D-Pad/Down", "`Down`"},
    {"D-Pad/Left", "`Left`"},
    {"D-Pad/Right", "`Right`"},
    {"IRPassthrough/Enabled", "True"},
    {"IRPassthrough/Object 1 X", "`IR Object 1 X`"},
    {"IRPassthrough/Object 1 Y", "`IR Object 1 Y`"},
    {"IRPassthrough/Object 1 Size", "`IR Object 1 Size`"},
    {"IRPassthrough/Object 2 X", "`IR Object 2 X`"},
    {"IRPassthrough/Object 2 Y", "`IR Object 2 Y`"},
    {"IRPassthrough/Object 2 Size", "`IR Object 2 Size`"},
    {"IRPassthrough/Object 3 X", "`IR Object 3 X`"},
    {"IRPassthrough/Object 3 Y", "`IR Object 3 Y`"},
    {"IRPassthrough/Object 3 Size", "`IR Object 3 Size`"},
    {"IRPassthrough/Object 4 X", "`IR Object 4 X`"},
    {"IRPassthrough/Object 4 Y", "`IR Object 4 Y`"},
    {"IRPassthrough/Object 4 Size", "`IR Object 4 Size`"},
    {"IMUAccelerometer/Up", "`Accel Up`"},
    {"IMUAccelerometer/Down", "`Accel Down`"},
    {"IMUAccelerometer/Left", "`Accel Left`"},
    {"IMUAccelerometer/Right", "`Accel Right`"},
    {"IMUAccelerometer/Forward", "`Accel Forward`"},
    {"IMUAccelerometer/Backward", "`Accel Backward`"},
    {"IMUGyroscope/Dead Zone", "3"},
    {"IMUGyroscope/Pitch Up", "`Gyro Pitch Up`"},
    {"IMUGyroscope/Pitch Down", "`Gyro Pitch Down`"},
    {"IMUGyroscope/Roll Left", "`Gyro Roll Left`"},
    {"IMUGyroscope/Roll Right", "`Gyro Roll Right`"},
    {"IMUGyroscope/Yaw Left", "`Gyro Yaw Left`"},
    {"IMUGyroscope/Yaw Right", "`Gyro Yaw Right`"},
    {"IMUIR/Enabled", "False"},
    {"Extension/Attach MotionPlus", "`Attached MotionPlus`"},
    {"Extension", "`Attached Extension`"},
    {"Nunchuk/Buttons/C", "`Nunchuk C`"},
    {"Nunchuk/Buttons/Z", "`Nunchuk Z`"},
    {"Nunchuk/Stick/Up", "`Nunchuk Y+`"},
    {"Nunchuk/Stick/Down", "`Nunchuk Y-`"},
    {"Nunchuk/Stick/Left", "`Nunchuk X-`"},
    {"Nunchuk/Stick/Right", "`Nunchuk X+`"},
    {"Nunchuk/Stick/Calibration", "100 100 100 100 100 100 100 100"},
    {"Nunchuk/IMUAccelerometer/Up", "`Nunchuk Accel Up`"},
    {"Nunchuk/IMUAccelerometer/Down", "`Nunchuk Accel Down`"},
    {"Nunchuk/IMUAccelerometer/Left", "`Nunchuk Accel Left`"},
    {"Nunchuk/IMUAccelerometer/Right", "`Nunchuk Accel Right`"},
    {"Nunchuk/IMUAccelerometer/Forward", "`Nunchuk Accel Forward`"},
    {"Nunchuk/IMUAccelerometer/Backward", "`Nunchuk Accel Backward`"},
    {"Classic/Buttons/A", "`Classic A`"},
    {"Classic/Buttons/B", "`Classic B`"},
    {"Classic/Buttons/X", "`Classic X`"},
    {"Classic/Buttons/Y", "`Classic Y`"},
    {"Classic/Buttons/ZL", "`Classic ZL`"},
    {"Classic/Buttons/ZR", "`Classic ZR`"},
    {"Classic/Buttons/-", "`Classic -`"},
    {"Classic/Buttons/+", "`Classic +`"},
    {"Classic/Buttons/Home", "`Classic HOME`"},
    {"Classic/Left Stick/Up", "`Classic Left Y+`"},
    {"Classic/Left Stick/Down", "`Classic Left Y-`"},
    {"Classic/Left Stick/Left", "`Classic Left X-`"},
    {"Classic/Left Stick/Right", "`Classic Left X+`"},
    {"Classic/Left Stick/Calibration", "100 100 100 100 100 100 100 100"},
    {"Classic/Right Stick/Up", "`Classic Right Y+`"},
    {"Classic/Right Stick/Down", "`Classic Right Y-`"},
    {"Classic/Right Stick/Left", "`Classic Right X-`"},
    {"Classic/Right Stick/Right", "`Classic Right X+`"},
    {"Classic/Right Stick/Calibration", "100 100 100 100 100 100 100 100"},
    {"Classic/Triggers/L", "`Classic L`"},
    {"Classic/Triggers/R", "`Classic R`"},
    {"Classic/Triggers/L-Analog", "`Classic L-Analog`"},
    {"Classic/Triggers/R-Analog", "`Classic R-Analog`"},
    {"Classic/D-Pad/Up", "`Classic Up`"},
    {"Classic/D-Pad/Down", "`Classic Down`"},
    {"Classic/D-Pad/Left", "`Classic Left`"},
    {"Classic/D-Pad/Right", "`Classic Right`"},
    {"Rumble/Motor", "`Motor`"},
    {"Options/Battery", "`Battery`"},
};

bool DevicePresent(const std::string& name)
{
  if (name.empty())
    return false;
  ciface::Core::DeviceQualifier q;
  q.FromString(name);
  return g_controller_interface.FindDevice(q) != nullptr;
}

// The real Wii Remotes in Dolphin's controller interface ("Connect Wii Remotes for Emulated
// Controllers", which the session turns on), in its order.
std::vector<std::string> RealWiimoteDevices()
{
  std::vector<std::string> out;
  for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
  {
    if (d.starts_with("Bluetooth/") && d.ends_with("/Wii Remote"))
      out.push_back(d);
  }
  return out;
}

// From the player's own settings (the base layer: the session's layer makes every port a standard
// controller and every Wii Remote an emulated one): the first controller of the kind set up that
// is plugged in, else the first set up, else port 1 / Wii Remote 1. Wii Remote N set up as a real
// one is the Nth real Wii Remote connected (in Dolphin's order), else the next one: two Dolphins
// on one computer (Wii Remote 1 and Wii Remote 2) each play their own.
LocalControllerInfo PickLocal(Controller kind)
{
  std::optional<LocalControllerInfo> first;
  if (kind == Controller::GameCube)
  {
    for (int i = 0; i < 4; ++i)
    {
      LocalControllerInfo p;
      p.controller = kind;
      p.index = i;
      const auto si = Config::GetBase(Config::GetInfoForSIDevice(i));
      if (si == SerialInterface::SIDEVICE_WIIU_ADAPTER)
      {
        p.adapter = true;
        p.device = "GameCube adapter";
        p.plugged = GCAdapter::DeviceConnected(i);
      }
      else if (si == SerialInterface::SIDEVICE_GC_CONTROLLER)
      {
        p.device = Pad::GetConfig()->GetController(i)->GetDefaultDevice().ToString();
        p.plugged = DevicePresent(p.device);
      }
      else
      {
        continue;
      }
      if (p.plugged)
        return p;
      if (!first)
        first = p;
    }
  }
  else
  {
    const std::vector<std::string> reals = RealWiimoteDevices();
    size_t real_n = 0;
    for (int i = 0; i < 4; ++i)
    {
      LocalControllerInfo p;
      p.controller = kind;
      p.index = i;
      const WiimoteSource source = Config::GetBase(Config::GetInfoForWiimoteSource(i));
      if (source == WiimoteSource::Emulated)
      {
        p.device = ::Wiimote::GetConfig()->GetController(i)->GetDefaultDevice().ToString();
        p.plugged = DevicePresent(p.device);
      }
      else if (source == WiimoteSource::Real)
      {
        p.real = true;
        const size_t n = static_cast<size_t>(i) < reals.size() ? static_cast<size_t>(i) : real_n;
        p.plugged = n < reals.size();
        p.device = p.plugged ? reals[n] : fmt::format("Bluetooth/{}/Wii Remote", i);
        ++real_n;
      }
      else
      {
        continue;
      }
      if (p.plugged)
        return p;
      if (!first)
        first = p;
    }
  }
  if (first)
    return *first;
  LocalControllerInfo p;
  p.controller = kind;
  p.adapter = kind == Controller::GameCube &&
              Config::GetBase(Config::GetInfoForSIDevice(0)) == SerialInterface::SIDEVICE_WIIU_ADAPTER;
  return p;
}

// CPU thread: the emulated Wii Remote a real one drives, bound to its device.
void UseRealWiimote(const std::string& device)
{
  if (!s.real_wiimote)
  {
    s.real_wiimote = std::make_unique<WiimoteEmu::Wiimote>(0);
    Common::IniFile ini;
    Common::IniFile::Section* section = ini.GetOrCreateSection("Profile");
    for (const auto& [key, value] : REAL_WIIMOTE_PROFILE)
      section->Set(key, std::string(value));
    s.real_wiimote->LoadConfig(section);
    s.real_wiimote_device.clear();
  }
  if (device != s.real_wiimote_device)
  {
    s.real_wiimote->SetDefaultDevice(device);
    s.real_wiimote_device = device;
  }
  // Binds its controls to the device (again: it may have connected since).
  s.real_wiimote->UpdateReferences(g_controller_interface);
}

std::string DescribeLocal(const LocalControllerInfo& p)
{
  const char* kind = p.controller == Controller::WiiRemote ? "Wii Remote" : "GameCube controller";
  if (p.device.empty())
    return fmt::format("{} (none set up in Dolphin)", kind);
  return fmt::format("{} {} ({}{})", kind, p.index + 1, p.device,
                     p.plugged ? "" : ", not plugged in");
}

// CPU thread, before each capture: a switch the launcher asked for, and the controller to read.
void UpdateLocalController()
{
  const int requested = s.requested_controller.exchange(-1);
  bool now = s.pick_countdown == 0;
  if (requested >= 0)
  {
    const Controller c = static_cast<Controller>(requested);
    if (c != s.controller[s.opts.local_slot])
    {
      s.opts.controller = c;
      s.controller[s.opts.local_slot] = c;
      AddLog(fmt::format("controller: now a {}",
                         c == Controller::WiiRemote ? "Wii Remote" : "GameCube controller"));
    }
    now = true;
  }
  if (!now)
  {
    // Counted in frames: a frame waits for GekkoNet in many captures.
    if (s.current_frame != s.pick_frame)
    {
      s.pick_frame = s.current_frame;
      --s.pick_countdown;
    }
    return;
  }
  s.pick_countdown = PICK_EVERY;
  s.pick_frame = s.current_frame;
  LocalControllerInfo p;
  if (s.opts.auto_pad)
  {
    p = PickLocal(s.controller[s.opts.local_slot]);
    s.opts.local_pad = p.index;
    s.local_adapter = p.adapter;
  }
  else
  {
    p.controller = s.controller[s.opts.local_slot];
    p.index = s.opts.local_pad;
    p.adapter = s.local_adapter;
    p.plugged = true;
  }
  s.use_real_wiimote = p.controller == Controller::WiiRemote && p.real;
  if (s.use_real_wiimote)
    UseRealWiimote(p.device);
  std::lock_guard lk(s.pick_mutex);
  s.picked = true;
  if (p.controller != s.pick.controller || p.index != s.pick.index || p.device != s.pick.device ||
      p.plugged != s.pick.plugged)
  {
    AddLog(fmt::format("plays with: {}", DescribeLocal(p)));
    if (p.controller == Controller::GameCube && p.adapter)
    {
      if (const auto o = GCAdapter::Origin(p.index))
      {
        AddLog(fmt::format("controller at rest: stick {},{} C-stick {},{} triggers {},{}", o->stickX,
                           o->stickY, o->substickX, o->substickY, o->triggerLeft,
                           o->triggerRight));
      }
    }
  }
  s.pick = p;
}

void CaptureLocalWiimote(Core::System& system, Input* in)
{
  WiimoteCommon::HIDWiimote* hid =
      s.use_real_wiimote && s.real_wiimote ?
          s.real_wiimote.get() :
          WiimoteCommon::GetHIDWiimoteSource(static_cast<unsigned int>(s.opts.local_pad));
  WiimoteEmu::DesiredWiimoteState state;
  if (hid)
  {
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Bluetooth);
    g_controller_interface.UpdateInput();
    const bool bar = static_cast<bool>(system.GetWiiIPC().GetGPIOOutFlags()[IOS::GPIO::SENSOR_BAR]);
    // The motion simulation (swing, shake, tilt) steps at the Wii Remote's 200 Hz: three or four
    // steps per frame, 3.33 on average.
    const int steps = (s.wiimote_captures++ % 3 == 0) ? 4 : 3;
    for (int i = 0; i < steps; ++i)
    {
      hid->PrepareInput(&state, bar ? WiimoteCommon::HIDWiimote::SensorBarState::Enabled :
                                      WiimoteCommon::HIDWiimote::SensorBarState::Disabled);
    }
  }
  if (s.wiimote_override)
  {
    const WiimoteOverride& o = *s.wiimote_override;
    state.buttons.hex = o.buttons & WiimoteCommon::ButtonData::BUTTON_MASK;
    if (o.nunchuk)
    {
      WiimoteEmu::Nunchuk::DataFormat nc{};
      nc.jx = o.stick_x;
      nc.jy = o.stick_y;
      nc.SetAccel({0x200, 0x200, 0x2CC});
      nc.SetButtons(o.nunchuk_buttons);
      state.extension.data = nc;
    }
    else
    {
      state.extension.data = std::monostate{};
    }
  }
  EncodeWiimote(state, in);
}

void AddLocalRequest(Input* in)
{
  if (s.local_move)
  {
    (*in)[INPUT_MOVE] = static_cast<u8>(INPUT_MOVE_SWAP | (s.local_move->first & 3) |
                                        ((s.local_move->second & 3) << 2));
  }
  if (!s.local_request)
    return;
  (*in)[INPUT_REQUEST] = static_cast<u8>(
      INPUT_REQUEST_ADMIT | (s.local_request->first & 3) |
      (s.local_request->second == Controller::WiiRemote ? INPUT_REQUEST_WIIMOTE : 0));
}

// A GameCube controller's state from the adapter, relative to the controller at rest, as the game
// sees it. The session's ports are standard controllers whose origin is a perfectly centered one,
// so the session moves the player's own origin into the values, as Dolphin's adapter device tells
// the game the origin itself: a C-stick that rests off center reaches as far in every direction
// (P+ in Dolphin's own netplay does the same).
GCPadStatus AdapterInput(int chan)
{
  GCPadStatus pad = GCAdapter::Input(chan);
  const auto origin = GCAdapter::Origin(chan);
  if (!origin)
    return pad;
  const auto shift = [](u8 v, u8 rest, int center) {
    return static_cast<u8>(std::clamp(int{v} - int{rest} + center, 0, 255));
  };
  pad.stickX = shift(pad.stickX, origin->stickX, GCPadStatus::MAIN_STICK_CENTER_X);
  pad.stickY = shift(pad.stickY, origin->stickY, GCPadStatus::MAIN_STICK_CENTER_Y);
  pad.substickX = shift(pad.substickX, origin->substickX, GCPadStatus::C_STICK_CENTER_X);
  pad.substickY = shift(pad.substickY, origin->substickY, GCPadStatus::C_STICK_CENTER_Y);
  pad.triggerLeft = shift(pad.triggerLeft, origin->triggerLeft, 0);
  pad.triggerRight = shift(pad.triggerRight, origin->triggerRight, 0);
  return pad;
}

constexpr u32 INPUT_LOG_EVERY = 600;
constexpr int C_STICK_PUSHED = 0x30;

// Every capture (many a frame while the frame waits for GekkoNet): the C-stick as read now, and,
// in the first capture of a frame, as GekkoNet's sample of the frame (it takes the first input
// added for each frame).
void LogLocalInput(const GCPadStatus& pad)
{
  auto& l = s.input_log;
  const int push = std::max(std::abs(int{pad.substickX} - GCPadStatus::C_STICK_CENTER_X),
                            std::abs(int{pad.substickY} - GCPadStatus::C_STICK_CENTER_Y));
  const bool pushed = push >= C_STICK_PUSHED;
  if (pushed && !l.raw_pushed)
    ++l.raw_pushes;
  l.raw_pushed = pushed;
  if (s.current_frame == l.frame)
    return;
  const Clock::time_point now = Clock::now();
  if (l.frame >= 0)
  {
    l.max_gap_ms = std::max(
        l.max_gap_ms, std::chrono::duration<double, std::milli>(now - l.at).count());
  }
  l.frame = s.current_frame;
  l.at = now;
  ++l.frames;
  if (pushed && !l.sampled_pushed)
    ++l.sampled_pushes;
  l.sampled_pushed = pushed;
  l.peak = std::max(l.peak, push);
  if (l.frames < INPUT_LOG_EVERY)
    return;
  if (l.raw_pushes > 0 || l.sampled_pushes > 0)
  {
    AddLog(fmt::format("input: {} frames, C-stick pushed {} times ({} in the frames' samples, "
                       "furthest {}), at most {:.1f} ms between samples, adapter at {:.0f} Hz",
                       l.frames, l.raw_pushes, l.sampled_pushes, l.peak, l.max_gap_ms,
                       GCAdapter::GetCurrentPollRate()));
  }
  l.frames = l.raw_pushes = l.sampled_pushes = 0;
  l.max_gap_ms = 0;
  l.peak = 0;
}

// Diagnostics: PPR_MACHINE_TEST_INPUT=SEED[:FIRST] plays this machine's GameCube pad from SEED,
// as a function of the frame alone (6-frame steps of a stick direction, a button or nothing, from
// frame FIRST, 900 by default: not at Brawl's boot message), so two runs take the same path
// through the game whatever their speed (A/B tests of rollback changes).
bool TestInput(GCPadStatus* pad)
{
  static const auto cfg = []() -> std::optional<std::pair<u64, s64>> {
    const char* e = std::getenv("PPR_MACHINE_TEST_INPUT");
    if (!e)
      return std::nullopt;
    const char* colon = std::strchr(e, ':');
    return std::pair<u64, s64>{std::strtoull(e, nullptr, 10),
                               colon ? std::strtoll(colon + 1, nullptr, 10) : 900};
  }();
  if (!cfg || s.current_frame < cfg->second)
    return false;
  u64 z = cfg->first ^ (static_cast<u64>(s.current_frame / 6) * 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  *pad = GCPadStatus{};
  const u32 kind = z % 10;
  if (kind < 4)
  {
    pad->stickX = static_cast<u8>(z >> 8);
    pad->stickY = static_cast<u8>(z >> 16);
  }
  else if (kind < 8)
  {
    static constexpr std::array<u16, 8> BUTTONS = {
        PAD_BUTTON_A,  PAD_BUTTON_A,    PAD_BUTTON_B,    PAD_BUTTON_START,
        PAD_BUTTON_UP, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT};
    pad->button = BUTTONS[(z >> 24) % BUTTONS.size()];
  }
  return true;
}

// The local controller as the session's input (no requests).
Input ReadLocalInput()
{
  UpdateLocalController();
  Input in{};
  if (s.controller[s.opts.local_slot] == Controller::WiiRemote)
  {
    CaptureLocalWiimote(Core::System::GetInstance(), &in);
    return in;
  }
  GCPadStatus pad;
  if (s.local_adapter)
  {
    pad = AdapterInput(s.opts.local_pad);
    Harness::ApplyPadOverride(s.opts.local_pad, &pad);
  }
  else
  {
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
    g_controller_interface.UpdateInput();
    pad = Pad::GetStatus(s.opts.local_pad);
  }
  TestInput(&pad);
  LogLocalInput(pad);
  EncodePad(pad, &in);
  return in;
}

void CaptureLocalInput()
{
  s.local_read_at = Clock::now();
  Input in = ReadLocalInput();
  AddLocalRequest(&in);
  s.local_input = in;
}

void RecordHistory(s64 frame, const std::array<Input, MAX_PLAYERS>& inputs, u32 checksum)
{
  if (frame < 0)
    return;
  HistoryEntry& e = s.history[static_cast<size_t>(frame) % HISTORY_SIZE];
  e.frame = frame;
  e.inputs = inputs;
  e.checksum = checksum;
}

void ProcessSessionEvents()
{
  int count = 0;
  GekkoSessionEvent** events = gekko_session_events(s.gekko, &count);
  for (int i = 0; i < count; ++i)
  {
    const GekkoSessionEvent& e = *events[i];
    switch (e.type)
    {
    case GekkoSessionStarted:
      s.gekko_started = true;
      AddLog("GekkoNet session started");
      break;
    case GekkoPlayerDisconnected:
    {
      const int h = e.data.disconnected.handle;
      const int slot = h >= 0 && h < MAX_PLAYERS ? s.handle_slot[h] : -1;
      // GekkoNet agrees with the others on the frame from which the player's input is the
      // disconnected input (all zero: nothing plugged in), so their controller is unplugged at the
      // same frame everywhere and the others play on, as at a LAN.
      AddLog(fmt::format("player {} left (unplugged)", slot + 1));
      OSD::AddMessage(fmt::format("Player {} left", slot + 1), 4000, OSD::Color::YELLOW);
      if (slot >= 0)
      {
        std::lock_guard lk(s.net_mutex);
        s.peers[slot].left = true;
      }
      break;
    }
    case GekkoDesyncDetected:
    {
      s.desyncs++;
      s.last_desync_frame = s.frame_base + e.data.desynced.frame;
      WARN_LOG_FMT(BRAWLBACK, "machine: desync at frame {} (local {:08x}, remote {:08x})",
                   s.last_desync_frame, e.data.desynced.local_checksum,
                   e.data.desynced.remote_checksum);
      // The checksum counts nothing that depends on a host's timing (docs/design/brawl-mods.md
      // §6.6), so the two machines' memory differs: the players are told, once a burst.
      const auto now = Clock::now();
      if (s.desync_notice_at == Clock::time_point{} || now - s.desync_notice_at > std::chrono::seconds(30))
      {
        s.desync_notice_at = now;
        const int h = e.data.desynced.remote_handle;
        const int slot = h >= 0 && h < MAX_PLAYERS ? s.handle_slot[h] : -1;
        std::string name = "another player";
        if (slot >= 0)
        {
          std::lock_guard lk(s.net_mutex);
          name = NameOf(slot);
        }
        ShowNotice(fmt::format("Desync with {} at frame {}: your games differ.", name,
                               s.last_desync_frame));
      }
      break;
    }
    default:
      break;
    }
  }
}

bool DigestsEnabled();

// CPU thread, before the frame's local input: the player's new setting (SetInputDelay) is the
// session's from now on, and the running GekkoNet session's at once. GekkoNet repeats this
// player's last input for the frames a higher delay adds and drops its next inputs for the frames
// a lower one takes away (they already went out): every machine gets the same inputs, and only
// this player's own are delayed, so nobody else needs to know (no epoch).
void ApplyRequestedDelay()
{
  const int requested = s.requested_delay.exchange(NO_DELAY_REQUEST);
  if (requested == NO_DELAY_REQUEST)
    return;
  s.opts.delay = requested;
  if (!s.gekko || s.opts.synctest > 0 || s.local_handle < 0)
    return;
  double rtt = -1;
  const int delay = PickInputDelay(&rtt);
  if (delay == s.input_delay)
    return;
  AddLog(requested >= 0 ? fmt::format("input delay {} -> {} (set)", s.input_delay, delay) :
         rtt < 0 ? fmt::format("input delay {} -> {} (auto, no round trip yet)", s.input_delay,
                               delay) :
                   fmt::format("input delay {} -> {} (auto, round trip {:.0f} ms)", s.input_delay,
                               delay, rtt));
  s.input_delay = delay;
  gekko_set_local_delay(s.gekko, s.local_handle, static_cast<unsigned char>(GekkoDelayFor(delay)));
}

// Polls GekkoNet with this frame's local input and turns its events into the passes to run.
void Update(Core::System& system)
{
  s.ops.Clear();
  gekko_network_poll(s.gekko);
  ApplyRequestedDelay();
  {
    const int target = gekko_current_frame(s.gekko) + GekkoDelayFor(s.input_delay);
    if (s.late_input && gekko_local_input_frame(s.gekko, s.local_handle) >= target)
    {
      // Read and sent at the game's read in the frame before (LateLocalInput).
    }
    else
    {
      const s64 global = s.frame_base + target;
      if (global >= 0)
      {
        auto& l = s.lat[static_cast<size_t>(global) % s.lat.size()];
        l.read_frame = global;
        l.read = s.local_read_at;
      }
      const int before = gekko_local_input_frame(s.gekko, s.local_handle);
      gekko_add_local_input(s.gekko, s.local_handle, s.local_input.data());
      if (s.late_input && gekko_local_input_frame(s.gekko, s.local_handle) > before)
      {
        ++s.late.fallback;
        ++s.late_window.fallback;
      }
    }
  }
  int count = 0;
  GekkoGameEvent** events = gekko_update_session(s.gekko, &count);
  ProcessSessionEvents();
  if (s.phase == Phase::Error)
    return;

  bool pending_load = false;
  int load_frame = 0;
  int load_before = -1;
  bool initial_save = false;
  unsigned int* initial_checksum = nullptr;
  for (int i = 0; i < count; ++i)
  {
    GekkoGameEvent& e = *events[i];
    switch (e.type)
    {
    case GekkoSaveEvent:
      if (e.data.save.state_len)
        *e.data.save.state_len = sizeof(u32);
      if (s.ops.count > 0)
      {
        s.ops.save_after[s.ops.count - 1] = true;
        s.ops.checksum[s.ops.count - 1] = e.data.save.checksum;
      }
      else
      {
        initial_save = true;
        initial_checksum = e.data.save.checksum;
      }
      break;
    case GekkoLoadEvent:
      pending_load = true;
      load_frame = static_cast<int>(s.frame_base + e.data.load.frame);
      break;
    case GekkoAdvanceEvent:
    {
      if (s.ops.count >= MAX_ADVANCE)
      {
        ERROR_LOG_FMT(BRAWLBACK, "machine: more than {} advances in one update", MAX_ADVANCE);
        break;
      }
      const int k = s.ops.count++;
      s.ops.frame[k] = static_cast<s32>(s.frame_base + e.data.adv.frame);
      if (s.plan.active && s.ops.frame[k] >= s.plan.frame)
      {
        // GekkoNet's frame limit holds every machine before the epoch's frame.
        ERROR_LOG_FMT(BRAWLBACK, "machine: GekkoNet advanced frame {} past epoch frame {}",
                      s.ops.frame[k], s.plan.frame);
      }
      if (pending_load && load_before < 0)
        load_before = k;
      pending_load = false;
      for (auto& in : s.ops.inputs[k])
        in.fill(0);
      if (e.data.adv.inputs && e.data.adv.input_len >= INPUT_SIZE)
      {
        const int n = static_cast<int>(e.data.adv.input_len / INPUT_SIZE);
        for (int h = 0; h < n && h < MAX_PLAYERS; ++h)
        {
          const int slot = s.handle_slot[h];
          if (slot >= 0)
            std::memcpy(s.ops.inputs[k][slot].data(), e.data.adv.inputs + h * INPUT_SIZE,
                        INPUT_SIZE);
        }
      }
      break;
    }
    default:
      break;
    }
  }

  if (load_before >= 0)
  {
    // GekkoNet's load names the frame whose end state to restore; the ring's newest slot holds the
    // end of the last frame run (s.current_frame), as in GameplaySession. Not the frame before
    // this update's last advance: while GekkoNet cannot advance (a player's input is missing, or
    // every machine holds before an epoch's frame), a rollback brings only the re-run frames, up
    // to the last frame run, and no new one. Counted from the last advance that load went one
    // frame short: the machine ran a frame ahead of its frame numbers from then on, desynced, and
    // at the next epoch its checksum was another frame's (bug 25, 2026-10-16: no third player
    // could join a REX room, "player 2 does not vouch for this machine").
    auto& rb = Rollback::RollbackManager::Get();
    const int frames_back = static_cast<int>(s.current_frame - load_frame);
    if (frames_back >= 1 && frames_back <= MAX_ROLLBACK_FRAMES && rb.m_ring_count >= 2 &&
        frames_back < rb.m_ring_count)
    {
      // The frames it re-runs must not push the presented frame after them back in wall-clock
      // time (CoreTimingManager::BeginRollbackBurst, as GameplaySession's BeginBurst): without it
      // the throttle re-anchored at the presented frame and each rollback's re-run time was lost
      // for good (prod 2026-10-14: a REX room at 57 frames/s; this PC, REX at 50 ms with 4-6
      // rollbacks a second: 54.7 frames/s before, 58.2 after).
      // PPR_MACHINE_OLD_THROTTLE=1 keeps the old behaviour, for comparisons.
      static const bool old_throttle = std::getenv("PPR_MACHINE_OLD_THROTTLE") != nullptr;
      if (!old_throttle)
        system.GetCoreTiming().BeginRollbackBurst();
      const auto load_start = Clock::now();
      s.loaded = std::make_pair(frames_back, static_cast<int>(rb.m_ring_count));
      rb.LoadFrame(system, frames_back);
      AfterLoad(system, frames_back);
      PaceLoad(Clock::now() - load_start);
      s.rollbacks++;
      s.burst_start = load_start;
      s.burst_frames = frames_back;
      // Diagnostics (PPR_MACHINE_DIGESTS): the first rollbacks of each epoch, to line up with the
      // first desync after a join.
      static int s_logged_epoch = -1, s_logged = 0;
      if (DigestsEnabled())
      {
        if (s_logged_epoch != s.epoch)
          s_logged_epoch = s.epoch, s_logged = 0;
        if (s_logged++ < 8)
        {
          WARN_LOG_FMT(BRAWLBACK, "machine: rollback {} of epoch {}: to frame {} ({} back), {} pass(es)",
                       s_logged, s.epoch, load_frame, frames_back, s.ops.count);
        }
      }
      s_pace.rollbacks++;
      s.max_rollback = std::max<u64>(s.max_rollback, static_cast<u64>(frames_back));
    }
    else if (frames_back != 0)
    {
      s.skipped_loads++;
      ERROR_LOG_FMT(BRAWLBACK, "machine: cannot load {} frame(s) back (ring {})", frames_back,
                    rb.m_ring_count);
    }
  }
  if (initial_save)
  {
    Rollback::RollbackManager::Get().SaveFrame(system);
    AfterSave(system);
    s.saves++;
    if (initial_checksum)
      *initial_checksum = Rollback::RollbackManager::Get().LastSaveChecksum();
  }
  if (s.ops.count == 0)
    s.stall_polls++;
  if (s.ops.count > 1)
    s.frames_resimulated += static_cast<u64>(s.ops.count - 1);
  s.frames_ahead = gekko_frames_ahead(s.gekko);
}

void UpdateTimeSync(Core::System& system)
{
  // As NetPlayClient::UpdateTimeSync (Slippi's): nudge the speed of the peer that runs ahead.
  constexpr s64 INTERVAL = 15;
  if (s.opts.synctest > 0 || s.current_frame < 0 || s.current_frame % INTERVAL != 0 ||
      s.current_frame == s.last_timesync_frame)
    return;
  s.last_timesync_frame = s.current_frame;
  const float ahead = s.frames_ahead;
  double factor = 1.0;
  if (ahead > 0.25f)
    factor = 1.0 - std::min(ahead / 2.0f, 1.0f) * 0.02;
  else if (ahead < -0.25f)
    factor = 1.0 + std::min(-ahead / 2.0f, 1.0f) * 0.01;
  s.speed = factor;
}

IOS::HLE::FS::HostFileSystem* HostFS(Core::System& system)
{
  const auto ios = system.GetIOS();
  if (!ios)
    return nullptr;
  return static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get());
}

// After every snapshot: keep its NAND journal mark, and let the journal forget what no snapshot
// in the ring can go back to any more.
void AfterSave(Core::System& system)
{
  auto* fs = HostFS(system);
  if (!fs)
    return;
  s.journal_marks.push_back(fs->LastJournalMark());
  while (s.journal_marks.size() > static_cast<size_t>(Rollback::NUM_SAVE_SLOTS))
  {
    s.journal_marks.pop_front();
    fs->JournalTrim(s.journal_marks.front());
  }
  if (fs->TakeJournalError())
  {
    s.journal_errors++;
    WARN_LOG_FMT(BRAWLBACK, "machine: the NAND journal failed; the NAND may differ");
  }
}

// After a load of `frames_back` snapshots back: the newer snapshots are gone.
void StartEventTrace(Core::System& system);

void AfterLoad(Core::System& system, int frames_back)
{
  // The frame after the loaded state starts here.
  StartEventTrace(system);
  for (int i = 0; i < frames_back && !s.journal_marks.empty(); ++i)
    s.journal_marks.pop_back();
  if (auto* fs = HostFS(system); fs && fs->TakeJournalError())
  {
    s.journal_errors++;
    WARN_LOG_FMT(BRAWLBACK, "machine: undoing NAND changes failed; the NAND may differ");
  }
}

// Sync test diagnostics: the granules each save copied (address -> CRC of the 64 bytes), per
// frame, so that a re-run whose checksum differs from the first run can say where.
struct GranuleMap
{
  s64 frame = -1;
  std::map<u32, u32> granules;
  std::map<u32, std::array<u8, ROLLBACK_PAGE_SIZE>> bytes;
  // PPR_MACHINE_EVENT_TRACE: the CoreTiming events the pass ran and the ticks at its end.
  std::vector<CoreTiming::CoreTimingManager::TracedEvent> events;
  u64 ticks = 0;
};
// The events of the pass running now (PPR_MACHINE_EVENT_TRACE, sync test).
std::vector<CoreTiming::CoreTimingManager::TracedEvent> s_pass_events;
bool EventTraceEnabled()
{
  static const bool on = std::getenv("PPR_MACHINE_EVENT_TRACE") != nullptr;
  return on;
}
void StartEventTrace(Core::System& system)
{
  if (s.opts.synctest <= 0 || !EventTraceEnabled())
    return;
  s_pass_events.clear();
  system.GetCoreTiming().m_event_trace = &s_pass_events;
}
std::array<GranuleMap, 16> s_first_runs;
std::vector<std::string> s_diag;
// PPR_MACHINE_DIGESTS: the last frames' granule digests, by frame.
struct FrameDigest
{
  s64 frame = -1;
  u32 checksum = 0;
  std::vector<std::pair<u32, u32>> granules;
  std::map<u32, std::array<u8, ROLLBACK_PAGE_SIZE>> bytes;  // PPR_MACHINE_DIGESTS=2
  Rollback::RollbackManager::ChecksumParts parts;  // what the checksum was made of
  bool resim = false;  // the pass that saved it last re-ran the frame
};
std::array<FrameDigest, 256> s_digests;
bool DigestsEnabled()
{
  static const bool on = std::getenv("PPR_MACHINE_DIGESTS") != nullptr;
  return on;
}
bool DigestBytesEnabled()
{
  static const bool on = [] {
    const char* e = std::getenv("PPR_MACHINE_DIGESTS");
    return e && std::strcmp(e, "2") == 0;
  }();
  return on;
}
u64 s_diag_frames = 0;
// PPR_MACHINE_IOLOG (MachineSession.h): by frame, the last run's MMIO reads and host writes.
std::mutex s_io_mutex;
std::map<s64, std::string> s_io_frames;
std::string* s_io_current = nullptr;
u64 s_io_events = 0;  // this frame's I/O log events so far
s64 s_io_frame = -1;

void DiagnoseSave(s64 frame)
{
  if (s.opts.synctest <= 0 || frame < 0)
    return;
  const auto& rb = Rollback::RollbackManager::Get();
  const auto& slot = rb.m_slots[(rb.m_ring_next + Rollback::NUM_SAVE_SLOTS - 1) % Rollback::NUM_SAVE_SLOTS];
  GranuleMap now;
  now.frame = frame;
  const u8* gpu = Rollback::GPUWrittenBitmap::Get().entries;
  auto add = [&](const Rollback::RegionDelta& d, u32 base) {
    for (u32 i = 0; i < d.page_count; ++i)
    {
      const u32 addr = base + d.page_indices[i] * ROLLBACK_PAGE_SIZE;
      if (gpu[(addr & 0x1FFFFFFF) >> ROLLBACK_PAGE_SHIFT] ||
          (addr >= 0x90000800 && addr < 0x90000800 + 0x12c800))
        continue;
      now.granules[addr] = Common::ComputeCRC32(d.page_data.data() + i * ROLLBACK_PAGE_SIZE,
                                                ROLLBACK_PAGE_SIZE);
      std::memcpy(now.bytes[addr].data(), d.page_data.data() + i * ROLLBACK_PAGE_SIZE,
                  ROLLBACK_PAGE_SIZE);
    }
  };
  add(slot.m_mem1_delta, 0x80000000);
  add(slot.m_mem2_delta, 0x90000000);
  auto& system = Core::System::GetInstance();
  if (EventTraceEnabled())
  {
    now.events = std::move(s_pass_events);
    now.ticks = system.GetCoreTiming().GetTicks();
    StartEventTrace(system);  // the next frame's pass starts now
  }
  GranuleMap& first = s_first_runs[static_cast<size_t>(frame) % s_first_runs.size()];
  if (first.frame != frame)
  {
    first = std::move(now);
    return;
  }
  // A re-run of `frame`: report what differs from its first run.
  std::vector<std::string> diffs;
  if (EventTraceEnabled())
  {
    // The first event at which the guest CPU stood elsewhere (or held other registers).
    const size_t n = std::min(first.events.size(), now.events.size());
    for (size_t k = 0; k < n; ++k)
    {
      const auto& a = first.events[k];
      const auto& b = now.events[k];
      if (a.pc != b.pc || a.lr != b.lr || a.sp != b.sp || a.gpr_hash != b.gpr_hash)
      {
        const auto& prev = k > 0 ? first.events[k - 1] : a;
        std::string regs;
        for (int r = 0; r < 32; ++r)
        {
          if (a.gpr[r] != b.gpr[r])
            regs += fmt::format(" r{}:{:08x}->{:08x}", r, a.gpr[r], b.gpr[r]);
        }
        diffs.push_back("regs" + regs);
        diffs.push_back(fmt::format(
            "cpu at event {} ({} t={} late={} / rerun t={} late={}): pc {:08x}->{:08x} lr {:08x}->{:08x} sp {:08x}->{:08x} "
            "gprs {:08x}->{:08x}; the event before: {} t={} pc {:08x} lr {:08x}",
            k, a.name ? *a.name : "?", a.time, a.late, b.time, b.late, a.pc, b.pc, a.lr, b.lr, a.sp, b.sp, a.gpr_hash,
            b.gpr_hash, prev.name ? *prev.name : "?", prev.time, prev.pc, prev.lr));
        break;
      }
    }
  }
  if (EventTraceEnabled())
  {
    // The first event that ran at another time, or another event, than in the first run.
    size_t i = 0;
    while (i < first.events.size() && i < now.events.size() &&
           first.events[i].time == now.events[i].time && first.events[i].name == now.events[i].name &&
           first.events[i].userdata == now.events[i].userdata)
    {
      ++i;
    }
    auto describe = [](const std::vector<CoreTiming::CoreTimingManager::TracedEvent>& v, size_t k) {
      if (k >= v.size())
        return std::string("(none)");
      return fmt::format("{} t={} late={} data={:x}", v[k].name ? *v[k].name : "?", v[k].time,
                         v[k].late, v[k].userdata);
    };
    const s64 prev_time = i > 0 ? first.events[i - 1].time : 0;
    if (first.ticks != now.ticks || i < std::max(first.events.size(), now.events.size()))
    {
      diffs.push_back(fmt::format("ticks {} -> {}; {} vs {} events; event {} (after t={}): {} -> {}",
                                  first.ticks, now.ticks, first.events.size(), now.events.size(), i,
                                  prev_time, describe(first.events, i), describe(now.events, i)));
    }
  }
  for (const auto& [addr, crc] : first.granules)
  {
    const auto it = now.granules.find(addr);
    if (it == now.granules.end())
    {
      // Not written by the re-run: are its bytes now what the first run wrote?
      auto& memory = system.GetMemory();
      const u32 phys = addr & 0x1FFFFFFF;
      const u8* live = phys < 0x10000000 ? memory.GetRAM() + phys :
                                               memory.GetEXRAM() + (phys - 0x10000000);
      const bool same = std::memcmp(live, first.bytes[addr].data(), ROLLBACK_PAGE_SIZE) == 0;
      diffs.push_back(fmt::format("{:08x} only first ({})", addr, same ? "same bytes" : "BYTES DIFFER"));
    }
    else if (it->second != crc)
    {
      // The differing 4-byte words: offset first->rerun.
      std::string words;
      const auto& a = first.bytes[addr];
      const auto& b = now.bytes[addr];
      for (u32 w = 0; w < ROLLBACK_PAGE_SIZE; w += 4)
      {
        if (std::memcmp(&a[w], &b[w], 4) != 0)
        {
          words += fmt::format(" +{:x}:{:02x}{:02x}{:02x}{:02x}->{:02x}{:02x}{:02x}{:02x}", w, a[w],
                               a[w + 1], a[w + 2], a[w + 3], b[w], b[w + 1], b[w + 2], b[w + 3]);
        }
      }
      diffs.push_back(fmt::format("{:08x} differs [{}]", addr, words));
    }
  }
  for (const auto& [addr, crc] : now.granules)
  {
    if (!first.granules.contains(addr))
      diffs.push_back(fmt::format("{:08x} only rerun", addr));
  }
  if (!diffs.empty())
  {
    s_diag_frames++;
    if (s_diag.size() < 200)
    {
      std::string line = fmt::format("frame {}: {} granules ({} first, {} rerun):", frame,
                                     diffs.size(), first.granules.size(), now.granules.size());
      for (size_t i = 0; i < diffs.size() && i < 12; ++i)
        line += " " + diffs[i];
      s_diag.push_back(std::move(line));
    }
  }
}

void PerformQueuedSave(Core::System& system)
{
  if (!s.queued_save.pending)
    return;
  s.queued_save.pending = false;
  auto& rb = Rollback::RollbackManager::Get();
  const auto save_start = Clock::now();
  rb.SaveFrame(system);
  AfterSave(system);
  s.saves++;
  DiagnoseSave(s.queued_save.frame);
  rb.SetCollectChecksumParts(DigestsEnabled());
  const u32 sum = rb.SaveChecksumWithSlice(s.queued_save.frame);
  PaceSave(Clock::now() - save_start);
  if (DigestsEnabled() && s.queued_save.frame >= 0)
  {
    FrameDigest& d = s_digests[static_cast<size_t>(s.queued_save.frame) % s_digests.size()];
    d.frame = s.queued_save.frame;
    d.checksum = sum;
    d.parts = rb.LastChecksumParts();
    d.resim = s.resim;
    d.granules = rb.LastSaveDigest();
    d.bytes.clear();
    if (DigestBytesEnabled())
    {
      auto& memory = system.GetMemory();
      for (const auto& [addr, crc] : d.granules)
      {
        const u8* src = addr < MEM2_BASE ? memory.GetRAM() + addr :
                                           memory.GetEXRAM() + (addr - MEM2_BASE);
        std::memcpy(d.bytes[addr].data(), src, ROLLBACK_PAGE_SIZE);
      }
    }
  }
  if (s.queued_save.checksum)
    *s.queued_save.checksum = sum;
  // Record the frame's inputs with its checksum (the pass that ran it last is the one that counts).
  const s64 frame = s.queued_save.frame;
  HistoryEntry& e = s.history[static_cast<size_t>(frame) % HISTORY_SIZE];
  if (e.frame == frame)
    e.checksum = sum;
}

bool StartBarrier()
{
  // Every peer holds the first frame until all have started GekkoNet; then the lowest slot says
  // go and starts half a round trip later (NetPlayClient::PollStartSync, for N players).
  if (s.opts.synctest > 0)
    return true;
  std::lock_guard lk(s.net_mutex);
  const auto now = Clock::now();
  int lowest = s.opts.local_slot;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if ((s.players >> slot) & 1)
    {
      lowest = std::min(lowest, slot);
    }
  }
  if (now - s.last_barrier_send > std::chrono::milliseconds(50))
  {
    s.last_barrier_send = now;
    picojson::object msg;
    msg["type"] = picojson::value("started");
    msg["epoch"] = picojson::value(static_cast<double>(s.epoch));
    BroadcastControl(msg);
  }
  if (lowest == s.opts.local_slot)
  {
    if (!s.start_at)
    {
      double rtt = 0;
      for (const PeerLink& p : s.peers)
      {
        if (p.slot < 0 || !((s.players >> p.slot) & 1))
          continue;
        if (p.started != s.epoch)
          return false;
        rtt = std::max(rtt, p.rtt_ms);
      }
      picojson::object go;
      go["type"] = picojson::value("go");
      go["epoch"] = picojson::value(static_cast<double>(s.epoch));
      BroadcastControl(go);
      s.start_at = now + std::chrono::microseconds(static_cast<s64>(rtt * 500));
      AddLog(fmt::format("start barrier: everyone started, go in {} ms", rtt / 2));
    }
    return now >= *s.start_at;
  }
  return s.peers[lowest].go == s.epoch;
}

bool PeersConnected()
{
  std::lock_guard lk(s.net_mutex);
  for (const PeerLink& p : s.peers)
  {
    if (p.slot >= 0 && ((s.players >> p.slot) & 1) && !p.hello)
      return false;
  }
  return true;
}

// A peer that said it leaves, or whose link dropped: GekkoNet drops it now instead of after its
// silence timeout, and agrees with the others on the frame its controller is unplugged.
void DropLeftPeers()
{
  std::array<bool, MAX_PLAYERS> drop{};
  {
    std::lock_guard lk(s.net_mutex);
    for (PeerLink& p : s.peers)
    {
      if (p.slot >= 0 && p.left && !p.dropped)
      {
        p.dropped = true;
        drop[p.slot] = true;
        // The previous player of a slot someone new has: the new one's link is not leaving.
        if (p.reseated)
        {
          p.reseated = false;
          p.left = false;
        }
      }
    }
  }
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (drop[slot] && s.gekko && s.slot_handle[slot] >= 0)
    {
      // GekkoNet's own PlayerDisconnected event for this is cleared by its next poll, unread.
      gekko_disconnect_actor(s.gekko, s.slot_handle[slot]);
      s.gone |= 1u << slot;
      AddLog(fmt::format("player {} left", slot + 1));
      OSD::AddMessage(fmt::format("Player {} left", slot + 1), 4000, OSD::Color::YELLOW);
    }
  }
}

// Slippi's ping line, as P+ online has it (GameplaySession's UpdatePingDisplay): GekkoNet's last
// round trip in ms to the player with the highest average ping, cyan, under the FPS box. Refreshed
// once a second while the session runs and left up for Duration::NORMAL, so it fades out after
// the session. Dolphin's "Show NetPlay Ping" (on by default) turns it off.
void UpdatePingDisplay()
{
  if (!s.gekko || s.opts.synctest > 0)
    return;
  const Clock::time_point now = Clock::now();
  if (now < s.next_ping_display)
    return;
  s.next_ping_display = now + std::chrono::seconds(1);
  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING))
    return;
  GekkoNetworkStats worst{};
  bool any = false;
  const u32 players = s.players.load();
  const u32 gone = s.gone.load();
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    const int h = s.slot_handle[slot];
    if (slot == s.opts.local_slot || h < 0 || !((players >> slot) & 1) || ((gone >> slot) & 1))
      continue;
    GekkoNetworkStats st{};
    gekko_network_stats(s.gekko, h, &st);
    if (!any || st.avg_ping > worst.avg_ping)
      worst = st;
    any = true;
  }
  if (any)
  {
    OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, fmt::format("Ping: {}", worst.last_ping),
                         OSD::Duration::NORMAL, OSD::Color::CYAN);
  }
}

// ------------------------------------------------------------------------------ epochs

void SetFrameLimit()
{
  if (s.gekko)
    gekko_set_frame_limit(s.gekko, s.plan.active ? static_cast<int>(s.plan.frame - s.frame_base) : -1);
}

// CPU thread, at the start of every pass of `frame` (first runs and re-runs alike): the epoch a
// request in this frame's inputs starts. A request counts in the first frame that carries it
// (the frame before does not), the same on every machine since it is input; a re-run of the
// request's frame or an earlier one derives the plan again, so a request seen in a mispredicted
// input never stands.
void PlanEpoch(s64 frame)
{
  if (s.plan.active && frame <= s.plan.request_frame)
  {
    s.plan = {};
    SetFrameLimit();
  }
  if (s.plan.active)
    return;
  const HistoryEntry& prev = s.history[static_cast<size_t>(frame - 1) % HISTORY_SIZE];
  const bool have_prev = frame > 0 && prev.frame == frame - 1;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (!((s.players >> slot) & 1))
      continue;
    // A slot move: as a request to admit, from the frame that first carries it.
    const u8 mv = s.latched[slot][INPUT_MOVE];
    if ((mv & INPUT_MOVE_SWAP) && (mv & 3) != ((mv >> 2) & 3) &&
        !(have_prev && prev.inputs[slot][INPUT_MOVE] == mv))
    {
      s.plan.active = true;
      s.plan.request_frame = frame;
      s.plan.frame = frame + EPOCH_LEAD;
      s.plan.from = slot;
      s.plan.admit = -1;
      s.plan.move = {mv & 3, (mv >> 2) & 3};
      SetFrameLimit();
      if (!s.resim)
      {
        AddLog(fmt::format("epoch {} at frame {}: player {} moves slot {} <-> slot {}", s.epoch + 1,
                           s.plan.frame, slot + 1, s.plan.move.first + 1, s.plan.move.second + 1));
      }
      if (slot == s.opts.local_slot)
        s.local_move.reset();
      break;
    }
    const u8 r = s.latched[slot][INPUT_REQUEST];
    if (!(r & INPUT_REQUEST_ADMIT))
      continue;
    if (have_prev && prev.inputs[slot][INPUT_REQUEST] == r)
      continue;
    s.plan.active = true;
    s.plan.request_frame = frame;
    s.plan.frame = frame + EPOCH_LEAD;
    s.plan.from = slot;
    s.plan.admit = r & 3;
    s.plan.controller = (r & INPUT_REQUEST_WIIMOTE) ? Controller::WiiRemote : Controller::GameCube;
    SetFrameLimit();
    if (!s.resim)
    {
      AddLog(fmt::format("epoch {} at frame {}: player {} admits player {}", s.epoch + 1,
                         s.plan.frame, slot + 1, s.plan.admit + 1));
    }
    // This player's request is in its inputs now; it stops asking.
    if (slot == s.opts.local_slot)
      s.local_request.reset();
    break;
  }
}

// The decider asks to admit a joiner that asked (one at a time).
void UpdateLocalRequest()
{
  if (s.plan.active || s.local_request || s.local_move || s.phase != Phase::Running)
    return;
  std::lock_guard lk(s.net_mutex);
  int lowest = -1;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (!Plays(slot))
      continue;
    if (slot != s.opts.local_slot && s.peers[slot].left)
      continue;
    lowest = slot;
    break;
  }
  if (lowest != s.opts.local_slot)
    return;
  for (const PeerLink& p : s.peers)
  {
    if (p.slot >= 0 && p.late && p.join_asked && p.hello && !p.left && !Plays(p.slot))
    {
      s.local_request = std::make_pair(p.slot, p.join_controller);
      AddLog(fmt::format("asking to admit player {}", p.slot + 1));
      return;
    }
  }
}

bool s_image_dostate = false;

// The session NAND's files a game writes (its save, shared2, /tmp) and the FS's metadata
// (fst.bin), at host level: a savestate carries /tmp through the FS's own listing, which skips
// names longer than the Wii's 12 characters (Brawl keeps its font cache in /tmp/font_latin1.arc)
// and reverses the directory order. Layout: u32 count, then per entry u8 kind (0 file, 1 directory),
// u16 path length, the path (relative to the NAND root, '/'), and for a file u32 size and bytes.
constexpr std::array<const char*, 3> NAND_IMAGE_DIRS = {"tmp", "title", "shared2"};
constexpr size_t MAX_NAND_IMAGE_FILES = 4096;

std::vector<u8> PackNand(Core::System& system)
{
  std::vector<u8> out(4);
  u32 count = 0;
  const std::string root = File::GetUserPath(D_SESSION_WIIROOT_IDX);
  auto add = [&](u8 kind, const std::string& rel, const std::string* data) {
    out.push_back(kind);
    const u16 len = static_cast<u16>(rel.size());
    out.insert(out.end(), reinterpret_cast<const u8*>(&len), reinterpret_cast<const u8*>(&len) + 2);
    out.insert(out.end(), rel.begin(), rel.end());
    if (data)
    {
      const u32 size = static_cast<u32>(data->size());
      out.insert(out.end(), reinterpret_cast<const u8*>(&size),
                 reinterpret_cast<const u8*>(&size) + 4);
      out.insert(out.end(), data->begin(), data->end());
    }
    count++;
  };
  // Every guest handle's buffered writes reach the host files first.
  auto* fs = HostFS(system);
  if (fs)
    fs->CloseHostFiles();
  std::function<void(const File::FSTEntry&, const std::string&)> walk =
      [&](const File::FSTEntry& e, const std::string& rel) {
        if (e.isDirectory)
        {
          add(1, rel, nullptr);
          // Sorted, so every player's machine packs the same bytes (its digest vouches for it).
          std::vector<const File::FSTEntry*> children;
          for (const auto& c : e.children)
            children.push_back(&c);
          std::ranges::sort(children, {}, [](const File::FSTEntry* c) { return c->virtualName; });
          for (const File::FSTEntry* c : children)
            walk(*c, rel + "/" + c->virtualName);
          return;
        }
        std::string data;
        File::ReadFileToString(e.physicalName, data);
        add(0, rel, &data);
      };
  for (const char* dir : NAND_IMAGE_DIRS)
  {
    if (File::IsDirectory(root + dir))
      walk(File::ScanDirectoryTree(root + dir, true), dir);
  }
  std::string fst;
  if (File::ReadFileToString(root + "fst.bin", fst))
    add(0, "fst.bin", &fst);
  if (fs)
    fs->ReopenHostFiles();
  std::memcpy(out.data(), &count, 4);
  return out;
}

bool IsAllowedNandPath(const std::string& path)
{
  bool ok = path == "fst.bin";
  for (const char* dir : NAND_IMAGE_DIRS)
  {
    const std::string d = dir;
    ok |= path == d || path.starts_with(d + "/");
  }
  for (const char c : path)
    ok &= c >= 0x20 && c < 0x7F && c != '\\' && c != ':';
  size_t start = 0;
  while (ok && start <= path.size())
  {
    const size_t slash = std::min(path.find('/', start), path.size());
    const std::string part = path.substr(start, slash - start);
    ok &= !part.empty() && part != "." && part != "..";
    start = slash + 1;
  }
  return ok;
}

// A joiner: its session NAND's game files become the image's. Every path came from a peer: only
// these directories, no "." or ".." component, printable, bounded.
bool UnpackNand(Core::System& system, const u8* data, size_t size, std::string* error)
{
  struct Entry
  {
    u8 kind = 0;
    std::string path;
    const u8* bytes = nullptr;
    u32 size = 0;
  };
  std::vector<Entry> entries;
  size_t pos = 0;
  auto need = [&](size_t n) { return size - pos >= n; };
  auto fail = [&](const char* why) {
    *error = why;
    return false;
  };
  if (!need(4))
    return fail("the NAND is cut short");
  u32 count;
  std::memcpy(&count, data, 4);
  pos = 4;
  if (count > MAX_NAND_IMAGE_FILES)
    return fail("the NAND has too many files");
  for (u32 i = 0; i < count; ++i)
  {
    Entry e;
    u16 len;
    if (!need(3))
      return fail("the NAND is cut short");
    e.kind = data[pos];
    std::memcpy(&len, data + pos + 1, 2);
    pos += 3;
    if (e.kind > 1 || len == 0 || len > 255 || !need(len))
      return fail("the NAND has an entry that is not valid");
    e.path.assign(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    if (!IsAllowedNandPath(e.path) || (e.path == "fst.bin" && e.kind != 0))
      return fail("the NAND has a path that is not allowed");
    if (e.kind == 0)
    {
      if (!need(4))
        return fail("the NAND is cut short");
      std::memcpy(&e.size, data + pos, 4);
      pos += 4;
      if (!need(e.size))
        return fail("the NAND is cut short");
      e.bytes = data + pos;
      pos += e.size;
    }
    entries.push_back(std::move(e));
  }
  const std::string root = File::GetUserPath(D_SESSION_WIIROOT_IDX);
  auto* fs = HostFS(system);
  if (fs)
    fs->CloseHostFiles();
  for (const char* dir : NAND_IMAGE_DIRS)
    File::DeleteDirRecursively(root + dir);
  bool ok = true;
  for (const Entry& e : entries)
  {
    const std::string host = root + e.path;
    if (e.kind == 1)
    {
      ok &= File::CreateFullPath(host + "/");
    }
    else
    {
      ok &= File::CreateFullPath(host) &&
            File::WriteStringToFile(
                host, std::string_view(reinterpret_cast<const char*>(e.bytes), e.size));
    }
  }
  if (fs)
  {
    fs->ReloadFst();
    fs->ReopenHostFiles();
  }
  if (!ok)
    return fail("the NAND could not be written");
  return true;
}

void Put32(std::vector<u8>* v, u32 x)
{
  const u8* b = reinterpret_cast<const u8*>(&x);
  v->insert(v->end(), b, b + 4);
}

// The decider, at an epoch's frame (the loop top, the previous frame saved): the whole machine
// for a joiner, compressed. Layout (little endian): magic, version, frame, checksum, MEM1 size,
// MEM2 size, the previous frame's inputs, the state (Dolphin's savestate with the NAND), the GPU
// written granules of MEM1 and MEM2, the NAND's game files, the session's SD card's manifest (wire
// form, or nothing), and the card's chunks the game has read so far (for the joiner to fetch
// first).
// The parts of a machine image that every player's machine has the same at the epoch's frame:
// the state (Dolphin's savestate in image mode), the NAND's game files, the inputs of the frame
// before. Their digest is how the other players vouch for the image one of them sends.
struct ImageParts
{
  s64 frame = -1;
  u32 mem1 = 0, mem2 = 0;
  std::array<Input, MAX_PLAYERS> last{};
  std::vector<u8> state;
  std::vector<u8> nand;
  // The session's SD card's manifest, canonical form (empty: no card).
  std::vector<u8> sd;
  // Not vouched for (not in the digest): what only drawing uses, for the image this machine sends
  // (VideoCommon_DoImageDrawState): the EFB copies its texture cache keeps in VRAM and the TEV
  // colour registers' both halves. A session's copies never reach guest RAM, so without them a
  // joiner drew every copy the game made before the join from whatever that RAM held; and bpmem
  // holds only the half of a colour register written last (bug report #114, wrong UI on the
  // joiner). Each GPU draws the copies its own way (and at its own resolution), so the players'
  // would differ; neither is ever read by the game.
  std::vector<u8> copies;
  u32 copies_count = 0;
};

std::string ImageDigest(s64 frame, u32 mem1, u32 mem2, const std::array<Input, MAX_PLAYERS>& last,
                        std::span<const u8> state, std::span<const u8> nand,
                        std::span<const u8> sd)
{
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  auto put = [&](const void* p, size_t n) {
    mbedtls_sha256_update_ret(&ctx, static_cast<const unsigned char*>(p), n);
  };
  const u64 state_size = state.size(), nand_size = nand.size();
  put(&frame, sizeof(frame));
  put(&mem1, sizeof(mem1));
  put(&mem2, sizeof(mem2));
  for (const Input& in : last)
    put(in.data(), in.size());
  put(&state_size, sizeof(state_size));
  put(state.data(), state.size());
  put(&nand_size, sizeof(nand_size));
  put(nand.data(), nand.size());
  const u64 sd_size = sd.size();
  put(&sd_size, sizeof(sd_size));
  put(sd.data(), sd.size());
  std::array<unsigned char, 32> out{};
  mbedtls_sha256_finish_ret(&ctx, out.data());
  mbedtls_sha256_free(&ctx);
  std::string hex;
  for (const unsigned char b : out)
    hex += fmt::format("{:02x}", b);
  return hex;
}

// The image's draw state (ImageParts::copies), in image mode on the video thread. Measured first
// (no readback), then read back from the GPU. PPR_MACHINE_IMAGE_COPIES=0 leaves them out (the
// joiner keeps its own cache, as before image version 4), for comparisons.
std::vector<u8> SaveImageCopies(u32* count)
{
  *count = 0;
  if (const char* env = std::getenv("PPR_MACHINE_IMAGE_COPIES"); env && std::string_view(env) == "0")
    return {};
  s_image_dostate = true;
  u8 dummy = 0;
  u8* ptr = &dummy;
  PointerWrap measure(&ptr, 0, PointerWrap::Mode::Measure);
  VideoCommon_DoImageDrawState(measure);
  std::vector<u8> out(measure.GetOffsetFromPreviousPosition(&dummy));
  ptr = out.data();
  PointerWrap write(&ptr, out.size(), PointerWrap::Mode::Write);
  *count = VideoCommon_DoImageDrawState(write);
  s_image_dostate = false;
  if (!write.IsWriteMode())
  {
    *count = 0;
    return {};
  }
  out.resize(write.GetOffsetFromPreviousPosition(out.data()));
  return out;
}

// `copies`: this player sends the image (ImageParts::copies).
bool MakeImageParts(Core::System& system, s64 frame, ImageParts* parts, bool copies)
{
  auto& memory = system.GetMemory();
  Rollback::RollbackManager::SyncGPUForDoState(system);
  Common::UniqueBuffer<u8> state(192u << 20);
  // Image mode on both sides: the host-only video state (textures, EFB contents, bounding box: the
  // joiner rebuilds them) stays out, and the loader refuses device changes.
  s_image_dostate = true;
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  const size_t state_size = ::State::SaveToBuffer(system, state);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  s_image_dostate = false;
  if (state_size == 0)
    return false;
  parts->frame = frame;
  parts->mem1 = memory.GetRamSizeReal();
  parts->mem2 = memory.GetExRamSizeReal();
  parts->last = s.history[static_cast<size_t>(frame - 1) % HISTORY_SIZE].inputs;
  parts->state.assign(state.data(), state.data() + state_size);
  parts->nand = PackNand(system);
  {
    std::lock_guard lk(s.net_mutex);
    parts->sd = s.sd_manifest_canon;
  }
  if (copies)
    parts->copies = SaveImageCopies(&parts->copies_count);
  return true;
}

std::string ImageDigest(const ImageParts& p)
{
  return ImageDigest(p.frame, p.mem1, p.mem2, p.last, p.state, p.nand, p.sd);
}

std::optional<ImageParts> s_image_parts;  // this player's, at the epoch it admits a joiner at

std::vector<u8> BuildImage(const ImageParts& parts, u32 sum, u64* raw_size)
{
  const u32 mem1 = parts.mem1, mem2 = parts.mem2;
  const s64 frame = parts.frame;
  const u32 g1 = mem1 / ROLLBACK_PAGE_SIZE;
  const u32 g2 = mem2 / ROLLBACK_PAGE_SIZE;
  const auto& nand = parts.nand;
  std::vector<u8> raw;
  raw.reserve(parts.state.size() + g1 + g2 + nand.size() + 1024);
  Put32(&raw, IMAGE_MAGIC);
  Put32(&raw, IMAGE_VERSION);
  Put32(&raw, static_cast<u32>(frame));
  Put32(&raw, static_cast<u32>(frame >> 32));
  Put32(&raw, sum);
  Put32(&raw, mem1);
  Put32(&raw, mem2);
  for (const Input& in : parts.last)
    raw.insert(raw.end(), in.begin(), in.end());
  Put32(&raw, static_cast<u32>(parts.state.size()));
  raw.insert(raw.end(), parts.state.begin(), parts.state.end());
  const u8* gpu = Rollback::GPUWrittenBitmap::Get().entries;
  Put32(&raw, g1);
  raw.insert(raw.end(), gpu, gpu + g1);
  Put32(&raw, g2);
  raw.insert(raw.end(), gpu + MEM2_FIRST_PAGE, gpu + MEM2_FIRST_PAGE + g2);
  Put32(&raw, static_cast<u32>(nand.size()));
  raw.insert(raw.end(), nand.begin(), nand.end());
  std::vector<u8> sd_wire;
  std::shared_ptr<SDCard::SessionCard> sd_card;
  {
    std::lock_guard lk(s.net_mutex);
    sd_wire = s.sd_manifest_wire;
    sd_card = s.sd_card;
  }
  Put32(&raw, static_cast<u32>(sd_wire.size()));
  raw.insert(raw.end(), sd_wire.begin(), sd_wire.end());
  const std::vector<u32> hot = sd_card ? sd_card->ReadChunks(MAX_SD_HOT_CHUNKS) : std::vector<u32>{};
  Put32(&raw, static_cast<u32>(hot.size()));
  for (const u32 c : hot)
    Put32(&raw, c);
  Put32(&raw, static_cast<u32>(parts.copies.size()));
  raw.insert(raw.end(), parts.copies.begin(), parts.copies.end());
  *raw_size = raw.size();

  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  if (!cctx)
    return {};
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
  // The state holds the NAND's /tmp twice (Brawl's is 37 MB): a long window finds the copy.
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 27);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 4);  // fails harmlessly without threads
  std::vector<u8> packed(ZSTD_compressBound(raw.size()));
  const size_t n = ZSTD_compress2(cctx, packed.data(), packed.size(), raw.data(), raw.size());
  ZSTD_freeCCtx(cctx);
  if (ZSTD_isError(n))
    return {};
  packed.resize(n);
  return packed;
}

s64 s_schedule_log_until = -1;  // PPR_MACHINE_SCHEDULE_LOG (StartScheduleLog)
void StartScheduleLog(Core::System& system, s64 frame, std::string_view why);

// A joiner, at its loop top: replaces its machine with the image (every value bounded: it came
// from a peer). Its own boot is gone; it is at the epoch's frame, as every player.
bool LoadImage(Core::System& system, std::string* error, bool self = false)
{
  State::ImageRx rx;
  {
    std::lock_guard lk(s.net_mutex);
    rx = std::move(s.image_rx);
    s.image_rx = {};
  }
  const unsigned long long content = ZSTD_getFrameContentSize(rx.data.data(), rx.data.size());
  if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN ||
      content != rx.raw_size || content > MAX_IMAGE_RAW)
  {
    *error = "the machine image's size is not what it said";
    return false;
  }
  std::vector<u8> raw(static_cast<size_t>(content));
  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  if (!dctx)
  {
    *error = "out of memory";
    return false;
  }
  ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 27);
  const size_t n = ZSTD_decompressDCtx(dctx, raw.data(), raw.size(), rx.data.data(), rx.data.size());
  ZSTD_freeDCtx(dctx);
  rx.data = {};
  if (ZSTD_isError(n) || n != raw.size())
  {
    *error = "the machine image does not decompress";
    return false;
  }

  size_t pos = 0;
  auto get32 = [&](u32* v) {
    if (raw.size() - pos < 4)
      return false;
    std::memcpy(v, raw.data() + pos, 4);
    pos += 4;
    return true;
  };
  u32 magic, version, frame_lo, frame_hi, sum, mem1, mem2;
  auto& memory = system.GetMemory();
  if (!get32(&magic) || !get32(&version) || !get32(&frame_lo) || !get32(&frame_hi) ||
      !get32(&sum) || !get32(&mem1) || !get32(&mem2) || magic != IMAGE_MAGIC ||
      version != IMAGE_VERSION)
  {
    *error = "not a machine image of this version";
    return false;
  }
  const s64 frame = static_cast<s64>((static_cast<u64>(frame_hi) << 32) | frame_lo);
  if (frame != rx.frame || mem1 != memory.GetRamSizeReal() || mem2 != memory.GetExRamSizeReal())
  {
    *error = "the machine image is of another frame or memory size";
    return false;
  }
  std::array<Input, MAX_PLAYERS> last{};
  for (Input& in : last)
  {
    if (raw.size() - pos < INPUT_SIZE)
    {
      *error = "the machine image is cut short";
      return false;
    }
    std::memcpy(in.data(), raw.data() + pos, INPUT_SIZE);
    pos += INPUT_SIZE;
  }
  u32 state_size;
  if (!get32(&state_size) || raw.size() - pos < state_size)
  {
    *error = "the machine image is cut short";
    return false;
  }
  const size_t state_pos = pos;
  pos += state_size;
  u32 g1, g2;
  if (!get32(&g1) || g1 != mem1 / ROLLBACK_PAGE_SIZE || raw.size() - pos < g1)
  {
    *error = "the machine image is cut short";
    return false;
  }
  const size_t g1_pos = pos;
  pos += g1;
  if (!get32(&g2) || g2 != mem2 / ROLLBACK_PAGE_SIZE || raw.size() - pos < g2)
  {
    *error = "the machine image is cut short";
    return false;
  }
  const size_t g2_pos = pos;
  pos += g2;
  u32 nand_size;
  if (!get32(&nand_size) || raw.size() - pos < nand_size)
  {
    *error = "the machine image is cut short";
    return false;
  }
  const size_t nand_pos = pos;
  pos += nand_size;
  u32 sd_size;
  if (!get32(&sd_size) || raw.size() - pos < sd_size)
  {
    *error = "the machine image is cut short";
    return false;
  }
  const size_t sd_pos = pos;
  pos += sd_size;
  u32 hot_count;
  if (!get32(&hot_count) || hot_count > MAX_SD_HOT_CHUNKS || (raw.size() - pos) / 4 < hot_count)
  {
    *error = "the machine image is cut short";
    return false;
  }
  std::vector<u32> sd_hot(hot_count);
  std::memcpy(sd_hot.data(), raw.data() + pos, size_t{hot_count} * 4);
  pos += size_t{hot_count} * 4;
  u32 copies_size;
  if (!get32(&copies_size) || raw.size() - pos < copies_size)
  {
    *error = "the machine image is cut short";
    return false;
  }
  const size_t copies_pos = pos;
  pos += copies_size;
  // The session's SD card (checked like everything a peer sends; the players vouch for it below).
  std::shared_ptr<const SDCard::Manifest> sd_manifest;
  std::vector<u8> sd_canon;
  if (sd_size > 0)
  {
    sf::Packet packet;
    packet.append(raw.data() + sd_pos, sd_size);
    std::optional<SDCard::Manifest> manifest = SDCard::ReadManifest(packet);
    if (!manifest)
    {
      *error = "the machine image's SD card is not valid";
      return false;
    }
    sd_canon = SDCard::CanonicalManifest(*manifest);
    sd_manifest = std::make_shared<const SDCard::Manifest>(std::move(*manifest));
  }

  // Every player vouches for the machine: its own digest of the image at this epoch must be this
  // image's (a player who sends a crafted machine is outvoted by any honest one).
  {
    const std::string digest =
        ImageDigest(frame, mem1, mem2, last, std::span<const u8>(raw.data() + state_pos, state_size),
                    std::span<const u8>(raw.data() + nand_pos, nand_size), sd_canon);
    std::lock_guard lk(s.net_mutex);
    for (const PeerLink& p : s.peers)
    {
      if (!MustVouch(p, rx.epoch))
        continue;
      if (p.epoch_at != rx.epoch || p.epoch_digest != digest)
      {
        *error = fmt::format("player {} does not vouch for this machine", p.slot + 1);
        return false;
      }
    }
  }

  // From here on this joiner plays the session's SD card, before the state loads (the emulated
  // card's state takes the storage installed then): its own card's chunks where they match, the
  // others from the players. The game's SD configuration is part of the build, so a card on one
  // side only means another build.
  std::shared_ptr<SDCard::SessionCard> sd_card;
  if (!self)
  {
    const bool inserted = Config::Get(Config::MAIN_WII_SD_CARD);
    if (sd_manifest && !inserted)
    {
      *error = "the session's game has an SD card, and this one has none";
      return false;
    }
    if (!sd_manifest && inserted)
    {
      *error = "the session's game has no SD card, and this one has one";
      return false;
    }
    if (sd_manifest)
    {
      // The card this machine already plays (a player that gets the machine again) or synced
      // before its join: the game must never wait for the network.
      {
        std::lock_guard lk(s.net_mutex);
        if (s.sd_card && s.sd_card->GetManifest() && s.sd_manifest_canon == sd_canon)
        {
          sd_card = s.sd_card;
          s.sd_source = rx.from;
        }
      }
      if (!sd_card)
      {
        *error = "the session's SD card is not the one this player got before joining";
        return false;
      }
      IOS::HLE::SetSDCardSessionStorage(sd_card);
      AddLog(fmt::format("SD card: playing the session's card ({:.1f} MB still to fetch)",
                         sd_card->MissingBytes() / 1048576.0));
    }
  }

  StartScheduleLog(system, frame, "image load");
  auto& rb = Rollback::RollbackManager::Get();
  // The ring and its base snapshot hold this machine's own boot: start them again after. (A
  // self-load, a diagnostic, loads the state the ring already ends with.)
  if (!self && rb.m_frame_save_enabled.load(std::memory_order_relaxed))
    rb.ToggleFrameSave();
  auto* fs = HostFS(system);
  if (fs)
    fs->JournalStop();
  // The video thread stops for the load, as for any savestate load (Core::PauseAndLock): the FIFO
  // pointers change under it, and with it running a joiner's GPU saw the old ones and raised a
  // command processor status no other machine had.
  auto& fifo = system.GetFifo();
  fifo.PauseAndLock();
  // Loaded from inside the loop top's hook: the JIT keeps running the hook's block, so its cache
  // is not cleared under it; the blocks of all of RAM are erased below instead (as a rollback
  // load erases those of what it restored).
  rb.m_skip_jit_clear_in_dostate.store(true, std::memory_order_seq_cst);
  s_image_dostate = true;
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  std::string state_failure;
  const bool ok = ::State::LoadFromBuffer(
      system, std::span<u8>(raw.data() + state_pos, static_cast<size_t>(state_size)), &state_failure);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  // The sender's EFB copies replace this machine's texture cache (its own boot's copies are of
  // another machine). Only drawn, so one that does not load leaves the game as it is.
  u32 copies = 0;
  bool copies_ok = true;
  if (ok && copies_size > 0)
  {
    u8* ptr = raw.data() + copies_pos;
    PointerWrap p(&ptr, copies_size, PointerWrap::Mode::Read);
    copies = VideoCommon_DoImageDrawState(p);
    copies_ok = p.IsReadMode();
  }
  s_image_dostate = false;
  rb.m_skip_jit_clear_in_dostate.store(false, std::memory_order_seq_cst);
  fifo.RestoreState(true);
  auto& jit = system.GetJitInterface();
  jit.EraseBlocksForRestoredRange(0x00000000, mem1);
  jit.EraseBlocksForRestoredRange(MEM2_BASE, mem2);
  if (!ok)
  {
    // Where it stopped (prod 2026-10-14: ANGE#858's Dolphin refused ONYX#210's REX TE image four
    // times, and nothing said which part).
    *error = "the machine image's state does not load (" + state_failure + ")";
    return false;
  }
  if (s_schedule_log_until >= 0)
    INFO_LOG_FMT(BRAWLBACK, "machine: schedule log: image state loaded, event order {}",
                 system.GetCoreTiming().GetEventFifoId());
  if (!self)
  {
    AddLog(fmt::format("machine image: {} EFB copies ({:.1f} MB){}", copies,
                       copies_size / 1048576.0, copies_ok ? "" : ", which did not load"));
  }
  if (!UnpackNand(system, raw.data() + nand_pos, nand_size, error))
    return false;
  u8* gpu = Rollback::GPUWrittenBitmap::Get().entries;
  std::memcpy(gpu, raw.data() + g1_pos, g1);
  std::memcpy(gpu + MEM2_FIRST_PAGE, raw.data() + g2_pos, g2);
  if (!self)
  {
    rb.ToggleFrameSave();
    s.journal_marks.clear();
    // The players' rings end with the epoch frame's state saved after the frame before it (and
    // GekkoNet's first save of the new session adds it again): the joiner's ring gets the same
    // snapshot, so a rollback in the first frames after the epoch finds it.
    rb.SaveFrame(system);
    AfterSave(system);
    s.saves++;
  }
  HistoryEntry& e = s.history[static_cast<size_t>(frame - 1) % HISTORY_SIZE];
  e.frame = frame - 1;
  e.inputs = last;
  e.checksum = 0;
  s.current_frame = frame - 1;
  s.latched = last;
  const u32 mine = rb.FullChecksum();
  if (mine != sum)
  {
    *error = fmt::format("the machine image loaded with checksum {:08x}, not {:08x}", mine, sum);
    return false;
  }
  s.epoch_sum = mine;
  return true;
}

// Diagnostics (PPR_MACHINE_SELFLOAD_AT=<frame>): at that frame this machine saves its own image
// and loads it again, as a joiner would. A machine image that loads exactly leaves it in sync.
void SelfLoad(Core::System& system, s64 frame)
{
  u64 raw = 0;
  const u32 sum = Rollback::RollbackManager::Get().FullChecksum();
  ImageParts parts;
  if (!MakeImageParts(system, frame, &parts, true))
    return;
  std::vector<u8> image = BuildImage(parts, sum, &raw);
  {
    std::lock_guard lk(s.net_mutex);
    s.image_rx = {};
    s.image_rx.from = s.opts.local_slot;
    s.image_rx.frame = frame;
    s.image_rx.sum = sum;
    s.image_rx.packed_size = image.size();
    s.image_rx.raw_size = raw;
    s.image_rx.data = std::move(image);
    s.image_rx.complete = true;
  }
  std::string why;
  const bool ok = LoadImage(system, &why, true);
  AddLog(fmt::format("self-load at frame {}: {}", frame, ok ? "loaded" : why));
}

void LogParts(Core::System& system, s64 frame);

// Diagnostics (PPR_MACHINE_DUMP_DIR): this machine's whole state at an epoch's frame, to compare
// a joiner's loaded machine with the players'.
void DumpEpochState(Core::System& system, s64 frame, const char* who)
{
  const char* dir = std::getenv("PPR_MACHINE_DUMP_DIR");
  if (!dir || !*dir)
    return;
  Rollback::RollbackManager::SyncGPUForDoState(system);
  Common::UniqueBuffer<u8> state(192u << 20);
  // In image mode, as MakeImageParts saves: a plain save kicked the GPU and scheduled an event the
  // machine would not have had without the dump.
  s_image_dostate = true;
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  const size_t n = ::State::SaveToBuffer(system, state);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  s_image_dostate = false;
  const std::string path = fmt::format("{}/state-{}-p{}-{}.bin", dir, frame, s.opts.local_slot + 1, who);
  if (std::FILE* f = std::fopen(path.c_str(), "wb"))
  {
    std::fwrite(state.data(), 1, n, f);
    std::fclose(f);
  }
  AddLog(fmt::format("dumped {} bytes to {}", n, path));
  LogParts(system, frame);
}

// Diagnostics: small device states and CoreTiming's event queue as hex, in the log.
void LogParts(Core::System& system, s64 frame)
{
  // Small devices one by one, to name what differs.
  auto part = [&](const char* name, auto&& fn) {
    std::vector<u8> buf(1 << 16);
    u8* ptr = buf.data();
    PointerWrap p(&ptr, buf.size(), PointerWrap::Mode::Write);
    fn(p);
    const size_t len = static_cast<size_t>(ptr - buf.data());
    std::string hex;
    for (size_t i = 0; i < len && i < 8192; ++i)
      hex += fmt::format("{:02x}", buf[i]);
    INFO_LOG_FMT(BRAWLBACK, "machine: frame {} part {} {} bytes: {}", frame, name, len, hex);
  };
  part("cp", [&](PointerWrap& p) { system.GetCommandProcessor().DoState(p); });
  part("pe", [&](PointerWrap& p) { system.GetPixelEngine().DoState(p); });
  part("pi", [&](PointerWrap& p) { system.GetProcessorInterface().DoState(p); });
  part("vi", [&](PointerWrap& p) { system.GetVideoInterface().DoState(p); });
  part("si", [&](PointerWrap& p) { system.GetSerialInterface().DoState(p); });
  part("ai", [&](PointerWrap& p) { system.GetAudioInterface().DoState(p); });
  part("ipc", [&](PointerWrap& p) { system.GetWiiIPC().DoState(p); });
  part("ct", [&](PointerWrap& p) { system.GetCoreTiming().DoState(p); });
  // The session NAND's files a game writes (host side: no guest handle is touched).
  {
    std::string nand;
    const std::string root = File::GetUserPath(D_SESSION_WIIROOT_IDX);
    std::function<void(const File::FSTEntry&)> walk = [&](const File::FSTEntry& e) {
      if (!e.isDirectory)
      {
        std::string data;
        File::ReadFileToString(e.physicalName, data);
        nand += fmt::format("{}:{}:{:08x} ", e.physicalName.substr(root.size()), data.size(),
                            Common::ComputeCRC32(reinterpret_cast<const u8*>(data.data()),
                                                 static_cast<u32>(data.size())));
      }
      for (const auto& c : e.children)
        walk(c);
    };
    for (const char* dir : {"tmp", "title", "shared2"})
      walk(File::ScanDirectoryTree(root + dir, true));
    if (auto fs = system.GetIOS() ? system.GetIOS()->GetFS() : nullptr)
    {
      if (const auto names = fs->ReadDirectory(0, 0, "/tmp"))
      {
        nand += "order:";
        for (const auto& n : *names)
          nand += n + ",";
      }
    }
    INFO_LOG_FMT(BRAWLBACK, "machine: frame {} part nand 0 bytes: {}", frame,
                 Common::ComputeCRC32(reinterpret_cast<const u8*>(nand.data()),
                                      static_cast<u32>(nand.size())));
    INFO_LOG_FMT(BRAWLBACK, "machine: frame {} nandlist {}", frame, nand);
  }
  const auto& ppc = system.GetPPCState();
  INFO_LOG_FMT(BRAWLBACK, "machine: frame {} cpu ticks {} downcount {} pc {:08x} exceptions {:08x}",
               frame, system.GetCoreTiming().GetTicks(), ppc.downcount, ppc.pc, ppc.Exceptions);
}

void SelfLoad(Core::System& system, s64 frame);

// Two slots swap players (a slot move's epoch): everything the session keeps by slot, the links
// and the inputs history included, so the session goes on with the players in their new slots.
void SwapSlots(int a, int b)
{
  std::lock_guard lk(s.net_mutex);
  std::swap(s.peers[a], s.peers[b]);
  for (const int slot : {a, b})
  {
    PeerLink& p = s.peers[slot];
    if (p.slot >= 0)
      p.slot = slot;
    // ENet's peers name their slot (slot + 1, and whether this side connected).
    for (ENetPeer* e : {p.peer, p.connecting})
    {
      if (!e || !e->data)
        continue;
      const intptr_t flags = reinterpret_cast<intptr_t>(e->data) & ~static_cast<intptr_t>(0xFF);
      e->data = reinterpret_cast<void*>(flags | static_cast<intptr_t>(slot + 1));
    }
  }
  if (s.opts.local_slot == a)
    s.opts.local_slot = b;
  else if (s.opts.local_slot == b)
    s.opts.local_slot = a;
  std::swap(s.controller[a], s.controller[b]);
  std::swap(s.latched[a], s.latched[b]);
  for (HistoryEntry& e : s.history)
    std::swap(e.inputs[a], e.inputs[b]);
  const auto swap_bits = [a, b](u32 v) {
    const u32 ba = (v >> a) & 1, bb = (v >> b) & 1;
    v &= ~((1u << a) | (1u << b));
    return v | (ba << b) | (bb << a);
  };
  s.gone = swap_bits(s.gone);
  s.gekko_rx.clear();
  s.gekko_tx.clear();
}

// Every player in the epoch's set: a new GekkoNet session from the epoch's frame on.
void StartEpoch(const State::EpochGo& go)
{
  // A player before this epoch (not the joiner, who just loaded the epoch's image).
  const bool member = LocalPlays();
  const int joiner = s.plan.admit >= 0 && !Plays(s.plan.admit) ? s.plan.admit : -1;
  if (go.move.first >= 0)
  {
    SwapSlots(go.move.first, go.move.second);
    AddLog(fmt::format("slots {} and {} swapped players", go.move.first + 1, go.move.second + 1));
    OSD::AddMessage(fmt::format("Player {} <-> player {}", go.move.first + 1, go.move.second + 1),
                    3000, OSD::Color::CYAN);
  }
  s.epoch = go.epoch;
  s.frame_base = go.frame;
  s.players = go.players;
  s.gone = 0;
  // Every machine runs the epoch's frame with the code caches a joiner starts with: the
  // instruction cache empty and no JIT blocks for RAM. A machine image carries neither (they are
  // filled as the JIT compiles), and code a game writes without invalidating the instruction
  // cache ran stale on the players and fresh on the joiner: their RAM drifted apart later.
  {
    auto& system = Core::System::GetInstance();
    auto& jit = system.GetJitInterface();
    system.GetPPCState().iCache.Reset(jit);
    jit.EraseBlocksForRestoredRange(0x00000000, system.GetMemory().GetRamSizeReal());
    jit.EraseBlocksForRestoredRange(MEM2_BASE, system.GetMemory().GetExRamSizeReal());
  }
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if ((go.players >> slot) & 1)
      s.controller[slot] = go.controllers[slot];
  }
  // How a join the players waited for ended (the joiner sees its game go on).
  if (member && joiner >= 0)
  {
    std::string name;
    {
      std::lock_guard lk(s.net_mutex);
      name = NameOf(joiner);
    }
    ShowNotice((go.players >> joiner) & 1 ? fmt::format("{} is in the game.", name) :
                                            fmt::format("{} could not join.", name));
  }
  s.plan = {};
  s.local_request.reset();
  s.local_move.reset();
  s_image_parts.reset();
  {
    std::lock_guard lk(s.net_mutex);
    s.epoch_go.reset();
    for (PeerLink& p : s.peers)
    {
      if (p.slot < 0)
        continue;
      if ((go.players >> p.slot) & 1)
      {
        p.join_asked = false;
        p.dropped = false;
      }
    }
  }
  s.start_at.reset();
  // Diagnostics (PPR_MACHINE_EPOCH_SELFLOAD=1): every player that did not just load the epoch's
  // machine image saves its own and loads it, so every machine goes on from an image.
  if (const char* e = std::getenv("PPR_MACHINE_EPOCH_SELFLOAD"); e && *e == '1' && member)
    SelfLoad(Core::System::GetInstance(), go.frame);
  DestroyGekko();
  if (!CreateGekko())
  {
    Fail("GekkoNet could not start");
    return;
  }
  s.phase = Phase::Starting;
  AddLog(fmt::format("epoch {}: frame {}, players {:04b}", s.epoch, s.frame_base, s.players.load()));
  OSD::AddMessage("Players changed", 2000, OSD::Color::CYAN);
}

// The players at the epoch's frame: it is final on every machine (every input before it is
// confirmed and GekkoNet ran its rollbacks). Each tells the decider its checksum.
void EnterEpoch()
{
  auto& rb = Rollback::RollbackManager::Get();
  s.phase = Phase::Epoch;
  s.epoch_since = Clock::now();
  s.epoch_sum = rb.FullChecksum();
  s.image_built = false;
  s.image_tenths_shown = -1;
  DumpEpochState(Core::System::GetInstance(), s.plan.frame, "member");
  const int joiner = s.plan.admit;
  const bool admitting = joiner >= 0 && !Plays(joiner);
  if (admitting && s.plan.from != s.opts.local_slot)
  {
    std::string name;
    {
      std::lock_guard lk(s.net_mutex);
      name = NameOf(joiner);
    }
    ShowNotice(fmt::format("{} is joining. The game waits until they have it.", name));
  }
  s_image_parts.reset();
  s.epoch_digest.clear();
  StartScheduleLog(Core::System::GetInstance(), s.plan.frame, "epoch");
  if (admitting)
  {
    ImageParts parts;
    if (MakeImageParts(Core::System::GetInstance(), s.plan.frame, &parts,
                       s.plan.from == s.opts.local_slot))
    {
      s.epoch_digest = ImageDigest(parts);
      // Diagnostics (PPR_MACHINE_DUMP_DIR): the parts, to compare players whose digests differ.
      if (const char* dir = std::getenv("PPR_MACHINE_DUMP_DIR"); dir && *dir)
      {
        for (const auto& [name, bytes] : {std::pair{"state", &parts.state}, std::pair{"nand", &parts.nand}})
        {
          const std::string path =
              fmt::format("{}/image-{}-p{}-{}.bin", dir, s.plan.frame, s.opts.local_slot + 1, name);
          File::WriteStringToFile(path, std::string_view(reinterpret_cast<const char*>(bytes->data()),
                                                         bytes->size()));
        }
      }
      s_image_parts = std::move(parts);
    }
  }
  AddLog(fmt::format("at epoch {} (frame {}), checksum {:08x}, image digest {}", s.epoch + 1,
                     s.plan.frame, s.epoch_sum, s.epoch_digest.substr(0, 16)));
  std::lock_guard lk(s.net_mutex);
  picojson::object at;
  at["type"] = picojson::value("epoch_at");
  at["epoch"] = picojson::value(static_cast<double>(s.epoch + 1));
  at["frame"] = picojson::value(static_cast<double>(s.plan.frame));
  at["sum"] = picojson::value(static_cast<double>(s.epoch_sum));
  if (!s.epoch_digest.empty())
    at["digest"] = picojson::value(s.epoch_digest);
  if (s.plan.from != s.opts.local_slot)
    SendControl(s.plan.from, at);
  // The joiner hears every player's digest of the image it is about to get.
  if (admitting && s.peers[joiner].hello)
    SendControl(joiner, at);
}

picojson::array ControllerNames(const std::array<Controller, MAX_PLAYERS>& c)
{
  picojson::array a;
  for (const Controller x : c)
    a.emplace_back(x == Controller::WiiRemote ? "wiimote" : "gc");
  return a;
}

// At an epoch that admits a joiner, every wait loop iteration (every player): the joiner's
// progress with the machine image, in the chat window at every tenth it reports.
void ShowJoinProgress()
{
  const int joiner = s.plan.admit;
  if (joiner < 0 || Plays(joiner))
    return;
  std::string notice;
  {
    std::lock_guard lk(s.net_mutex);
    const PeerLink& j = s.peers[joiner];
    if (j.image_epoch != s.epoch + 1 || j.image_tenths <= s.image_tenths_shown)
      return;
    s.image_tenths_shown = j.image_tenths;
    notice = s.plan.from == s.opts.local_slot ?
                 fmt::format("Sending the game to {}: {}%", NameOf(joiner), j.image_tenths * 10) :
                 fmt::format("{} is getting the game: {}%", NameOf(joiner), j.image_tenths * 10);
  }
  ShowNotice(notice);
}

// At an epoch, every wait loop iteration. The decider waits for every player's checksum, sends
// the joiner the machine, waits for the joiner's checksum, and says who plays from now on.
void EpochStep(Core::System& system)
{
  const int next = s.epoch + 1;
  // GekkoNet keeps sending this player's inputs and acks: a player still waiting for the last
  // frames before the epoch to be confirmed gets them.
  if (s.gekko)
    gekko_network_poll(s.gekko);
  ShowJoinProgress();
  if (Clock::now() - s.epoch_since > EPOCH_TIMEOUT)
  {
    Fail("The players did not agree on the change of players");
    return;
  }
  if (s.plan.from != s.opts.local_slot)
  {
    std::optional<State::EpochGo> go;
    {
      std::lock_guard lk(s.net_mutex);
      if (s.epoch_go && s.epoch_go->epoch == next && s.epoch_go->frame == s.plan.frame)
        go = s.epoch_go;
    }
    if (go)
      StartEpoch(*go);
    return;
  }
  const int joiner = s.plan.admit;
  u32 others = 0;
  bool joiner_ok = false, joiner_gone = false, all_at = true;
  {
    std::lock_guard lk(s.net_mutex);
    for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    {
      if (!Plays(slot) || slot == s.opts.local_slot)
        continue;
      const PeerLink& p = s.peers[slot];
      if (p.left)
        continue;
      others |= 1u << slot;
      if (p.epoch_at != next)
        all_at = false;
      else if (p.epoch_sum != s.epoch_sum)
        WARN_LOG_FMT(BRAWLBACK, "machine: player {} is at epoch {} with checksum {:08x}, not {:08x}",
                     slot + 1, next, p.epoch_sum, s.epoch_sum);
    }
    if (joiner >= 0 && !Plays(joiner))
    {
      const PeerLink& j = s.peers[joiner];
      joiner_gone = j.slot < 0 || j.left || !j.hello;
      joiner_ok = !joiner_gone && j.epoch_at == next && j.epoch_sum == s.epoch_sum;
      if (!joiner_gone && j.epoch_at == next && j.epoch_sum != s.epoch_sum)
      {
        WARN_LOG_FMT(BRAWLBACK, "machine: player {} loaded the machine with checksum {:08x}, not {:08x}",
                     joiner + 1, j.epoch_sum, s.epoch_sum);
        joiner_gone = true;
      }
    }
  }
  if (!all_at)
    return;
  const bool admitting = joiner >= 0 && !Plays(joiner);
  if (admitting && !joiner_gone && !s.image_built)
  {
    s.image_built = true;
    const auto t0 = Clock::now();
    u64 raw = 0;
    std::vector<u8> image =
        s_image_parts ? BuildImage(*s_image_parts, s.epoch_sum, &raw) : std::vector<u8>{};
    const u32 copies_count = s_image_parts ? s_image_parts->copies_count : 0;
    const size_t copies_bytes = s_image_parts ? s_image_parts->copies.size() : 0;
    s_image_parts.reset();
    s.image_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    s.image_packed = image.size();
    s.image_raw = raw;
    if (image.empty())
    {
      AddLog("the machine image could not be made");
      joiner_gone = true;
    }
    else
    {
      AddLog(fmt::format("machine image for player {}: {:.1f} MB ({:.1f} MB raw, {} EFB copies "
                         "{:.1f} MB) in {:.0f} ms",
                         joiner + 1, image.size() / 1048576.0, raw / 1048576.0, copies_count,
                         copies_bytes / 1048576.0, s.image_ms));
      std::string name;
      {
        std::lock_guard lk(s.net_mutex);
        name = NameOf(joiner);
      }
      ShowNotice(fmt::format("Sending the game to {} ({:.1f} MB). The game waits until they have it.",
                             name, image.size() / 1048576.0));
      std::vector<u8> header(36);
      const u32 magic = IMAGE_MAGIC, ep = static_cast<u32>(next), sum = s.epoch_sum;
      const s64 frame = s.plan.frame;
      const u64 packed = image.size();
      std::memcpy(header.data(), &magic, 4);
      std::memcpy(header.data() + 4, &ep, 4);
      std::memcpy(header.data() + 8, &frame, 8);
      std::memcpy(header.data() + 16, &sum, 4);
      std::memcpy(header.data() + 20, &packed, 8);
      std::memcpy(header.data() + 28, &raw, 8);
      std::lock_guard lk(s.net_mutex);
      PeerLink& j = s.peers[joiner];
      if (j.peer)
      {
        enet_peer_send(j.peer, CHANNEL_IMAGE,
                       enet_packet_create(header.data(), header.size(), ENET_PACKET_FLAG_RELIABLE));
      }
      s.image_tx = std::move(image);
      s.image_to = joiner;
      s.image_tx_sent = 0;
    }
  }
  if (admitting && !joiner_gone && !joiner_ok)
    return;  // the image is on its way, or loading there
  State::EpochGo go;
  go.epoch = next;
  go.frame = s.plan.frame;
  go.players = (1u << s.opts.local_slot) | others | (admitting && joiner_ok ? 1u << joiner : 0);
  go.controllers = s.controller;
  if (admitting && joiner_ok)
    go.controllers[joiner] = s.plan.controller;
  if (s.plan.move.first >= 0)
  {
    // As after the swap (every machine swaps its own state at StartEpoch).
    const int a = s.plan.move.first, b = s.plan.move.second;
    const u32 ba = (go.players >> a) & 1, bb = (go.players >> b) & 1;
    go.players = (go.players & ~((1u << a) | (1u << b))) | (ba << b) | (bb << a);
    std::swap(go.controllers[a], go.controllers[b]);
    go.move = s.plan.move;
    s.move_done = s.plan.move;
  }
  {
    std::lock_guard lk(s.net_mutex);
    picojson::object m;
    m["type"] = picojson::value("epoch_go");
    m["epoch"] = picojson::value(static_cast<double>(go.epoch));
    m["frame"] = picojson::value(static_cast<double>(go.frame));
    m["players"] = picojson::value(static_cast<double>(go.players));
    m["controllers"] = picojson::value(ControllerNames(go.controllers));
    if (go.move.first >= 0)
    {
      picojson::array mv;
      mv.emplace_back(static_cast<double>(go.move.first));
      mv.emplace_back(static_cast<double>(go.move.second));
      m["move"] = picojson::value(mv);
    }
    BroadcastControl(m);
    if (admitting && !joiner_ok && s.peers[joiner].hello)
    {
      picojson::object no;
      no["type"] = picojson::value("join_refused");
      no["why"] = picojson::value("the machine image did not load");
      SendControl(joiner, no);
    }
    s.image_tx.clear();
  }
  StartEpoch(go);
}

// A joiner, every wait loop iteration until it is admitted: ask, load the image, wait for the go.
// A joiner's SD card before it asks to join (docs/design/brawl-mods.md §6.5): the game must
// never wait for the network, so a joiner gets the whole session card first, while the room plays
// on. It hashes its own card (the cache, or a full read), asks a player for the session card's
// manifest and file list (checked against the root every player's hello gave), and works out what
// differs. Its player agrees (the launcher's dialog, or Options::sd_sync), then it fetches every
// chunk it has not got, and writes them into its own card if its player chose that. Returns true
// once it has the whole card (or the session has none); until then it does not ask to join.
bool SDSyncStep()
{
  using Step = State::SDSync::Step;
  State::SDSync& y = s.sd_sync;
  if (y.step == Step::Ready)
    return true;
  if (y.step == Step::Failed)
    return false;
  auto fail = [&](const std::string& why) {
    y.step = Step::Failed;
    Fail(why);
    return false;
  };

  // The session's players and the card they play (their hellos).
  std::string root;
  int source = -1;
  bool card = false, disagree = false, waiting = false;
  std::string other_version;
  {
    std::lock_guard lk(s.net_mutex);
    for (const PeerLink& p : s.peers)
    {
      if (p.slot < 0 || !p.hello || p.late)
        continue;
      if (p.proto != PROTOCOL_VERSION)
        other_version = NameOf(p.slot);
      card |= p.sd_card;
      if (p.sd_card && p.sd_root.empty())
        waiting = true;  // still hashing its card
      if (p.sd_root.empty())
        continue;
      if (root.empty())
        root = p.sd_root;
      else if (p.sd_root != root)
        disagree = true;
      if (source < 0)
        source = p.slot;
    }
  }
  if (!other_version.empty())
    return fail(fmt::format("{} has another version of Brawl Online: everyone needs the latest",
                            other_version));
  if (!card)
  {
    y.step = Step::Ready;
    return true;
  }
  if (disagree)
    return fail("the room's players do not play the same SD card");
  if (waiting || root.empty())
    return false;

  const auto now = Clock::now();
  if (y.step == Step::Idle)
  {
    if (s.sd_path.empty())
      return fail("the room's game has an SD card, and this one has none");
    AddLog("SD card: comparing this player's card with the session's");
    y.hash_thread = std::thread([path = s.sd_path] {
      State::SDSync& t = s.sd_sync;
      std::optional<SDCard::ImageHashes> own =
          SDCard::HashImage(path, &t.hash_cancel, [](u64 done, u64 total) {
            s.sd_sync.hash_done = done;
            s.sd_sync.hash_total = total;
          });
      // No card, or one that cannot be read: everything comes from the players.
      if (!own)
        own = SDCard::ImageHashes{};
      {
        std::lock_guard lk(t.own_mutex);
        t.own = std::move(own);
      }
      t.hash_finished = true;
    });
    std::lock_guard lk(s.net_mutex);
    y.from = source;
    y.asked = now;
    y.received = false;
    s.sd_tx.emplace_back(source, std::vector<u8>{SD_MANIFEST_REQUEST});
    y.step = Step::Checking;
    return false;
  }

  if (y.step == Step::Checking)
  {
    const u64 total = y.hash_total.load();
    if (!y.hash_finished && total > 0)
    {
      const int tenths = static_cast<int>(y.hash_done.load() * 10 / total);
      if (tenths > y.tenths_shown)
      {
        y.tenths_shown = tenths;
        ShowNotice(fmt::format("Checking your SD card: {}%", tenths * 10));
      }
    }
    std::vector<u8> packet;
    double packet_ms = 0;
    {
      std::lock_guard lk(s.net_mutex);
      if (!y.received)
      {
        if (now - y.asked > std::chrono::seconds(60))
          return fail("the room's host did not send its SD card's list");
        return false;
      }
      if (!y.hash_finished)
        return false;
      packet = y.packet;
      packet_ms = y.packet_ms;
    }
    // [u32 manifest size][manifest][file list], from a player.
    if (packet.size() < 4)
      return fail("the SD card's list from the host is not valid");
    u32 msize;
    std::memcpy(&msize, packet.data(), 4);
    if (msize > packet.size() - 4)
      return fail("the SD card's list from the host is not valid");
    sf::Packet mp;
    mp.append(packet.data() + 4, msize);
    std::optional<SDCard::Manifest> manifest = SDCard::ReadManifest(mp);
    if (!manifest)
      return fail("the SD card's list from the host is not valid");
    if (SDCard::RootHex(SDCard::ManifestRoot(*manifest)) != root)
      return fail("the SD card's list from the host is not the card the room plays");
    std::vector<SDCard::CardFile> files;
    if (packet.size() > 4 + msize)
    {
      sf::Packet fp;
      fp.append(packet.data() + 4 + msize, packet.size() - 4 - msize);
      if (auto list = SDCard::ReadFileList(fp, manifest->ChunkCount()))
        files = std::move(*list);
      y.files_wire.assign(packet.begin() + 4 + msize, packet.end());
    }
    auto shared = std::make_shared<const SDCard::Manifest>(std::move(*manifest));
    {
      std::lock_guard lk(y.own_mutex);
      y.card = SDCard::SessionCard::CreateGuestCard(
          s.sd_path, shared, File::GetUserPath(D_CACHE_IDX) + "MachineSD",
          std::make_shared<SessionCardRemote>(), y.own ? &*y.own : nullptr);
    }
    const std::vector<u32> missing = y.card->MissingChunks();
    y.missing_bytes = y.card->MissingBytes();
    const std::vector<const SDCard::CardFile*> touched = SDCard::FilesTouching(files, missing);
    y.missing_files = static_cast<u32>(touched.size());
    y.names.clear();
    for (size_t i = 0; i < touched.size() && i < 50; ++i)
      y.names.push_back(touched[i]->path);
    // The link's pace, from the list's own trip (at least 64 KB/s: a guess, not a promise).
    y.rate = std::max(64.0 * 1024, packet.size() / std::max(packet_ms / 1000.0, 0.001));
    {
      std::unique_ptr<SDCard::CardServer> old;
      {
        std::lock_guard lk(s.net_mutex);
        s.sd_source = y.from;
        old = SetSessionCardLocked(y.card);
        s.sd_files_wire = y.files_wire;
      }
    }
    if (missing.empty())
    {
      AddLog("SD card: this player's card is the session's");
      y.step = Step::Ready;
      return true;
    }
    AddLog(fmt::format("SD card: {:.1f} MB of the session's card differ from this player's ({} "
                       "files{}{})",
                       y.missing_bytes / 1048576.0, y.missing_files,
                       y.names.empty() ? "" : ", ", y.names.empty() ? "" : y.names.front()));
    y.step = Step::Asking;
    if (y.choice.load() == static_cast<int>(SDSyncChoice::Ask))
    {
      ShowNotice(fmt::format("Your SD card is {:.0f} MB different from the host's: answer in the "
                             "room window.{}",
                             std::ceil(y.missing_bytes / 1048576.0),
                             SDBigDifferenceNote(y.missing_bytes, true)));
    }
    return false;
  }

  if (y.step == Step::Asking)
  {
    const auto choice = static_cast<SDSyncChoice>(y.choice.load());
    if (choice == SDSyncChoice::Ask)
      return false;
    if (choice == SDSyncChoice::No)
      return fail("the host's SD card was not downloaded");
    AddLog(choice == SDSyncChoice::Overwrite ?
               "SD card: downloading the host's card, then writing it into this player's" :
               "SD card: downloading the host's card");
    {
      std::lock_guard lk(s.net_mutex);
      picojson::object m;
      m["type"] = picojson::value("sd_sync");
      m["state"] = picojson::value("start");
      m["bytes"] = picojson::value(static_cast<double>(y.missing_bytes));
      BroadcastControl(m);
    }
    ShowNotice(fmt::format("Your SD card does not match the host's. Getting {:.0f} MB from the "
                           "players.{}",
                           std::ceil(y.missing_bytes / 1048576.0),
                           SDBigDifferenceNote(y.missing_bytes, true)));
    y.card->StartSync();
    y.download_since = now;
    y.progress.clear();
    y.rate_now = 0;
    y.tenths_shown = 0;
    y.step = Step::Downloading;
    return false;
  }

  if (y.step == Step::Downloading)
  {
    const SDCard::SessionCard::SyncProgress pr = y.card->GetSyncProgress();
    if (pr.failed)
      return fail("could not get the host's SD card: a player stopped sending it");
    // The rate over the last 3 s.
    y.progress.emplace_back(now, pr.done);
    while (y.progress.size() > 2 && now - y.progress.front().first > std::chrono::seconds(3))
      y.progress.pop_front();
    const double span = std::chrono::duration<double>(now - y.progress.front().first).count();
    if (span > 0.5)
      y.rate_now = (pr.done - y.progress.front().second) / span;
    const int tenths = pr.total ? static_cast<int>(pr.done * 10 / pr.total) : 10;
    if (tenths > y.tenths_shown)
    {
      y.tenths_shown = tenths;
      ShowNotice(fmt::format("Getting the host's SD card: {}% ({:.1f} MB/s)", tenths * 10,
                             y.rate_now / 1048576.0));
    }
    if (!pr.finished)
      return false;
    const double secs = std::chrono::duration<double>(now - y.download_since).count();
    AddLog(fmt::format("SD card: got {:.1f} MB of the host's card in {:.1f} s", pr.total / 1048576.0,
                       secs));
    {
      std::lock_guard lk(s.net_mutex);
      picojson::object m;
      m["type"] = picojson::value("sd_sync");
      m["state"] = picojson::value("done");
      BroadcastControl(m);
    }
    if (static_cast<SDSyncChoice>(y.choice.load()) == SDSyncChoice::Overwrite)
    {
      std::string why;
      std::lock_guard lk(y.own_mutex);
      if (!y.card->WriteFetchedToImage(y.own ? *y.own : SDCard::ImageHashes{}, &why))
        return fail("could not write the host's SD card into yours: " + why);
      AddLog(fmt::format("SD card: wrote the host's card into {}", s.sd_path));
    }
    y.step = Step::Ready;
    return true;
  }
  return false;
}

// A room joiner hears from every member before it checks its SD card or asks to join, and its
// game shows nothing while it waits. A member it still has no link to is named in the chat after
// NO_LINK_NOTICE, and after NO_LINK_LIMIT the join gives up saying who, rather than a frozen game
// until JOIN_TIMEOUT's "Not admitted" (bug report #149: three joins of a REX room whose host never
// answered, 30 attempts and no hello either way, each after the player said Yes to downloading
// the host's SD card; the download waits for that link, so it never started). Under net_mutex:
// returns why to fail, or "".
constexpr auto NO_LINK_NOTICE = std::chrono::seconds(10);
constexpr auto NO_LINK_LIMIT = std::chrono::seconds(60);

std::string JoinNoLinkStep(Clock::time_point now, std::vector<std::string>* notices)
{
  std::vector<std::string> shown, given_up;
  for (PeerLink& p : s.peers)
  {
    if (p.slot < 0 || !p.no_link_watch || p.hello || p.left)
      continue;
    if (now - p.link_wanted >= NO_LINK_LIMIT)
      given_up.push_back(NameOf(p.slot));
    else if (now - p.link_wanted >= NO_LINK_NOTICE && !p.no_link_shown)
      shown.push_back(NameOf(p.slot));
    if (now - p.link_wanted >= NO_LINK_NOTICE)
      p.no_link_shown = true;
  }
  if (!given_up.empty())
  {
    return fmt::format("no connection to {} after {} s: your network or theirs blocks a direct "
                       "connection between you",
                       fmt::join(given_up, ", "), NO_LINK_LIMIT.count());
  }
  if (!shown.empty())
  {
    const auto choice = static_cast<SDSyncChoice>(s.sd_sync.choice.load());
    notices->push_back(fmt::format(
        "Can't connect to {} yet. {}", fmt::join(shown, ", "),
        choice == SDSyncChoice::Yes || choice == SDSyncChoice::Overwrite ?
            "The SD card download starts once connected." :
            "Joining waits for it."));
  }
  return "";
}

void JoinStep(Core::System& system)
{
  if (Clock::now() - s.join_since > JOIN_TIMEOUT)
  {
    Fail("Not admitted to the session");
    return;
  }
  std::optional<State::EpochGo> go;
  bool image = false;
  std::optional<std::string> refused;
  std::string no_link;
  std::vector<std::string> notices;
  bool ready_to_sync = false;
  {
    std::lock_guard lk(s.net_mutex);
    if (s.join_refused)
      refused = s.join_refused_why;
    if (s.opts.room && !s.join_sent)
      no_link = JoinNoLinkStep(Clock::now(), &notices);
    // The image's progress, in the chat window at every tenth (the game waits for it: across an
    // ocean a REX image took about a minute), and to the players for theirs.
    const State::ImageRx& rx = s.image_rx;
    if (rx.from >= 0 && !s.join_loaded && rx.packed_size > 0)
    {
      if (s.join_tenths_shown < 0)
      {
        notices.push_back(fmt::format("Getting the game from {} ({:.1f} MB)", NameOf(rx.from),
                                      rx.packed_size / 1048576.0));
        s.join_tenths_shown = 0;
      }
      const int tenths = static_cast<int>(rx.data.size() * 10 / rx.packed_size);
      if (tenths > s.join_tenths_shown)
      {
        s.join_tenths_shown = tenths;
        notices.push_back(fmt::format("Getting the game: {}%", tenths * 10));
        picojson::object m;
        m["type"] = picojson::value("image_progress");
        m["epoch"] = picojson::value(static_cast<double>(rx.epoch));
        m["tenths"] = picojson::value(static_cast<double>(tenths));
        BroadcastControl(m);
      }
    }
    if (!s.join_sent && (!s.opts.room || s.room_known))
    {
      // The session's players (not other joiners, who may not be there yet). A room's joiner
      // knows them by their hello (`playing`), so it hears from every member first.
      bool all = true;
      for (const PeerLink& p : s.peers)
        all &= p.slot < 0 || p.late || p.hello;
      ready_to_sync = all;
      if (all && s.sd_sync.step == State::SDSync::Step::Ready)
      {
        picojson::object m;
        m["type"] = picojson::value("join");
        m["controller"] = picojson::value(s.opts.controller == Controller::WiiRemote ? "wiimote" : "gc");
        m["rtc"] = picojson::value(static_cast<double>(s.opts.rtc));
        BroadcastControl(m);
        s.join_sent = true;
        AddLog("asked to join");
      }
    }
    image = !s.join_loaded && s.image_rx.complete;
    // Not before every player's digest of this epoch's image is in.
    for (const PeerLink& p : s.peers)
    {
      if (MustVouch(p, s.image_rx.epoch) && p.epoch_at != s.image_rx.epoch)
        image = false;
    }
    if (s.join_loaded && s.epoch_go && s.epoch_go->frame == s.current_frame + 1)
      go = s.epoch_go;
  }
  for (const std::string& n : notices)
    ShowNotice(n);
  if (refused)
  {
    Fail("Could not join: " + *refused);
    return;
  }
  if (!no_link.empty())
  {
    Fail(no_link);
    return;
  }
  // The SD card first (its player may take a while to answer, and a download as long as it
  // takes): the join's timeout counts from when it is done.
  if (ready_to_sync && !s.join_sent && s.sd_sync.step != State::SDSync::Step::Ready)
  {
    s.join_since = Clock::now();
    SDSyncStep();
    return;
  }
  if (image)
  {
    const int from = s.image_rx.from;
    const int epoch = s.image_rx.epoch;
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - s.image_rx.started).count();
    const u64 packed = s.image_rx.packed_size;
    std::string why;
    const auto t0 = Clock::now();
    if (!LoadImage(system, &why))
    {
      Fail("Could not load the session's machine: " + why);
      return;
    }
    s.join_loaded = true;
    DumpEpochState(system, s.current_frame + 1, "joiner");
    AddLog(fmt::format("machine image from player {} ({:.1f} MB in {:.0f} ms) loaded in {:.0f} ms; "
                       "at frame {}",
                       from + 1, packed / 1048576.0, ms,
                       std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
                       s.current_frame + 1));
    std::lock_guard lk(s.net_mutex);
    picojson::object at;
    at["type"] = picojson::value("epoch_at");
    at["epoch"] = picojson::value(static_cast<double>(epoch));
    at["frame"] = picojson::value(static_cast<double>(s.current_frame + 1));
    at["sum"] = picojson::value(static_cast<double>(s.epoch_sum));
    SendControl(from, at);
    return;
  }
  if (go)
  {
    if (!((go->players >> s.opts.local_slot) & 1))
    {
      Fail("Could not join: not admitted");
      return;
    }
    StartEpoch(*go);
  }
}

// Every Wii Remote is plugged in exactly while its player's input says so (the same frame on
// every machine): a joiner's connects, a leaver's disconnects.
void SyncWiimotes(Core::System& system)
{
  const auto ios = system.GetIOS();
  if (!ios)
    return;
  // The session forces Bluetooth passthrough off (the netplay config layer): the device is the
  // emulated stack.
  const auto bt = std::static_pointer_cast<IOS::HLE::BluetoothEmuDevice>(
      ios->GetDeviceByName("/dev/usb/oh1/57e/305"));
  if (!bt)
    return;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    auto* wiimote = bt->AccessWiimoteByIndex(static_cast<size_t>(slot));
    if (!wiimote)
      continue;
    // Activate(true) only acts on a Wii Remote that is off, Activate(false) on a connected one.
    wiimote->Activate((s.latched[slot][0] & INPUT_WIIMOTE_CONNECTED) != 0);
  }
}

enum class Wait
{
  Frames,  // this pass has a frame to run
  Ended,   // the session ended: run the pass as a plain local frame
  Yield,   // the emulation is pausing or stopping: run no game logic this pass
};

// Called at iteration 0 of a pass: runs the connection, the barrier and GekkoNet until this pass
// has a frame to run. Waits on the host, never in the guest: guest code run while waiting would
// move emulated time in a way no other machine (and no re-run) repeats.
// Diagnostics (PPR_MACHINE_PACE_LOG=1): where the session's wall-clock time goes, a line a
// second: displayed and re-run frames, rollbacks, the waits for GekkoNet (no frame to run yet;
// the throttle forgets them), displayed frames that came late, the time-sync speed and frames
// ahead, and how far the throttle fell behind its schedule.

bool PaceLogOn()
{
  static const bool on = std::getenv("PPR_MACHINE_PACE_LOG") != nullptr;
  return on;
}

void PaceWait(Clock::duration waited)
{
  if (!PaceLogOn())
    return;
  s_pace.waits++;
  s_pace.wait_ms += std::chrono::duration<double, std::milli>(waited).count();
}

void PaceLoad(Clock::duration took)
{
  if (!PaceLogOn())
    return;
  const double ms = std::chrono::duration<double, std::milli>(took).count();
  const auto& rb = Rollback::RollbackManager::Get();
  s_pace.load_ms += ms;
  s_pace.load_max_ms = std::max(s_pace.load_max_ms, ms);
  s_pace.load_sync_ms += rb.m_last_load_sync_us / 1000.0;
  s_pace.load_dostate_ms += rb.m_last_load_dostate_us / 1000.0;
  s_pace.load_granules += rb.m_last_load_granules;
  s_pace.load_changed += rb.m_last_load_changed;
  s_pace.load_jit_ms += rb.m_last_load_jit_us / 1000.0;
  s_pace.load_jit_dropped += rb.m_last_load_jit_dropped;
}

void PaceSave(Clock::duration took)
{
  if (!PaceLogOn())
    return;
  const double ms = std::chrono::duration<double, std::milli>(took).count();
  s_pace.save_ms += ms;
  s_pace.save_max_ms = std::max(s_pace.save_max_ms, ms);
  s_pace.saves++;
}

void PaceFrame(Core::System& system, bool resim)
{
  if (!PaceLogOn())
    return;
  const auto now = Clock::now();
  if (s_pace.since == Clock::time_point{})
    s_pace.since = now;
  if (!Rollback::FrameProfile::Enabled())
  {
    Rollback::FrameProfile::SetEnabled(true);
    Rollback::FrameProfile::SetTimeEvents(true);
    s_pace_pass_c0 = Rollback::FrameProfile::Snapshot();
  }
  if (s_pace_pass_resim && s_pace_pass_start != Clock::time_point{})
  {
    const double ms = std::chrono::duration<double, std::milli>(now - s_pace_pass_start).count();
    s_pace.resim_ms += ms;
    s_pace.resim_max_ms = std::max(s_pace.resim_max_ms, ms);
  }
  {
    const auto c = Rollback::FrameProfile::Snapshot();
    Rollback::FrameProfile::Accumulate(s_pace_pass_resim ? &s_pace.resim_c : &s_pace.shown_c,
                                       Rollback::FrameProfile::Delta(c, s_pace_pass_c0));
    s_pace_pass_c0 = c;
  }
  s_pace_pass_start = now;
  s_pace_pass_resim = resim;
  if (resim)
  {
    s_pace.resim++;
    return;
  }
  if (s_pace.last_frame != Clock::time_point{})
  {
    const double ms = std::chrono::duration<double, std::milli>(now - s_pace.last_frame).count();
    if (ms > 25)
    {
      s_pace.late25++;
      s_pace.late_ms += ms - 1000.0 / 59.94;
    }
    if (ms > 50)
      s_pace.late50++;
  }
  s_pace.last_frame = now;
  s_pace.frames++;
  s_pace.speed_sum += s.speed;
  s_pace.ahead_sum += s.frames_ahead;
  s_pace.ahead_min = std::min(s_pace.ahead_min, s.frames_ahead);
  s_pace.ahead_max = std::max(s_pace.ahead_max, s.frames_ahead);
  const double span = std::chrono::duration<double, std::milli>(now - s_pace.since).count();
  if (span < 1000)
    return;
  const double behind = std::chrono::duration<double, std::milli>(
                            system.GetCoreTiming().TakeMaxBehindSchedule())
                            .count();
  const u32 n = std::max<u32>(s_pace.frames, 1);
  NOTICE_LOG_FMT(BRAWLBACK,
                 "machine pace: {:.0f} ms: {} frames ({:.2f}/s), {} re-run, {} rollbacks; {} waits "
                 "{:.1f} ms; {} frames over 25 ms ({} over 50), {:.1f} ms late in all; speed "
                 "{:.4f}; ahead {:.2f} [{:.2f}, {:.2f}]; behind schedule at most {:.1f} ms",
                 span, s_pace.frames, s_pace.frames * 1000.0 / span, s_pace.resim, s_pace.rollbacks,
                 s_pace.waits, s_pace.wait_ms, s_pace.late25, s_pace.late50, s_pace.late_ms,
                 s_pace.speed_sum / n, s_pace.ahead_sum / n, s_pace.ahead_min, s_pace.ahead_max,
                 behind);
  if (s_pace.rollbacks > 0 || s_pace.saves > 0)
  {
    const u32 r = std::max<u32>(s_pace.rollbacks, 1);
    NOTICE_LOG_FMT(
        BRAWLBACK,
        "machine pace costs: loads {:.1f} ms (each {:.2f}, at most {:.2f}: video thread "
        "{:.2f}, devices {:.2f}, {} granules, {} changed, JIT {:.2f}, {} dropped its blocks); "
        "re-run frames {:.1f} ms (each {:.2f}, at most {:.2f}); {} saves {:.1f} ms (each "
        "{:.2f}, at most {:.2f})",
        s_pace.load_ms, s_pace.load_ms / r, s_pace.load_max_ms, s_pace.load_sync_ms / r,
        s_pace.load_dostate_ms / r, s_pace.load_granules / r, s_pace.load_changed / r,
        s_pace.load_jit_ms / r, s_pace.load_jit_dropped, s_pace.resim_ms,
        s_pace.resim ? s_pace.resim_ms / s_pace.resim : 0.0, s_pace.resim_max_ms, s_pace.saves,
        s_pace.save_ms, s_pace.saves ? s_pace.save_ms / s_pace.saves : 0.0, s_pace.save_max_ms);
  }
  {
    // Per frame, in ms: JIT compiling (blocks), CoreTiming events by kind, waits for the GPU
    // thread, and the GPU thread's busy time; shown frames add the throttle's sleep.
    auto line = [](const Rollback::FrameProfile::Counters& c, u32 frames) {
      const double f = std::max<u32>(frames, 1) * 1e6;
      u64 wait = 0;
      for (const u64 w : c.gpu_wait_ns)
        wait += w;
      return fmt::format(
          "jit {:.2f} ({:.1f} blocks), events gpu {:.2f} vi {:.2f} audio {:.2f} ios "
          "{:.2f} devices {:.2f}, gpu waits {:.2f}, gpu busy {:.2f}, throttle {:.2f}",
          c.jit_ns / f, c.jit_blocks / (f / 1e6), c.event_ns[0] / f, c.event_ns[1] / f,
          c.event_ns[2] / f, c.event_ns[3] / f, c.event_ns[4] / f, wait / f, c.gpu_busy_ns / f,
          c.throttle_ns / f);
    };
    NOTICE_LOG_FMT(BRAWLBACK, "machine pace profile: re-run frame: {}; shown frame: {}",
                   line(s_pace.resim_c, s_pace.resim), line(s_pace.shown_c, s_pace.frames));
  }
  const auto last = s_pace.last_frame;
  s_pace = {};
  s_pace.since = now;
  s_pace.last_frame = last;
}

// A wait the game stands still in for a while (an epoch while a joiner gets the machine, a
// joiner's own wait, a handshake): the video thread shows the last frame again about 30 times a
// second, so the overlay keeps drawing (the chat window with a join's progress, OSD messages), as
// in a paused replay. Before, the chat window froze with the game. Nothing reaches the machine.
void RedrawWhileWaiting(Core::System& system, Clock::duration waited_for)
{
  static Clock::time_point last;
  const auto now = Clock::now();
  if (waited_for < std::chrono::milliseconds(100) || now - last < std::chrono::milliseconds(33))
    return;
  last = now;
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->RedrawLastFrame();
  });
  // The event wakes the GPU thread, which then sleeps again only once GpuMaySleep is called. That
  // is a CoreTiming event (1000 a second of emulated time), and emulated time stands still here:
  // without this the GPU thread spun for the whole wait (as playback did, bug report #17).
  system.GetFifo().GpuMaySleep();
}

void LateEndOfFrame();

// The boundary sleep, until `due` (the boundary's scheduled host time). A rollback's load and
// re-runs come after it, and the presented frame after them keeps its schedule
// (BeginRollbackBurst), so what they take past `due` makes that frame late, the more so in a
// game that copies its frame early in the field (bug report #152: Naruto: Super Clash of Ninja 4
// at 200 ms "feels like frame skip", a Wii game with the same player smooth; Mario Party 4 at
// 200 ms showed a third of its frames after a rollback late). Without the sleep the CPU thread
// reached the boundary ahead of schedule by the rest of the field, which the re-runs used. Now
// the sleep ends early by what the pending rollback is expected to take (the frames it re-runs
// at the session's average per frame, and a margin), looked at again as remote inputs come in
// during it; with none pending it sleeps to `due` as before. PPR_MACHINE_RERUN_IN_SLEEP=0: the
// whole sleep always (A/B).
void BoundarySleep(CoreTiming::CoreTimingManager& ct, Clock::time_point due)
{
  static const bool rerun_in_sleep = [] {
    const char* e = std::getenv("PPR_MACHINE_RERUN_IN_SLEEP");
    return !(e && *e == '0');
  }();
  constexpr auto SLICE = std::chrono::milliseconds(2);
  constexpr auto MARGIN = std::chrono::milliseconds(1);
  for (;;)
  {
    auto wake = due;
    if (rerun_in_sleep && s.gekko)
    {
      gekko_receive(s.gekko);
      if (const int frames = gekko_rollback_frames(s.gekko); frames > 0)
      {
        wake -= std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double, std::milli>(frames * s.burst_ms_per_frame)) +
                MARGIN;
      }
    }
    const auto now = Clock::now();
    if (now >= wake)
      return;
    s.mutex.unlock();
    {
      Rollback::FrameProfile::Scope sleep_scope{Rollback::FrameProfile::AddThrottle};
      ct.SleepUntil(rerun_in_sleep ? std::min(wake, now + SLICE) : wake);
    }
    s.mutex.lock();
  }
}

Wait WaitForFrames(Core::System& system)
{
  // A peer that sends nothing for this long is gone (GekkoNet drops one after 5 s of silence; a
  // start barrier or handshake that never completes ends the same way).
  constexpr auto STALL_LIMIT = std::chrono::seconds(10);
  constexpr auto CONNECT_LIMIT = std::chrono::seconds(60);
  auto& cpu = system.GetCPU();
  bool waited = false;
  // Each limit counts from the start of its own phase: an epoch (or a joiner's wait for the
  // machine image) can take up to EPOCH_TIMEOUT inside this same wait, and the handshake after it
  // gets its own CONNECT_LIMIT.
  // The boundary sleep (on; PPR_MACHINE_BOUNDARY_SLEEP=0: off): at a VI-field boundary the
  // throttle's sleep comes before the local controller is read, not at the frame's first SI poll
  // or Wii Remote update after it, so the read is as fresh as the game's own and still sent at the
  // frame's start (docs/design/brawl-mods.md 6.2).
  static const bool boundary_sleep = [] {
    const char* e = std::getenv("PPR_MACHINE_BOUNDARY_SLEEP");
    return !(e && *e == '0');
  }();
  if (boundary_sleep && s.phase == Phase::Running && SessionBoundary() == Boundary::ViField)
  {
    auto& ct = system.GetCoreTiming();
    if (const auto due = ct.ScheduledHostTime(); due && *due > Clock::now())
      BoundarySleep(ct, *due);
  }
  Phase timed_phase = s.phase;
  auto since = Clock::now();
  for (;;)
  {
    if (s.phase != timed_phase)
    {
      timed_phase = s.phase;
      since = Clock::now();
    }
    const auto waited_for = Clock::now() - since;
    if ((s.phase == Phase::Running && waited_for > STALL_LIMIT) ||
        ((s.phase == Phase::Connecting || s.phase == Phase::Starting) &&
         waited_for > CONNECT_LIMIT))
    {
      Fail(s.phase == Phase::Running ? "Connection lost" : "Could not connect to the players");
      return Wait::Ended;
    }
    const Phase phase = s.phase;
    if (phase == Phase::Error || phase == Phase::Stopped || phase == Phase::Idle)
      return Wait::Ended;
    // A stop (or a pause, which a session refuses) needs the CPU thread back.
    if (cpu.GetState() != CPU::State::Running)
      return Wait::Yield;
    if (phase == Phase::Connecting)
    {
      if (PeersConnected())
      {
        if (!CreateGekko())
        {
          Fail("GekkoNet could not start");
          return Wait::Ended;
        }
        s.phase = Phase::Starting;
        AddLog("every peer connected; GekkoNet handshake");
      }
    }
    if (s.phase == Phase::Joining)
      JoinStep(system);
    if (s.phase == Phase::Epoch)
      EpochStep(system);
    if (s.phase == Phase::Error)
      return Wait::Ended;
    if (s.phase == Phase::Starting)
    {
      if (!s.gekko_started)
      {
        gekko_network_poll(s.gekko);
        int count = 0;
        gekko_update_session(s.gekko, &count);
        ProcessSessionEvents();
      }
      else if (StartBarrier())
      {
        s.phase = Phase::Running;
        // Rush Frame Presentation's pacing, as P+ online (GameplaySession's StartRunning): each
        // frame is shown at its copy's due time, not whenever the throttle let the CPU reach the
        // copy (bug #30: a REX room's frames came 16 ms apart with a 21 ms gap every seventh).
        system.GetCoreTiming().SetPresentPacing(true);
        AddLog("running");
        OSD::AddMessage("Session started", 3000, OSD::Color::GREEN);
      }
      else
      {
        gekko_network_poll(s.gekko);
      }
    }
    if (s.phase == Phase::Running)
    {
      DropLeftPeers();
      UpdateLocalRequest();
      if (s.late_input)
        LateEndOfFrame();
      CaptureLocalInput();
      Update(system);
      if (s.phase == Phase::Error)
        return Wait::Ended;
      UpdatePingDisplay();
      if (s.ops.count > 0)
        break;
      // At the epoch's frame: every frame before it ran with confirmed inputs (GekkoNet holds
      // here, and ran its last rollback in an earlier update).
      if (s.plan.active && s.current_frame == s.plan.frame - 1)
      {
        const int confirmed = gekko_confirmed_frame(s.gekko);
        if (confirmed >= 0 && s.frame_base + confirmed >= s.plan.frame - 1)
          EnterEpoch();
      }
    }
    RedrawWhileWaiting(system, Clock::now() - since);
    waited = true;
    // The lock is let go while sleeping, so status and control requests get through. OnLoopTop
    // holds it exactly once.
    s.mutex.unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(250));
    s.mutex.lock();
  }
  if (waited)
  {
    PaceWait(Clock::now() - since);
    system.GetCoreTiming().ResetThrottleToNow();
  }
  return Wait::Frames;
}

// Sync test diagnostics (PPR_MACHINE_STATE_CHECK=N): the machine's state as an image carries it
// at the start of every Nth frame's first pass, compared with the state a re-run of that frame
// starts from. What differs is guest state a rollback did not restore.
struct PassState
{
  s64 frame = -1;
  std::vector<u8> state;
  std::vector<u8> nand;
};
std::array<PassState, 4> s_pass_states;

// The last marker-like text (DoMarker names, device names) before `pos`.
std::string MarkerBefore(const std::vector<u8>& st, size_t pos)
{
  size_t run = 0;
  for (size_t i = pos; i-- > 0 && pos - i < (1u << 20);)
  {
    const u8 c = st[i];
    if (c >= 0x20 && c < 0x7f)
    {
      if (++run >= 6)
      {
        size_t end = i + run;
        while (end < st.size() && st[end] >= 0x20 && st[end] < 0x7f)
          ++end;
        return fmt::format("\"{}\" {} before", std::string(st.begin() + i, st.begin() + end),
                           pos - i);
      }
    }
    else
    {
      run = 0;
    }
  }
  return "?";
}

void CheckPassState(Core::System& system)
{
  static const int every = [] {
    const char* e = std::getenv("PPR_MACHINE_STATE_CHECK");
    return e ? std::atoi(e) : 0;
  }();
  if (every <= 0 || s.opts.synctest <= 0 || s.current_frame <= 0 || s.current_frame % every)
    return;
  Rollback::RollbackManager::SyncGPUForDoState(system);
  std::vector<u8> st = SaveImageStateForTest(system);
  std::vector<u8> nand = PackNand(system);
  PassState& ps = s_pass_states[static_cast<size_t>(s.current_frame / every) % s_pass_states.size()];
  if (ps.frame != s.current_frame)
  {
    ps.frame = s.current_frame;
    ps.state = std::move(st);
    ps.nand = std::move(nand);
    return;
  }
  if (nand != ps.nand)
  {
    // The NAND's files (the journal undoes a rolled-back frame's writes): where they differ.
    size_t i = 0;
    while (i < nand.size() && i < ps.nand.size() && nand[i] == ps.nand[i])
      ++i;
    std::string line = fmt::format("nand at frame {} start differs ({} vs {} bytes) at {} ({})",
                                   s.current_frame, ps.nand.size(), nand.size(), i,
                                   MarkerBefore(i < nand.size() ? nand : ps.nand,
                                                std::min(i, std::max(nand.size(), ps.nand.size()) - 1)));
    AddLog(line);
    if (s_diag.size() < 200)
      s_diag.push_back(std::move(line));
  }
  if (st == ps.state)
    return;
  // PPR_MACHINE_DUMP_DIR: both states of the first frame that differs, to read offline.
  static bool dumped = false;
  if (const char* dir = std::getenv("PPR_MACHINE_DUMP_DIR"); dir && *dir && !dumped)
  {
    dumped = true;
    for (const auto& [name, bytes] : {std::pair{"first", &ps.state}, std::pair{"rerun", &st}})
    {
      File::WriteStringToFile(fmt::format("{}/passstate-{}-{}.bin", dir, s.current_frame, name),
                              std::string_view(reinterpret_cast<const char*>(bytes->data()),
                                               bytes->size()));
    }
  }
  std::string line = fmt::format("state at frame {} start ({} vs {} bytes):", s.current_frame,
                                 ps.state.size(), st.size());
  const size_t n = std::min(st.size(), ps.state.size());
  int ranges = 0;
  for (size_t i = 0; i < n && ranges < 10;)
  {
    if (st[i] == ps.state[i])
    {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < n && (st[j] != ps.state[j] || (j + 16 < n && std::memcmp(&st[j], &ps.state[j], 16))))
      ++j;
    std::string a, b;
    for (size_t k = i; k < std::min(j, i + 16); ++k)
    {
      a += fmt::format("{:02x}", ps.state[k]);
      b += fmt::format("{:02x}", st[k]);
    }
    std::string ctx;
    for (size_t k = i >= 48 ? i - 48 : 0; k < i; ++k)
      ctx += fmt::format("{:02x}", st[k]);
    line += fmt::format(" [{} +{}: {} -> {}; {}; before: {}]", i, j - i, a, b, MarkerBefore(st, i), ctx);
    ++ranges;
    i = j;
  }
  AddLog(line.substr(0, 600));
  if (s_diag.size() < 200)
    s_diag.push_back(std::move(line));
}

// The first pass after a rollback's load starts from the state the ring kept for the end of the
// frame before it: the emulated time that frame's last run started at. Another time means the
// load restored some other frame's state while the passes kept their frame numbers.
void CheckLoadedTime(Core::System& system)
{
  if (!s.loaded)
    return;
  const auto [back, ring] = *s.loaded;
  s.loaded.reset();
  const HistoryEntry& e = s.history[static_cast<size_t>(s.current_frame) % HISTORY_SIZE];
  const u64 now = system.GetCoreTiming().GetTicks();
  if (e.frame != s.current_frame || e.ticks == 0 || e.ticks == now)
    return;
  if (s.load_mismatches++ < 20)
  {
    WARN_LOG_FMT(BRAWLBACK,
                 "machine: rollback {} frame(s) back (ring {}): frame {} starts at {} ticks, its last "
                 "run at {} ({:+})",
                 back, ring, s.current_frame, now, e.ticks, static_cast<s64>(now - e.ticks));
  }
}

// Diagnostics (PPR_MACHINE_SCHEDULE_LOG=N): from an epoch (or a joiner's image load) for N frames,
// every CoreTiming event scheduled and each frame's event order, to line the players up.
void StartScheduleLog(Core::System& system, s64 frame, std::string_view why)
{
  static const s64 frames = [] {
    const char* e = std::getenv("PPR_MACHINE_SCHEDULE_LOG");
    return e ? std::strtoll(e, nullptr, 10) : 0;
  }();
  if (frames <= 0)
    return;
  auto& ct = system.GetCoreTiming();
  s_schedule_log_until = frame + frames;
  ct.m_log_schedules = true;
  INFO_LOG_FMT(BRAWLBACK, "machine: schedule log: {} at frame {}, event order {}", why, frame,
               ct.GetEventFifoId());
}

std::FILE* LatLog()
{
  static std::FILE* const f = [] {
    const char* env = std::getenv("PPR_MACHINE_INPUT_LAT_LOG");
    if (!env || !*env)
      return static_cast<std::FILE*>(nullptr);
    std::string path = env;
    if (const size_t at = path.find("%p"); at != std::string::npos)
      path.replace(at, 2, std::to_string(Clock::now().time_since_epoch().count()));
    return std::fopen(path.c_str(), "w");
  }();
  return f;
}

double LatMs(Clock::time_point a, Clock::time_point b)
{
  if (a == Clock::time_point{} || b == Clock::time_point{})
    return -1;
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// A frame's first run: the line of the frame before it, and this one's start.
}  // namespace
Boundary SessionBoundary();
GCPadStatus DecodePad(const Input& in);
namespace
{

void LatOnForwardPass(s64 frame)
{
  std::FILE* f = LatLog();
  if (!f)
    return;
  const auto now = Clock::now();
  const auto& prev = s.lat[static_cast<size_t>(frame - 1 + s.lat.size()) % s.lat.size()];
  if (frame > 0 && prev.frame == frame - 1)
  {
    // F frame | begin->first SI read ms | SI reads | begin->first Wii Remote update ms | updates |
    // local read->first SI read ms | local read->begin ms | frame length ms
    const bool has_read = prev.read_frame == prev.frame;
    std::fprintf(f, "F %lld %.3f %d %.3f %d %.3f %.3f %.3f\n", static_cast<long long>(prev.frame),
                 LatMs(prev.begin, prev.first_si), prev.si_n, LatMs(prev.begin, prev.first_bt),
                 prev.bt_n, has_read ? LatMs(prev.read, prev.first_si) : -1.0,
                 has_read ? LatMs(prev.read, prev.begin) : -1.0, LatMs(prev.begin, now));
    std::fflush(f);
  }
  static const bool reads = std::getenv("PPR_MACHINE_INPUT_LAT_READS") != nullptr;
  if (reads && SessionBoundary() == Boundary::BrawlLoop)
  {
    // P frame | Brawl's gfPadStatus slots' buttons at this loop top (0x805BAD00, 0x40 apart) |
    // the buttons of the inputs this frame runs with
    auto& memory = Core::System::GetInstance().GetMemory();
    std::fprintf(f, "P %lld", static_cast<long long>(frame));
    for (u32 p = 0; p < 4; ++p)
      std::fprintf(f, " %04x", memory.Read_U16(0x805BAD00 + p * 0x40));
    for (int p = 0; p < 4; ++p)
      std::fprintf(f, " %04x", DecodePad(s.latched[p]).button);
    std::fprintf(f, "\n");
  }
  auto& l = s.lat[static_cast<size_t>(frame) % s.lat.size()];
  const s64 read_frame = l.read_frame;
  const auto read = l.read;
  l = {};
  l.frame = frame;
  l.begin = now;
  l.read_frame = read_frame;
  l.read = read;
}

void LatOnRead(bool si, int port)
{
  if (s.phase != Phase::Running || !s.pass_open || s.resim || !LatLog())
    return;
  auto& l = s.lat[static_cast<size_t>(s.current_frame) % s.lat.size()];
  if (l.frame != s.current_frame)
    return;
  auto& first = si ? l.first_si : l.first_bt;
  if (first == Clock::time_point{})
    first = Clock::now();
  ++(si ? l.si_n : l.bt_n);
  static const bool reads = std::getenv("PPR_MACHINE_INPUT_LAT_READS") != nullptr;
  if (reads)
  {
    // R frame kind(S/B) port | real ms since the frame's start | emulated ms since its start
    auto& system = Core::System::GetInstance();
    const auto& h = s.history[static_cast<size_t>(s.current_frame) % HISTORY_SIZE];
    const double emu_ms = static_cast<double>(system.GetCoreTiming().GetTicks() - h.ticks) *
                          1000.0 / static_cast<double>(system.GetSystemTimers().GetTicksPerSecond());
    std::fprintf(LatLog(), "R %lld %c %d %.3f %.3f\n", static_cast<long long>(s.current_frame),
                 si ? 'S' : 'B', port, LatMs(l.begin, Clock::now()), emu_ms);
  }
}

void BeginPass(Core::System& system)
{
  // The inputs this pass runs with, for the SI (GetPad) and the Wii Remotes (GetWiimote).
  s.latched = s.ops.inputs[s.iteration];
  s.current_frame = s.ops.frame[s.iteration];
  s.pass_open = true;
  s.resim = s.ops.count > 1 && s.iteration != s.ops.count - 1;
  if (s_schedule_log_until >= 0) [[unlikely]]
  {
    auto& ct = system.GetCoreTiming();
    INFO_LOG_FMT(BRAWLBACK, "machine: schedule log: frame {}{} begins, event order {}",
                 s.current_frame, s.resim ? " (re-run)" : "", ct.GetEventFifoId());
    if (s.current_frame > s_schedule_log_until)
    {
      ct.m_log_schedules = false;
      s_schedule_log_until = -1;
    }
  }
  if (!s.resim && s.burst_frames > 0)
  {
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - s.burst_start).count() /
        s.burst_frames;
    s.burst_ms_per_frame += (ms - s.burst_ms_per_frame) * BURST_AVERAGE_WEIGHT;
    s.burst_frames = 0;
  }
  if (!s.resim)
  {
    LatOnForwardPass(s.current_frame);
    s.consumed = {};
    s.read_outside_pass = false;
  }
  SyncWiimotes(system);
  PlanEpoch(s.current_frame);
  CheckLoadedTime(system);
  RecordHistory(s.current_frame, s.latched, 0);
  s.history[static_cast<size_t>(s.current_frame) % HISTORY_SIZE].ticks = system.GetCoreTiming().GetTicks();
  auto& core_timing = system.GetCoreTiming();
  core_timing.SetRollbackResimulating(s.resim);
  UpdateTimeSync(system);
  core_timing.SetRollbackSpeedAdjustment(s.speed);
  PaceFrame(system, s.resim);
  if (!s.resim)
    Rollback::PresentStats::OnDisplayedFrameStart(s.ops.count > 1);
  CheckPassState(system);
#if defined(_M_X86_64)
  {
    // The host's float mode (MXCSR: rounding, flush to zero) is the guest's FPSCR's on the CPU
    // thread. Host code that ran on this thread since may have changed it (a pass that presented
    // a frame runs more of it than a re-run): set it again, and say when it was off.
    const u32 before = _mm_getcsr() & ~0x3Fu;
    PowerPC::RoundingModeUpdated(system.GetPPCState());
    const u32 after = _mm_getcsr() & ~0x3Fu;
    static int reported = 0;
    if (before != after && reported < 20)
    {
      ++reported;
      AddLog(fmt::format("host float mode was {:08x}, the guest's is {:08x} (frame {}, {})", before,
                         after, s.current_frame, s.resim ? "re-run" : "first run"));
    }
  }
#endif
  // Diagnostics: PPR_MACHINE_JIT_CLEAR_EACH_PASS compiles every pass from an empty JIT cache.
  static const bool clear_each_pass = std::getenv("PPR_MACHINE_JIT_CLEAR_EACH_PASS") != nullptr;
  if (clear_each_pass)
    system.GetJitInterface().ClearSafe();
}

bool Active()
{
  const Phase p = s.phase;
  return p == Phase::Armed || p == Phase::Connecting || p == Phase::Starting ||
         p == Phase::Running || p == Phase::Epoch || p == Phase::Joining;
}

void CheckEnvironment()
{
  if (s.env_checked)
    return;
  s.env_checked = true;
  const char* env = std::getenv("PPR_MACHINE_SESSION");
  if (!env || !*env)
  {
    // A Brawl Mods room (Online/ModsRoom.h): its session.
    std::string why;
    if (const auto opts = Online::ModsRoom::FromEnvironment(&why))
    {
      if (!Arm(*opts, &why))
        ERROR_LOG_FMT(BRAWLBACK, "machine: could not arm the room's session: {}", why);
    }
    else if (!why.empty())
    {
      ERROR_LOG_FMT(BRAWLBACK, "machine: {}", why);
    }
    return;
  }
  picojson::value v;
  const std::string err = picojson::parse(v, env);
  std::string why;
  if (!err.empty())
  {
    ERROR_LOG_FMT(BRAWLBACK, "machine: PPR_MACHINE_SESSION is not JSON: {}", err);
    return;
  }
  const auto opts = ParseOptions(v, &why);
  if (!opts)
  {
    ERROR_LOG_FMT(BRAWLBACK, "machine: PPR_MACHINE_SESSION: {}", why);
    return;
  }
  // Arm takes the lock again (recursive).
  if (!Arm(*opts, &why))
    ERROR_LOG_FMT(BRAWLBACK, "machine: could not arm: {}", why);
}

template <typename T>
T Pinned(const Config::Info<T>& info)
{
  // A setting that changes what the game computes: Dolphin's default, or the game's own value
  // from Dolphin's game settings (Sys/GameSettings), never the player's own choice, so every
  // machine runs with the same.
  const auto& layer = Config::GetLayer(Config::LayerType::GlobalGame);
  if (layer)
  {
    if (const auto v = layer->template Get<T>(info.GetLocation()))
      return *v;
  }
  return info.GetDefaultValue();
}
}  // namespace

GCPadStatus DecodePad(const Input& in)
{
  GCPadStatus pad{};
  // No GameCube controller in this port this frame: nobody plays it, its player plays a Wii
  // Remote, the session has not started, or the player left. The SI answers as for an empty port.
  if (!(in[0] & INPUT_GC_PRESENT))
  {
    pad.isConnected = false;
    return pad;
  }
  pad.button = static_cast<u16>((in[1] << 8) | in[2]);
  pad.stickX = in[3];
  pad.stickY = in[4];
  pad.substickX = in[5];
  pad.substickY = in[6];
  pad.triggerLeft = in[7];
  pad.triggerRight = in[8];
  pad.isConnected = true;
  return pad;
}

void EncodePad(const GCPadStatus& pad, Input* in)
{
  (*in)[0] |= INPUT_GC_PRESENT;
  // PAD_GET_ORIGIN and the other non-button bits stay local.
  const u16 buttons = pad.button & 0x1F7F;
  (*in)[1] = static_cast<u8>(buttons >> 8);
  (*in)[2] = static_cast<u8>(buttons & 0xFF);
  (*in)[3] = pad.stickX;
  (*in)[4] = pad.stickY;
  (*in)[5] = pad.substickX;
  (*in)[6] = pad.substickY;
  (*in)[7] = pad.triggerLeft;
  (*in)[8] = pad.triggerRight;
}

std::optional<Options> ParseOptions(const picojson::value& json, std::string* error)
{
  if (!json.is<picojson::object>())
  {
    *error = "options must be an object";
    return std::nullopt;
  }
  const auto& o = json.get<picojson::object>();
  auto num = [&](const char* key, double def) {
    const auto it = o.find(key);
    return it != o.end() && it->second.is<double>() ? it->second.get<double>() : def;
  };
  Options opts;
  const double slot = num("slot", 0);
  const double port = num("port", 0);
  const double delay = num("delay", 2);
  const double synctest = num("synctest", 0);
  const double pad = num("pad", 0);
  if (slot < 0 || slot >= MAX_PLAYERS || port < 0 || port > 65535 || delay < 0 || delay > 9 ||
      synctest < 0 || synctest > MAX_ROLLBACK_FRAMES || pad < 0 || pad > 3)
  {
    *error = "slot, port, delay, synctest or pad out of range";
    return std::nullopt;
  }
  opts.local_slot = static_cast<int>(slot);
  opts.local_port = static_cast<u16>(port);
  // "delay": "auto" picks it from the round trip (AUTO_DELAY), as a room on automatic.
  const auto d = o.find("delay");
  const bool auto_delay = d != o.end() && d->second.is<std::string>() &&
                          d->second.get<std::string>() == "auto";
  opts.delay = auto_delay ? AUTO_DELAY : static_cast<int>(delay);
  opts.synctest = static_cast<int>(synctest);
  opts.local_pad = static_cast<int>(pad);
  if (const auto it = o.find("sd_sync"); it != o.end())
  {
    const auto choice =
        it->second.is<std::string>() ? ParseSDSyncChoice(it->second.get<std::string>()) : std::nullopt;
    if (!choice)
    {
      *error = "sd_sync must be ask, yes, overwrite or no";
      return std::nullopt;
    }
    opts.sd_sync = *choice;
  }
  const double rtc = num("rtc", static_cast<double>(opts.rtc));
  if (rtc < 946684800.0 || rtc > 4102444800.0)
  {
    *error = "rtc must be seconds since 1970 between 2000 and 2100";
    return std::nullopt;
  }
  opts.rtc = static_cast<u64>(rtc);
  if (const auto it = o.find("name"); it != o.end() && it->second.is<std::string>())
    opts.name = it->second.get<std::string>().substr(0, 64);
  auto controller_of = [](const picojson::object& obj, Controller* c) {
    const auto it = obj.find("controller");
    if (it == obj.end())
      return true;
    if (!it->second.is<std::string>())
      return false;
    const std::string& v = it->second.get<std::string>();
    if (v == "gc")
      *c = Controller::GameCube;
    else if (v == "wiimote")
      *c = Controller::WiiRemote;
    else
      return false;
    return true;
  };
  if (!controller_of(o, &opts.controller))
  {
    *error = "controller must be \"gc\" or \"wiimote\"";
    return std::nullopt;
  }
  if (const auto it = o.find("peers"); it != o.end() && it->second.is<picojson::array>())
  {
    for (const auto& pv : it->second.get<picojson::array>())
    {
      if (!pv.is<picojson::object>())
        continue;
      const auto& po = pv.get<picojson::object>();
      Peer p;
      const auto sl = po.find("slot");
      const auto h = po.find("host");
      const auto pt = po.find("port");
      if (sl == po.end() || !sl->second.is<double>() || h == po.end() ||
          !h->second.is<std::string>() || pt == po.end() || !pt->second.is<double>())
      {
        *error = "a peer needs slot, host and port";
        return std::nullopt;
      }
      const double ps = sl->second.get<double>();
      const double pp = pt->second.get<double>();
      if (ps < 0 || ps >= MAX_PLAYERS || ps == slot || pp <= 0 || pp > 65535)
      {
        *error = "a peer's slot or port is out of range";
        return std::nullopt;
      }
      p.slot = static_cast<int>(ps);
      if (!controller_of(po, &p.controller))
      {
        *error = "a peer's controller must be \"gc\" or \"wiimote\"";
        return std::nullopt;
      }
      p.host = h->second.get<std::string>();
      p.port = static_cast<u16>(pp);
      if (const auto l = po.find("late"); l != po.end() && l->second.is<bool>())
        p.late = l->second.get<bool>();
      opts.peers.push_back(std::move(p));
    }
  }
  if (const auto b = o.find("boundary"); b != o.end())
  {
    const std::string v = b->second.is<std::string>() ? b->second.get<std::string>() : "";
    if (v == "auto")
      opts.boundary = Boundary::Auto;
    else if (v == "brawl")
      opts.boundary = Boundary::BrawlLoop;
    else if (v == "vi")
      opts.boundary = Boundary::ViField;
    else
    {
      *error = "boundary must be \"auto\", \"brawl\" or \"vi\"";
      return std::nullopt;
    }
  }
  if (const auto j = o.find("join"); j != o.end() && j->second.is<bool>())
    opts.join = j->second.get<bool>();
  if (const auto r = o.find("room"); r != o.end() && r->second.is<bool>())
    opts.room = r->second.get<bool>();
  if (opts.join && opts.peers.empty() && !opts.room)
  {
    *error = "a joiner needs the session's players as peers";
    return std::nullopt;
  }
  if (opts.synctest > 0 && !opts.peers.empty())
  {
    *error = "a sync test has no peers";
    return std::nullopt;
  }
  return opts;
}

bool Arm(const Options& options, std::string* error)
{
  std::lock_guard lk(s.mutex);
  s.env_checked = true;
  if (Active())
  {
    *error = "a session is already armed";
    return false;
  }
  StopNetwork();
  DestroyGekko();
  ResetSD();
  s.opts = options;
  s.error.clear();
  {
    std::lock_guard llk(s.log_mutex);
    s.log.clear();
  }
  s.peers = {};
  // A joiner plays from the epoch that admits it; its peers' set comes with that epoch.
  s.players = options.join ? 0u : 1u << options.local_slot;
  s.gone = 0;
  s.controller = {};
  s.controller[options.local_slot] = options.controller;
  s.wiimote_captures = 0;
  s.requested_controller = -1;
  s.pick_countdown = 0;
  s.use_real_wiimote = false;
  {
    std::lock_guard plk(s.pick_mutex);
    s.picked = false;
    s.pick = {};
    s.pick.controller = options.controller;
    s.pick.index = options.local_pad;
  }
  s.local_adapter = Config::Get(Config::GetInfoForSIDevice(options.local_pad)) ==
                    SerialInterface::SIDEVICE_WIIU_ADAPTER;
  for (const Peer& p : options.peers)
  {
    PeerLink& link = s.peers[p.slot];
    link.slot = p.slot;
    link.host = p.host;
    link.port = p.port;
    link.late = p.late;
    link.ips = ResolveIps({&p.host});
    if (!p.late && !options.join)
      s.players |= 1u << p.slot;
    s.controller[p.slot] = p.controller;
  }
  s.ops.Clear();
  s.iteration = 0;
  s.resim = false;
  s.boundary = options.boundary;
  s.pass_open = false;
  s.vi_frame_ended = false;
  g_vi_boundary = false;
  s.queued_save = {};
  s.latched = {};
  s.local_input = {};
  s.boot_local = false;
  s.current_frame = -1;
  s.frame_base = 0;
  s.epoch = 0;
  s.plan = {};
  s.local_request.reset();
  s.local_move.reset();
  s.move_done.reset();
  s.image_built = false;
  s.image_ms = 0;
  s.image_packed = s.image_raw = 0;
  s.join_sent = false;
  s.join_loaded = false;
  s.join_tenths_shown = -1;
  {
    std::lock_guard nlk(s.net_mutex);
    s.epoch_go.reset();
    s.join_refused = false;
    s.room_known = false;
    s.room_network = {};
    s.probe = {};
    s.image_rx = {};
    s.image_tx.clear();
    s.image_to = -1;
  }
  s.speed = 1.0;
  s.last_timesync_frame = -1;
  s.barrier_done = false;
  s.start_at.reset();
  s.rollbacks = s.max_rollback = s.frames_resimulated = s.stall_polls = s.skipped_loads = 0;
  s.loaded.reset();
  s.burst_frames = 0;
  s.burst_ms_per_frame = BURST_MS_PER_FRAME_START;
  s.load_mismatches = 0;
  s.desyncs = s.saves = 0;
  s.last_desync_frame = -1;
  s.desync_notice_at = {};
  s.input_log = {};
  s.pick_frame = -1;
  for (auto& e : s.history)
    e.frame = -1;
  if (!StartNetwork())
  {
    *error = s.error;
    return false;
  }
  // Copies kept in VRAM leave guest RAM alone from the boot on (PresentStats.h).
  Rollback::PresentStats::SetCopiesLeaveGuestRAM(true);
  s.phase = Phase::Armed;
  AddLog(fmt::format(
      "armed: slot {}, {} peer(s), port {}, delay {}{}", options.local_slot, options.peers.size(),
      options.local_port,
      options.delay < 0 ?
          fmt::format("auto, {}", Gprb::Session::DelayLevelName(AutoDelayLevel(options.delay))) :
          std::to_string(options.delay),
      options.synctest ? fmt::format(", sync test {}", options.synctest) : ""));
  return true;
}

void Stop()
{
  if (s.phase == Phase::Idle)
    return;
  // First, without the lock: a CPU thread waiting at a boundary (WaitForFrames, which holds the
  // lock) sees it and lets go.
  if (Active())
    s.phase = Phase::Stopped;
#ifdef _WIN32
  // The UI thread stops the session (the room's stop callback, then Core::Stop) right after
  // MainWindow::RequestStop asked to leave exclusive fullscreen. The CPU thread can hold the lock
  // at a frame boundary while a rollback's load waits for the video thread (SyncGPU), and the
  // video thread be in DXGI's SetFullscreenState waiting for this thread to answer a message sent
  // to the render window: take the lock in tries and answer those messages between them, as
  // CPUManager::Stop waits (bug report #86).
  std::unique_lock lk(s.mutex, std::defer_lock);
  while (!lk.try_lock())
  {
    Common::AnswerSentMessages();
    Common::SleepCurrentThread(1);
  }
#else
  std::lock_guard lk(s.mutex);
#endif
  {
    std::lock_guard nlk(s.net_mutex);
    picojson::object msg;
    msg["type"] = picojson::value("leave");
    if (s.host)
    {
      BroadcastControl(msg);
      enet_host_flush(s.host);
    }
  }
  StopNetwork();
  DestroyGekko();
  ResetSD();
  Rollback::RollbackManager::Get().m_full_ios_in_snapshots = false;
  Rollback::RollbackManager::Get().SetChecksumMasks({});
  Rollback::PresentStats::SetCopiesLeaveGuestRAM(false);
  Core::System::GetInstance().GetCoreTiming().SetPresentPacing(false);
  if (auto* fs = HostFS(Core::System::GetInstance()))
    fs->JournalStop();
  s.journal_marks.clear();
  s.phase = Phase::Stopped;
  AddLog("stopped");
  OSD::AddMessage("Session ended", 3000, OSD::Color::YELLOW);
}

void RecheckEnvironment()
{
  std::lock_guard lk(s.mutex);
  if (!Active())
    s.env_checked = false;
}

bool JoinsRunningSession()
{
  return IsArmed() && s.opts.join;
}

bool IsArmed()
{
  if (!s.env_checked)
  {
    std::lock_guard lk(s.mutex);
    CheckEnvironment();
  }
  return Active();
}

bool InImageDoState()
{
  return s_image_dostate;
}

std::vector<u8> SaveImageStateForTest(Core::System& system)
{
  Common::UniqueBuffer<u8> state(192u << 20);
  s_image_dostate = true;
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  const size_t n = ::State::SaveToBuffer(system, state);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  s_image_dostate = false;
  return std::vector<u8>(state.data(), state.data() + n);
}

bool LoadImageStateForTest(Core::System& system, std::vector<u8> state)
{
  s_image_dostate = true;
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  const bool ok = ::State::LoadFromBuffer(system, std::span<u8>(state.data(), state.size()));
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  s_image_dostate = false;
  // An image load leaves the NAND files closed (a joiner opens them after UnpackNand).
  if (auto* fs = HostFS(system))
    fs->ReopenHostFiles();
  return ok;
}

// Net mutex held: this side stops using a peer's link (a joiner gone from the room).
static void ForgetLink(PeerLink& p)
{
  if (p.peer)
    enet_peer_disconnect(p.peer, 0);
  if (p.connecting && p.connecting != p.peer)
  {
    p.connecting->data = nullptr;
    enet_peer_reset(p.connecting);
  }
  p = PeerLink{};
}

void SetRoom(int local_slot, const std::vector<RoomMember>& members)
{
  if (local_slot < 0 || local_slot >= MAX_PLAYERS)
    return;
  std::lock_guard lk(s.net_mutex);
  if (!Active() || !s.opts.room || !s.host)
    return;
  if (local_slot != s.opts.local_slot)
  {
    // A joiner's slot is the room's. Nothing went out with the one it was armed with: it had no
    // peers before the room's first state.
    if (s.room_known || !s.opts.join || LocalPlays())
    {
      if (!s.room_known)
        AddLog(fmt::format("the room has this player in slot {}, the session in slot {}",
                           local_slot + 1, s.opts.local_slot + 1));
      return;
    }
    s.controller[s.opts.local_slot] = Controller::GameCube;
    s.controller[local_slot] = s.opts.controller;
    s.opts.local_slot = local_slot;
  }
  if (!s.room_known)
  {
    AddLog(fmt::format("room: this player is in slot {}", local_slot + 1));
    // The adapter toward the internet: the one a UDP "connect" to a public address picks.
    ENetAddress internet{};
    enet_address_set_host_ip(&internet, "1.1.1.1");
    internet.port = 53;
    s.room_network = Config::GetChosenNetwork(LanHost(internet));
    if (s.room_network.mask == 0 && Config::GetChosenNetwork().mask != 0)
    {
      AddLog(fmt::format("the network chosen in the launcher ({}) is the one toward the internet: "
                         "players are reached as with Automatic",
                         Config::GetChosenNetwork().address));
    }
  }
  const bool first_view = !s.room_known;
  s.room_known = true;
  std::array<const RoomMember*, MAX_PLAYERS> by_slot{};
  for (const RoomMember& m : members)
  {
    if (m.slot >= 0 && m.slot < MAX_PLAYERS)
      by_slot[m.slot] = &m;
  }
  const RoomMember* self = by_slot[local_slot];
  // A slot move in flight: the session swapped two players and the room has not yet (or the
  // reverse). Those slots stay as they are until the room and the session agree again (the
  // player is not gone, nor someone new).
  std::array<bool, MAX_PLAYERS> settling{};
  for (const RoomMember& m : members)
  {
    for (int slot = 0; slot < MAX_PLAYERS; ++slot)
    {
      const bool here = slot == local_slot ? (self && self->code == m.code) :
                                             (s.peers[slot].slot >= 0 && s.peers[slot].code == m.code);
      if (here && slot != m.slot && m.slot >= 0 && m.slot < MAX_PLAYERS)
      {
        settling[slot] = true;
        settling[m.slot] = true;
      }
    }
  }
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    if (slot == local_slot || settling[slot])
      continue;
    PeerLink& p = s.peers[slot];
    const RoomMember* m = by_slot[slot];
    if (!m)
    {
      // Gone from the room. A player's link stays until it drops (a player whose connection to
      // mm dropped still plays); a joiner's is forgotten.
      if (p.slot >= 0 && !Plays(slot))
      {
        AddLog(fmt::format("room: player {} ({}) is gone", slot + 1, p.code));
        ForgetLink(p);
      }
      continue;
    }
    const bool fresh = p.slot < 0 || p.code != m->code;
    if (fresh)
    {
      if (p.slot >= 0)
      {
        // Someone else has this slot now: whoever was in it has left the session.
        const bool was_playing = Plays(slot);
        AddLog(fmt::format("room: {} took slot {} from {}", m->code, slot + 1, p.code));
        if (p.peer)
          enet_peer_disconnect(p.peer, 0);
        if (p.connecting && p.connecting != p.peer)
        {
          p.connecting->data = nullptr;
          enet_peer_reset(p.connecting);
        }
        p.peer = nullptr;
        p.connecting = nullptr;
        p.hello = false;
        p.join_asked = false;
        p.epoch_at = -1;
        p.epoch_digest.clear();
        if (was_playing && !p.dropped)
        {
          p.left = true;
          p.reseated = true;
        }
      }
      else
      {
        p = PeerLink{};
        AddLog(fmt::format("room: {} is in slot {}", m->code, slot + 1));
      }
      p.slot = slot;
      p.code = m->code;
      // A session's player takes a new member for a joiner; a joiner waits for its hello to know.
      p.late = !(s.opts.join && !LocalPlays());
      p.last_connect = {};
      p.link_wanted = Clock::now();
      p.attempts = 0;
      p.unkept_hellos = 0;
      p.no_link_logs = 0;
      p.no_link_watch = first_view && s.opts.join;
      p.no_link_shown = false;
    }
    p.name = m->name;
    // Its address: on this player's network (the same public address) or on the network chosen
    // in the launcher (a VPN such as ZeroTier, room_network), its LAN one.
    const bool same_public =
        self && !self->host.empty() && self->host == m->host && !m->lan_host.empty();
    const bool on_chosen = s.room_network.Contains(m->lan_host);
    const bool same_net = same_public || on_chosen;
    const std::string& host = same_net ? m->lan_host : m->host;
    const u16 port = same_net ? m->lan_port : m->port;
    if (host != p.host || port != p.port)
    {
      AddLog(fmt::format("room: player {} ({}) is reached at {}:{} ({})", slot + 1, m->code, host,
                         port,
                         same_public ? "LAN: the same internet address as ours" :
                         on_chosen   ? "LAN: on the network chosen in the launcher" :
                                       "internet"));
      p.host = host;
      p.port = port;
      p.last_connect = {};
    }
    p.ips = ResolveIps({&m->host, &m->lan_host});
  }
}

void ProbeEndpoint(const std::string& mm_host, u16 mm_port, const std::string& token)
{
  std::lock_guard lk(s.net_mutex);
  if (!s.host || token.empty())
    return;
  if (s.probe.token == token && (s.probe.state == "probing" || s.probe.state == "done"))
    return;
  if (s.probe.peer)
  {
    s.probe.peer->data = nullptr;
    enet_peer_reset(s.probe.peer);
  }
  s.probe = {};
  s.probe.host = mm_host;
  s.probe.port = mm_port;
  s.probe.token = token;
  s.probe.state = "probing";
}

std::string ProbeState()
{
  std::lock_guard lk(s.net_mutex);
  return s.probe.state;
}

bool IsRunning()
{
  return s.phase == Phase::Running;
}

void SetSDSyncChoice(SDSyncChoice choice)
{
  s.sd_sync.choice = static_cast<int>(choice);
}

void SetInputDelay(int delay)
{
  s.requested_delay =
      delay < 0 ? std::clamp(delay, AUTO_DELAY_SMOOTH, AUTO_DELAY) : std::clamp(delay, 0, 9);
}

int CurrentInputDelay()
{
  return IsRunning() && s.gekko ? s.input_delay : -1;
}

std::optional<SDSyncChoice> ParseSDSyncChoice(std::string_view text)
{
  if (text == "ask")
    return SDSyncChoice::Ask;
  if (text == "yes")
    return SDSyncChoice::Yes;
  if (text == "overwrite")
    return SDSyncChoice::Overwrite;
  if (text == "no")
    return SDSyncChoice::No;
  return std::nullopt;
}

u64 GetInitialRTC()
{
  return s.opts.rtc;
}

bool KeepsSessionClock()
{
  return (s.phase == Phase::Stopped || s.phase == Phase::Error) && Core::WantsDeterminism();
}

std::shared_ptr<SDCard::SessionCard> CreateSDCard(const std::string& image_path)
{
  using SDCard::SessionCard;
  ResetSD();
  if (!Config::Get(Config::MAIN_WII_SD_CARD))
    return SessionCard::CreateOwnCard(image_path);
  if (JoinsRunningSession())
  {
    // Its own card until the machine image comes, then the session's.
    s.sd_path = image_path;
    return SessionCard::CreateOwnCard(image_path);
  }
  const auto start = Clock::now();
  std::optional<SDCard::Manifest> manifest = SDCard::BuildManifest(image_path);
  if (!manifest)
  {
    AddLog(fmt::format("SD card: could not read {}", image_path));
    return SessionCard::CreateOwnCard(image_path);
  }
  const u64 in_use = static_cast<u64>(std::ranges::count_if(
                         manifest->chunks, [](const auto& c) { return !c.zero; })) *
                     SDCard::CHUNK_SIZE;
  AddLog(fmt::format("SD card: the session's card is this player's ({:.1f} GB, {:.1f} MB in use; "
                     "read in {:.1f} s)",
                     manifest->size / 1073741824.0, in_use / 1048576.0,
                     std::chrono::duration<double>(Clock::now() - start).count()));
  // Its files, for a joiner whose card differs (which files it gets).
  std::vector<u8> files_wire;
  {
    File::IOFile image(image_path, "rb");
    const std::optional<std::vector<SDCard::CardFile>> files =
        image ? SDCard::ListCardFiles(image, manifest->size) : std::nullopt;
    if (files)
    {
      sf::Packet packet;
      SDCard::WriteFileList(*files, packet);
      const u8* data = static_cast<const u8*>(packet.getData());
      files_wire.assign(data, data + packet.getDataSize());
    }
  }
  auto shared = std::make_shared<const SDCard::Manifest>(std::move(*manifest));
  std::shared_ptr<SessionCard> card = SessionCard::CreateHostCard(image_path, shared);
  std::unique_ptr<SDCard::CardServer> old;
  {
    std::lock_guard lk(s.net_mutex);
    old = SetSessionCardLocked(card);
    s.sd_files_wire = std::move(files_wire);
  }
  return card;
}

// Only with a Real one set up: with the setting on, Dolphin opens every Wii Remote-like Bluetooth
// device it finds, a Wii U Pro Controller driven by WiinUSoft too, which then lost it for as long as
// Dolphin ran (bug #77, 2026-10-21). The player's own setting (the base layer) is kept either way.
void ConnectRealWiimotesIfSetUp()
{
  if (Config::Get(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE))
    return;
  for (int i = 0; i < MAX_WIIMOTES; ++i)
  {
    if (Config::GetBase(Config::GetInfoForWiimoteSource(i)) == WiimoteSource::Real)
    {
      Config::SetCurrent(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE, true);
      WiimoteReal::HandleWiimotesInControllerInterfaceSettingChange();
      return;
    }
  }
}

NetPlay::NetSettings MakeNetSettings()
{
  NetPlay::NetSettings ns{};
  ns.m_RollbackMode = true;
  ns.rollback_delay = s.opts.delay < 0 ? Gprb::Session::DEFAULT_INPUT_DELAY : s.opts.delay;
  // The CPU thread is the player's choice: with the deterministic GPU thread (forced for rollback
  // by the netplay layer) single and dual core compute the same.
  ns.cpu_thread = Config::Get(Config::MAIN_CPU_THREAD);
  ns.cpu_core = Config::Get(Config::MAIN_CPU_CORE);
  ns.enable_cheats = false;
  ns.selected_language = Pinned(Config::MAIN_GC_LANGUAGE);
  ns.override_region_settings = Pinned(Config::MAIN_OVERRIDE_REGION_SETTINGS);
  ns.dsp_hle = Pinned(Config::MAIN_DSP_HLE);
  ns.dsp_enable_jit = Pinned(Config::MAIN_DSP_JIT);
  ns.ram_override_enable = false;
  ns.mem1_size = Pinned(Config::MAIN_MEM1_SIZE);
  ns.mem2_size = Pinned(Config::MAIN_MEM2_SIZE);
  ns.fallback_region = Pinned(Config::MAIN_FALLBACK_REGION);
  // The SD card is read-only in a session (docs/design/brawl-mods.md §4).
  ns.allow_sd_writes = false;
  ns.oc_enable = false;
  ns.oc_factor = 1.0f;
  ns.vi_oc_enable = false;
  ns.vi_oc_factor = 1.0f;
  for (ExpansionInterface::Slot slot : ExpansionInterface::SLOTS)
    ns.exi_device[slot] = ExpansionInterface::EXIDeviceType::None;
  ns.memcard_size_override = -1;
  for (size_t i = 0; i < Config::SYSCONF_SETTINGS.size(); ++i)
  {
    std::visit(
        [&](auto* info) { ns.sysconf_settings[i] = static_cast<u32>(Pinned(*info)); },
        Config::SYSCONF_SETTINGS[i].config_info);
  }
  // Nothing the host GPU computes may reach guest RAM or registers: its results depend on the GPU
  // and driver, and in dual core on when the GPU thread gets to them. Brawl copies a few rendered
  // pixels into RAM for its lighting (docs/determinism-findings.md); in dual core with a real
  // backend those copies landed at host-dependent times and a re-run diverged (frame 1285 of the
  // Minus sync test, never in single core or with the Null backend). EFB and XFB copies stay on
  // the GPU (the copies' RAM keeps what it had: the same everywhere), the CPU reads no EFB, and
  // bounding box and performance counters are off.
  ns.efb_access_enable = false;
  ns.bbox_enable = false;
  ns.force_progressive = Pinned(Config::GFX_HACK_FORCE_PROGRESSIVE);
  ns.efb_to_texture_enable = true;
  ns.xfb_to_texture_enable = true;
  ns.disable_copy_to_vram = Pinned(Config::GFX_HACK_DISABLE_COPY_TO_VRAM);
  ns.immediate_xfb_enable = Config::Get(Config::GFX_HACK_IMMEDIATE_XFB);
  ns.efb_emulate_format_changes = Pinned(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  ns.safe_texture_cache_color_samples = Pinned(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  ns.perf_queries_enable = false;
  ns.float_exceptions = Pinned(Config::MAIN_FLOAT_EXCEPTIONS);
  ns.divide_by_zero_exceptions = Pinned(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS);
  // On, whatever the game's settings: every machine computes FPSCR's FPRF and PowerPC's NaNs, so
  // Jit64, JitArm64 and their interpreter fallbacks (each JIT falls back for other instructions)
  // give the same results; a host's own NaN rules would differ (docs/design/brawl-mods.md §6.6).
  ns.fprf = true;
  ns.accurate_nans = true;
  ns.accurate_fmadds = Pinned(Config::MAIN_ACCURATE_FMADDS);
  ns.disable_icache = Pinned(Config::MAIN_DISABLE_ICACHE);
  ns.sync_on_skip_idle = Pinned(Config::MAIN_SYNC_ON_SKIP_IDLE);
  ns.sync_gpu = Pinned(Config::MAIN_SYNC_GPU);
  ns.sync_gpu_max_distance = Pinned(Config::MAIN_SYNC_GPU_MAX_DISTANCE);
  ns.sync_gpu_min_distance = Pinned(Config::MAIN_SYNC_GPU_MIN_DISTANCE);
  ns.sync_gpu_overclock = Pinned(Config::MAIN_SYNC_GPU_OVERCLOCK);
  ns.jit_follow_branch = Pinned(Config::MAIN_JIT_FOLLOW_BRANCH);
  ns.fast_disc_speed = false;
  ns.mmu = Pinned(Config::MAIN_MMU);
  ns.fastmem = Config::Get(Config::MAIN_FASTMEM);
  ns.skip_ipl = true;
  ns.load_ipl_dump = false;
  ns.defer_efb_copies = Pinned(Config::GFX_HACK_DEFER_EFB_COPIES);
  ns.efb_access_tile_size = Pinned(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE);
  ns.efb_access_defer_invalidation = Pinned(Config::GFX_HACK_EFB_DEFER_INVALIDATION);
  // Fused multiply-adds on every machine (the default was the host CPU's support: an x86 without
  // FMA3 and an arm64 would round differently).
  ns.use_fma = true;
  ns.savedata_load = true;
  // The session runs on a temporary NAND (WiiRoot), thrown away at the end: the game may write
  // its save there (Brawl asks to create one, and asks again if the write fails).
  ns.savedata_write = true;
  ns.savedata_sync_all_wii = false;
  ns.strict_settings_sync = false;
  ns.golf_mode = false;
  ns.hide_remote_gbas = false;
  // Player ids are slot + 1. Every player's port is a standard controller; empty ports have none
  // (BootManager sets the session's SI devices after this layer).
  ns.pad_map.fill(0);
  ns.wiimote_map.fill(0);
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    // Every port is a standard controller and every Wii Remote an emulated one, the same on every
    // machine whoever plays (a joiner's too); whether one is plugged in or connected is the
    // frame's input.
    ns.pad_map[slot] = static_cast<NetPlay::PlayerId>(slot + 1);
    ns.wiimote_map[slot] = static_cast<NetPlay::PlayerId>(slot + 1);
  }
  ns.local_player_id = static_cast<NetPlay::PlayerId>(s.opts.local_slot + 1);
  return ns;
}

bool IsResimulationPass()
{
  return Active() && s.resim;
}

namespace
{
// The bytes of an input that belong to a device (1: the GameCube pad, 2: the Wii Remote).
bool SameOutside(const Input& a, const Input& b, u8 devices)
{
  for (int i = 0; i < INPUT_SIZE; ++i)
  {
    const bool gc = i >= 1 && i <= 8;
    const bool wii = i >= 9 && i <= 40;
    if ((gc && (devices & 1)) || (wii && (devices & 2)))
      continue;
    if (a[i] != b[i])
      return false;
  }
  return true;
}

void LateLogWindow()
{
  if (s.late_window_from < 0)
    s.late_window_from = s.current_frame;
  if (s.current_frame - s.late_window_from < 600)
    return;
  double tx_avg, tx_max, gap_max;
  u64 tx_n;
  {
    std::lock_guard lk(s.net_mutex);
    tx_avg = s.tx_n ? s.tx_wait_sum / static_cast<double>(s.tx_n) : -1;
    tx_max = s.tx_wait_max;
    tx_n = s.tx_n;
    gap_max = s.loop_gap_max;
    s.tx_wait_sum = s.tx_wait_max = 0;
    s.tx_n = 0;
    s.loop_gap_max = 0;
  }
  const auto& w = s.late_window;
  INFO_LOG_FMT(BRAWLBACK,
               "machine: input frames {}-{}: late input {}: early {}, fallback {}, dup {}, skipped "
               "{}; remote late accepted {}, refused {}, missed {}; gekko packets waited avg {:.2f} "
               "ms max {:.2f} ms ({}), net loop gap max {:.2f} ms; unread devices accepted {}; "
               "reads outside a pass {}",
               s.late_window_from, s.current_frame, s.late_input ? "on" : "off", w.early,
               w.fallback, w.dup, w.skipped, w.accepted, w.refused, w.missed, tx_avg, tx_max, tx_n,
               gap_max, w.unread, w.outside);
  s.late_window = {};
  s.late_window_from = s.current_frame;
}

// CPU thread: everything GekkoNet queued since the boundary goes now, in one datagram a peer.
void FlushGekkoNow()
{
  std::lock_guard lk(s.net_mutex);
  if (!s.host)
    return;
  SendQueuedGekko(Clock::now());
  enet_host_flush(s.host);
}

// The local controller, read now (the game's first read of the local player's device in this
// frame's first run), for the frame `input_delay` later, and sent at once.
void LateLocalInput()
{
  const int h = s.local_handle;
  const int target = static_cast<int>(s.current_frame - s.frame_base) + s.input_delay;
  const int last = gekko_local_input_frame(s.gekko, h);
  if (target <= last)
  {
    ++s.late.dup;
    ++s.late_window.dup;
    return;
  }
  if (target != last + 1)
  {
    ++s.late.skipped;
    ++s.late_window.skipped;
    return;
  }
  const auto read_at = Clock::now();
  Input in = ReadLocalInput();
  AddLocalRequest(&in);
  if (!gekko_add_local_input_at(s.gekko, h, target, in.data()))
  {
    ++s.late.skipped;
    ++s.late_window.skipped;
    return;
  }
  gekko_send_local_inputs(s.gekko);
  if (s.direct_send)
    FlushGekkoNow();
  const s64 global = s.frame_base + target;
  auto& l = s.lat[static_cast<size_t>(global) % s.lat.size()];
  l.read_frame = global;
  l.read = read_at;
  ++s.late.early;
  ++s.late_window.early;
}

// A remote player's input for this frame, if it came since the frame began and the game has not
// read the parts of it that differ from the prediction: it replaces the prediction.
void LateRemoteInput(int slot, u8 device)
{
  const int h = s.slot_handle[slot];
  if (h < 0)
    return;
  const int frame = static_cast<int>(s.current_frame - s.frame_base);
  Input real{};
  if (!gekko_received_input(s.gekko, h, frame, real.data()))
  {
    gekko_receive(s.gekko);
    if (!gekko_received_input(s.gekko, h, frame, real.data()))
    {
      ++s.late.missed;
      ++s.late_window.missed;
      return;
    }
  }
  if (real == s.latched[slot])
    return;
  // Only parts of it the game has not read in this frame (this device, and others not yet read).
  const u8 unread = static_cast<u8>(~s.consumed[slot] & 3);
  if (!SameOutside(real, s.latched[slot], unread) || !gekko_accept_late_input(s.gekko, h, frame))
  {
    ++s.late.refused;
    ++s.late_window.refused;
    return;
  }
  s.latched[slot] = real;
  RecordHistory(s.current_frame, s.latched, 0);
  ++s.late.accepted;
  ++s.late_window.accepted;
}

// At the boundary after a frame's run, before GekkoNet's update: a remote player's input for that
// frame that came since and differs from the prediction only in a device the game never read in
// the frame (a Wii Remote it got no report for, in a menu that reads only player 1) changed
// nothing, so it is the frame's input without a rollback. Whether a frame reads a device is the
// same on every machine (the machines run the same emulation).
s64 s_late_end_done = -1;
void LateEndOfFrame()
{
  if (s.phase != Phase::Running || !s.gekko || s.current_frame < 0 ||
      s_late_end_done == s.current_frame)
  {
    return;
  }
  s_late_end_done = s.current_frame;
  LateLogWindow();
  if (s.read_outside_pass)
    return;
  const int frame = static_cast<int>(s.current_frame - s.frame_base);
  bool received = false;
  for (int slot = 0; slot < MAX_PLAYERS; ++slot)
  {
    const int h = s.slot_handle[slot];
    if (h < 0 || slot == s.opts.local_slot)
      continue;
    const u8 unread = static_cast<u8>(~s.consumed[slot] & 3);
    if (!unread)
      continue;
    if (!received)
    {
      gekko_receive(s.gekko);
      received = true;
    }
    Input real{};
    if (!gekko_received_input(s.gekko, h, frame, real.data()) || real == s.latched[slot])
      continue;
    if (!SameOutside(real, s.latched[slot], unread) || !gekko_accept_late_input(s.gekko, h, frame))
      continue;
    s.latched[slot] = real;
    RecordHistory(s.current_frame, s.latched, 0);
    ++s.late.unread;
    ++s.late_window.unread;
  }
}

// The game reads `device` of the player in `slot` (GetPad, GetWiimote).
void LateInputAt(u8 device, int slot)
{
  if (s.phase == Phase::Running && !s.pass_open)
  {
    s.read_outside_pass = true;
    ++s.late.outside;
    ++s.late_window.outside;
  }
  if (s.phase != Phase::Running || !s.pass_open || s.resim || !s.gekko || slot < 0 ||
      slot >= MAX_PLAYERS || (s.consumed[slot] & device))
  {
    return;
  }
  std::lock_guard lk(s.mutex);
  if (slot == s.opts.local_slot)
  {
    const bool mine = device == 1 ? s.controller[slot] == Controller::GameCube :
                                    s.controller[slot] == Controller::WiiRemote;
    if (mine)
      LateLocalInput();
  }
  else if (s.slot_handle[slot] >= 0)
  {
    LateRemoteInput(slot, device);
  }
  s.consumed[slot] |= device;
}
}  // namespace

bool GetPad(int port, GCPadStatus* status)
{
  if (!IsArmed())
    return false;
  if (port < 0 || port >= MAX_PLAYERS)
    return false;
  // Every port is a standard controller on every machine; whether one is plugged in this frame is
  // its player's input (before the session runs: nothing, or the boot's player, LatchBootInput).
  if (s.late_input)
    LateInputAt(1, port);
  *status = DecodePad(s.latched[port]);
  LatOnRead(true, port);
  return true;
}

int LocalRumblePad(int port, bool* adapter)
{
  if (port != s.opts.local_slot || s.controller[s.opts.local_slot] != Controller::GameCube ||
      s.opts.local_pad < 0 || s.opts.local_pad >= 4)
  {
    return -1;
  }
  *adapter = s.local_adapter;
  return s.opts.local_pad;
}

bool GetWiimote(int index, WiimoteEmu::DesiredWiimoteState* state)
{
  if (!IsArmed() || index < 0 || index >= MAX_PLAYERS)
    return false;
  // Every Wii Remote is the session's: one nobody plays is at rest (no button connects it).
  if (s.late_input)
    LateInputAt(2, index);
  const Input& in = s.latched[index];
  LatOnRead(false, index);
  ResetWiimoteState(state);
  // Before the session runs (unless the boot's player plays one), or a player who sent nothing
  // yet: a Wii Remote at rest.
  if (!(in[0] & INPUT_WIIMOTE_CONNECTED))
    return true;
  WiimoteEmu::SerializedWiimoteState ser{};
  ser.length = std::min<u8>(in[9], static_cast<u8>(ser.data.size()));
  std::memcpy(ser.data.data(), in.data() + 10, ser.length);
  // The bytes came from a peer: anything that does not parse is a Wii Remote at rest.
  if (!WiimoteEmu::DeserializeDesiredState(state, ser))
    ResetWiimoteState(state);
  return true;
}

void SetWiimoteOverride(std::optional<WiimoteOverride> value)
{
  std::lock_guard lk(s.mutex);
  s.wiimote_override = value;
}

void SetLocalController(Controller controller)
{
  s.requested_controller = static_cast<int>(controller);
}

std::optional<std::string> RequestMove(int from, int to)
{
  if (from < 0 || from >= MAX_PLAYERS || to < 0 || to >= MAX_PLAYERS || from == to)
    return "slots must be two different slots, 1 to 4";
  std::lock_guard lk(s.mutex);
  if (s.phase != Phase::Running)
    return "the game is not running yet";
  if (s.plan.active || s.local_request || s.local_move)
    return "a change of players is under way: try again in a moment";
  std::lock_guard nlk(s.net_mutex);
  bool someone = false;
  for (const int slot : {from, to})
  {
    if (Plays(slot))
    {
      someone = true;
      continue;
    }
    // A player joining (or leaving) there: its slot is settling.
    if (slot != s.opts.local_slot && s.peers[slot].slot >= 0)
      return fmt::format("player {} is joining or leaving: try again in a moment", slot + 1);
  }
  if (!someone)
    return "nobody plays in either slot";
  s.local_move = std::make_pair(from, to);
  AddLog(fmt::format("asking to move slot {} <-> slot {}", from + 1, to + 1));
  return std::nullopt;
}

std::optional<std::pair<int, int>> TakeMoveDone()
{
  std::lock_guard lk(s.mutex);
  return std::exchange(s.move_done, std::nullopt);
}

LocalControllerInfo GetLocalControllerInfo()
{
  const int requested = s.requested_controller.load();
  {
    std::lock_guard lk(s.pick_mutex);
    // Asked for and not yet taken, or before the session's first frame (a joiner waiting for the
    // game): what the next frame will pick.
    if (s.picked && requested < 0)
      return s.pick;
    if (!s.opts.auto_pad)
    {
      LocalControllerInfo p = s.pick;
      if (requested >= 0)
        p.controller = static_cast<Controller>(requested);
      return p;
    }
  }
  return PickLocal(requested >= 0 ? static_cast<Controller>(requested) : s.opts.controller);
}

// The boundary the session runs at: Auto becomes BrawlLoop for a Brawl build, ViField otherwise
// (asked while the session waits for its first boundary: the game is known by then).
Boundary SessionBoundary()
{
  if (s.phase == Phase::Armed && s.opts.boundary == Boundary::Auto)
  {
    s.boundary = IsBrawlGameId(SConfig::GetInstance().GetGameID()) ? Boundary::BrawlLoop :
                                                                    Boundary::ViField;
  }
  return s.boundary == Boundary::Auto ? Boundary::BrawlLoop : s.boundary;
}

// The session's first boundary (either kind): snapshots start; a joiner asks to join (false: its
// frame runs on its own, it waits at the next boundary), the others wait for the players.
bool EnterFirstBoundary(const char* where)
{
  auto& rb = Rollback::RollbackManager::Get();
  // The boot's local input (LatchBootInput) ends here: from here on every input is the session's.
  s.latched = {};
  // Brawl's hard-coded exclusions are Brawl's addresses; any other game keeps all of its RAM.
  rb.SetBrawlExclusions(s.boundary == Boundary::BrawlLoop);
  rb.SetChecksumMasks(s.boundary == Boundary::BrawlLoop ? BRAWL_DISC_REVISION :
                                                         std::vector<Rollback::MemoryRegion>{});
  rb.m_full_ios_in_snapshots = true;
  s.journal_marks.clear();
  if (!rb.m_frame_save_enabled.load(std::memory_order_relaxed))
    rb.ToggleFrameSave();
  if (s.opts.join)
  {
    // A joiner runs this frame by itself and waits at the next boundary, which (as on every
    // player's machine) starts a JIT block of its own: the image loads in the same place.
    s.phase = Phase::Joining;
    s.join_since = Clock::now();
    AddLog(fmt::format("first boundary ({}): asking to join", where));
    return false;
  }
  s.phase = Phase::Connecting;
  AddLog(fmt::format("first boundary ({}): waiting for the players", where));
  return true;
}

enum class Top
{
  Run,    // run the frame (a pass began), or the game runs on its own (no session any more)
  Yield,  // give the CPU back without running anything; take this boundary again
};

Top FrameTop(Core::System& system);

bool s_hook_yield = false;

bool OnLoopTop(const Core::CPUThreadGuard& guard)
{
  if (!IsArmed())
    return false;
  std::lock_guard lk(s.mutex);
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  auto run_original = [&] {
    ppc.gpr[25] = 1;
    ppc.npc = BRAWL_LOOP_TOP + 4;
  };
  if (SessionBoundary() != Boundary::BrawlLoop)
  {
    run_original();
    return true;
  }

  // The hook fires only for main.dol's loop; until the game reaches scBoot it runs freely, the
  // same on every machine (no input but a lone player's, BootIsLocal; the session's pinned
  // settings and clock).
  if (s.phase == Phase::Armed)
  {
    if (system.GetMemory().Read_U32(BRAWL_LOOP_TOP) != BRAWL_LOOP_TOP_OPCODE ||
        !HLE_Misc::IsBootScene(guard, ppc.gpr[23]))
    {
      run_original();
      return true;
    }
    if (!EnterFirstBoundary("scBoot"))
    {
      run_original();
      return true;
    }
  }
  if (FrameTop(system) == Top::Yield)
  {
    // Back to this hook, with no game code run and no emulated time gone (TakeHookYield). It
    // went on to the loop end and back before, 14 cycles each pass for as long as a pause held
    // the CPU (another thread's CPUThreadGuard; the harness reading memory: 784 cycles in one
    // frame), so that machine's frame came later than the others' and their memory parted.
    ppc.npc = BRAWL_LOOP_TOP;
    s_hook_yield = true;
    return true;
  }
  run_original();
  return true;
}

bool TakeHookYield()
{
  return std::exchange(s_hook_yield, false);
}

// Before the first boundary nothing is the session's yet. A session this machine starts alone (a
// room's host) or that joins a running one (its machine then comes from another player's image)
// has no other machine to boot the same as, so its player plays the boot: a mod's own menu before
// Brawl's scBoot waits for a button (bug report #61: Balanced Brawl's Gecko menu, forever).
// Players who start together still boot with no input, the same on every machine.
bool BootIsLocal()
{
  return s.phase == Phase::Armed &&
         (s.opts.join || s.players.load() == (1u << s.opts.local_slot));
}

// Every VI field of such a boot (CPU thread): the local controller is GameCube port 1 or Wii
// Remote 1, as for a console of one, until the first boundary (EnterFirstBoundary).
void LatchBootInput(Core::System& system)
{
  // UpdateLocalController counts frames to pick the controller again, and none run yet.
  if (s.pick_countdown > 0)
    --s.pick_countdown;
  s.latched = {};
  s.latched[0] = ReadLocalInput();
  s.boot_local = true;
  SyncWiimotes(system);
}

bool g_vi_boundary = false;

void OnViField()
{
  if (IsArmed())
    g_vi_boundary = true;
}

void OnViBoundary(Core::System& system)
{
  std::lock_guard lk(s.mutex);
  // A VI-field session's first boundary is the game's first field: it has no boot to play.
  if (BootIsLocal() && SessionBoundary() == Boundary::BrawlLoop)
    LatchBootInput(system);
  if (!Active() || SessionBoundary() != Boundary::ViField)
  {
    g_vi_boundary = false;
    return;
  }
  auto& ppc = system.GetPPCState();
  if (s.phase == Phase::Armed)
  {
    g_vi_boundary = false;
    // Only once the game runs: not from the boot's own steps (the apploader), nor from the JIT's
    // first Advance before any block set the next instruction (Nyx's fork waits for npc too).
    if (system.GetCPU().GetState() != CPU::State::Running || ppc.npc == 0)
      return;
    if (!EnterFirstBoundary("VI field"))
      return;
  }
  if (!s.vi_frame_ended)
  {
    // The end of the frame that ran since the last boundary (Brawl's FrameEnd and LoopEnd hooks).
    s.vi_frame_ended = true;
    if (s.pass_open)
    {
      s.pass_open = false;
      if (s.phase == Phase::Running && s.iteration < s.ops.count)
      {
        if (s.ops.save_after[s.iteration])
        {
          s.queued_save.pending = true;
          s.queued_save.frame = s.ops.frame[s.iteration];
          s.queued_save.checksum = s.ops.checksum[s.iteration];
        }
        s.iteration++;
        if (s.iteration >= s.ops.count)
          s.iteration = 0;
      }
    }
  }
  if (FrameTop(system) == Top::Yield)
    return;  // g_vi_boundary stays set: the next Advance takes this boundary again
  s.vi_frame_ended = false;
  g_vi_boundary = false;
  // Diagnostics: PPR_MACHINE_VI_DEBUG=N logs the first N boundaries' timing state.
  static const int vi_debug = [] {
    const char* e = std::getenv("PPR_MACHINE_VI_DEBUG");
    return e ? std::atoi(e) : 0;
  }();
  static int vi_logged = 0;
  if (vi_logged < vi_debug && s.pass_open)
  {
    ++vi_logged;
    auto& ct = system.GetCoreTiming();
    std::string ev = ct.GetScheduledEventsSummary();
    for (auto& c : ev)
      if (c == '\n')
        c = '|';
    NOTICE_LOG_FMT(BRAWLBACK, "vi-debug frame {} {} timer {} slice {} down {} pc {:08x} npc {:08x} ev {}",
                   s.current_frame, s.resim ? "rerun" : "first", ct.GetGlobals().global_timer,
                   ct.GetGlobals().slice_length, ppc.downcount, ppc.pc, ppc.npc, ev.substr(0, 400));
  }
}

bool IsBrawlGameId(std::string_view game_id)
{
  // A launcher (.dol/.elf) gets "ID-<file name>"; "00000000" is no game yet.
  return game_id.starts_with("RSB") || game_id.starts_with("ID-") || game_id == "00000000" ||
         game_id.empty();
}

// The start of a frame, at either boundary: the save of the frame that ended, the wait for the
// next frames (and a rollback), the pass's inputs.
Top FrameTop(Core::System& system)
{
  if (s.phase == Phase::Error || s.phase == Phase::Stopped)
  {
    system.GetCoreTiming().SetRollbackResimulating(false);
    system.GetCoreTiming().SetRollbackSpeedAdjustment(1.0);
    system.GetCoreTiming().SetPresentPacing(false);
    return Top::Run;
  }

  PerformQueuedSave(system);
  if (s.iteration == 0 || s.iteration >= s.ops.count)
  {
    s.iteration = 0;
    switch (WaitForFrames(system))
    {
    case Wait::Frames:
      break;
    case Wait::Ended:
      // The session ended: from here the game runs on its own (as a local game).
      system.GetCoreTiming().SetRollbackResimulating(false);
      system.GetCoreTiming().SetRollbackSpeedAdjustment(1.0);
      system.GetCoreTiming().SetPresentPacing(false);
      return Top::Run;
    case Wait::Yield:
      s.ops.count = 0;
      return Top::Yield;
    }
  }
  static const s64 selfload_at = [] {
    const char* e = std::getenv("PPR_MACHINE_SELFLOAD_AT");
    return e ? std::strtoll(e, nullptr, 10) : -1;
  }();
  static bool selfloaded = false;
  if (selfload_at >= 0 && !selfloaded && s.phase == Phase::Running && s.iteration == 0 &&
      s.ops.count == 1 && s.ops.frame[0] >= selfload_at)
  {
    selfloaded = true;
    SelfLoad(system, s.ops.frame[0]);
  }
  // Diagnostics (PPR_MACHINE_DUMP_AT=<frame>, with PPR_MACHINE_DUMP_DIR): the state at the start of
  // that frame's first run, on every machine.
  // PPR_MACHINE_PARTS_AT=+A,+B: the small parts at every first run of frames A..B after the epoch.
  static const std::pair<s64, s64> parts_range = [] {
    const char* e = std::getenv("PPR_MACHINE_PARTS_AT");
    if (!e || *e != '+')
      return std::pair<s64, s64>{-1, -1};
    char* end = nullptr;
    const s64 a = std::strtoll(e + 1, &end, 10);
    const s64 b = end && *end == ',' && end[1] == '+' ? std::strtoll(end + 2, nullptr, 10) : a;
    return std::pair<s64, s64>{a, b};
  }();
  static s64 parts_logged = -1;
  if (parts_range.first >= 0 && s.epoch > 0 && s.phase == Phase::Running && s.gekko &&
      s.iteration < s.ops.count)
  {
    const s64 f = s.ops.frame[s.iteration];
    // Only a run with every input of the frame confirmed (the run that counts).
    const bool confirmed = s.frame_base + gekko_confirmed_frame(s.gekko) >= f;
    if (confirmed && f > parts_logged && f >= s.frame_base + parts_range.first &&
        f <= s.frame_base + parts_range.second)
    {
      parts_logged = f;
      LogParts(system, f);
    }
  }
  static const s64 io_frames = [] {
    const char* e = std::getenv("PPR_MACHINE_IOLOG");
    return e ? std::strtoll(e, nullptr, 10) : 0;
  }();
  static bool io_written = false;
  // PPR_MACHINE_IOLOG=-K: the last K frames' logs, written at the first desync (whenever it comes).
  const auto write_io_log = [&](std::string_view why) {
    io_written = true;
    s_io_current = nullptr;
    g_io_log = false;
    const char* dir = std::getenv("PPR_MACHINE_DUMP_DIR");
    if (!dir || !*dir)
      return;
    const std::string path = fmt::format("{}/iolog-p{}.txt", dir, s.opts.local_slot + 1);
    if (std::FILE* out = std::fopen(path.c_str(), "wb"))
    {
      for (const auto& [frame, text] : s_io_frames)
        std::fwrite(text.data(), 1, text.size(), out);
      std::fclose(out);
    }
    AddLog(fmt::format("I/O log of {} frames written to {} ({})", s_io_frames.size(), path, why));
    s_io_frames.clear();
  };
  if (io_frames < 0 && !io_written && s.epoch > 0 && s.phase == Phase::Running && s.iteration < s.ops.count)
  {
    const s64 f = s.ops.frame[s.iteration];
    std::lock_guard io_lk(s_io_mutex);
    if (s.desyncs > 0)
    {
      write_io_log(fmt::format("the first desync, at frame {}", s.last_desync_frame));
    }
    else
    {
      s_io_current = &(s_io_frames[f] = std::string());
      s_io_events = 0;
      s_io_frame = f;
      const auto& cpu = system.GetPPCState();
      *s_io_current += fmt::format("F {} ticks {} pc {:08x} lr {:08x} msr {:08x} dec {:08x}\n", f,
                                   system.GetCoreTiming().GetTicks(), cpu.pc, cpu.spr[SPR_LR], cpu.msr.Hex,
                                   cpu.spr[SPR_DEC]);
      g_io_log = true;
      while (!s_io_frames.empty() && s_io_frames.begin()->first <= f + io_frames)
        s_io_frames.erase(s_io_frames.begin());
    }
  }
  // PPR_MACHINE_IOLOG_FROM=F: the N frames from frame F (counted from boot) instead.
  static const s64 io_from = [] {
    const char* e = std::getenv("PPR_MACHINE_IOLOG_FROM");
    return e ? std::strtoll(e, nullptr, 10) : -1;
  }();
  if (io_frames > 0 && !io_written && s.epoch > 0 && s.phase == Phase::Running && s.iteration < s.ops.count)
  {
    const s64 f = s.ops.frame[s.iteration];
    const s64 first = io_from >= 0 ? io_from : s.frame_base;
    std::lock_guard io_lk(s_io_mutex);
    if (f < first)
    {
      s_io_current = nullptr;
      g_io_log = false;
    }
    else if (f < first + io_frames)
    {
      // A re-run replaces the frame's log: the last run is the one that counts.
      s_io_current = &(s_io_frames[f] = std::string());
      s_io_events = 0;
      s_io_frame = f;
      const auto& cpu = system.GetPPCState();
      *s_io_current += fmt::format("F {} ticks {} pc {:08x} lr {:08x} msr {:08x} dec {:08x}\n", f,
                                   system.GetCoreTiming().GetTicks(), cpu.pc, cpu.spr[SPR_LR], cpu.msr.Hex,
                                   cpu.spr[SPR_DEC]);
      g_io_log = true;
    }
    else
    {
      s_io_current = nullptr;
      g_io_log = false;
      // Written once no rollback can reach the logged frames any more.
      if (f >= first + io_frames + 32)
        write_io_log(io_from >= 0 ? "the frames asked for" : "the first frames after the epoch");
    }
  }
  // A comma-separated list of frames; "+N" counts from the last epoch's frame.
  static const std::string dump_at = [] {
    const char* e = std::getenv("PPR_MACHINE_DUMP_AT");
    return std::string(e ? e : "");
  }();
  static std::set<s64> dumped;
  if (!dump_at.empty() && s.phase == Phase::Running && s.iteration < s.ops.count)
  {
    const s64 f = s.ops.frame[s.iteration];
    size_t pos = 0;
    while (pos < dump_at.size())
    {
      const size_t end = std::min(dump_at.find(',', pos), dump_at.size());
      const std::string item = dump_at.substr(pos, end - pos);
      pos = end + 1;
      if (item.empty())
        continue;
      const s64 target = item[0] == '+' ? (s.epoch > 0 ? s.frame_base + std::strtoll(item.c_str() + 1, nullptr, 10) : -1) :
                                          std::strtoll(item.c_str(), nullptr, 10);
      if (target == f && !dumped.contains(f))
      {
        dumped.insert(f);
        DumpEpochState(system, f, "at");
      }
    }
  }
  BeginPass(system);
  return Top::Run;
}

bool OnFrameEnd(const Core::CPUThreadGuard& guard)
{
  if (!IsArmed())
    return false;
  std::lock_guard lk(s.mutex);
  auto& system = guard.GetSystem();
  auto& ppc = system.GetPPCState();
  if (SessionBoundary() != Boundary::BrawlLoop)
  {
    PowerPC::MMU::HostWrite<u32>(guard, ppc.gpr[0], ppc.gpr[23] + 0x100);
    ppc.npc = BRAWL_LOOP_END;
    return true;
  }
  // The original instruction first (stw r0,0x100(r23)), so the save at the next loop top holds
  // this pass's frame counter (HLE_Misc::BrawlbackGekkoNetFrameEnd).
  PowerPC::MMU::HostWrite<u32>(guard, ppc.gpr[0], ppc.gpr[23] + 0x100);
  ppc.npc = BRAWL_LOOP_END;
  if (s.phase == Phase::Running && s.iteration < s.ops.count && s.ops.save_after[s.iteration])
  {
    s.queued_save.pending = true;
    s.queued_save.frame = s.ops.frame[s.iteration];
    s.queued_save.checksum = s.ops.checksum[s.iteration];
  }
  return true;
}

bool OnLoopEnd(const Core::CPUThreadGuard& guard)
{
  if (!IsArmed())
    return false;
  std::lock_guard lk(s.mutex);
  auto& ppc = guard.GetSystem().GetPPCState();
  if (SessionBoundary() == Boundary::BrawlLoop && s.phase == Phase::Running && s.ops.count > 0)
  {
    s.pass_open = false;
    s.iteration++;
    if (s.iteration >= s.ops.count)
      s.iteration = 0;
  }
  ppc.npc = BRAWL_LOOP_TOP;
  return true;
}

picojson::value Status()
{
  std::unique_lock lk(s.mutex, std::defer_lock);
  (void)lk.try_lock_for(std::chrono::milliseconds(200));
  picojson::object o;
  o["phase"] = picojson::value(PhaseName(s.phase));
  o["armed"] = picojson::value(Active());
  if (!lk.owns_lock())
  {
    o["busy"] = picojson::value(true);
    return picojson::value(o);
  }
  o["error"] = picojson::value(s.error);
  o["slot"] = picojson::value(static_cast<double>(s.opts.local_slot));
  o["players"] = picojson::value(static_cast<double>(s.players));
  o["frame"] = picojson::value(static_cast<double>(s.current_frame));
  o["rollbacks"] = picojson::value(static_cast<double>(s.rollbacks));
  o["max_rollback"] = picojson::value(static_cast<double>(s.max_rollback));
  o["frames_resimulated"] = picojson::value(static_cast<double>(s.frames_resimulated));
  o["stall_polls"] = picojson::value(static_cast<double>(s.stall_polls));
  o["skipped_loads"] = picojson::value(static_cast<double>(s.skipped_loads));
  o["saves"] = picojson::value(static_cast<double>(s.saves));
  o["desyncs"] = picojson::value(static_cast<double>(s.desyncs));
  o["journal_errors"] = picojson::value(static_cast<double>(s.journal_errors));
  o["last_desync_frame"] = picojson::value(static_cast<double>(s.last_desync_frame));
  o["frames_ahead"] = picojson::value(static_cast<double>(s.frames_ahead));
  o["speed"] = picojson::value(s.speed);
  o["synctest"] = picojson::value(static_cast<double>(s.opts.synctest));
  o["epoch"] = picojson::value(static_cast<double>(s.epoch));
  {
    // What this machine's UDP socket sent and received (ENet's counters; datagrams, UDP payload
    // bytes): the session's upload, for traffic tests.
    std::unique_lock nlk(s.net_mutex, std::try_to_lock);
    if (nlk.owns_lock() && s.host)
    {
      picojson::object net;
      net["sent_bytes"] = picojson::value(static_cast<double>(s.host->totalSentData));
      net["sent_datagrams"] = picojson::value(static_cast<double>(s.host->totalSentPackets));
      net["received_bytes"] = picojson::value(static_cast<double>(s.host->totalReceivedData));
      net["received_datagrams"] = picojson::value(static_cast<double>(s.host->totalReceivedPackets));
      o["net"] = picojson::value(net);
    }
  }
  o["frame_base"] = picojson::value(static_cast<double>(s.frame_base));
  o["input_delay"] = picojson::value(static_cast<double>(s.input_delay));
  o["plan_frame"] = picojson::value(static_cast<double>(s.plan.active ? s.plan.frame : -1));
  o["image_ms"] = picojson::value(s.image_ms);
  o["image_mb"] = picojson::value(s.image_packed / 1048576.0);
  o["image_raw_mb"] = picojson::value(s.image_raw / 1048576.0);
  o["room"] = picojson::value(s.opts.room);
  o["gone"] = picojson::value(static_cast<double>(s.gone.load()));
  {
    // A joiner's SD card before its join (SDSyncStep), for the launcher's dialog and progress.
    using Step = State::SDSync::Step;
    const State::SDSync& y = s.sd_sync;
    if (y.step != Step::Idle)
    {
      picojson::object sd;
      const char* step = y.step == Step::Checking    ? "checking" :
                         y.step == Step::Asking      ? "ask" :
                         y.step == Step::Downloading ? "downloading" :
                         y.step == Step::Ready       ? "ready" :
                                                       "failed";
      sd["step"] = picojson::value(step);
      const u64 hash_total = y.hash_total.load();
      sd["checked"] = picojson::value(
          y.hash_finished ? 1.0 : hash_total ? static_cast<double>(y.hash_done.load()) / hash_total : 0.0);
      sd["bytes"] = picojson::value(static_cast<double>(y.missing_bytes));
      sd["files"] = picojson::value(static_cast<double>(y.missing_files));
      picojson::array names;
      for (const std::string& n : y.names)
        names.emplace_back(n);
      sd["names"] = picojson::value(names);
      sd["eta_s"] = picojson::value(y.rate > 0 ? y.missing_bytes / y.rate : 0.0);
      if (y.card && y.step == Step::Downloading)
      {
        const auto pr = y.card->GetSyncProgress();
        sd["done"] = picojson::value(static_cast<double>(pr.done));
        sd["total"] = picojson::value(static_cast<double>(pr.total));
        sd["rate"] = picojson::value(y.rate_now);
        if (y.rate_now > 0)
          sd["eta_s"] = picojson::value((pr.total - pr.done) / y.rate_now);
      }
      o["sd_sync"] = picojson::value(sd);
    }
  }
  picojson::array peers;
  {
    std::lock_guard nlk(s.net_mutex);
    o["room_known"] = picojson::value(s.room_known);
    o["probe"] = picojson::value(s.probe.state);
    picojson::object rx;
    rx["from"] = picojson::value(static_cast<double>(s.image_rx.from));
    rx["epoch"] = picojson::value(static_cast<double>(s.image_rx.epoch));
    rx["received"] = picojson::value(static_cast<double>(s.image_rx.data.size()));
    rx["size"] = picojson::value(static_cast<double>(s.image_rx.packed_size));
    rx["complete"] = picojson::value(s.image_rx.complete);
    rx["sent"] = picojson::value(static_cast<double>(s.image_tx_sent));
    rx["to_send"] = picojson::value(static_cast<double>(s.image_tx.size()));
    o["image_rx"] = picojson::value(rx);
    for (const PeerLink& p : s.peers)
    {
      if (p.slot < 0)
        continue;
      picojson::object po;
      po["slot"] = picojson::value(static_cast<double>(p.slot));
      po["connected"] = picojson::value(p.hello);
      po["started"] = picojson::value(static_cast<double>(p.started));
      po["late"] = picojson::value(p.late);
      po["join_asked"] = picojson::value(p.join_asked);
      po["left"] = picojson::value(p.left);
      po["rtt_ms"] = picojson::value(p.rtt_ms);
      po["code"] = picojson::value(p.code);
      po["epoch_at"] = picojson::value(static_cast<double>(p.epoch_at));
      po["host"] = picojson::value(fmt::format("{}:{}", p.host, p.port));
      peers.emplace_back(po);
    }
  }
  o["peers"] = picojson::value(peers);
  picojson::array log;
  std::lock_guard llk(s.log_mutex);
  for (const auto& l : s.log)
    log.emplace_back(l);
  o["log"] = picojson::value(log);
  picojson::array diag;
  for (const auto& l : s_diag)
    diag.emplace_back(l);
  o["diag"] = picojson::value(diag);
  o["diag_frames"] = picojson::value(static_cast<double>(s_diag_frames));
  const auto& rb = Rollback::RollbackManager::Get();
  const auto t = rb.GetTimingStats();
  o["save_us_avg"] = picojson::value(t.save_count ? double(t.save_us_total) / t.save_count : 0.0);
  o["load_us_avg"] = picojson::value(t.load_count ? double(t.load_us_total) / t.load_count : 0.0);
  o["save_us_max"] = picojson::value(static_cast<double>(t.save_us_max));
  o["load_us_max"] = picojson::value(static_cast<double>(t.load_us_max));
  return picojson::value(o);
}

picojson::value Digest(s64 frame, const std::vector<u32>& addrs)
{
  std::lock_guard lk(s.mutex);
  picojson::object o;
  if (frame < 0)
    return picojson::value(o);
  const FrameDigest& d = s_digests[static_cast<size_t>(frame) % s_digests.size()];
  if (d.frame != frame)
    return picojson::value(o);
  o["frame"] = picojson::value(static_cast<double>(frame));
  o["checksum"] = picojson::value(static_cast<double>(d.checksum));
  o["resim"] = picojson::value(d.resim);
  {
    std::string changed;
    for (const auto& [addr, crc, kind] : d.parts.changed)
      changed += fmt::format("{:08x}:{:08x}:{} ", addr, crc, kind);
    std::string slice_gpu;
    for (const u32 addr : d.parts.slice_gpu)
      slice_gpu += fmt::format("{:08x} ", addr);
    o["changed"] = picojson::value(changed);
    o["change_crc"] = picojson::value(static_cast<double>(d.parts.change_crc));
    o["slice_first"] = picojson::value(static_cast<double>(d.parts.slice_first));
    o["slice_crc"] = picojson::value(static_cast<double>(d.parts.slice_crc));
    o["slice_gpu"] = picojson::value(slice_gpu);
  }
  std::string text;
  for (const auto& [addr, crc] : d.granules)
    text += fmt::format("{:08x}:{:08x} ", addr, crc);
  o["granules"] = picojson::value(text);
  picojson::object bytes;
  for (const u32 a : addrs)
  {
    const auto it = d.bytes.find(a & ~(ROLLBACK_PAGE_SIZE - 1));
    if (it == d.bytes.end())
      continue;
    std::string hex;
    for (const u8 b : it->second)
      hex += fmt::format("{:02x}", b);
    bytes[fmt::format("{:08x}", it->first)] = picojson::value(hex);
  }
  o["bytes"] = picojson::value(bytes);
  return picojson::value(o);
}

picojson::value History(s64 since)
{
  std::lock_guard lk(s.mutex);
  picojson::object frames;
  const s64 newest = s.current_frame;
  // Frames GekkoNet may still roll back are not final yet.
  const s64 upto = newest - MAX_ROLLBACK_FRAMES - 1;
  for (s64 f = std::max<s64>(since, upto - static_cast<s64>(HISTORY_SIZE) + 1); f <= upto; ++f)
  {
    if (f < 0)
      continue;
    const HistoryEntry& e = s.history[static_cast<size_t>(f) % HISTORY_SIZE];
    if (e.frame != f)
      continue;
    picojson::array row;
    for (const Input& in : e.inputs)
    {
      std::string hex;
      const int n = (in[0] & INPUT_WIIMOTE_CONNECTED) ? 10 + std::min<int>(in[9], 31) : 9;
      for (int i = 0; i < n; ++i)
        hex += fmt::format("{:02x}", in[i]);
      row.emplace_back(hex);
    }
    row.emplace_back(static_cast<double>(e.checksum));
    frames[std::to_string(f)] = picojson::value(row);
  }
  picojson::object o;
  o["frames"] = picojson::value(frames);
  return picojson::value(o);
}

bool g_io_log = false;
bool g_io_cpu_store = false;

bool IoLogLight()
{
  static const bool light = [] {
    const char* e = std::getenv("PPR_MACHINE_IOLOG_LIGHT");
    return e && *e && *e != '0';
  }();
  return light;
}

// The end of an I/O log line: CRCs of the FPRs (both halves) and GPRs, CR and FPSCR, so the first
// event after which two machines' registers differ brackets the instruction. With
// PPR_MACHINE_IOLOG_REGS=FRAME:FIRST-LAST (FRAME counted from the epoch), the FPRs and GPRs in full
// for those events of that frame.
void IoLogRegs(const PowerPC::PowerPCState& ppc)
{
  static const std::array<s64, 3> regs_at = [] {
    std::array<s64, 3> r{-1, 0, -1};
    if (const char* e = std::getenv("PPR_MACHINE_IOLOG_REGS"))
      std::sscanf(e, "%lld:%lld-%lld", reinterpret_cast<long long*>(&r[0]),
                  reinterpret_cast<long long*>(&r[1]), reinterpret_cast<long long*>(&r[2]));
    return r;
  }();
  const u64 n = s_io_events++;
  if (IoLogLight())
  {
    *s_io_current += "\n";
    return;
  }
  *s_io_current += fmt::format(" f {:08x} g {:08x} cr {:08x} fpscr {:08x}\n",
                               Common::ComputeCRC32(reinterpret_cast<const u8*>(ppc.ps), sizeof(ppc.ps)),
                               Common::ComputeCRC32(reinterpret_cast<const u8*>(ppc.gpr), sizeof(ppc.gpr)),
                               ppc.cr.Get(), ppc.fpscr.Hex);
  if (s_io_frame - s.frame_base == regs_at[0] && static_cast<s64>(n) >= regs_at[1] && static_cast<s64>(n) <= regs_at[2])
  {
    for (int i = 0; i < 32; ++i)
      *s_io_current += fmt::format("  f{} {:016x} {:016x}\n", i, ppc.ps[i].PS0AsU64(), ppc.ps[i].PS1AsU64());
    for (int i = 0; i < 32; ++i)
      *s_io_current += fmt::format("  r{} {:08x}\n", i, ppc.gpr[i]);
    // The code before pc as RAM holds it and as the instruction cache would give it (a peek: no
    // line is filled), "-" where the cache has no valid line.
    auto& memory = Core::System::GetInstance().GetMemory();
    for (u32 a = ppc.pc - 0x60; a <= ppc.pc + 0x8; a += 4)
    {
      if (a < 0x80000000 || a >= 0x81800000)
        continue;
      const u32 phys = a & 0x1FFFFFFF;
      u32 ram;
      std::memcpy(&ram, memory.GetRAM() + phys, 4);
      const u8 way = ppc.iCache.lookup_table[(phys & memory.GetRamMask()) >> 5];
      const u32 set = (phys >> 5) & 0x7f;
      const std::string ic = way == 0xff ? std::string("-") :
                                           fmt::format("{:08x}", Common::swap32(ppc.iCache.data[set][way][(phys >> 2) & 7]));
      *s_io_current += fmt::format("  i{:08x} {:08x} {}\n", a, Common::swap32(ram), ic);
    }
  }
}

bool IoLogInRegsWindow()
{
  static const std::array<s64, 3> regs_at = [] {
    std::array<s64, 3> r{-1, 0, -1};
    if (const char* e = std::getenv("PPR_MACHINE_IOLOG_REGS"))
      std::sscanf(e, "%lld:%lld-%lld", reinterpret_cast<long long*>(&r[0]),
                  reinterpret_cast<long long*>(&r[1]), reinterpret_cast<long long*>(&r[2]));
    return r;
  }();
  return s_io_frame - s.frame_base == regs_at[0] && static_cast<s64>(s_io_events) >= regs_at[1] &&
         static_cast<s64>(s_io_events) <= regs_at[2];
}

void IoLogStep(u32 pc, u32 inst)
{
  std::lock_guard lk(s_io_mutex);
  if (!s_io_current || !IoLogInRegsWindow())
    return;
  const auto& ppc = Core::System::GetInstance().GetPPCState();
  std::string line = fmt::format("  x{:08x} {:08x} f {:08x}", pc, inst,
                                 Common::ComputeCRC32(reinterpret_cast<const u8*>(ppc.ps), sizeof(ppc.ps)));
  for (int i = 0; i < 8; ++i)
    line += fmt::format(" {:016x}:{:016x}", ppc.ps[i].PS0AsU64(), ppc.ps[i].PS1AsU64());
  *s_io_current += line + "\n";
}

bool IoLogBlocks()
{
  static const bool on = std::getenv("PPR_MACHINE_IOLOG_BLOCKS") != nullptr;
  return on;
}

void IoLogBlock(u32 pc)
{
  if (!g_io_log)
    return;
  std::lock_guard lk(s_io_mutex);
  if (!s_io_current)
    return;
  auto& system = Core::System::GetInstance();
  const auto& ppc = system.GetPPCState();
  *s_io_current += fmt::format("B {:08x} t {} f {:08x} g {:08x}\n", pc, system.GetCoreTiming().GetTicks(),
                               Common::ComputeCRC32(reinterpret_cast<const u8*>(ppc.ps), sizeof(ppc.ps)),
                               Common::ComputeCRC32(reinterpret_cast<const u8*>(ppc.gpr), sizeof(ppc.gpr)));
  // PPR_MACHINE_IOLOG_BLOCK_DUMP=pc,pc,... (hex): every FPR at those blocks' entries.
  static const std::vector<u32> dump_at = [] {
    std::vector<u32> v;
    if (const char* e = std::getenv("PPR_MACHINE_IOLOG_BLOCK_DUMP"))
    {
      for (const std::string& item : SplitString(e, ','))
        v.push_back(static_cast<u32>(std::strtoul(item.c_str(), nullptr, 16)));
    }
    return v;
  }();
  if (std::ranges::find(dump_at, pc) != dump_at.end())
  {
    std::string fpr = "  fpr";
    for (const auto& ps : ppc.ps)
      fpr += fmt::format(" {:016x}:{:016x}", ps.PS0AsU64(), ps.PS1AsU64());
    *s_io_current += fpr + "\n";
  }
}

void IoLogEvent(const std::string& name, s64 time, s64 late)
{
  std::lock_guard lk(s_io_mutex);
  if (!s_io_current)
    return;
  const auto& ppc = Core::System::GetInstance().GetPPCState();
  *s_io_current += fmt::format("E {} t {} late {} pc {:08x} lr {:08x} g {:08x}\n", name, time, late, ppc.pc,
                               ppc.spr[SPR_LR],
                               Common::ComputeCRC32(reinterpret_cast<const u8*>(ppc.gpr), sizeof(ppc.gpr)));
  // Events run between JIT blocks, with every register in ppcState: all FPRs (ps0 and ps1), to
  // find the register two JITs part on.
  std::string fpr = "  fpr";
  for (const auto& ps : ppc.ps)
    fpr += fmt::format(" {:016x}:{:016x}", ps.PS0AsU64(), ps.PS1AsU64());
  *s_io_current += fpr + "\n";
}

void IoLogRead(u32 addr, u32 size, u64 value)
{
  std::lock_guard lk(s_io_mutex);
  if (!s_io_current)
    return;
  const auto& ppc = Core::System::GetInstance().GetPPCState();
  *s_io_current += fmt::format("R {:08x} {} {:x} pc {:08x}", addr, size, value, ppc.pc);
  IoLogRegs(ppc);
}

void IoLogWrite(u32 addr, size_t size)
{
  if (g_io_cpu_store && IoLogLight())
    return;
  std::lock_guard lk(s_io_mutex);
  if (!s_io_current)
    return;
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  const u32 phys = addr & 0x1FFFFFFF;
  const u8* src = nullptr;
  if (phys + size <= memory.GetRamSizeReal())
    src = memory.GetRAM() + phys;
  else if (phys >= 0x10000000 && memory.GetEXRAM() && phys - 0x10000000 + size <= memory.GetExRamSizeReal())
    src = memory.GetEXRAM() + (phys - 0x10000000);
  const u32 crc = src ? Common::ComputeCRC32(src, static_cast<u32>(size)) : 0;
  *s_io_current += fmt::format("W {:08x} {:x} {:08x} pc {:08x}", addr, size, crc, system.GetPPCState().pc);
  if (!g_io_cpu_store)
  {
    // Who wrote it: a CoreTiming event's callback, or another thread (a device's worker).
    const std::string* ev = system.GetCoreTiming().m_current_event;
    *s_io_current += fmt::format(" by {}", !Core::IsCPUThread() ? std::string("thread") :
                                            ev                  ? *ev :
                                                                  std::string("cpu"));
  }
  IoLogRegs(system.GetPPCState());
}
}  // namespace Machine

namespace Machine
{
static std::pair<u32, u32> WatchRange()
{
  const char* e = std::getenv("PPR_MACHINE_WATCH");
  if (!e)
    return {0, 0};
  char* end = nullptr;
  const u32 lo = static_cast<u32>(std::strtoul(e, &end, 16)) & 0x1FFFFFFF;
  const u32 hi = end && *end == '-' ? static_cast<u32>(std::strtoul(end + 1, nullptr, 16)) & 0x1FFFFFFF : lo + 64;
  return {lo, hi};
}
u32 g_watch_lo = WatchRange().first;
u32 g_watch_hi = WatchRange().second;

void WatchWrite(u32 addr, size_t size)
{
  auto& system = Core::System::GetInstance();
  const std::string* ev = system.GetCoreTiming().m_current_event;
  NOTICE_LOG_FMT(BRAWLBACK, "watch: frame {} {} write {:08x}+{:x} by {} pc {:08x} lr {:08x}",
                 s.current_frame, s.resim ? "rerun" : "first", addr, size,
                 !Core::IsCPUThread() ? std::string("thread") : ev ? *ev : std::string("cpu"),
                 system.GetPPCState().pc, system.GetPPCState().spr[SPR_LR]);
}
}  // namespace Machine
