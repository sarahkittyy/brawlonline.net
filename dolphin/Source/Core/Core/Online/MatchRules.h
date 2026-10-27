// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The rules of a match (docs/design/room-rules.md §3): get-ticket-resp `rules` (ours; the server's
// server/crates/common/src/rules.rs MatchRules), checked here before they reach the game's
// SESSION block (ppom.h Rules, PPOM v6), where OnlineMenu::applyRules puts them into the match.
// The server is treated as hostile, as for its room messages (RoomMessages.h): every value is
// range-checked, and a field this client does not know refuses the match instead of being
// ignored, so a match never runs under rules it was not meant to. Pure functions, unit-tested
// (UnitTests/Core/Online/MatchRulesTest.cpp).

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Online
{
struct MatchRules
{
  // Brawl's GameRule (gmSetRule::m_rule).
  enum Style : u8
  {
    STYLE_TIME = 0,
    STYLE_STOCK = 1,
    STYLE_COIN = 2,
  };
  u8 style = STYLE_STOCK;
  u8 stocks = 4;        // 1-99
  u8 time_minutes = 8;  // MORE RULES' time limit, every style: time/coin 1-99; stock 0-99 (0 none)
  u8 damage_ratio = 10;  // times 10: 5-20
  bool team_attack = true;
  // P+'s PAUSE: NORMAL, or HOLD (START held 50 frames); P+ has no pause that is off.
  bool pause_normal = true;
  // 0 none, 1 low, 2 medium, 3 high; P+'s 4 very high, 5 intense, 6 bomb rain (2026-10-27)
  u8 item_frequency = 0;
  std::array<u8, 8> item_switch{0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  bool hazards = false;  // stage hazards off by default (the user's, 2026-10-12)
  // The stages (srStageKind) whose hazards are the other way from `hazards`, at most
  // MAX_HAZARD_STAGES, each once (the game sets the played stage's). The server sends it only when
  // not empty.
  std::vector<u8> hazard_stages;
  bool auto_l_cancel = false;  // MORE RULES' AUTO L-CANCEL
  bool input_assist = false;   // MORE RULES' INPUT ASSIST (P+'s 3-frame buffer)
  // RULES' STOCK CONTROL: each player picks their own stock count on their panel (the lock-in's
  // port values carry it); stock matches only. The server sends it only when on.
  bool stock_control = false;
  // P+'s Special Versus box (docs/design/special-rules.md): the set rule's +0x18..+0x1F, each 0
  // (off), 1 or 2 (SPECIAL_ROWS's names). The server sends `special` only when a row is on.
  std::array<u8, 8> special{};
  // P+'s STAMINA TYPE 1-5 (set rule +7 = type - 1), only with special[0] == 2 (stamina).
  u8 stamina_type = 1;

  bool operator==(const MatchRules&) const = default;
};

constexpr u8 MAX_ITEM_FREQUENCY = 6;
constexpr u8 STAMINA_TYPES = 5;
// The Special Versus rows in the set rule's order, and each one's two values as the server names
// them (server/crates/common/src/rules.rs SpecialRules).
struct SpecialRow
{
  const char* key;
  const char* values[2];
};
constexpr SpecialRow SPECIAL_ROWS[8] = {
    {"stamina", {"300", "stamina"}}, {"size", {"mega", "mini"}},     {"attack", {"element", "regen"}},
    {"body", {"metal", "clear"}},    {"status", {"turbo", "bunny"}}, {"gravity", {"light", "heavy"}},
    {"speed", {"wild", "fast"}},     {"other", {"meter", "all-star"}},
};

constexpr size_t MAX_HAZARD_STAGES = 8;

struct ParsedRules
{
  // The rules, or nullopt when the response has none (an older server): the game then plays its
  // built-in set, P+'s competitive defaults.
  std::optional<MatchRules> rules;
  // Not empty: `rules` is present but cannot be used; the match is refused with this text (it
  // fits the CSS status line) and `detail` is logged.
  std::string error;
  std::string detail;
};

// get-ticket-resp's `rules`.
ParsedRules ParseRules(const picojson::object& get_ticket_resp);

// ppom.h Rules (SESSION +0x220 from PPOM v6): `valid` then the fields, 0x20 bytes.
constexpr u32 RULES_SIZE = 0x20;
constexpr u32 RU_VALID = 0x00, RU_STYLE = 0x01, RU_STOCKS = 0x02, RU_TIME = 0x03, RU_DAMAGE = 0x04,
              RU_TEAM_ATTACK = 0x05, RU_PAUSE = 0x06, RU_ITEM_FREQUENCY = 0x07,
              RU_ITEM_SWITCH = 0x08, RU_HAZARDS = 0x10, RU_AUTO_L_CANCEL = 0x11,
              RU_INPUT_ASSIST = 0x12, RU_STOCK_CONTROL = 0x13,
              // Session.restore's time limit (+0x14, u32) is Dolphin's own, not a rule.
              RU_HAZARD_STAGES = 0x18;  // MAX_HAZARD_STAGES stage ids, 0 after the last
std::array<u8, RULES_SIZE> SessionBytes(const std::optional<MatchRules>& rules);
// v9 (docs/design/special-rules.md): SESSION's Special block starts with the 8 rows and the stamina
// type (all 0 without rules).
constexpr u32 SPECIAL_RULES_SIZE = 9;
std::array<u8, SPECIAL_RULES_SIZE> SpecialBytes(const std::optional<MatchRules>& rules);
// The other way (the game's edited rules, ROOM_RULES): nullopt unless `valid` is 1 and every
// value is in range, as ParseRules checks the server's. `special` (a v9 game's request): the 8
// rows and the stamina type; without it the rules have none.
std::optional<MatchRules> FromSessionBytes(const std::array<u8, RULES_SIZE>& b,
                                           const std::array<u8, SPECIAL_RULES_SIZE>* special = nullptr);

// For logs, the harness and the client status (the server's field names).
picojson::object ToJson(const MatchRules& rules);
}  // namespace Online
