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
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Online/MatchRules.h"

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
// The most stocks a pick on the STOCKS box (stock control) can say (server rules.rs
// MAX_PICKED_STOCKS); 0 is no pick and is not sent.
constexpr u8 MAX_PICKED_STOCKS = 99;
// The most HP a pick on the STAMINA box can say (a stamina match; server rules.rs MAX_PICKED_HP).
constexpr u16 MAX_PICKED_HP = 999;
// MATCH SETUP's set format (server rules.rs MAX_BANS; a strike pattern of more turns is not
// shown, the page shows the ruleset's as it is).
constexpr u8 MAX_BANS = 3;
constexpr size_t MAX_STRIKE_TURNS = 16;

// A typed or received code: ASCII or full-width letters, any case -> "KFQB"; nullopt when it is
// not 4 letters of the 20 (the server's rule: such a code is "Room not found.").
std::optional<std::string> NormalizeRoomCode(const std::u16string& typed);
std::optional<std::string> NormalizeRoomCode(const std::string& ascii);
// A ruleset's code (docs/design/room-rules.md §2): 6 letters of the same 20, any case, ASCII or
// full width -> "BCDFGH"; nullopt otherwise (the server's common::rules::parse_ruleset_code).
constexpr size_t RULESET_CODE_LEN = 6;
std::optional<std::string> NormalizeRulesetCode(const std::u16string& typed);
std::optional<std::string> NormalizeRulesetCode(const std::string& ascii);

// A connect code from outside (the launcher's Direct invite, join-room.json): the server's rule
// (server/crates/common/src/codes.rs parse_connect_code): 2-4 ASCII letters, '#', 1-4 digits with a
// value of 1-9999, at most 8 characters as written back. Any case and leading zeros are taken;
// the result is canonical ("abc#007" -> "ABC#7"). nullopt for anything else (spaces, full-width
// or non-ASCII characters, a second '#').
constexpr size_t MAX_CONNECT_CODE_LEN = 8;
std::optional<std::string> NormalizeConnectCode(const std::string& ascii);

// "2026-10-09T15:00:00.000Z" (the launcher's toISOString, join-room.json's createdAt) -> ms since
// the epoch; nullopt for anything else. Read digit by digit, never with sscanf's %lf or strtod:
// those follow the C locale, which Dolphin sets to the system's, and with a decimal comma
// (es-UY, de-DE, pt-BR, ...) every request read as unparseable, so too old, and was ignored.
std::optional<s64> ParseIsoUtc(const std::string& t);

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
  // The coin placed on the CSS (ready or not: the others see a pick before the ready), or the
  // lock-in; none while the coin is in the hand, or hidden (a set's blind game 1).
  std::optional<u8> character;
  std::optional<u8> costume;
  // With the rules' stock control: the stock count the player picked on their panel, shown as the
  // lock-in is (1-99; none: not picked, the rules' stocks).
  std::optional<u8> stocks;
  // In a stamina match (the rules' special stamina): the HP the player picked on their panel,
  // shown as `stocks` is (1-MAX_PICKED_HP; none: P+'s default; docs/design/special-rules.md).
  std::optional<u16> hp;
  bool in_game = false;
  // The panel's SIT OUT: the room's games start without this player (by choice, or outside a set
  // that is on).
  bool sitting_out = false;
  // Watching the room's game from the room CSS (room-watch `watching`, 2026-10-19): the next game
  // does not wait for them.
  bool watching = false;
};

struct Slot
{
  bool open = false;
  bool host = false;
  std::optional<Player> player;
};

