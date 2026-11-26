// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Quick chat (core Online/QuickChat): the launcher's file, the D-pad taken from the CSS, and the
// menu's presses.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BrawlOnline/Online/QuickChat.h"
#include "InputCommon/GCPadStatus.h"

namespace QC = Online::QuickChat;

namespace
{
std::vector<std::string> s_sent;
QC::Result s_answer = QC::Result::Sent;

QC::Result FakeSend(const std::string& text)
{
  s_sent.push_back(text);
  return s_answer;
}

QC::Messages Some()
{
  QC::Messages m;
  m[4 * QC::Up + QC::Up] = "ggs";
  m[4 * QC::Up + QC::Right] = "one more?";
  m[4 * QC::Left + QC::Down] = "brb";
  return m;
}

// Usable at `now`: on the CSS, the chat window shown, these messages.
void Ready(u64 now, const QC::Messages& m = Some())
{
  s_sent.clear();
  s_answer = QC::Result::Sent;
  QC::ResetForTesting(FakeSend);
  QC::SetMessages(m);
  QC::SetOnCss(true);
  QC::SetChatShown(true, now);
}

u16 Filter(u16 buttons, u64 now, int chan = 0)
{
  QC::FilterPad(chan, &buttons, now);
  return buttons;
}

std::string Json(const std::vector<std::string>& messages, int version = 1)
{
  std::string out = "{\"version\": " + std::to_string(version) + ", \"messages\": [";
  for (size_t i = 0; i < messages.size(); ++i)
    out += (i ? ", \"" : "\"") + messages[i] + "\"";
  return out + "]}";
}
}  // namespace

TEST(QuickChat, ParsesSixteenCleanedMessages)
{
  std::vector<std::string> m(16, "");
  m[0] = "  gg  wp ";
  m[5] = "\\u202eflip";  // a bidi override is taken out
  m[15] = "ねこ";
  const auto parsed = QC::Parse(Json(m));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ((*parsed)[0], "gg wp");
  EXPECT_EQ((*parsed)[5], "flip");
  EXPECT_EQ((*parsed)[15], "ねこ");
  EXPECT_EQ((*parsed)[1], "");
}

TEST(QuickChat, CutsLongMessagesToChatLength)
{
  std::vector<std::string> m(16, "");
  m[3] = std::string(150, 'a');
  const auto parsed = QC::Parse(Json(m));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ((*parsed)[3].size(), 100u);
}

TEST(QuickChat, RefusesOtherShapes)
{
  EXPECT_FALSE(QC::Parse(Json(std::vector<std::string>(15, "a"))));
  EXPECT_FALSE(QC::Parse(Json(std::vector<std::string>(17, "a"))));
  EXPECT_FALSE(QC::Parse(Json(std::vector<std::string>(16, "a"), 2)));
  EXPECT_FALSE(
      QC::Parse("{\"version\": 1, \"messages\": [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]}"));
  EXPECT_FALSE(QC::Parse("[]"));
  EXPECT_FALSE(QC::Parse("not json"));
}

TEST(QuickChat, LeavesTheDpadAloneWhenItCantBeUsed)
{
  const u16 up = PAD_BUTTON_UP | PAD_BUTTON_A;
  // Nothing set.
  Ready(1000, QC::Messages{});
  EXPECT_EQ(Filter(up, 1000), up);
  // Not on the CSS.
  Ready(1000);
  QC::SetOnCss(false);
  EXPECT_EQ(Filter(up, 1000), up);
  // The chat window not shown, or not drawn for a while.
  Ready(1000);
  QC::SetChatShown(false, 1000);
  EXPECT_EQ(Filter(up, 1000), up);
  Ready(1000);
  EXPECT_EQ(Filter(up, 1000 + QC::SHOWN_STALE_MS + 1), up);
  EXPECT_FALSE(QC::Usable(1000 + QC::SHOWN_STALE_MS + 1));
}

