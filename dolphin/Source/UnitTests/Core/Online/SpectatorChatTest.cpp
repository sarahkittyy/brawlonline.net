// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Online::SpectatorChat: the launcher's chat file for a spectated game (docs/spectate-protocol.md
// §4.1) into the chat window's view, what the spectator does back to the launcher, and a file
// with anything in it.

#include <string>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#include "BrawlOnline/Online/Chat.h"
#include "BrawlOnline/Online/SpectatorChat.h"

namespace Chat = Online::Chat;
namespace SC = Online::SpectatorChat;

namespace
{
constexpr char STREAM[] = "3fa9c0d1e2b4";
constexpr char ME[] = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr char BOB[] = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
constexpr char CAROL[] = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

picojson::object Person(const char* uid, const char* name)
{
  picojson::object o;
  o["uid"] = picojson::value(uid);
  o["name"] = picojson::value(name);
  o["code"] = picojson::value(std::string(name).substr(0, 3) + "#1");
  return o;
}

picojson::object LineOf(double id, const char* uid, const char* name, const char* text)
{
  picojson::object o = Person(uid, name);
  o["id"] = picojson::value(id);
  o["text"] = picojson::value(text);
  return o;
}

struct File
{
  picojson::object o;
  picojson::array spectators;
  picojson::array lines;
  picojson::array hidden;

  File(const char* stream = STREAM)
  {
    o["stream"] = picojson::value(stream);
    o["you"] = picojson::value(ME);
    o["count"] = picojson::value(3.0);
    spectators.emplace_back(Person(ME, "me"));
    spectators.emplace_back(Person(BOB, "bob"));
  }
  std::string Text()
  {
    o["spectators"] = picojson::value(spectators);
    o["lines"] = picojson::value(lines);
    o["hidden"] = picojson::value(hidden);
    return picojson::value(o).serialize();
  }
  void Done(const std::string& run, double seq)
  {
    picojson::object d;
    d["run"] = picojson::value(run);
    d["seq"] = picojson::value(seq);
    o["done"] = picojson::value(d);
  }
};

Chat::View View()
{
  auto v = SC::GetView(0);
  EXPECT_TRUE(v.has_value());
  return v.value_or(Chat::View{});
}

picojson::object Out()
{
  const picojson::value v = SC::Outbox();
  return v.is<picojson::object>() ? v.get<picojson::object>() : picojson::object{};
}

class SpectatorChatTest : public ::testing::Test
{
protected:
  void SetUp() override { SC::ResetForTesting(); }
  void TearDown() override { SC::ResetForTesting(); }
};
}  // namespace

TEST_F(SpectatorChatTest, NoFileNoChat)
{
  EXPECT_FALSE(SC::Active());
  EXPECT_TRUE(SC::Outbox().is<picojson::null>());
  EXPECT_EQ(SC::Send("hi"), Chat::SendResult::NoGroup);
  // A missing file is no spectator chat, and the players' chat is what the window gets.
  SC::Follow("this file does not exist.chat.json", "nor this.spectators.json");
  EXPECT_FALSE(SC::Active());
  EXPECT_FALSE(Chat::GetView(0).value_or(Chat::View{}).spectate);
}

TEST_F(SpectatorChatTest, TheFileIsTheView)
{
  File f;
  f.lines.emplace_back(LineOf(5, BOB, "bob", "  nice\xe2\x80\x8b  combo "));
  f.lines.emplace_back(LineOf(6, ME, "me", "thanks"));
  picojson::object notice;
  notice["id"] = picojson::value(7.0);
  notice["notice"] = picojson::value("Chat is busy. Try again in a moment.");
  f.lines.emplace_back(notice);
  SC::Apply(f.Text());
  ASSERT_TRUE(SC::Active());

  const Chat::View v = View();
  EXPECT_TRUE(v.active);
  EXPECT_TRUE(v.spectate);
  EXPECT_EQ(v.spectators, 3u);
  ASSERT_EQ(v.watching.size(), 2u);
  EXPECT_EQ(v.watching[1].name, "bob");
  EXPECT_EQ(v.watching[1].code, "bob#1");
  EXPECT_TRUE(v.watching[0].is_me);
  ASSERT_EQ(v.lines.size(), 3u);
  EXPECT_EQ(v.lines[0].kind, Chat::Line::Kind::Message);
  EXPECT_EQ(v.lines[0].text, "nice combo");
  EXPECT_EQ(v.lines[1].kind, Chat::Line::Kind::Own);
  EXPECT_EQ(v.lines[2].kind, Chat::Line::Kind::System);
  EXPECT_LT(v.lines[0].id, v.lines[1].id);
  EXPECT_LT(v.lines[1].id, v.lines[2].id);

  // The window's calls come here while it is active.
  const Chat::View through = Chat::GetView(0).value_or(Chat::View{});
  EXPECT_TRUE(through.spectate);
  EXPECT_EQ(through.lines.size(), 3u);
  // Nothing new: nothing to copy.
  EXPECT_FALSE(SC::GetView(v.serial).has_value());
}

