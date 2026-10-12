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
#include <optional>
#include <string>
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
constexpr int MAX_PLAYERS = 4;

// One player's input for one frame (docs/design/brawl-mods.md §6.2).
//   0      flags (INPUT_*)
//   1-8    GameCube pad: buttons (2, big endian), stick X, stick Y, C-stick X, C-stick Y, L, R
//   9      Wii Remote state length (0-31)
//   10-40  WiimoteEmu::SerializedWiimoteState::data
//   41     a membership request (INPUT_REQUEST_*): the frame it first appears in starts an epoch
//          EPOCH_LEAD frames later (docs/design/brawl-mods.md §6.3)
//   42-47  reserved, 0
constexpr int INPUT_SIZE = 48;
constexpr u8 INPUT_GC_PRESENT = 1 << 0;
constexpr u8 INPUT_WIIMOTE_CONNECTED = 1 << 1;
constexpr int INPUT_REQUEST = 41;
constexpr u8 INPUT_REQUEST_ADMIT = 0x80;     // admit the player in slot (bits 0-1)
constexpr u8 INPUT_REQUEST_WIIMOTE = 1 << 2;  // ... who plays a Wii Remote
using Input = std::array<u8, INPUT_SIZE>;

GCPadStatus DecodePad(const Input& input);
void EncodePad(const GCPadStatus& pad, Input* input);

enum class Boundary
{
  BrawlLoop,  // the Brawl game loop's HLE hooks
};

// What a player plays with. A player in slot N drives GameCube port N, or Wii Remote N (with
// whatever extension their own Wii Remote config has: it travels in their input). The other
// device of that slot is not plugged in.
enum class Controller
{
  GameCube,
  WiiRemote,
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
  int delay = 2;               // this player's input delay in frames (0-9)
  Boundary boundary = Boundary::BrawlLoop;
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
  std::string name;
  // Join a running session: boot, connect to every peer (the session's players), ask to join,
  // and continue from the machine image the session sends at its next epoch.
  bool join = false;
};

// Parses options from JSON ({"slot":0,"port":5000,"delay":2,"peers":[{"slot":1,"host":"..",
// "port":5001}],"rtc":..,"synctest":0}); empty on error, with the reason in *error.
std::optional<Options> ParseOptions(const picojson::value& json, std::string* error);

// Before boot: the next boot runs this session. Also armed from the environment variable
// PPR_MACHINE_SESSION (JSON, as ParseOptions) at the first call of IsArmed.
bool Arm(const Options& options, std::string* error);
// Ends the session (any thread); the game keeps running locally.
void Stop();
// A session is armed or running: the whole-machine rollback rules apply (pinned configuration,
// synchronous disc reads, controllers from the session).
bool IsArmed();
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
// The emulated wall clock at boot (EXI IPL), seconds since 1970.
u64 GetInitialRTC();

// CPU thread, from the Brawl game-loop hooks. Each returns true when it handled the hook (the
// session is armed); then the caller does nothing else.
bool OnLoopTop(const Core::CPUThreadGuard& guard);
bool OnFrameEnd(const Core::CPUThreadGuard& guard);
bool OnLoopEnd(const Core::CPUThreadGuard& guard);
// This pass re-runs a frame that was already shown (not presented, not throttled).
bool IsResimulationPass();

// CPU thread: the GameCube controller in `port` for the frame running now. False when the session
// is not armed (the caller reads the local controller as usual).
bool GetPad(int port, GCPadStatus* status);
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

picojson::value Status();
// Confirmed frames from `since`: frame -> [input bytes of each player (hex), checksum].
picojson::value History(s64 since);
// With PPR_MACHINE_DIGESTS=1: the granule digest of a recent frame's save (RollbackManager::
// LastSaveDigest), to diff two machines whose checksums differ.
// With PPR_MACHINE_DIGESTS=2 the bytes too: those of the granules in `addrs` (hex).
picojson::value Digest(s64 frame, const std::vector<u32>& addrs = {});
}  // namespace Machine
