// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The mm server's room messages as Dolphin parses them (Core/Online/RoomMessages.h): everything
// the server sends is checked before it can reach the game's memory.

#include <clocale>
#include <string>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Core/Online/RoomMessages.h"

using namespace Online::Rooms;

namespace
{
// docs/rooms-protocol.md §2, `room-state`.
const char* STATE = R"({
  "type": "room-state", "code": "KFQB", "public": true, "teams": false,
  "mode": "1v1", "status": "waiting", "you": 2, "host": 1,
  "slots": [
    {"slot": 1, "open": true, "host": true, "player": {"displayName": "alice", "connectCode": "ALIC#4", "ready": true, "team": 0, "character": 12, "costume": 3, "inGame": false}},
    {"slot": 2, "open": true, "host": false, "player": {"displayName": "bob", "connectCode": "BO#77", "ready": false, "team": 1, "character": 9, "costume": null, "inGame": false}},
    {"slot": 3, "open": false, "host": false, "player": null},
    {"slot": 4, "open": false, "host": false, "player": null}
  ],
  "statusText": "Waiting on: bob"
})";

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
  const auto at = s.find(from);
  EXPECT_NE(at, std::string::npos) << from;
  if (at != std::string::npos)
    s.replace(at, from.size(), to);
  return s;
}

Message::Kind KindOf(const std::string& json)
{
  return Parse(json).kind;
}
}  // namespace

TEST(RoomMessages, RoomCodes)
{
  EXPECT_EQ(NormalizeRoomCode(std::string("KFQB")), "KFQB");
  EXPECT_EQ(NormalizeRoomCode(std::string("kfqb")), "KFQB");
  EXPECT_EQ(NormalizeRoomCode(std::u16string(u"\uFF2B\uFF26\uFF31\uFF22")), "KFQB");  // full width
  EXPECT_EQ(NormalizeRoomCode(std::u16string(u"\uFF4B\uFF46qb")), "KFQB");
  EXPECT_EQ(NormalizeRoomCode(std::string("KFQA")), std::nullopt);   // a vowel
  EXPECT_EQ(NormalizeRoomCode(std::string("KFQY")), std::nullopt);   // Y is left out
  EXPECT_EQ(NormalizeRoomCode(std::string("KFQ")), std::nullopt);
  EXPECT_EQ(NormalizeRoomCode(std::string("KFQBB")), std::nullopt);
  EXPECT_EQ(NormalizeRoomCode(std::string("KF#B")), std::nullopt);
  EXPECT_EQ(NormalizeRoomCode(std::string("KF\xC3\x9F" "B")), std::nullopt);
  EXPECT_EQ(NormalizeRoomCode(std::string(1000, 'K')), std::nullopt);
  EXPECT_EQ(NormalizeRoomCode(std::u16string(u"KF\u0000QB")), std::nullopt);  // NUL ends it
}