TEST_F(SpectatorChatTest, ATypedLineShowsUntilTheLauncherTookIt)
{
  File f;
  SC::Apply(f.Text());
  EXPECT_EQ(Chat::Send("  gg  "), Chat::SendResult::Sent);
  EXPECT_EQ(SC::Send(" \xe2\x80\x8b "), Chat::SendResult::Empty);
  Chat::View v = View();
  ASSERT_EQ(v.lines.size(), 1u);
  EXPECT_EQ(v.lines[0].kind, Chat::Line::Kind::Own);
  EXPECT_EQ(v.lines[0].text, "gg");
  EXPECT_EQ(v.lines[0].name, "me");

  picojson::object out = Out();
  const std::string run = out["run"].to_str();
  EXPECT_EQ(run.size(), 16u);
  const auto& items = out["out"].get<picojson::array>();
  ASSERT_EQ(items.size(), 1u);
  const auto& item = items[0].get<picojson::object>();
  EXPECT_EQ(item.at("seq").get<double>(), 1);
  EXPECT_EQ(item.at("stream").get<std::string>(), STREAM);
  EXPECT_EQ(item.at("send").get<std::string>(), "gg");

  // The launcher sent it: the server's line is in the file, the typed one goes.
  f.lines.emplace_back(LineOf(12, ME, "me", "gg"));
  f.Done(run, 1);
  SC::Apply(f.Text());
  v = View();
  ASSERT_EQ(v.lines.size(), 1u);
  EXPECT_EQ(v.lines[0].kind, Chat::Line::Kind::Own);
  EXPECT_TRUE(Out()["out"].get<picojson::array>().empty());

  // Another run's "done" (a Dolphin before this one) takes nothing.
  EXPECT_EQ(SC::Send("again"), Chat::SendResult::Sent);
  f.Done("0000000000000000", 99);
  SC::Apply(f.Text());
  EXPECT_EQ(Out()["out"].get<picojson::array>().size(), 1u);
}

TEST_F(SpectatorChatTest, TheLocalSendLimit)
{
  File f;
  SC::Apply(f.Text());
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(SC::Send("x"), Chat::SendResult::Sent);
  EXPECT_EQ(SC::Send("x"), Chat::SendResult::TooFast);
}

TEST_F(SpectatorChatTest, OverIsReadOnly)
{
  File f;
  f.lines.emplace_back(LineOf(1, BOB, "bob", "bye"));
  f.o["over"] = picojson::value(true);
  SC::Apply(f.Text());
  EXPECT_TRUE(SC::Active());
  EXPECT_EQ(View().lines.size(), 1u);
  EXPECT_EQ(SC::Send("hi"), Chat::SendResult::NoGroup);
}

