// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// get-ticket-resp's `rules` as Dolphin parses them (Core/Online/MatchRules.h): checked before they
// can reach the game's SESSION block.

#include <string>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Core/Online/MatchRules.h"

using namespace Online;

namespace
{
ParsedRules Parse(const std::string& rules_json)
{
  picojson::value v;
  const std::string err = picojson::parse(v, "{\"type\": \"get-ticket-resp\"" +
                                                 (rules_json.empty() ? "" : ", \"rules\": " + rules_json) +
                                                 "}");
  EXPECT_TRUE(err.empty()) << err;
  return ParseRules(v.get<picojson::object>());
}

// The server's wire shape (server/crates/common/src/rules.rs, test wire_shape).
const char* FULL = R"({"style": "time", "stocks": 3, "timeMinutes": 6, "damageRatio": 1.3,
  "teamAttack": false, "pause": "hold", "itemFrequency": 2, "itemSwitch": [1, 2, 3, 4, 5, 6, 7, 255],
  "hazards": false, "autoLCancel": true, "inputAssist": true})";
}  // namespace

TEST(MatchRules, NoRulesIsNotAnError)
{
  for (const std::string json : {"", "null"})
  {
    const auto p = Parse(json);
    EXPECT_FALSE(p.rules) << json;
    EXPECT_TRUE(p.error.empty()) << json;
  }
}

TEST(MatchRules, EveryField)
{
  const auto p = Parse(FULL);
  ASSERT_TRUE(p.rules) << p.detail;
  const MatchRules& r = *p.rules;
  EXPECT_EQ(r.style, MatchRules::STYLE_TIME);
  EXPECT_EQ(r.stocks, 3);
  EXPECT_EQ(r.time_minutes, 6);
  EXPECT_EQ(r.damage_ratio, 13);
  EXPECT_FALSE(r.team_attack);
  EXPECT_FALSE(r.pause_normal);
  EXPECT_EQ(r.item_frequency, 2);
  EXPECT_EQ(r.item_switch, (std::array<u8, 8>{1, 2, 3, 4, 5, 6, 7, 255}));
  EXPECT_FALSE(r.hazards);
  EXPECT_TRUE(r.auto_l_cancel);
  EXPECT_TRUE(r.input_assist);
  EXPECT_FALSE(r.stock_control);

  // Back to the server's shape and in again: the same rules.
  const std::string again = picojson::value(ToJson(r)).serialize();
  const auto q = Parse(again);
  ASSERT_TRUE(q.rules) << again;
  EXPECT_EQ(*q.rules, r);
}

TEST(MatchRules, MissingFieldsArePPlusDefaults)
{
  const auto p = Parse("{}");
  ASSERT_TRUE(p.rules);
  EXPECT_EQ(*p.rules, MatchRules{});
  const MatchRules d;
  EXPECT_EQ(d.style, MatchRules::STYLE_STOCK);
  EXPECT_EQ(d.stocks, 4);
  EXPECT_EQ(d.time_minutes, 8);
  EXPECT_EQ(d.damage_ratio, 10);
  EXPECT_TRUE(d.team_attack && d.pause_normal && !d.hazards);
  EXPECT_FALSE(d.auto_l_cancel || d.input_assist);
  EXPECT_EQ(d.item_frequency, 0);
  // Stock with no time limit.
  const auto none = Parse(R"({"timeMinutes": 0})");
  ASSERT_TRUE(none.rules);
  EXPECT_EQ(none.rules->time_minutes, 0);
}

TEST(MatchRules, BadValuesRefuseTheMatch)
{
  for (const std::string json : {
           R"([])",
           R"("stock")",
           R"({"style": "stamina"})",
           R"({"style": 1})",
           R"({"stocks": 0})",
           R"({"stocks": 100})",
           R"({"stocks": 2.5})",
           R"({"stocks": "4"})",
           R"({"style": "time", "timeMinutes": 0})",
           R"({"timeMinutes": 100})",
           R"({"timeMinutes": -1})",
           R"({"damageRatio": 0.4})",
           R"({"damageRatio": 2.1})",
           R"({"damageRatio": 1.05})",
           R"({"damageRatio": "1"})",
           R"({"itemFrequency": 7})",
           R"({"staminaType": 0})",
           R"({"staminaType": 6})",
           R"({"staminaType": 2})",
           R"({"special": []})",
           R"({"special": {"size": 1}})",
           R"({"special": {"stamina": "stamina"}, "staminaType": 6})",
           R"({"itemSwitch": [0, 6, 0, 0, 0, 0, 0]})",
           R"({"itemSwitch": [0, 6, 0, 0, 0, 0, 0, 256]})",
           R"({"itemSwitch": "0006000000000000"})",
           R"({"pause": 1})",
           R"({"pause": false})",
           R"({"pause": "off"})",
           R"({"inputAssist": 1})",
           R"({"autoLCancel": "true"})",
       })
  {
    const auto p = Parse(json);
    EXPECT_FALSE(p.rules) << json;
    EXPECT_EQ(p.error, "Invalid rules from the server.") << json;
    EXPECT_FALSE(p.detail.empty()) << json;
  }
}

