// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Rooms (docs/rooms-protocol.md, docs/rooms-game-interface.md), Dolphin's side:
// - the online connection to mm (ENet, `hello`), opened once user.json is read and the game is
//   logged in, kept while Dolphin runs (reconnected with backoff), closed on logout and exit. It
//   is what counts the player online, and carries the room's requests and pushed state;
// - the room as the game sees it (GameBridge writes it into the PPOM block): the server's
//   validated view, the status line, the room's game number and the next stage pickers;
// - a room's game: on `room-start`, the mode-3 ticket from the P2P port (Client::FindMatch) while
//   the online connection stays open, the gameplay session with ports = slots and the room's
//   host deciding, `room-back` once the match is over (or the start failed);
// - the launcher hand-off: <User>/Online/join-room.json in (a room to join, a new room to create,
//   or a Direct invite naming the other player's connect code), game-status.json out.
//
// One background thread owns the connection and the files. Every function is thread-safe.

#pragma once

#include <optional>
#include <string>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Online/RoomMessages.h"

namespace Online::Rooms
{
// LOCAL.room / RoomStatus.phase (ppom.h RoomPhase).
enum class Phase : u8
{
  None = 0,
  Joining = 1,
  In = 2,
};

// LOCAL.screen (ppom.h Screen).
enum class Screen : u8
{
  Unknown = 0,
  Menus = 1,
  OnlineCss = 2,
  Room = 3,
  OnlineBusy = 4,
  Match = 5,
  Offline = 6,
  Other = 7,
};

// Starts the thread (idempotent). GameBridge::Reset calls it at every boot.
void Start();
// A new boot (GameBridge::Reset): the game has not shown its menus yet, so a launcher request
// is held (accepted) until it does instead of being refused by the boot's scenes.
void OnBoot();
// Leaves politely, closes the connection, stops the thread, removes game-status.json.
void Shutdown();

// ---- From the game (GameBridge, CPU thread) ----
// A CMD_ROOM op (Create, Join, Leave, Slot, Teams, Public, Team). Poll does nothing.
void Submit(const Request& request);
// The room game's session plays a set (several games, docs/design/room-rules.md §5): it ends when
// GameSetup says the set is won, not after its first game.
void SetSetSession(bool set);
// A game of the room's set ended: sent to the server as room-result.
void ReportSetGame(u32 set_game, u8 winner_side, u16 stage);
// LOCAL.screen, every frame.
void SetScreen(u8 screen);
// The game's lock-in, every frame: ready for `game` with the character and costume.
void SetLocalLock(bool ready, u32 game, u8 char_kind, u8 costume);

// What GameBridge writes into the PPOM block.
struct Snapshot
{
  Phase phase = Phase::None;
  std::optional<View> view;   // in a room
  u32 game = 1;               // the room's next game number (lock in with it)
  u8 pickers = 0;             // ports (bits) that pick the next room game's stage
  u8 join_seq = 0;            // bumped on every accepted launcher join
  // The launcher's Direct invite (join-room.json kind "direct"): bumped on every accepted one,
  // with the other player's connect code (canonical, NormalizeConnectCode; "" before the first).
  u8 direct_seq = 0;
  std::string direct_code;
  // The launcher's create (join-room.json kind "create"): bumped when Dolphin accepted one and
  // sent room-create itself; the game goes to the room CSS as Create Room without sending one.
  u8 create_seq = 0;
  std::string text;           // the status line ("" = the game's own)
  bool error = false;
  u32 serial = 0;             // changes with text, error, phase or the view
  bool game_active = false;   // a room game runs (room-start .. room-back)
};
Snapshot GetSnapshot();
// The screen the game reports (LOCAL.screen, Screen), for the chat window: Match while a match runs.
Screen GetScreen();

// Harness (`rooms_status`, `rooms_request`).
picojson::object Status();
// Tests without the game plugin: the ops a game would send, plus `ready` (with character and
// costume) standing in for the lock-in, and `screen`.
std::optional<std::string> HarnessRequest(const picojson::object& args);
}  // namespace Online::Rooms