TEST_F(SpectatorChatTest, IgnoreGoesToTheLauncherAndShowsAtOnce)
{
  File f;
  f.lines.emplace_back(LineOf(1, BOB, "bob", "hey"));
  SC::Apply(f.Text());
  EXPECT_FALSE(Chat::IsHidden(BOB));
  Chat::SetHidden(BOB, true);
  EXPECT_TRUE(SC::IsHidden(BOB));
  const auto items = Out()["out"].get<picojson::array>();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].get<picojson::object>().at("hide").get<std::string>(), BOB);
  EXPECT_TRUE(items[0].get<picojson::object>().at("hidden").get<bool>());
  // The launcher hasn't written it to the ignore list yet: still ignored here.
  SC::Apply(f.Text());
  EXPECT_TRUE(SC::IsHidden(BOB));
  for (const Chat::Member& m : View().members)
  {
    if (m.uid == BOB)
      EXPECT_TRUE(m.hidden);
  }
  // Taken: the file's list is the list.
  const std::string run = Out()["run"].to_str();
  f.hidden.emplace_back(BOB);
  f.hidden.emplace_back("not a uid");
  f.Done(run, 1);
  SC::Apply(f.Text());
  EXPECT_TRUE(SC::IsHidden(BOB));
  f.hidden.clear();
  SC::Apply(f.Text());
  EXPECT_FALSE(SC::IsHidden(BOB));
}

TEST_F(SpectatorChatTest, ReportNamesTheirNewestLine)
{
  File f;
  f.lines.emplace_back(LineOf(3, CAROL, "carol", "one"));
  f.lines.emplace_back(LineOf(4, BOB, "bob", "two"));
  f.lines.emplace_back(LineOf(9, CAROL, "carol", "three"));
  SC::Apply(f.Text());
  EXPECT_FALSE(Chat::Report(ME, ""));
  EXPECT_FALSE(SC::Report("dddddddd-dddd-4ddd-8ddd-dddddddddddd"));
  EXPECT_TRUE(Chat::Report(CAROL, ""));
  const auto items = Out()["out"].get<picojson::array>();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].get<picojson::object>().at("report").get<double>(), 9);
  // A line's author can be clicked even when the list leaves them out.
  bool carol = false;
  for (const Chat::Member& m : View().members)
    carol |= m.uid == CAROL;
  EXPECT_TRUE(carol);
}

TEST_F(SpectatorChatTest, AnotherStreamIsAnotherChat)
{
  File f;
  f.lines.emplace_back(LineOf(1, BOB, "bob", "first game"));
  SC::Apply(f.Text());
  EXPECT_EQ(SC::Send("typed here"), Chat::SendResult::Sent);
  File g("0123456789ab");
  SC::Apply(g.Text());
  const Chat::View v = View();
  EXPECT_TRUE(v.lines.empty());
  // The line typed for the last stream still goes out under it (the launcher drops it).
  const auto items = Out()["out"].get<picojson::array>();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].get<picojson::object>().at("stream").get<std::string>(), STREAM);
  // No stream: gone.
  File none("");
  SC::Apply(none.Text());
  EXPECT_FALSE(SC::Active());
}

TEST_F(SpectatorChatTest, AnythingInTheFile)
{
  File f;
  SC::Apply(f.Text());
  const u64 before = View().serial;
  // Not JSON, too deep, not an object: nothing changes.
  SC::Apply("not json");
  SC::Apply(std::string(100, '[') + std::string(100, ']'));
  SC::Apply("[1, 2]");
  EXPECT_TRUE(SC::Active());
  EXPECT_FALSE(SC::GetView(before).has_value());

  File bad;
  bad.o["stream"] = picojson::value("../../etc");
  SC::Apply(bad.Text());
  EXPECT_FALSE(SC::Active());

  File g;
  g.o["count"] = picojson::value(-5.0);
  g.spectators.emplace_back(Person("not-a-uid", "x"));
  g.spectators.emplace_back(Person(BOB, "bob twice"));
  g.lines.emplace_back(LineOf(5, BOB, "bob", "ok"));
  g.lines.emplace_back(LineOf(4, BOB, "bob", "older id: skipped"));
  g.lines.emplace_back(LineOf(5, BOB, "bob", "same id: skipped"));
  g.lines.emplace_back(LineOf(0, BOB, "bob", "no id"));
  g.lines.emplace_back(LineOf(6, "x", "x", "not a uid"));
  g.lines.emplace_back(LineOf(7, BOB, "bob", "\xe2\x80\xae\xe2\x80\x8b"));
  picojson::object weird = Person(BOB, "\xe2\x80\xae" "bob\x07");
  weird["id"] = picojson::value(8.0);
  weird["text"] = picojson::value("fine");
  g.lines.emplace_back(weird);
  g.lines.emplace_back(picojson::value(3.0));
  SC::Apply(g.Text());
  const Chat::View v = View();
  EXPECT_EQ(v.watching.size(), 2u);
  // At least those listed.
  EXPECT_EQ(v.spectators, 2u);
  ASSERT_EQ(v.lines.size(), 2u);
  EXPECT_EQ(v.lines[0].text, "ok");
  EXPECT_EQ(v.lines[1].text, "fine");
  EXPECT_EQ(v.lines[1].name, "bob");

  // More lines than the window keeps: the newest.
  File many;
  for (int i = 1; i <= 150; ++i)
    many.lines.emplace_back(LineOf(i, BOB, "bob", "line"));
  SC::Apply(many.Text());
  EXPECT_EQ(View().lines.size(), Chat::MAX_LINES);
}

