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
  r.op = Op::Poll;
  EXPECT_EQ(RequestJson(r), std::nullopt);
  EXPECT_EQ(HelloJson("u", "k", "0.1.0", "mac"),
            R"({"appVersion":"0.1.0","platform":"mac","type":"hello","user":{"playKey":"k","uid":"u"}})");
}
