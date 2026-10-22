// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Whole-machine rollback session ("Brawl Mods", docs/design/brawl-mods.md).
//
// Every player runs the same emulated machine from boot. Nothing in the game is known: inputs
// enter where the hardware takes them (the SI's GameCube controllers, the emulated Bluetooth
// stack's Wii Remotes), snapshots cover the whole machine (RollbackManager without region mode:
// RAM, the locked cache and every device's state), and the menus are shared as on one console.
//
// The session is armed before boot (Arm), so the configuration that decides what the game
// computes is pinned from the first instruction (MakeNetSettings, applied by BootManager as
// Dolphin's netplay does). The first frame boundary waits for every peer; from then on GekkoNet
// decides which frames run with which inputs.
//
// Frame boundaries: for Brawl builds, the game loop's HLE hooks (HLE_Misc.cpp, the same as the
// Brawlback netplay path: every Brawl build runs this main.dol). Other games: later, the start of
// each VI field.
//
// Threads: Arm/Stop/Status from any thread; OnLoopTop/OnFrameEnd/OnLoopEnd and GetPad on the CPU
// thread; the network on its own thread.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace NetPlay
{
struct NetSettings;
}

namespace WiimoteEmu
{
struct DesiredWiimoteState;
}

namespace Machine
{
namespace SDCard
{
class SessionCard;
}

constexpr int MAX_PLAYERS = 4;
// Options::delay below 0: picked from the round trip each time a GekkoNet session starts, by a
// profile of the launcher's (Gprb::Session::ProfileInputDelay). In an Other Games room each
// player's own profile applies to their own inputs, as a number of frames does.
constexpr int AUTO_DELAY = -1;  // Normal
constexpr int AUTO_DELAY_AGGRESSIVE = -2;
constexpr int AUTO_DELAY_SMOOTH = -3;
// The profile of an Options::delay below 0: -1 Aggressive, 0 Normal, 1 Smooth.
constexpr int AutoDelayLevel(int delay)
{
  return delay == AUTO_DELAY_AGGRESSIVE ? -1 : delay == AUTO_DELAY_SMOOTH ? 1 : 0;
}
// The player's setting ([Online] InputDelay: 1-9 frames, 0 Normal, -1 Aggressive, -2 Smooth,
// Gprb::Session::DELAY_*) as an Options::delay.
constexpr int DelayFromSetting(int setting)
{
  return setting > 0   ? setting :
         setting == -1 ? AUTO_DELAY_AGGRESSIVE :
         setting == -2 ? AUTO_DELAY_SMOOTH :
                         AUTO_DELAY;
}

// One player's input for one frame (docs/design/brawl-mods.md §6.2).
//   0      flags (INPUT_*)
//   1-8    GameCube pad: buttons (2, big endian), stick X, stick Y, C-stick X, C-stick Y, L, R
//   9      Wii Remote state length (0-31)
//   10-40  WiimoteEmu::SerializedWiimoteState::data
//   41     a membership request (INPUT_REQUEST_*): the frame it first appears in starts an epoch
//          EPOCH_LEAD frames later (docs/design/brawl-mods.md §6.3)
//   42     a slot move (INPUT_MOVE_*): two slots swap players at an epoch, the same way
//   43-47  reserved, 0
constexpr int INPUT_SIZE = 48;
constexpr u8 INPUT_GC_PRESENT = 1 << 0;
constexpr u8 INPUT_WIIMOTE_CONNECTED = 1 << 1;
constexpr int INPUT_REQUEST = 41;
constexpr u8 INPUT_REQUEST_ADMIT = 0x80;     // admit the player in slot (bits 0-1)
constexpr u8 INPUT_REQUEST_WIIMOTE = 1 << 2;  // ... who plays a Wii Remote
constexpr int INPUT_MOVE = 42;
constexpr u8 INPUT_MOVE_SWAP = 0x80;  // swap slot (bits 0-1) and slot (bits 2-3)
using Input = std::array<u8, INPUT_SIZE>;

GCPadStatus DecodePad(const Input& input);
void EncodePad(const GCPadStatus& pad, Input* input);

enum class Boundary
{
  Auto,       // BrawlLoop for a Brawl build (IsBrawlGameId), else ViField
  BrawlLoop,  // the Brawl game loop's HLE hooks: one game frame per boundary
  ViField,    // the start of each VI field (60 a second), for any game
};

// What a player plays with. A player in slot N drives GameCube port N, or Wii Remote N (with
// whatever extension their own Wii Remote config has: it travels in their input). The other
// device of that slot is not plugged in.
enum class Controller
{
  GameCube,
  WiiRemote,
};

// A joiner whose SD card differs from the session's (docs/design/brawl-mods.md §6.5): download
// the session's card before the join, also writing it into the player's own card, or leave.
enum class SDSyncChoice
{
  Ask,  // the player answers (the launcher's dialog, SetSDSyncChoice)
  Yes,
  Overwrite,
  No,
};

struct Peer
{
  int slot = -1;     // the peer's in-game port (0-3)
  std::string host;  // its address
  u16 port = 0;
  Controller controller = Controller::GameCube;
  // Not in the session at its start: a player who joins later (connected to, admitted at an
  // epoch when it asks).
  bool late = false;
};

struct Options
{
  int local_slot = 0;          // this player's in-game port (0-3)
  std::vector<Peer> peers;     // every other player
  u16 local_port = 0;          // UDP port to bind (0: any)
  // This player's input delay in frames (0-9), or AUTO_DELAY (or another profile's): picked from
  // the round trip each time a GekkoNet session starts (the session's start and every epoch), as
  // P+ picks it.
  int delay = 2;
  Boundary boundary = Boundary::Auto;  // "boundary": "auto", "brawl" or "vi"
  // The session's wall clock at boot, in seconds since 1970-01-01 (as Dolphin's custom RTC and
  // netplay's initial time); every player must use the same. Must be after 2000-01-01, the
  // console's epoch: an earlier clock leaves the SDK's timeouts waiting forever (Brawl's PAD
  // library never finished resetting the controllers). Default 2026-01-01.
  u64 rtc = 1767225600;
  // Sync test (one player, no network): a GekkoNet stress session that loads the state from
  // `synctest` frames back on every frame and runs the frames again, comparing checksums.
  int synctest = 0;
  // The local controller (Dolphin's GameCube pad / Wii Remote config index) this player plays with.
  int local_pad = 0;
  Controller controller = Controller::GameCube;
  // Instead of `local_pad`: the first plugged-in controller of the kind, of the ones set up in
  // Dolphin's Controllers settings (SetLocalController), picked again as controllers come and go.
  bool auto_pad = false;
  std::string name;
  // Join a running session: boot, connect to every peer (the session's players), ask to join,
  // and continue from the machine image the session sends at its next epoch.
  bool join = false;
  // The players come from a Brawl Mods room (Online/ModsRoom.h): SetRoom gives the members and
  // their addresses as they change, and a joiner's slot. `peers` may be empty.
  bool room = false;
  // A joiner whose SD card differs from the session's: "sd_sync" ("ask", "yes", "overwrite",
  // "no"); the launcher asks its player.
  SDSyncChoice sd_sync = SDSyncChoice::Ask;
};

// A Brawl Mods room's member (Online/ModsRoom.h).
struct RoomMember
{
  int slot = -1;     // 0-3
  std::string code;  // connect code: who it is
  std::string name;
  std::string host;  // its public endpoint, as mm saw its probe ("" until it probed)
  u16 port = 0;
  std::string lan_host;  // the LAN endpoint its session reported
  u16 lan_port = 0;
};

// Parses options from JSON ({"slot":0,"port":5000,"delay":2,"peers":[{"slot":1,"host":"..",
// "port":5001}],"rtc":..,"synctest":0}); empty on error, with the reason in *error.
std::optional<Options> ParseOptions(const picojson::value& json, std::string* error);

// Before boot: the next boot runs this session. Also armed from the environment variable
// PPR_MACHINE_SESSION (JSON, as ParseOptions) at the first call of IsArmed, or for a Brawl Mods
// room from PPR_MODS_ROOM (Online/ModsRoom.h).
bool Arm(const Options& options, std::string* error);
// The environment (or the command line's room) changed before boot: the next IsArmed reads it again.
// Something may have asked IsArmed already (on macOS, before main read its options).
void RecheckEnvironment();

// A room session's members changed (any thread; Online::ModsRoom): `local_slot` is this player's
// slot in the room (a joiner takes it the first time). A new member is a joiner; a member in a
// slot whose player left the room is unplugged once its link drops (or at once when someone else
// has the slot now). A peer on this player's network (the same public address) is reached at its
// LAN address. A peer's hello is taken only from one of its addresses.
void SetRoom(int local_slot, const std::vector<RoomMember>& members);
// Sends mm the room's endpoint token from the session's UDP socket (with its LAN address), so mm
// records where peers reach this player; again every 2 s until mm answers (any thread).
void ProbeEndpoint(const std::string& mm_host, u16 mm_port, const std::string& token);
// "" (none), "probing", "done" or "failed: <why>".
std::string ProbeState();
// Ends the session (any thread); the game keeps running locally.
void Stop();
// A session is armed or running: the whole-machine rollback rules apply (pinned configuration,
// synchronous disc reads, controllers from the session).
bool IsArmed();
// The session was armed to join a running one (its machine comes from another player's image).
bool JoinsRunningSession();
// The session is past its start barrier (GekkoNet runs).
bool IsRunning();

// A machine image is being saved or loaded (both sides): Dolphin's savestate code leaves out the
// host-only video state and refuses a change of device type (docs/brawl-mods-security.md).
bool InImageDoState();

// Testing the image loader (the harness fuzzes it): this machine's state as a machine image
// carries it, and loading one with the image's rules. The caller holds the CPU (CPUThreadGuard).
std::vector<u8> SaveImageStateForTest(Core::System& system);
bool LoadImageStateForTest(Core::System& system, std::vector<u8> state);

// The configuration every player runs with (ConfigLoaders::GenerateNetPlayConfigLoader).
NetPlay::NetSettings MakeNetSettings();
// Turns on "Connect Wii Remotes for Emulated Controllers" (the current-run layer) when one of the
// player's Wii Remotes is set up as Real (BootManager at boot, the Wii Remote settings when the
// player sets one up mid-run); once on, it stays on for the run.
void ConnectRealWiimotesIfSetUp();
// The SD card this boot plays on (Core, before the hardware starts; the card in the SD
// configuration, `image_path`). Everyone plays the session's card: the player who starts the
// session plays its own (its manifest goes to joiners with the machine image), and a joiner,
// which boots on its own card, plays the session's from its image on (its own chunks where they
// match, the others fetched from the players). Writes never reach anyone's image.
std::shared_ptr<SDCard::SessionCard> CreateSDCard(const std::string& image_path);
// The emulated wall clock at boot (EXI IPL), seconds since 1970.
u64 GetInitialRTC();
// The session stopped or failed while the game runs on: it keeps the session's clock for as long
// as Core::WantsDeterminism holds (a joiner that declined the host's SD card read the PC's clock
// instead and failed its assert, bug #30).
bool KeepsSessionClock();

// CPU thread, from the Brawl game-loop hooks. Each returns true when it handled the hook (the
// session is armed); then the caller does nothing else.
bool OnLoopTop(const Core::CPUThreadGuard& guard);
bool OnFrameEnd(const Core::CPUThreadGuard& guard);
bool OnLoopEnd(const Core::CPUThreadGuard& guard);

// The VI-field boundary (any game but Brawl): a field started (CPU thread, from the VI's event).
// CoreTiming::Advance then calls OnViBoundary once its events and external exceptions are done,
// between two JIT blocks, where a snapshot can be taken and loaded (Nyx's Dolphin-Wide-Rollback
// does the same). OnViBoundary may leave g_vi_boundary set: it gave the CPU back (a pause or stop
// while it waited for the players), and the CPU cores leave right after Advance so that the next
// Advance takes the same boundary again, at the same instruction.
extern bool g_vi_boundary;
// Brawl's loop-top hook gave the CPU back (a pause or stop while it waited for the players): every
// CPU core leaves for its CPU loop right after the hook, before counting the hook block's cycles,
// and comes back to the hook (npc) at the same emulated time. Returns and clears the request.
bool TakeHookYield();
void OnViField();
void OnViBoundary(Core::System& system);
// Brawl's game-loop hooks (HLE) are for Brawl only: a disc whose ID starts with RSB, or a
// launcher (.dol/.elf, an "ID-" game ID) of a Brawl build. Any other game's code sits there.
bool IsBrawlGameId(std::string_view game_id);

// This pass re-runs a frame that was already shown (not presented, not throttled).
bool IsResimulationPass();

// CPU thread: the GameCube controller in `port` for the frame running now. False when the session
// is not armed (the caller reads the local controller as usual).
bool GetPad(int port, GCPadStatus* status);
// CPU thread, while armed: the local GameCube controller that gets in-game port `port`'s rumble
// (the SI's motor commands), -1 for none. Only this player's own port rumbles, on the controller
// they play with (`adapter`: a GameCube controller adapter's port); every other port is another
// player's.
int LocalRumblePad(int port, bool* adapter);
// CPU thread, from the emulated Bluetooth stack's Wii Remote update: replaces `state` with Wii
// Remote `index`'s state for the frame running now. False when the session is not armed or no
// player plays that Wii Remote.
bool GetWiimote(int index, WiimoteEmu::DesiredWiimoteState* state);

// Harness: replaces what the local Wii Remote reads (core buttons; a Nunchuk's stick and C/Z).
struct WiimoteOverride
{
  u16 buttons = 0;  // WiimoteCommon::ButtonData::hex
  bool nunchuk = false;
  u8 stick_x = 0x80, stick_y = 0x80;
  u8 nunchuk_buttons = 0;  // Nunchuk::BUTTON_C | BUTTON_Z
};
void SetWiimoteOverride(std::optional<WiimoteOverride> value);

// The launcher's controller menu (a room's "controller" request), mid-game too (any thread): from
// the next frame this player plays that kind of controller. The other machines see its GameCube
// port unplugged and its Wii Remote connect, or the reverse, as when a controller is plugged in.
void SetLocalController(Controller controller);
// A joiner's player answered the SD card's dialog (any thread).
void SetSDSyncChoice(SDSyncChoice choice);
// The player's input delay setting changed (any thread; the launcher's input-delay.json,
// Online::Rooms): a profile (AUTO_DELAY...) or 0-9 frames, as Options::delay. The running GekkoNet
// session takes it from the next frame, and so does every later one (each epoch).
void SetInputDelay(int delay);
// This player's input delay in the running GekkoNet session (any thread), -1 when none runs.
int CurrentInputDelay();
// "ask", "yes", "overwrite" or "no"; nullopt for anything else.
std::optional<SDSyncChoice> ParseSDSyncChoice(std::string_view text);
// The room's host moves a player (any thread): slots `from` and `to` (0-3) swap players at an
// epoch (either may be empty), the same frame on every machine; the game sees that player's
// controllers unplugged from one port and plugged into the other. An error when it cannot now (no
// running session, a change of players already planned, a player joining in either slot).
std::optional<std::string> RequestMove(int from, int to);
// A move this player asked for took effect (once): the slots, for the room (mm).
std::optional<std::pair<int, int>> TakeMoveDone();
// What this player plays with now (any thread).
struct LocalControllerInfo
{
  Controller controller = Controller::GameCube;
  int index = 0;         // the GameCube port or Wii Remote (0-3) it is set up as in Dolphin
  std::string device;    // Dolphin's device ("" when none of the kind is set up)
  bool plugged = false;
  bool adapter = false;  // a GameCube controller adapter's port
  bool real = false;     // a real Wii Remote
};
LocalControllerInfo GetLocalControllerInfo();

picojson::value Status();
// Confirmed frames from `since`: frame -> [input bytes of each player (hex), checksum].
picojson::value History(s64 since);
// With PPR_MACHINE_DIGESTS=1: the granule digest of a recent frame's save (RollbackManager::
// LastSaveDigest), to diff two machines whose checksums differ.
// With PPR_MACHINE_DIGESTS=2 the bytes too: those of the granules in `addrs` (hex).
picojson::value Digest(s64 frame, const std::vector<u32>& addrs = {});

// Diagnostics, PPR_MACHINE_IOLOG=N (with PPR_MACHINE_DUMP_DIR): the first N frames after an epoch,
// every MMIO read the MMU makes (all of them with an interpreter core; a JIT inlines some) and
// every host write into guest memory (MarkRangeDirty: DMA, IOS replies, DVD reads), to
// iolog-p<slot>.txt. Two machines' logs show the first outside input that differs. With an
// interpreter core, CPU stores go through MarkRangeDirty too, and every event carries CRCs of the
// FPRs and GPRs: the first event after which the registers differ brackets the instruction.
// PPR_MACHINE_IOLOG=-K: the last K frames instead, written at the first desync.
// PPR_MACHINE_IOLOG_LIGHT=1: only what comes from outside the CPU (no CPU stores, no registers),
// fast enough to run for minutes.
extern bool g_io_log;
// Diagnostics, PPR_MACHINE_WATCH=lo-hi (physical, hex): every host write into that range is
// logged with who made it and the frame (a sync test's first run or re-run).
extern u32 g_watch_lo, g_watch_hi;
void WatchWrite(u32 addr, size_t size);
extern bool g_io_cpu_store;  // MMU::Write is marking a CPU store dirty
void IoLogRead(u32 addr, u32 size, u64 value);
void IoLogWrite(u32 addr, size_t size);
// In the PPR_MACHINE_IOLOG_REGS window: each instruction an interpreter core ran, with the FPRs.
void IoLogStep(u32 pc, u32 inst);
// Each CoreTiming event (name, time, how late, the guest CPU where it fired): two machines that
// count cycles differently (two JITs) part at the first event that differs.
void IoLogEvent(const std::string& name, s64 time, s64 late);
// PPR_MACHINE_IOLOG_BLOCKS=1 (read when a block is compiled): Jit64 and JitArm64 call IoLogBlock at
// each block's entry, logging its pc with CRCs of the FPRs (both halves) and GPRs: the first block
// after which two JITs' registers differ is the one that computed differently.
bool IoLogBlocks();
void IoLogBlock(u32 pc);
}  // namespace Machine