TEST(MatchRules, StockControl)
{
  // Sent only when on (server rules.rs): missing is off.
  const auto p = Parse(R"({"stocks": 3, "stockControl": true})");
  ASSERT_TRUE(p.rules) << p.detail;
  EXPECT_TRUE(p.rules->stock_control);
  EXPECT_EQ(SessionBytes(p.rules)[RU_STOCK_CONTROL], 1);
  EXPECT_EQ(FromSessionBytes(SessionBytes(p.rules)), p.rules);
  EXPECT_EQ(ToJson(*p.rules).at("stockControl").get<bool>(), true);
  EXPECT_EQ(ToJson(MatchRules{}).count("stockControl"), 0u);
  EXPECT_FALSE(Parse(R"({"stockControl": 1})").rules);
}

TEST(MatchRules, HazardStages)
{
  // The stages whose hazards are the other way from `hazards` (Fountain of Dreams in Ranked,
  // 2026-10-26); sent only when there are some.
  const auto p = Parse(R"({"hazards": false, "hazardStages": [31, 1]})");
  ASSERT_TRUE(p.rules) << p.detail;
  EXPECT_EQ(p.rules->hazard_stages, (std::vector<u8>{31, 1}));
  const auto b = SessionBytes(p.rules);
  EXPECT_EQ(b[RU_HAZARD_STAGES], 31);
  EXPECT_EQ(b[RU_HAZARD_STAGES + 1], 1);
  EXPECT_EQ(b[RU_HAZARD_STAGES + 2], 0);
  EXPECT_EQ(FromSessionBytes(b), p.rules);
  EXPECT_EQ(ToJson(*p.rules).at("hazardStages").get<picojson::array>().size(), 2u);
  EXPECT_EQ(ToJson(MatchRules{}).count("hazardStages"), 0u);
  const auto eight = Parse(R"({"hazardStages": [1, 2, 3, 4, 5, 6, 7, 8]})");
  ASSERT_TRUE(eight.rules) << eight.detail;
  EXPECT_EQ(FromSessionBytes(SessionBytes(eight.rules)), eight.rules);
  for (const char* json : {R"({"hazardStages": [1, 2, 3, 4, 5, 6, 7, 8, 9]})",
                           R"({"hazardStages": [0]})", R"({"hazardStages": [256]})",
                           R"({"hazardStages": [31, 31]})", R"({"hazardStages": 31})",
                           R"({"hazardStages": ["31"]})"})
  {
    const auto bad = Parse(json);
    EXPECT_FALSE(bad.rules) << json;
    EXPECT_EQ(bad.error, "Invalid rules from the server.") << json;
  }
  // The game's edits: a stage twice is refused; ids stop at the first 0.
  auto twice = SessionBytes(MatchRules{});
  twice[RU_HAZARD_STAGES] = twice[RU_HAZARD_STAGES + 1] = 31;
  EXPECT_FALSE(FromSessionBytes(twice));
  auto gap = SessionBytes(MatchRules{});
  gap[RU_HAZARD_STAGES + 1] = 31;
  ASSERT_TRUE(FromSessionBytes(gap));
  EXPECT_TRUE(FromSessionBytes(gap)->hazard_stages.empty());
}

TEST(MatchRules, UnknownRuleRefusesTheMatch)
{
  // A rule this client cannot apply: playing without it would not be the match the server set up.
  const auto p = Parse(R"({"stocks": 3, "targetScore": 5})");
  EXPECT_FALSE(p.rules);
  EXPECT_EQ(p.error, "Update to play with these rules.");
  EXPECT_NE(p.detail.find("targetScore"), std::string::npos);
  // The text fits the CSS status line (38 characters, docs/game-code.md).
  EXPECT_LE(p.error.size(), 38u);
  EXPECT_LE(std::string("Invalid rules from the server.").size(), 38u);
}