TEST(RoomMessages, ConnectCodes)
{
  EXPECT_EQ(NormalizeConnectCode("SARA#123"), "SARA#123");
  EXPECT_EQ(NormalizeConnectCode("sara#123"), "SARA#123");
  EXPECT_EQ(NormalizeConnectCode("AB#1"), "AB#1");
  EXPECT_EQ(NormalizeConnectCode("ABC#9999"), "ABC#9999");
  EXPECT_EQ(NormalizeConnectCode("ABC#007"), "ABC#7");          // the server's canonical form
  EXPECT_EQ(NormalizeConnectCode("ABCD#0123"), "ABCD#123");
  EXPECT_EQ(NormalizeConnectCode("ABCD#1234"), std::nullopt);   // 9 characters
  EXPECT_EQ(NormalizeConnectCode("A#12"), std::nullopt);        // prefix 2-4 letters
  EXPECT_EQ(NormalizeConnectCode("ABCDE#1"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("#12"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("ABC#0"), std::nullopt);       // 1-9999
  EXPECT_EQ(NormalizeConnectCode("ABC#"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("ABC#12345"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("ABC"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("ABC#1#2"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("AB1#2"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode(" ABC#12"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("ABC#12 "), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("ABC#-1"), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode("\xEF\xBC\xA1" "BC#1"), std::nullopt);   // a full-width A
  EXPECT_EQ(NormalizeConnectCode(std::string("AB\0C#1", 6)), std::nullopt);
  EXPECT_EQ(NormalizeConnectCode(std::string(1000, 'A') + "#1"), std::nullopt);
}

static void ExpectIsoTimes()
{
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05.123Z"), 1791935705123);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05Z"), 1791935705000);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05.1Z"), 1791935705100);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05.123456Z"), 1791935705123);
  EXPECT_EQ(ParseIsoUtc("2000-02-29T00:00:00.000Z"), 951782400000);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05,123Z"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05.Z"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05.123"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05.123+00:00"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("2026-13-13T23:55:05.123Z"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T24:55:05.123Z"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("1999-10-13T23:55:05.123Z"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("2026-1-13T23:55:05.123Z"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("yesterday"), std::nullopt);
  EXPECT_EQ(ParseIsoUtc(""), std::nullopt);
  EXPECT_EQ(ParseIsoUtc("2026-10-13T23:55:05." + std::string(40, '1') + "Z"), std::nullopt);
}

TEST(RoomMessages, IsoTimes)
{
  ExpectIsoTimes();
}

// Player report 2026-10-13 (Windows, es-UY): Dolphin sets the C locale to the system's, and the
// old sscanf("%lf") stopped at the '.', so every launcher request was ignored as too old.
TEST(RoomMessages, IsoTimesUnderADecimalCommaLocale)
{
  const std::string before = std::setlocale(LC_ALL, nullptr);
  bool set = false;
  for (const char* name : {"es-UY", "de-DE", "es_UY.UTF-8", "de_DE.UTF-8", "de_DE", "fr_FR.UTF-8"})
  {
    if (std::setlocale(LC_ALL, name) && std::localeconv()->decimal_point[0] == ',')
    {
      set = true;
      break;
    }
  }
  if (!set)
  {
    std::setlocale(LC_ALL, before.c_str());
    GTEST_SKIP() << "no decimal-comma locale installed";
  }
  ExpectIsoTimes();
  std::setlocale(LC_ALL, before.c_str());
}

TEST(RoomMessages, CleanText)
{
  EXPECT_EQ(CleanText("Room not found.", 200), "Room not found.");
  EXPECT_EQ(CleanText("a\x01" "b\nc\x7F" "d", 200), "abcd");
  EXPECT_EQ(CleanText("\xC2\x85x", 200), "x");                // C1 control
  EXPECT_EQ(CleanText("\xFF\xFE", 200), "\xEF\xBF\xBD\xEF\xBF\xBD");  // invalid -> U+FFFD
  EXPECT_EQ(CleanText("\xC0\xAF", 200), "\xEF\xBF\xBD");  // an overlong "/"
  EXPECT_EQ(CleanText("\xED\xA0\x80", 200), "\xEF\xBF\xBD");  // a surrogate
  EXPECT_EQ(CleanText("abc\xC3\xA9", 4), "abc");              // never half a character
  EXPECT_EQ(CleanText(std::string(500, 'x'), 64).size(), 64u);
}

TEST(RoomMessages, ParsesTheProtocolsRoomState)
{
  const Message m = Parse(STATE);
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  const View& v = *m.view;
  EXPECT_EQ(v.code, "KFQB");
  EXPECT_TRUE(v.is_public);
  EXPECT_FALSE(v.teams);
  EXPECT_EQ(v.mode, Mode::OneVsOne);
  EXPECT_EQ(v.status, Status::Waiting);
  EXPECT_EQ(v.you, 2);
  EXPECT_EQ(v.host, 1);
  EXPECT_EQ(v.status_text, "Waiting on: bob");
  ASSERT_TRUE(v.slots[0].player);
  EXPECT_TRUE(v.slots[0].host);
  EXPECT_EQ(v.slots[0].player->name, "alice");
  EXPECT_EQ(v.slots[0].player->character, 12);
  EXPECT_EQ(v.slots[0].player->costume, 3);
  EXPECT_EQ(v.slots[0].player->stocks, std::nullopt);
  ASSERT_TRUE(v.slots[1].player);
  EXPECT_EQ(v.slots[1].player->team, 1);
  // Not ready: the coin placed on the CSS, shown before the ready (the user's, 2026-10-18).
  EXPECT_EQ(v.slots[1].player->character, 9);
  EXPECT_EQ(v.slots[1].player->costume, std::nullopt);
  EXPECT_FALSE(v.slots[1].player->sitting_out);
  // An older mm: no SIT OUT, no pickers of its own.
  EXPECT_FALSE(v.sit_out_known);
  EXPECT_EQ(v.pickers, std::nullopt);
  EXPECT_FALSE(v.slots[2].open);
  EXPECT_FALSE(v.slots[2].player);
  // No rules (an older server): none, and no error.
  EXPECT_FALSE(v.rules);
  EXPECT_TRUE(v.rules_error.empty());
}

TEST(RoomMessages, TeamColors)
{
  // The rainbow team battle when the server says nothing (before 2026-10-21), and teamColors only
  // with Teams on.
  const Message rainbow = Parse(Replace(STATE, R"("teams": false,)", R"("teams": true,)"));
  ASSERT_EQ(rainbow.kind, Message::Kind::RoomState) << rainbow.why;
  EXPECT_TRUE(rainbow.view->teams);
  EXPECT_FALSE(rainbow.view->team_colors);
  const Message colors = Parse(Replace(STATE, R"("teams": false,)", R"("teams": true, "teamColors": true,)"));
  ASSERT_EQ(colors.kind, Message::Kind::RoomState) << colors.why;
  EXPECT_TRUE(colors.view->team_colors);
  const Message off = Parse(Replace(STATE, R"("teams": false,)", R"("teams": false, "teamColors": true,)"));
  ASSERT_EQ(off.kind, Message::Kind::RoomState) << off.why;
  EXPECT_FALSE(off.view->team_colors);
  Request r;
  r.op = Op::Teams;
  r.flag = true;
  EXPECT_EQ(RequestJson(r), R"({"colors":false,"on":true,"type":"room-teams"})");
  r.colors = true;
  EXPECT_EQ(RequestJson(r), R"({"colors":true,"on":true,"type":"room-teams"})");
  r.flag = false;
  EXPECT_EQ(RequestJson(r), R"({"colors":false,"on":false,"type":"room-teams"})");
}

TEST(RoomMessages, PickedStocks)
{
  // Stock control: a ready player's picked stock count (1-99), missing or null when none.
  const auto with = [](const std::string& v) {
    return Parse(Replace(STATE, R"("costume": 3,)", R"("costume": 3, "stocks": )" + v + ","));
  };
  const Message m = with("5");
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  EXPECT_EQ(m.view->slots[0].player->stocks, 5);
  EXPECT_EQ(with("null").view->slots[0].player->stocks, std::nullopt);
  for (const char* bad : {"0", "100", "-1", "\"3\""})
    EXPECT_EQ(with(bad).kind, Message::Kind::Invalid) << bad;
  // Sent with a ready only.
  Request r;
  r.op = Op::Ready;
  r.flag = true;
  r.character = 12;
  r.costume = 2;
  r.stocks = 3;
  EXPECT_EQ(RequestJson(r), R"({"character":12,"costume":2,"ready":true,"stocks":3,"type":"room-ready"})");
  r.flag = false;
  EXPECT_EQ(RequestJson(r), R"({"character":12,"costume":2,"ready":false,"type":"room-ready"})");
  r.flag = true;
  r.stocks = 100;
  EXPECT_EQ(RequestJson(r), std::nullopt);
}

// A stamina match: a ready player's picked HP (1-999), as the stocks.
TEST(RoomMessages, PickedHp)
{
  const auto with = [](const std::string& v) {
    return Parse(Replace(STATE, R"("costume": 3,)", R"("costume": 3, "hp": )" + v + ","));
  };
  const Message m = with("250");
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  EXPECT_EQ(m.view->slots[0].player->hp, 250);
  EXPECT_EQ(with("null").view->slots[0].player->hp, std::nullopt);
  for (const char* bad : {"0", "1000", "-1", "\"3\""})
    EXPECT_EQ(with(bad).kind, Message::Kind::Invalid) << bad;
  Request r;
  r.op = Op::Ready;
  r.flag = true;
  r.character = 12;
  r.costume = 2;
  r.hp = 300;
  EXPECT_EQ(RequestJson(r), R"({"character":12,"costume":2,"hp":300,"ready":true,"type":"room-ready"})");
  r.flag = false;
  EXPECT_EQ(RequestJson(r), R"({"character":12,"costume":2,"ready":false,"type":"room-ready"})");
  r.flag = true;
  r.hp = 1000;
  EXPECT_EQ(RequestJson(r), std::nullopt);
}

// The rules menu's MATCH SETUP page (room-format) and what room-state shows of it.
TEST(RoomMessages, Format)
{
  Request r;
  r.op = Op::Format;
  r.stage_choice = "strike";
  r.strikes = {1, 2, 1};
  r.bans = 2;
  r.dsr = true;
  r.alternate_stages = true;
  EXPECT_EQ(RequestJson(r), R"({"alternateStages":true,"bans":2,"dsr":true,"stageChoice":"strike",)"
                            R"("strikes":[1,2,1],"type":"room-format"})");
  r.stage_choice = "rps";
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r.stage_choice = "random";
  r.bans = 4;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r.bans = 0;
  r.strikes = {1, 0};
  EXPECT_EQ(RequestJson(r), std::nullopt);

  const auto with = [](const std::string& ruleset) {
    return Parse(Replace(STATE, R"("statusText")", R"("ruleset": )" + ruleset + R"(, "statusText")"));
  };
  const Message m = with(R"({"name": "x", "bestOf": 3, "strikes": [2, 2], "bans": 3, "dsr": true, "alternateStages": true})");
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  EXPECT_EQ(m.view->ruleset.strikes, (std::vector<u8>{2, 2}));
  EXPECT_EQ(m.view->ruleset.bans, 3);
  EXPECT_TRUE(m.view->ruleset.dsr);
  EXPECT_TRUE(m.view->ruleset.alternate_stages);
  // A pattern with a bad turn is not shown; too many bans are none.
  const Message bad = with(R"({"name": "x", "strikes": [1, 0], "bans": 9})");
  ASSERT_EQ(bad.kind, Message::Kind::RoomState) << bad.why;
  EXPECT_TRUE(bad.view->ruleset.strikes.empty());
  EXPECT_EQ(bad.view->ruleset.bans, 0);
}

TEST(RoomMessages, RoomRules)
{
  // The room's rules come with room-state (docs/design/room-rules.md); checked as a ticket's.
  const std::string with = Replace(STATE, R"("statusText")",
                                   R"("rules": {"stocks": 3, "autoLCancel": true, "pause": "hold"}, "statusText")");
  const Message m = Parse(with);
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  ASSERT_TRUE(m.view->rules);
  EXPECT_EQ(m.view->rules->stocks, 3);
  EXPECT_TRUE(m.view->rules->auto_l_cancel);
  EXPECT_FALSE(m.view->rules->pause_normal);
  // Unusable rules do not cost the room: no rules, and the text to show.
  for (const char* bad : {R"({"stocks": 0})", R"({"newRule": true})", R"("stock")"})
  {
    const Message b = Parse(Replace(STATE, R"("statusText")", std::string(R"("rules": )") + bad + R"(, "statusText")"));
    ASSERT_EQ(b.kind, Message::Kind::RoomState) << bad;
    EXPECT_FALSE(b.view->rules) << bad;
    EXPECT_FALSE(b.view->rules_error.empty()) << bad;
  }
}

TEST(RoomMessages, RoomRulesetStages)
{
  // The ruleset's stages and the loser's choice (docs/rooms-protocol.md §2, `ruleset.stages`).
  const auto with = [](const std::string& ruleset) {
    return Parse(Replace(STATE, R"("statusText")", R"("ruleset": )" + ruleset + R"(, "statusText")"));
  };
  Message m = with(R"({"name": "Casual", "bestOf": 0, "stageChoice": "loser-picks-list",
                       "stages": {"starters": [1, 2], "counterpicks": [42]}})");
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  EXPECT_EQ(m.view->ruleset.stage_choice, "loser-picks-list");
  EXPECT_TRUE(m.view->ruleset.LoserPicks());
  EXPECT_TRUE(m.view->ruleset.PickFromList());
  EXPECT_EQ(m.view->ruleset.starters, (std::vector<u16>{1, 2}));
  EXPECT_EQ(m.view->ruleset.counterpicks, (std::vector<u16>{42}));
  // Any stage: the lists only feed the random stages.
  m = with(R"({"name": "P+ default", "stageChoice": "loser-picks", "stages": {"starters": [1]}})");
  EXPECT_TRUE(m.view->ruleset.LoserPicks());
  EXPECT_FALSE(m.view->ruleset.PickFromList());
  EXPECT_TRUE(m.view->ruleset.counterpicks.empty());
  // A set picks in its own steps; ids outside 1-255 and unknown choices are left out.
  m = with(R"({"name": "x", "bestOf": 3, "stageChoice": "loser-picks-list",
               "stages": {"starters": [0, 256, 3, "x"], "counterpicks": "no"}})");
  EXPECT_FALSE(m.view->ruleset.PickFromList());
  EXPECT_EQ(m.view->ruleset.starters, (std::vector<u16>{3}));
  m = with(R"({"name": "x", "stageChoice": "pick-anything"})");
  EXPECT_EQ(m.view->ruleset.stage_choice, "loser-picks");
  EXPECT_TRUE(m.view->ruleset.starters.empty());
}

TEST(RoomMessages, StagesRequest)
{
  // The host's STAGE SWITCH (room-stages, docs/rooms-protocol.md).
  Request r;
  r.op = Op::Stages;
  r.starters = {1, 2};
  r.counterpicks = {42};
  r.stage_choice = "loser-picks-list";
  auto j = RequestJson(r);
  ASSERT_TRUE(j);
  picojson::value v;
  ASSERT_TRUE(picojson::parse(v, *j).empty());
  EXPECT_EQ(v.get("type").to_str(), "room-stages");
  EXPECT_EQ(v.get("stageChoice").to_str(), "loser-picks-list");
  EXPECT_EQ(v.get("stages").get("starters").serialize(), "[1,2]");
  EXPECT_EQ(v.get("stages").get("counterpicks").serialize(), "[42]");
  // "As it is": no stageChoice.
  r.stage_choice.clear();
  j = RequestJson(r);
  ASSERT_TRUE(j);
  EXPECT_EQ(j->find("stageChoice"), std::string::npos);
  // Out of range: nothing is sent.
  r.stage_choice = "strike";
  EXPECT_FALSE(RequestJson(r));
  r.stage_choice.clear();
  r.counterpicks = {256};
  EXPECT_FALSE(RequestJson(r));
  r.starters.clear();
  r.counterpicks.clear();
  EXPECT_FALSE(RequestJson(r));
}

TEST(RoomMessages, Unreachable)
{
  // A room game's players this one could not connect to (room-unreachable, client -> server).
  Request r;
  r.op = Op::Unreachable;
  r.match_id = "mode.room-KFQB-1";
  r.ports = {4};
  auto j = RequestJson(r);
  ASSERT_TRUE(j);
  picojson::value v;
  ASSERT_TRUE(picojson::parse(v, *j).empty());
  EXPECT_EQ(v.get("type").to_str(), "room-unreachable");
  EXPECT_EQ(v.get("matchId").to_str(), "mode.room-KFQB-1");
  EXPECT_EQ(v.get("slots").serialize(), "[4]");
  r.ports = {0};
  EXPECT_FALSE(RequestJson(r));
  r.ports = {1, 2, 3, 4};
  EXPECT_FALSE(RequestJson(r));
  r.ports.clear();
  EXPECT_FALSE(RequestJson(r));
  r.ports = {4};
  r.match_id.clear();
  EXPECT_FALSE(RequestJson(r));

  // The pairs, server -> every member: lower slot first, never a slot with itself.
  Message m = Parse(
      R"({"type": "room-unreachable", "code": "KFQB", "matchId": "mode.room-KFQB-1", "pairs": [[4, 1], [3, 4]]})");
  ASSERT_EQ(m.kind, Message::Kind::RoomUnreachable);
  EXPECT_EQ(m.match_id, "mode.room-KFQB-1");
  ASSERT_EQ(m.pairs.size(), 2u);
  EXPECT_EQ(m.pairs[0], (std::array<u8, 2>{1, 4}));
  EXPECT_EQ(m.pairs[1], (std::array<u8, 2>{3, 4}));
  EXPECT_EQ(Parse(R"({"type": "room-unreachable", "matchId": "m", "pairs": [[1, 1]]})").kind,
            Message::Kind::Invalid);
  EXPECT_EQ(Parse(R"({"type": "room-unreachable", "matchId": "m", "pairs": [[1, 5]]})").kind,
            Message::Kind::Invalid);
  EXPECT_EQ(Parse(R"({"type": "room-unreachable", "matchId": "m", "pairs": [[1]]})").kind,
            Message::Kind::Invalid);
  EXPECT_EQ(Parse(R"({"type": "room-unreachable", "pairs": [[1, 2]]})").kind,
            Message::Kind::Invalid);
}

TEST(RoomMessages, RefusesBadRoomStates)
{
  const std::string ok = STATE;
  // Each of these must be refused as a whole (nothing of it reaches the game).
  const std::pair<const char*, const char*> bad[] = {
      {R"("code": "KFQB")", R"("code": "KFQA")"},
      {R"("code": "KFQB")", R"("code": 7)"},
      {R"("mode": "1v1")", R"("mode": "2v2")"},
      {R"("status": "waiting")", R"("status": "paused")"},
      {R"("you": 2)", R"("you": 5)"},
      {R"("you": 2)", R"("you": 3)"},          // an empty slot
      {R"("you": 2)", R"("you": 1.5)"},
      {R"("host": 1)", R"("host": 4)"},        // an empty slot
      {R"("host": 1)", R"("host": 0)"},
      {R"("public": true)", R"("public": 1)"},
      {R"("slot": 4,)", R"("slot": 3,)"},      // two slot 3s
      {R"("slot": 4,)", R"("slot": 5,)"},
      {R"("team": 1)", R"("team": 3)"},
      {R"("team": 1)", R"("team": -1)"},
      {R"("character": 12)", R"("character": 256)"},
      {R"("character": 12)", R"("character": "fox")"},
      {R"("ready": true)", R"("ready": "yes")"},
      {R"("displayName": "bob")", R"("displayName": null)"},
      {R"("player": null},
    {"slot": 4)", R"("player": 5},
    {"slot": 4)"},
      {R"("statusText": "Waiting on: bob")", R"("statusText": ["x"])"},
  };
  for (const auto& [from, to] : bad)
  {
    const Message m = Parse(Replace(ok, from, to));
    EXPECT_EQ(m.kind, Message::Kind::Invalid) << to;
    EXPECT_FALSE(m.view) << to;
  }
  // Three slots, five slots.
  picojson::value v;
  ASSERT_TRUE(picojson::parse(v, ok).empty());
  auto o = v.get<picojson::object>();
  auto slots = o["slots"].get<picojson::array>();
  std::string why;
  o["slots"] = picojson::value(picojson::array(slots.begin(), slots.begin() + 3));
  EXPECT_FALSE(ParseView(o, &why));
  slots.push_back(slots[3]);
  o["slots"] = picojson::value(slots);
  EXPECT_FALSE(ParseView(o, &why));
}

TEST(RoomMessages, CutsLongAndHostileStrings)
{
  const std::string long_name(300, 'n');
  const Message m = Parse(Replace(STATE, R"("displayName": "bob")",
                                  "\"displayName\": \"" + long_name + "\\u0007\""));
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  EXPECT_EQ(m.view->slots[1].player->name.size(), MAX_NAME_BYTES);
  const Message t = Parse(Replace(STATE, R"("statusText": "Waiting on: bob")",
                                  R"("statusText": "line\nbreak\u0000")"));
  ASSERT_EQ(t.kind, Message::Kind::RoomState);
  EXPECT_EQ(t.view->status_text, "linebreak");
}

TEST(RoomMessages, OtherMessages)
{
  Message m = Parse(R"({"type": "hello-resp"})");
  EXPECT_EQ(m.kind, Message::Kind::HelloResp);
  EXPECT_TRUE(m.error.empty());
  EXPECT_FALSE(m.game_alive) << "an older mm: no game-alive";
  EXPECT_TRUE(Parse(R"({"type": "hello-resp", "gameAlive": true})").game_alive);
  EXPECT_FALSE(Parse(R"({"type": "hello-resp", "gameAlive": 1})").game_alive);
  m = Parse(R"({"type": "hello-resp", "error": "Update to 0.2.1 to play online.", "latestVersion": "0.2.1"})");
  EXPECT_EQ(m.error, "Update to 0.2.1 to play online.");
  EXPECT_EQ(m.latest_version, "0.2.1");

  m = Parse(R"({"type": "room-error", "op": "room-join", "error": "This room is full."})");
  EXPECT_EQ(m.kind, Message::Kind::RoomError);
  EXPECT_EQ(m.op, "room-join");
  EXPECT_EQ(m.error, "This room is full.");
  EXPECT_EQ(KindOf(R"({"type": "room-error", "op": "room-join"})"), Message::Kind::Invalid);

  m = Parse(R"({"type": "room-left", "code": "KFQB", "reason": "Removed from the room."})");
  EXPECT_EQ(m.kind, Message::Kind::RoomLeft);
  EXPECT_EQ(m.code, "KFQB");
  EXPECT_EQ(m.reason, "Removed from the room.");

  m = Parse(R"({"type": "room-start", "code": "kfqb", "matchId": "mode.room-KFQB-1", "timeoutSecs": 15})");
  EXPECT_EQ(m.kind, Message::Kind::RoomStart);
  EXPECT_EQ(m.code, "KFQB");
  EXPECT_EQ(m.timeout_secs, 15);
  m = Parse(R"({"type": "room-start", "code": "KFQB", "matchId": "x", "timeoutSecs": 1e9})");
  EXPECT_EQ(m.timeout_secs, 15);
  EXPECT_EQ(KindOf(R"({"type": "room-start", "code": "AAAA", "matchId": "x"})"), Message::Kind::Invalid);

  m = Parse(R"({"type": "error", "error": "Signed in from another game."})");
  EXPECT_EQ(m.kind, Message::Kind::Error);
  EXPECT_EQ(KindOf(R"({"type": "something-new"})"), Message::Kind::Unknown);
  EXPECT_EQ(KindOf("not json"), Message::Kind::Invalid);
  EXPECT_EQ(KindOf("[1, 2]"), Message::Kind::Invalid);
  EXPECT_EQ(KindOf(std::string(20000, ' ') + "{}"), Message::Kind::Invalid);
}

// SIT OUT and the stage pickers (the user's, 2026-10-18): the server says who sits out, who picks
// the next stage, and which slots play a starting game and who hosts it.
TEST(RoomMessages, Watch)
{
  const Message m = Parse(Replace(STATE, R"("costume": 3, "inGame": false)",
                                  R"("costume": 3, "inGame": false, "watching": true)"));
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  EXPECT_TRUE(m.view->slots[0].player->watching);
  EXPECT_FALSE(m.view->slots[1].player->watching);
  Request r;
  r.op = Op::Watch;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r.want = true;
  EXPECT_EQ(RequestJson(r), R"({"type":"room-watch","want":true})");
  r.want.reset();
  r.watching = false;
  EXPECT_EQ(RequestJson(r), R"({"type":"room-watch","watching":false})");
  EXPECT_STREQ(OpName(Op::Watch), "watch");
}

TEST(RoomMessages, SitOutAndPickers)
{
  const Message m = Parse(Replace(Replace(STATE, R"("costume": 3, "inGame": false)",
                                          R"("costume": 3, "inGame": false, "sittingOut": true)"),
                                  R"("statusText")", R"("pickers": 2, "statusText")"));
  ASSERT_EQ(m.kind, Message::Kind::RoomState) << m.why;
  EXPECT_TRUE(m.view->slots[0].player->sitting_out);
  EXPECT_FALSE(m.view->slots[1].player->sitting_out);
  EXPECT_TRUE(m.view->sit_out_known);
  EXPECT_EQ(m.view->pickers, 2);
  // Pickers out of range are left out (this machine's own then).
  EXPECT_EQ(Parse(Replace(STATE, R"("statusText")", R"("pickers": 16, "statusText")")).view->pickers,
            std::nullopt);

  Message st = Parse(R"({"type": "room-start", "code": "KFQB", "matchId": "m", "players": [1, 3],
                         "host": 3, "pickers": 4})");
  ASSERT_EQ(st.kind, Message::Kind::RoomStart) << st.why;
  EXPECT_EQ(st.start_players, (std::vector<int>{1, 3}));
  EXPECT_EQ(st.start_host, 3);
  EXPECT_EQ(st.start_pickers, 4);
  // An older mm: every member plays, the room's host, this machine's pickers.
  st = Parse(R"({"type": "room-start", "code": "KFQB", "matchId": "m"})");
  EXPECT_TRUE(st.start_players.empty());
  EXPECT_EQ(st.start_host, 0);
  EXPECT_EQ(st.start_pickers, std::nullopt);
  for (const char* bad : {R"("players": [1, 1])", R"("players": [0])", R"("players": [1, 2], "host": 3)",
                          R"("players": "1,2")"})
  {
    EXPECT_EQ(KindOf(std::string(R"({"type": "room-start", "code": "KFQB", "matchId": "m", )") + bad + "}"),
              Message::Kind::Invalid)
        << bad;
  }

  Request r;
  r.op = Op::SitOut;
  r.slot = 3;
  r.flag = true;
  EXPECT_EQ(RequestJson(r), R"({"sitOut":true,"slot":3,"type":"room-sit-out"})");
  r.slot = 0;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::Back;
  EXPECT_EQ(RequestJson(r), R"({"type":"room-back"})");
  r.pickers = 2;
  EXPECT_EQ(RequestJson(r), R"({"pickers":2,"type":"room-back"})");
  r.pickers = 16;
  EXPECT_EQ(RequestJson(r), std::nullopt);
}

TEST(RoomMessages, Requests)
{
  Request r;
  r.op = Op::Join;
  r.code = "kfqb";
  EXPECT_EQ(RequestJson(r), R"({"code":"KFQB","type":"room-join"})");
  r.code = "KFQA";
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::Slot;
  r.slot = 3;
  r.flag = true;
  EXPECT_EQ(RequestJson(r), R"({"open":true,"slot":3,"type":"room-slot"})");
  r.slot = 5;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::Team;
  r.team = 3;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::Ready;
  r.flag = true;
  r.character = 12;
  r.costume = 2;
  EXPECT_EQ(RequestJson(r), R"({"character":12,"costume":2,"ready":true,"type":"room-ready"})");
  r.flag = false;
  // Not ready: the placed coin; in the hand, none.
  EXPECT_EQ(RequestJson(r), R"({"character":12,"costume":2,"ready":false,"type":"room-ready"})");
  r.character.reset();
  EXPECT_EQ(RequestJson(r), R"({"ready":false,"type":"room-ready"})");
  r = {};
  r.op = Op::Rules;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  Online::MatchRules rules;
  rules.stocks = 3;
  rules.auto_l_cancel = true;
  r.rules = rules;
  EXPECT_EQ(RequestJson(r),
            R"({"rules":{"autoLCancel":true,"damageRatio":1,"hazards":false,"inputAssist":false,)"
            R"("itemFrequency":0,"itemSwitch":[0,6,0,0,0,0,0,0],"pause":"normal","stocks":3,)"
            R"("style":"stock","teamAttack":true,"timeMinutes":8},"type":"room-rules"})");
  r = {};
  r.op = Op::Poll;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  EXPECT_EQ(HelloJson("u", "k", "0.1.0", "mac"),
            R"({"appVersion":"0.1.0","gameAlive":true,"platform":"mac","type":"hello","user":{"playKey":"k","uid":"u"}})");
  EXPECT_EQ(HelloJson("u", "k", "0.1.0", "win", "ab12"),
            R"({"appVersion":"0.1.0","chatKey":"ab12","gameAlive":true,"platform":"win","type":"hello",)"
            R"("user":{"playKey":"k","uid":"u"}})");
}

TEST(RoomMessages, RulesetCodesAndRequests)
{
  EXPECT_EQ(NormalizeRulesetCode(std::string("bcdfgh")), "BCDFGH");
  EXPECT_EQ(NormalizeRulesetCode(std::u16string(u"ＢCDFGH")), "BCDFGH");  // full width B
  for (const char* bad : {"BCDFG", "BCDFGHJ", "BCDFGA", "KFQB", "", "BCD FG"})
    EXPECT_EQ(NormalizeRulesetCode(std::string(bad)), std::nullopt) << bad;
  Request r;
  r.op = Op::Create;
  r.flag = true;
  r.remembered = true;
  EXPECT_EQ(RequestJson(r), R"({"public":true,"remembered":true,"type":"room-create"})");
  r.ruleset = "bcdfgh";
  EXPECT_EQ(RequestJson(r), R"({"public":true,"ruleset":"BCDFGH","type":"room-create"})");
  r.ruleset = "KFQB";
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::Ruleset;
  EXPECT_EQ(RequestJson(r), R"({"code":null,"type":"room-ruleset"})");
  r.ruleset = "BCDFGH";
  EXPECT_EQ(RequestJson(r), R"({"code":"BCDFGH","type":"room-ruleset"})");
  r = {};
  r.op = Op::Result;
  r.game = 2;
  r.winner_side = 1;
  r.stage = 0x21;
  EXPECT_EQ(RequestJson(r), R"({"game":2,"stage":33,"type":"room-result","winnerSide":1})");
  r.winner_side = SIDE_VOID;
  EXPECT_EQ(RequestJson(r), R"({"game":2,"stage":33,"type":"room-result","winnerSide":253})");
  r.winner_side = 2;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::SetAction;
  r.flag = true;
  EXPECT_EQ(RequestJson(r), R"({"action":"resume","type":"room-set"})");
  r = {};
  r.op = Op::BestOf;
  r.best_of = 5;
  EXPECT_EQ(RequestJson(r), R"({"bestOf":5,"type":"room-best-of"})");
  r.best_of = 4;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::Step;
  r.game = 2;
  r.actions = {std::vector<u8>{0x21, 2}, std::vector<u8>{}};
  EXPECT_EQ(RequestJson(r), R"({"actions":[[33,2],[]],"game":2,"matchStarted":false,"type":"room-step"})");
  r.flag = true;
  EXPECT_EQ(RequestJson(r), R"({"actions":[[33,2],[]],"game":2,"matchStarted":true,"type":"room-step"})");
  r.actions[1] = {0};
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r.actions[1] = {};
  r.game = 0;
  EXPECT_EQ(RequestJson(r), std::nullopt);
}

TEST(RoomMessages, RulesetList)
{
  const Message m = Parse(R"({"type": "ruleset-list", "rulesets": [
    {"code": "BCDFGH", "name": "NorCal Weekly", "version": 3, "by": "sarah", "bestOf": 3, "players": "1v1"},
    {"code": "nope", "name": "bad code"},
    {"code": "kfqbtr", "name": "Bo1 pools", "version": 1, "by": "bo", "bestOf": 0, "players": "1v1"}]})");
  ASSERT_EQ(m.kind, Message::Kind::RulesetList);
  ASSERT_EQ(m.rulesets.size(), 2u);
  EXPECT_EQ(m.rulesets[0].code, "BCDFGH");
  EXPECT_EQ(m.rulesets[0].name, "NorCal Weekly");
  EXPECT_EQ(m.rulesets[0].best_of, 3);
  EXPECT_EQ(m.rulesets[1].code, "KFQBTR");
  Request r;
  r.op = Op::RulesetList;
  EXPECT_EQ(RequestJson(r), R"({"type":"room-rulesets"})");
}
