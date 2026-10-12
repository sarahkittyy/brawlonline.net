// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The mm server's room messages as Dolphin parses them (Core/Online/RoomMessages.h): everything
// the server sends is checked before it can reach the game's memory.

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
  ASSERT_TRUE(v.slots[1].player);
  EXPECT_EQ(v.slots[1].player->team, 1);
  // Not ready: the character is not passed on (docs/design/rooms.md #15).
  EXPECT_EQ(v.slots[1].player->character, std::nullopt);
  EXPECT_FALSE(v.slots[2].open);
  EXPECT_FALSE(v.slots[2].player);
  // No rules (an older server): none, and no error.
  EXPECT_FALSE(v.rules);
  EXPECT_TRUE(v.rules_error.empty());
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
  EXPECT_EQ(RequestJson(r), R"({"ready":false,"type":"room-ready"})");
  r = {};
  r.op = Op::Rules;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  Online::MatchRules rules;
  rules.stocks = 3;
  rules.auto_l_cancel = true;
  r.rules = rules;
  EXPECT_EQ(RequestJson(r),
            R"({"rules":{"autoLCancel":true,"damageRatio":1,"hazards":true,"inputAssist":false,)"
            R"("itemFrequency":0,"itemSwitch":[0,6,0,0,0,0,0,0],"pause":"normal","stocks":3,)"
            R"("style":"stock","teamAttack":true,"timeMinutes":8},"type":"room-rules"})");
  r = {};
  r.op = Op::Poll;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  EXPECT_EQ(HelloJson("u", "k", "0.1.0", "mac"),
            R"({"appVersion":"0.1.0","platform":"mac","type":"hello","user":{"playKey":"k","uid":"u"}})");
  EXPECT_EQ(HelloJson("u", "k", "0.1.0", "win", "ab12"),
            R"({"appVersion":"0.1.0","chatKey":"ab12","platform":"win","type":"hello","user":{"playKey":"k","uid":"u"}})");
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
  r.winner_side = 2;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  r = {};
  r.op = Op::SetAction;
  r.flag = true;
  EXPECT_EQ(RequestJson(r), R"({"action":"resume","type":"room-set"})");
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