TEST(QuickChat, OpensAGroupAndSendsItsMessage)
{
  Ready(1000);
  // The D-pad never reaches the game; the other buttons do.
  EXPECT_EQ(Filter(PAD_BUTTON_UP | PAD_BUTTON_A, 1000), PAD_BUTTON_A);
  auto v = QC::Tick(1000, nullptr);
  EXPECT_EQ(v.state, QC::View::State::Open);
  EXPECT_EQ(v.group, QC::Up);
  // Held: one press.
  EXPECT_EQ(Filter(PAD_BUTTON_UP, 1010), 0);
  EXPECT_EQ(QC::Tick(1010, nullptr).state, QC::View::State::Open);
  EXPECT_TRUE(s_sent.empty());
  // Let go, right: "one more?".
  Filter(0, 1020);
  Filter(PAD_BUTTON_RIGHT, 1030);
  auto result = QC::Result::None;
  v = QC::Tick(1030, &result);
  EXPECT_EQ(result, QC::Result::Sent);
  EXPECT_EQ(v.state, QC::View::State::Sent);
  EXPECT_EQ(v.chosen, QC::Right);
  ASSERT_EQ(s_sent.size(), 1u);
  EXPECT_EQ(s_sent[0], "one more?");
  EXPECT_EQ(QC::Tick(1030 + QC::SENT_MS, nullptr).state, QC::View::State::Closed);
}

TEST(QuickChat, SameDirectionTwiceSendsThatOne)
{
  Ready(1000);
  QC::PressForTesting(QC::Up);
  QC::PressForTesting(QC::Up);
  QC::Tick(1000, nullptr);
  ASSERT_EQ(s_sent.size(), 1u);
  EXPECT_EQ(s_sent[0], "ggs");
}

TEST(QuickChat, EmptyGroupsDontOpenAndEmptySlotsSendNothing)
{
  Ready(1000);
  QC::PressForTesting(QC::Right);  // nothing in Right's group
  EXPECT_EQ(QC::Tick(1000, nullptr).state, QC::View::State::Closed);
  QC::PressForTesting(QC::Left);
  QC::PressForTesting(QC::Up);  // Left's Up is empty: still open
  const auto v = QC::Tick(1000, nullptr);
  EXPECT_EQ(v.state, QC::View::State::Open);
  EXPECT_EQ(v.group, QC::Left);
  EXPECT_TRUE(s_sent.empty());
}

TEST(QuickChat, ClosesAfterAWhileOrOnB)
{
  Ready(1000);
  QC::PressForTesting(QC::Up);
  QC::Tick(1000, nullptr);
  QC::SetChatShown(true, 1000 + QC::OPEN_MS);
  EXPECT_EQ(QC::Tick(1000 + QC::OPEN_MS, nullptr).state, QC::View::State::Closed);

  Ready(1000);
  Filter(PAD_BUTTON_UP, 1000);
  QC::Tick(1000, nullptr);
  // B closes the group and doesn't reach the game until it is let go, the group closed or not.
  EXPECT_EQ(Filter(PAD_BUTTON_B, 1010), 0);
  EXPECT_EQ(QC::Tick(1010, nullptr).state, QC::View::State::Closed);
  EXPECT_EQ(Filter(PAD_BUTTON_B, 1020), 0);
  EXPECT_EQ(Filter(0, 1030), 0);
  EXPECT_EQ(Filter(PAD_BUTTON_B, 1040), PAD_BUTTON_B);
  EXPECT_TRUE(s_sent.empty());
}

TEST(QuickChat, AFailedSendStaysOpen)
{
  Ready(1000);
  s_answer = QC::Result::TooFast;
  QC::PressForTesting(QC::Left);
  QC::PressForTesting(QC::Down);
  auto result = QC::Result::None;
  const auto v = QC::Tick(1000, &result);
  EXPECT_EQ(result, QC::Result::TooFast);
  EXPECT_EQ(v.state, QC::View::State::Open);
  ASSERT_EQ(s_sent.size(), 1u);
  EXPECT_EQ(s_sent[0], "brb");
}

TEST(QuickChat, OnlyPortsZeroToThree)
{
  Ready(1000);
  EXPECT_EQ(Filter(PAD_BUTTON_UP, 1000, 4), PAD_BUTTON_UP);
  EXPECT_EQ(Filter(PAD_BUTTON_UP, 1000, -1), PAD_BUTTON_UP);
  EXPECT_EQ(Filter(PAD_BUTTON_UP, 1000, 3), 0);
}