// room-state's `ruleset`: what the room plays (docs/design/room-rules.md).
struct RulesetView
{
  std::string code;      // a published ruleset's code, "" none
  std::string name;      // "P+ default", "Custom", or the published name
  u32 version = 0;
  std::string based_on;  // custom rules made from a published one ("NorCal Weekly v3")
  u8 best_of = 0;
  // How the stage is chosen without a set: "loser-picks" (any stage; also with a set: its steps
  // decide), "loser-picks-list" (the loser picks from the stages, on their normal layout),
  // "strike" (struck every game, GameSetup), "random" (a random stage every game), "anyone-picks"
  // (every player picks at once, game 1 too: the first pick the host sees plays).
  std::string stage_choice = "loser-picks";
  // The ruleset's stages (srStageKind): struck from, and picked from with the counterpicks. Empty
  // from an mm that sends none.
  std::vector<u16> starters;
  std::vector<u16> counterpicks;
  // The rules menu's MATCH SETUP page (2026-10-27, docs/design/special-rules.md): the strike
  // pattern (at most MAX_STRIKE_TURNS turns shown), the winner's bans and DSR (a set), alternate
  // stages (absent from an older mm: none).
  std::vector<u8> strikes;
  u8 bans = 0;
  bool dsr = false;
  bool alternate_stages = false;
  // Only "loser picks" without a set leaves a stage to a player (the last game's loser).
  bool LoserPicks() const
  {
    return best_of == 0 && (stage_choice == "loser-picks" || stage_choice == "loser-picks-list");
  }
  // ... held to the stages.
  bool PickFromList() const { return best_of == 0 && stage_choice == "loser-picks-list"; }
  // Every player of a game picks its stage (no set).
  bool AnyonePicks() const { return best_of == 0 && stage_choice == "anyone-picks"; }
};
// room-state's `set`: the room's set (sides: side 0 the lower slot).
struct SetView
{
  u8 best_of = 0;
  u32 game = 1;
  std::array<u8, 2> score{};
  std::array<int, 2> sides{};  // each side's first (lowest) slot, 1-4
  std::array<u8, 2> side_slots{};  // each side's slots as bits (bit s: slot s + 1)
  bool paused = false;
  std::string waiting_for;
  bool can_resume = false;  // the results differed and everyone is here: the host may replay
  bool finished = false;
  u8 winner = 0xFF;  // the side that won it, once finished
};

// `room-state`, validated.
struct View
{
  std::string code;
  bool is_public = true;
  bool teams = false;
  // With Teams on: P+'s plain team battle, every player in their team's costume (the user's,
  // 2026-10-21); false is its rainbow team battle, any costume and a team glow on each fighter.
  bool team_colors = false;
  Mode mode = Mode::OneVsOne;
  Status status = Status::Waiting;
  int you = 0;   // 1-4
  int host = 0;  // 1-4
  std::array<Slot, ROOM_SLOTS> slots{};  // by slot - 1 (= in-game port)
  std::string status_text;
  // The room's match rules (docs/design/room-rules.md), checked as the ticket's are; nullopt when
  // the server sent none or ones this client cannot use (`rules_error`: the text to show).
  std::optional<MatchRules> rules;
  std::string rules_error;
  RulesetView ruleset;
  std::optional<SetView> set;
  // The ports (bits 0-3) that pick the next game's stage, as the server decided them (the last
  // game's loser if they play, else a player drawn for this game); nullopt from an older mm (or
  // none to pick): this machine's own.
  std::optional<u8> pickers;
  // The server knows SIT OUT and shows a placed coin (players carry `sittingOut`): an older mm
  // ignores a pick that is not a ready, so none is sent to it.
  bool sit_out_known = false;
};

// One ruleset of `ruleset-list` (the game's Ruleset line).
struct RulesetEntry
{
  std::string code;  // 6 letters
  std::string name;
  u32 version = 0;
  std::string by;
  u8 best_of = 0;
};
constexpr size_t RULESET_LIST_MAX = 15;

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
    RulesetList,       // the player's rulesets to offer (room-rulesets)
    RoomUnreachable,   // pairs of the room's game that could not connect to each other
  };
  Kind kind = Kind::Unknown;
  std::string type;
  std::string why;         // Invalid: what was wrong
  std::string error;       // HelloResp / RoomError / Error
  std::string latest_version;  // HelloResp
  bool game_alive = false;     // HelloResp: this mm takes `game-alive` (Online/GameAlive.h)
  std::string op;          // RoomError
  std::string code;        // RoomLeft / RoomStart
  std::string reason;      // RoomLeft
  std::string match_id;    // RoomStart / RoomUnreachable
  int timeout_secs = 15;   // RoomStart
  // RoomStart: the slots (1-4) that play the game (empty from an older mm: every member), its host
  // (the session's decider, 0: the room's) and its stage pickers (bits; nullopt: this machine's).
  std::vector<int> start_players;
  int start_host = 0;
  std::optional<u8> start_pickers;
  std::optional<View> view;  // RoomState
  std::vector<RulesetEntry> rulesets;  // RulesetList
  // RoomUnreachable: every pair of slots (1-4, lower first) of the game `match_id` that could not
  // connect to each other, as reported so far.
  std::vector<std::array<u8, 2>> pairs;
};