TEST(MatchRules, SessionBytes)
{
  EXPECT_EQ(SessionBytes(std::nullopt), (std::array<u8, RULES_SIZE>{}));
  const auto p = Parse(FULL);
  ASSERT_TRUE(p.rules);
  const auto b = SessionBytes(p.rules);
  const std::array<u8, RULES_SIZE> want{1, 0, 3, 6, 13, 0, 0, 2, 1, 2, 3, 4, 5, 6, 7, 255, 0, 1, 1};
  EXPECT_EQ(b, want);
  // And back: the game's edited rules (ROOM_RULES) come in through the same layout.
  EXPECT_EQ(FromSessionBytes(b), p.rules);
  EXPECT_EQ(FromSessionBytes(SessionBytes(MatchRules{})), MatchRules{});
  for (const auto& [at, value] : std::initializer_list<std::pair<u32, u8>>{
           {RU_VALID, 0}, {RU_STYLE, 3}, {RU_STOCKS, 0}, {RU_STOCKS, 100}, {RU_TIME, 100},
           {RU_DAMAGE, 4}, {RU_DAMAGE, 21}, {RU_ITEM_FREQUENCY, 7}, {RU_PAUSE, 2},
           {RU_AUTO_L_CANCEL, 7}, {RU_INPUT_ASSIST, 2}, {RU_STOCK_CONTROL, 2}})
  {
    auto bad = SessionBytes(MatchRules{});
    bad[at] = value;
    EXPECT_FALSE(FromSessionBytes(bad)) << at << " " << int(value);
  }
  auto time0 = SessionBytes(MatchRules{});
  time0[RU_STYLE] = MatchRules::STYLE_TIME;
  time0[RU_TIME] = 0;
  EXPECT_FALSE(FromSessionBytes(time0));   // a time match needs a time limit
  const auto d = SessionBytes(MatchRules{});
  // (byte 0x10: hazards, off by default since 2026-10-12)
  const std::array<u8, RULES_SIZE> want_d{1, 1, 4, 8, 10, 1, 1, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(d, want_d);
}

// P+'s Special Versus box and STAMINA TYPE (docs/design/special-rules.md; server rules.rs
// special_rules_wire_shape), P+'s item frequencies past HIGH.
TEST(MatchRules, SpecialModes)
{
  const auto p = Parse(R"({"special": {"stamina": "stamina", "size": "mini", "attack": "element",
    "body": "metal", "status": "turbo", "gravity": "heavy", "speed": "wild", "other": "all-star"},
    "staminaType": 4, "itemFrequency": 6})");
  ASSERT_TRUE(p.rules) << p.detail;
  EXPECT_EQ(p.rules->special, (std::array<u8, 8>{2, 2, 1, 1, 1, 2, 1, 2}));
  EXPECT_EQ(p.rules->stamina_type, 4);
  EXPECT_EQ(p.rules->item_frequency, 6);
  EXPECT_EQ(SpecialBytes(p.rules), (std::array<u8, SPECIAL_RULES_SIZE>{2, 2, 1, 1, 1, 2, 1, 2, 4}));
  EXPECT_EQ(SpecialBytes(std::nullopt), (std::array<u8, SPECIAL_RULES_SIZE>{}));
  // Every value of every row, and "off".
  const auto all = Parse(R"({"special": {"stamina": "300", "size": "mega", "attack": "regen",
    "body": "clear", "status": "bunny", "gravity": "light", "speed": "fast", "other": "meter"}})");
  ASSERT_TRUE(all.rules) << all.detail;
  EXPECT_EQ(all.rules->special, (std::array<u8, 8>{1, 1, 2, 2, 2, 1, 2, 1}));
  const auto off = Parse(R"({"special": {"size": "off"}})");
  ASSERT_TRUE(off.rules) << off.detail;
  EXPECT_EQ(off.rules->special, (std::array<u8, 8>{}));
  // A row or value this build does not know is a newer server's: "Update to play".
  for (const std::string json : {R"({"special": {"hat": "bunny"}})", R"({"special": {"size": "huge"}})"})
  {
    const auto u = Parse(json);
    EXPECT_FALSE(u.rules) << json;
    EXPECT_EQ(u.error, "Update to play with these rules.") << json;
  }
  // Back to the server's JSON: only the rows that are on, the type only when not 1.
  const auto j = picojson::value(ToJson(*p.rules)).serialize();
  EXPECT_NE(j.find(R"("special":{"attack":"element","body":"metal","gravity":"heavy","other":"all-star",)"
                   R"("size":"mini","speed":"wild","stamina":"stamina","status":"turbo"})"),
            std::string::npos)
      << j;
  EXPECT_NE(j.find(R"("staminaType":4)"), std::string::npos) << j;
  const auto plain = picojson::value(ToJson(MatchRules{})).serialize();
  EXPECT_EQ(plain.find("special"), std::string::npos) << plain;
  EXPECT_EQ(plain.find("staminaType"), std::string::npos) << plain;
  // The game's ROOM_RULES: the rows follow the rules; the type means something with stamina only.
  const auto b = SessionBytes(p.rules);
  auto sp = SpecialBytes(p.rules);
  EXPECT_EQ(FromSessionBytes(b, &sp), p.rules);
  sp[0] = 1;
  EXPECT_EQ(FromSessionBytes(b, &sp)->stamina_type, 1);
  sp[0] = 3;
  EXPECT_FALSE(FromSessionBytes(b, &sp));
  sp = SpecialBytes(p.rules);
  sp[8] = 6;
  EXPECT_FALSE(FromSessionBytes(b, &sp));
  sp[8] = 0;
  EXPECT_EQ(FromSessionBytes(b, &sp)->stamina_type, 1);
  // Without the special bytes (an older game): none.
  EXPECT_EQ(FromSessionBytes(b)->special, (std::array<u8, 8>{}));
}
