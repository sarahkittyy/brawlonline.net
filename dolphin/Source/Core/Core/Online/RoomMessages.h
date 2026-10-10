// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The mm server's room messages (docs/rooms-protocol.md §2), parsed and validated before any of
// it reaches the game's memory (docs/rooms-game-interface.md). Everything the server sends is
// treated as hostile: sizes, ranges, codes and strings are checked here, as Rollback/PeerData.h
// does for the other players' packets. Pure functions, unit-tested (UnitTests/Core/Online).

#pragma once

#include <array>
#include <optional>
#include <string>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Online::Rooms
{
// Room codes: 4 letters of 20 (no vowels, no Y), docs/design/rooms.md #5.
constexpr size_t ROOM_CODE_LEN = 4;
constexpr char ROOM_CODE_ALPHABET[] = "BCDFGHJKLMNPQRSTVWXZ";
constexpr int ROOM_SLOTS = 4;
constexpr u8 NUM_TEAMS = 3;  // 0 red, 1 blue, 2 green
// What the game can show: names 15 UTF-16 units (ppom.h NAME_LEN 16 with the NUL), codes 8,
// the status line 63. Longer server strings are cut (in bytes here, generously; GameBridge cuts
// again in UTF-16 units).
constexpr size_t MAX_NAME_BYTES = 64;
constexpr size_t MAX_CODE_BYTES = 32;
constexpr size_t MAX_TEXT_BYTES = 200;

// A typed or received code: ASCII or full-width letters, any case -> "KFQB"; nullopt when it is
// not 4 letters of the 20 (the server's rule: such a code is "Room not found.").
std::optional<std::string> NormalizeRoomCode(const std::u16string& typed);
std::optional<std::string> NormalizeRoomCode(const std::string& ascii);

// A server string made safe to show and log: control characters dropped, invalid UTF-8 replaced,
// cut to `max_bytes` on a character boundary.
std::string CleanText(const std::string& s, size_t max_bytes);

enum class Mode : u8
{
  OneVsOne = 0,
  Ffa = 1,
  Teams = 2,
};
enum class Status : u8
{
  Waiting = 0,
  Starting = 1,
  InGame = 2,
};

struct Player
{
  std::string name;
  std::string code;
  bool ready = false;
  u8 team = 0;                    // 0-2
  std::optional<u8> character;    // only while ready
  std::optional<u8> costume;
  bool in_game = false;
};

struct Slot
{
  bool open = false;
  bool host = false;
  std::optional<Player> player;
};

// `room-state`, validated.
struct View
{
  std::string code;
  bool is_public = true;
  bool teams = false;
  Mode mode = Mode::OneVsOne;
  Status status = Status::Waiting;
  int you = 0;   // 1-4
  int host = 0;  // 1-4
  std::array<Slot, ROOM_SLOTS> slots{};  // by slot - 1 (= in-game port)
  std::string status_text;
};

// One message from mm on the online connection.
struct Message
{
  enum class Kind
  {
    Unknown,     // a type we do not know (ignored)
    Invalid,     // a known type that failed validation (ignored, logged)
    HelloResp,   // error empty: accepted
    RoomState,
    RoomError,
    RoomLeft,
    RoomStart,
    Error,       // fatal: the server disconnects
    CreateTicketResp,  // a ticket answer on the online connection (refused; ignored)
  };
  Kind kind = Kind::Unknown;
  std::string type;
  std::string why;         // Invalid: what was wrong
  std::string error;       // HelloResp / RoomError / Error
  std::string latest_version;  // HelloResp
  std::string op;          // RoomError
  std::string code;        // RoomLeft / RoomStart
  std::string reason;      // RoomLeft
  std::string match_id;    // RoomStart
  int timeout_secs = 15;   // RoomStart
  std::optional<View> view;  // RoomState
};

// Parses one JSON packet (the raw bytes as received). Never throws.
Message Parse(const std::string& packet);
// Parses room-state's object (for tests). nullopt with `why` when it is not valid.
std::optional<View> ParseView(const picojson::object& o, std::string* why);

// Requests (client -> server).
std::string HelloJson(const std::string& uid, const std::string& play_key,
                      const std::string& app_version);
enum class Op : u8
{
  Poll = 0,
  Create = 1,
  Join = 2,
  Leave = 3,
  Slot = 4,
  Teams = 5,
  Public = 6,
  Team = 7,
  // Dolphin's own (not game ops)
  Ready = 100,
  Back = 101,
};
struct Request
{
  Op op = Op::Poll;
  bool flag = false;   // Create: public; Slot: open; Teams: on; Public: public; Ready: ready
  int slot = 0;        // Slot: 1-4
  u8 team = 0;         // Team
  std::string code;    // Join (normalised)
  std::optional<u8> character, costume;  // Ready
};
// The JSON for `r`, or nullopt for Poll and anything out of range.
std::optional<std::string> RequestJson(const Request& r);
const char* OpName(Op op);

const char* ModeName(Mode m);
const char* StatusName(Status s);
}  // namespace Online::Rooms