// Parses one JSON packet (the raw bytes as received). Never throws.
Message Parse(const std::string& packet);
// Parses room-state's object (for tests). nullopt with `why` when it is not valid.
std::optional<View> ParseView(const picojson::object& o, std::string* why);

// Requests (client -> server).
// `chat_key`: the chat identity's public key (docs/chat-protocol.md §1), left out when empty.
std::string HelloJson(const std::string& uid, const std::string& play_key,
                      const std::string& app_version, const std::string& platform,
                      const std::string& chat_key = {});
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
  Rules = 8,  // the host's rules (ppom.h RoomRulesRequest)
  SetAction = 9,  // the host resumes (flag) or restarts the room's set
  Ruleset = 10,   // the host plays a published ruleset (`ruleset`) or P+'s default (empty)
  RulesetList = 11,  // the player's rulesets to offer (room-rulesets)
  BestOf = 12,       // the host's set length (`best_of` 0 / 3 / 5 / 7; the game's ROOM_BEST_OF)
  Stages = 13,       // v7, the host's STAGE SWITCH (`starters`, `counterpicks`, `stage_choice`)
  SitOut = 14,       // v8, SIT OUT: `slot` 1-4 sits out (`flag`) or plays (a player themselves,
                     // the host anyone)
  Format = 15,       // v9, the MATCH SETUP page: `stage_choice`, `strikes`, `bans`, `dsr`,
                     // `alternate_stages` (room-format)
  // Dolphin's own (not game ops)
  Ready = 100,
  Back = 101,
  Result = 102,  // a game of the room's set (room-result)
  Step = 103,    // the set's game being set up (room-step)
  Unreachable = 104,  // the room's game could not connect to these players (room-unreachable)
  Watch = 105,        // the room's games this player is not in (room-watch: `want`, `watching`)
};
// room-result winnerSide: a draw, and a game that was quit (it did not happen; the server plays it
// again from its first step once every player of the set reported it, room_set.rs).
constexpr u8 SIDE_DRAW = 0xFE;
constexpr u8 SIDE_VOID = 0xFD;

struct Request
{
  Op op = Op::Poll;
  bool flag = false;   // Create: public; Slot: open; Teams: on; Public: public; Ready: ready;
                       // SitOut: sits out
  int slot = 0;        // Slot, SitOut: 1-4
  u8 team = 0;         // Team
  bool colors = false;  // Teams: the team colours (with `flag`), else the rainbow team battle
  std::string code;    // Join (normalised)
  std::string ruleset;  // Create (the launcher's box), Ruleset: a ruleset code, "" none
  bool remembered = false;  // Create: the game's own Create Room (the account's remembered ruleset)
  std::optional<u8> character, costume;  // Ready
  std::optional<u8> stocks;              // Ready: the picked stock count (stock control), 1-99
  std::optional<u16> hp;                 // Ready: the picked HP (a stamina match), 1-MAX_PICKED_HP
  std::optional<u8> pickers;             // Back: the next game's stage pickers as this game saw it
  std::optional<MatchRules> rules;        // Rules
  u32 game = 0;                           // Result: the set's game
  u8 winner_side = 0;                     // Result: 0 / 1, SIDE_DRAW, SIDE_VOID
  u16 stage = 0;                          // Result
  u8 best_of = 0;                         // BestOf: 0 (no set), 3, 5, 7
  std::vector<u16> starters, counterpicks;  // Stages: stage ids 1-255
  std::string stage_choice;                 // Stages: "loser-picks" / "loser-picks-list", "" as it is;
                                            // Format: any of the five
  std::vector<u8> strikes;                  // Format: the strike pattern (turns 1-255)
  u8 bans = 0;                              // Format: 0-MAX_BANS
  bool dsr = false;                         // Format
  bool alternate_stages = false;            // Format
  std::array<std::vector<u8>, 2> actions;  // Step: each side's stage actions (`game`; `flag`:
                                           // its match started)
  std::string match_id;     // Unreachable: the room's game
  std::vector<int> ports;   // Unreachable: the players' slots (1-4)
  std::optional<bool> want, watching;  // Watch (at least one)
};
// The JSON for `r`, or nullopt for Poll and anything out of range.
std::optional<std::string> RequestJson(const Request& r);
const char* OpName(Op op);

const char* ModeName(Mode m);
const char* StatusName(Status s);
}  // namespace Online::Rooms