TEST_F(SpectatorChatTest, TheSpectatorsWindowLoadsPageByPage)
{
  File f;
  SC::Apply(f.Text());
  EXPECT_FALSE(SC::GetList(0).value_or(SC::List{}).open);
  // Scrolled to the end of a list never opened: nothing asked.
  SC::LoadMore();
  EXPECT_TRUE(SC::Outbox().is<picojson::null>());

  SC::OpenList();
  SC::List l = SC::GetList(0).value_or(SC::List{});
  EXPECT_TRUE(l.open);
  EXPECT_TRUE(l.loading);
  EXPECT_TRUE(l.names.empty());
  picojson::object out = Out();
  const std::string run = out["run"].to_str();
  auto items = out["out"].get<picojson::array>();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].get<picojson::object>().at("list").get<std::string>(), "first");
  // Asked again while a page is on its way: nothing more.
  SC::LoadMore();
  EXPECT_EQ(Out()["out"].get<picojson::array>().size(), 1u);

  // The launcher's first page, then "done".
  const auto page = [](int n, bool more, const char* stream = STREAM) {
    picojson::object o;
    o["stream"] = picojson::value(stream);
    o["count"] = picojson::value(10000.0);
    o["more"] = picojson::value(more);
    picojson::array names;
    for (int i = 0; i < n; ++i)
    {
      picojson::object p;
      p["uid"] = picojson::value(fmt::format("{:08x}-cccc-4ccc-8ccc-cccccccccccc", i));
      p["name"] = picojson::value(fmt::format("n{}", i));
      p["code"] = picojson::value("C#1");
      names.emplace_back(p);
    }
    picojson::object bad;
    bad["uid"] = picojson::value("not a uid");
    names.emplace_back(bad);
    o["names"] = picojson::value(names);
    return picojson::value(o).serialize();
  };
  SC::ApplyList(page(200, true));
  f.Done(run, 1);
  SC::Apply(f.Text());
  l = SC::GetList(l.serial).value_or(SC::List{});
  EXPECT_TRUE(l.open);
  EXPECT_FALSE(l.loading);
  EXPECT_TRUE(l.more);
  EXPECT_EQ(l.count, 10000u);
  ASSERT_EQ(l.names.size(), 200u);
  EXPECT_EQ(l.names[199].name, "n199");
  SC::LoadMore();
  items = Out()["out"].get<picojson::array>();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].get<picojson::object>().at("list").get<std::string>(), "next");

  // Another game's list: not this one.
  SC::ApplyList(page(5, false, "0123456789ab"));
  EXPECT_EQ(SC::GetList(0).value_or(SC::List{}).names.size(), 200u);
  // The last page.
  SC::ApplyList(page(400, false));
  l = SC::GetList(0).value_or(SC::List{});
  EXPECT_EQ(l.names.size(), 400u);
  EXPECT_FALSE(l.more);

  // Closed, then another stream: the list is gone and stays closed.
  SC::CloseList();
  EXPECT_FALSE(SC::GetList(0).value_or(SC::List{}).open);
  SC::OpenList();
  EXPECT_TRUE(SC::GetList(0).value_or(SC::List{}).open);
  File g("0123456789ab");
  SC::Apply(g.Text());
  EXPECT_FALSE(SC::GetList(0).value_or(SC::List{}).open);
}
