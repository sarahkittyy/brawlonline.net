// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The online chat window (VideoCommon/OnlineChatUI.cpp) driven frame by frame through ImGui, with
// the mouse as the render widget passes it (OnScreenUI::SetMousePos / SetMousePress): a room's chat
// with another player, whose name is clicked in the menu bar and on a message line.

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>
#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Online/Chat.h"
#include "Core/Online/ChatCrypto.h"
#include "Core/Online/Rooms.h"
#include "VideoCommon/OnlineChatUI.h"

namespace CC = Online::ChatCrypto;
namespace Chat = Online::Chat;
namespace UI = VideoCommon::OnlineChatUI;

namespace
{
constexpr char GROUP[] = "room-KFQB-12";
constexpr char ME[] = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr char BOB[] = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

CC::Key32 Fill(u8 v)
{
  CC::Key32 k;
  k.fill(v);
  return k;
}

std::string Json(const picojson::object& o)
{
  return picojson::value(o).serialize();
}

// A base layer that keeps what was last saved to it (Dolphin's own writes the SYSCONF too, which
// needs a NAND).
class SavedLayer final : public Config::ConfigLayerLoader
{
public:
  explicit SavedLayer(std::optional<std::string>* chat_text_size,
                      std::optional<std::string>* chat_window = nullptr)
      : ConfigLayerLoader(Config::LayerType::Base), m_chat_text_size(chat_text_size),
        m_chat_window(chat_window)
  {
  }
  void Load(Config::Layer*) override {}
  void Save(Config::Layer* layer) override
  {
    *m_chat_text_size = layer->Get<std::string>(Config::ONLINE_CHAT_TEXT_SIZE.GetLocation());
    if (m_chat_window)
      *m_chat_window = layer->Get<std::string>(Config::ONLINE_CHAT_WINDOW.GetLocation());
  }

private:
  std::optional<std::string>* m_chat_text_size;
  std::optional<std::string>* m_chat_window;
};

class OnlineChatUITest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_dir = File::CreateTempDir();
    ASSERT_FALSE(m_dir.empty());
    File::SetUserPath(D_USER_IDX, m_dir + DIR_SEP);
    Chat::ResetForTesting(ME);
    m_my_id = Chat::IdentityPublicKeyHex();
    ASSERT_EQ(m_my_id.size(), 64u);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    UI::SetFont(nullptr);
    UI::ResetForTesting();
  }
  void TearDown() override
  {
    ImGui::DestroyContext();
    Chat::ResetForTesting(ME);
    File::DeleteDirRecursively(m_dir);
  }

  picojson::object Member(const std::string& uid, const std::string& name, const std::string& id,
                          std::optional<CC::Key32> kx, std::optional<CC::Sig64> sig)
  {
    picojson::object m;
    m["uid"] = picojson::value(uid);
    m["displayName"] = picojson::value(name);
    m["connectCode"] = picojson::value(name.substr(0, 3) + "#1");
    m["idKey"] = picojson::value(id);
    m["kx"] = kx ? picojson::value(CC::ToHex(*kx)) : picojson::value();
    m["kxSig"] = sig ? picojson::value(CC::ToHex(*sig)) : picojson::value();
    return m;
  }

  std::string Group(const picojson::array& members)
  {
    picojson::object o;
    o["type"] = picojson::value("chat-group");
    o["group"] = picojson::value(GROUP);
    o["kind"] = picojson::value("room");
    o["you"] = picojson::value(ME);
    o["members"] = picojson::value(members);
    return Json(o);
  }

  // A room with Bob, who has said something.
  void JoinWithBob()
  {
    const CC::Identity& bob_id = m_bob_id;
    const CC::GroupKey bob_kx = CC::GroupKeyFromSecret(Fill(102));
    const auto bob = Member(BOB, "bob", CC::ToHex(bob_id.PublicKey()), bob_kx.public_key,
                            bob_id.Sign(CC::KxSigMessage(GROUP, bob_kx.public_key)));
    ASSERT_TRUE(Chat::OnServerMessage(
        Group({picojson::value(Member(ME, "me", m_my_id, {}, {})), picojson::value(bob)})));
    std::optional<CC::Key32> my_kx;
    for (const std::string& json : Chat::TakeOutbox())
    {
      picojson::value v;
      ASSERT_TRUE(picojson::parse(v, json).empty());
      auto& o = v.get<picojson::object>();
      if (o["type"].get<std::string>() == "chat-key")
        my_kx = CC::FromHexFixed<32>(o["kx"].get<std::string>());
    }
    ASSERT_TRUE(my_kx);
    m_bob_key = *CC::PairKey(bob_kx, *my_kx, GROUP);
    BobSays("hello");
  }

  void BobSays(const std::string& text)
  {
    const u64 seq = ++m_bob_seq;
    const CC::Sig64 sig = m_bob_id.Sign(CC::MessageSigMessage(GROUP, BOB, seq, text));
    picojson::object msg;
    msg["type"] = picojson::value("chat-msg");
    msg["group"] = picojson::value(GROUP);
    msg["from"] = picojson::value(BOB);
    msg["box"] = picojson::value(CC::ToHex(CC::Seal(m_bob_key, CC::RandomNonce(),
                                                    CC::AssociatedData(GROUP, BOB, ME),
                                                    CC::EncodePlaintext(seq, sig, text))));
    ASSERT_TRUE(Chat::OnServerMessage(Json(msg)));
  }

  UI::TestFrame Frame()
  {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    UI::Display(1.0f);
    ImGui::Render();
    UI::TestFrame f = UI::LastFrameForTesting();
    m_frames.push_back(f);
    return f;
  }

  // The mouse as RenderWidget passes it: moves, then the button mask on press and on release.
  void Click(float x, float y, int button)
  {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    Frame();
    io.AddMouseButtonEvent(button, true);
    Frame();
    io.AddMouseButtonEvent(button, false);
    Frame();
  }

  // The middle of an item's rectangle (x0, y0, x1, y1).
  static ImVec2 Centre(const std::array<float, 4>& r)
  {
    return ImVec2((r[0] + r[2]) / 2, (r[1] + r[3]) / 2);
  }
  void ClickItem(const std::array<float, 4>& r)
  {
    const ImVec2 at = Centre(r);
    Click(at.x, at.y, 0);
    Frame();
    Frame();
  }

  // The title bar's collapse arrow (on the left, FramePadding in): clicked, and two frames after.
  void ClickArrow()
  {
    const UI::TestFrame& f = m_frames.back();
    const ImGuiStyle& style = ImGui::GetStyle();
    Click(f.x + style.FramePadding.x + f.font_px / 2, f.y + style.FramePadding.y + f.font_px / 2,
          0);
    Frame();
    Frame();
  }

  std::string m_dir;
  std::string m_my_id;
  std::vector<UI::TestFrame> m_frames;
  const CC::Identity m_bob_id{Fill(2)};
  CC::Key32 m_bob_key{};
  u64 m_bob_seq = 0;
};
}  // namespace

TEST_F(OnlineChatUITest, ClickingANameOpensItsMenu)
{
  JoinWithBob();
  UI::TestFrame f;
  for (int i = 0; i < 5; ++i)
    f = Frame();
  ASSERT_FALSE(f.collapsed);
  ASSERT_EQ(f.names.size(), 2u);  // the menu bar's and the message line's

  for (int button : {0, 1})
  {
    for (size_t n = 0; n < 2; ++n)
    {
      const UI::TestFrame::Name name = Frame().names.at(n);
      SCOPED_TRACE(testing::Message() << "button " << button << " name " << n);
      Click((name.x0 + name.x1) / 2, (name.y0 + name.y1) / 2, button);
      bool open = false;
      for (int i = 0; i < 4; ++i)
        open = Frame().member_menu_open;
      EXPECT_TRUE(open);
      EXPECT_EQ(m_frames.back().menu_uid, BOB);
      // Away from it: closed again.
      Click(1200, 700, 0);
      Frame();
      EXPECT_FALSE(Frame().member_menu_open);
    }
  }
}

TEST_F(OnlineChatUITest, NamesWorkWithAMenuOpenAndQuickClicks)
{
  JoinWithBob();
  for (int i = 0; i < 5; ++i)
    Frame();
  const UI::TestFrame::Name a = m_frames.back().names.at(0);
  const UI::TestFrame::Name b = m_frames.back().names.at(1);
  // A press and its release in one frame (a quick click between two frames).
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent((a.x0 + a.x1) / 2, (a.y0 + a.y1) / 2);
  Frame();
  io.AddMouseButtonEvent(0, true);
  io.AddMouseButtonEvent(0, false);
  for (int i = 0; i < 4; ++i)
    Frame();
  EXPECT_TRUE(m_frames.back().member_menu_open);
  // With the menu open, a click on the other name (outside the menu) opens the menu again.
  Click((b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, 1);
  for (int i = 0; i < 4; ++i)
    Frame();
  EXPECT_TRUE(m_frames.back().member_menu_open);
}

// The box keeps focus after a message is sent, so the player is often typing when they click a
// name: the first click must open the menu (ImGui doesn't hover other items while the box is the
// active item; a right click doesn't even take the focus from it).
TEST_F(OnlineChatUITest, ClickingANameWhileTypingOpensItsMenu)
{
  JoinWithBob();
  for (int i = 0; i < 5; ++i)
    Frame();
  for (int button : {0, 1})
  {
    for (size_t n = 0; n < 2; ++n)
    {
      SCOPED_TRACE(testing::Message() << "button " << button << " name " << n);
      UI::Activate();
      for (int i = 0; i < 6; ++i)
        Frame();
      ASSERT_TRUE(m_frames.back().typing);
      const UI::TestFrame::Name name = m_frames.back().names.at(n);
      Click((name.x0 + name.x1) / 2, (name.y0 + name.y1) / 2, button);
      bool open = false;
      for (int i = 0; i < 4; ++i)
        open = Frame().member_menu_open;
      EXPECT_TRUE(open);
      Click(1200, 700, 0);
      Frame();
      EXPECT_FALSE(Frame().member_menu_open);
    }
  }
}

// In a match an idle window fades; the mouse over it brings it back, and it must stay back while a
// button is held on it (a press on a name used to read as the mouse leaving: the window flashed
// faded until the button was let go).
TEST_F(OnlineChatUITest, PressingOnAFadedWindowDoesntFadeIt)
{
  JoinWithBob();
  picojson::object match;
  match["op"] = picojson::value("screen");
  match["screen"] = picojson::value(5.0);  // Rooms::Screen::Match
  ASSERT_FALSE(Online::Rooms::HarnessRequest(match));
  struct ScreenBack
  {
    ~ScreenBack()
    {
      picojson::object other;
      other["op"] = picojson::value("screen");
      other["screen"] = picojson::value(0.0);
      Online::Rooms::HarnessRequest(other);
    }
  } screen_back;

  ImGui::GetIO().AddMousePosEvent(1200, 700);
  UI::TestFrame f;
  for (int i = 0; i < 5; ++i)
    f = Frame();
  EXPECT_LT(f.alpha, 0.5f);  // faded: idle (no delay in tests) and the mouse away

  const UI::TestFrame::Name name = f.names.at(0);
  const float x = (name.x0 + name.x1) / 2, y = (name.y0 + name.y1) / 2;
  m_frames.clear();
  ImGui::GetIO().AddMousePosEvent(x, y);
  Frame();
  Frame();
  const size_t hovering = m_frames.size();
  ImGui::GetIO().AddMouseButtonEvent(0, true);
  for (int i = 0; i < 4; ++i)
    Frame();
  ImGui::GetIO().AddMouseButtonEvent(0, false);
  for (int i = 0; i < 4; ++i)
    Frame();
  for (size_t i = hovering - 1; i < m_frames.size(); ++i)
    EXPECT_EQ(m_frames[i].alpha, 1.0f) << "frame " << i;
  EXPECT_TRUE(m_frames.back().member_menu_open);
}

TEST_F(OnlineChatUITest, ClicksDontMoveOrResizeTheWindow)
{
  JoinWithBob();
  for (int i = 0; i < 5; ++i)
    Frame();
  const UI::TestFrame before = m_frames.back();
  m_frames.clear();
  // In the window's empty space, on the menu bar, on a name, and outside the window.
  const UI::TestFrame::Name name = before.names.at(1);
  Click(before.x + before.w / 2, before.y + before.h / 2, 0);
  Click(before.x + before.w - 10, before.y + 30, 0);
  Click((name.x0 + name.x1) / 2, (name.y0 + name.y1) / 2, 0);
  Click(1200, 700, 0);
  Click(before.x + before.w / 2, before.y + before.h / 2, 1);
  for (const UI::TestFrame& f : m_frames)
  {
    EXPECT_EQ(f.x, before.x);
    EXPECT_EQ(f.y, before.y);
    EXPECT_EQ(f.w, before.w);
    EXPECT_EQ(f.h, before.h);
    EXPECT_FALSE(f.collapsed);
  }
}

// Options > Increase / Decrease font size: the menu stays open between clicks, each click is one
// step, and the size is saved ([Online] ChatTextSize), so the next game's window has it too.
TEST_F(OnlineChatUITest, OptionsMenuChangesTheTextSizeAndKeepsIt)
{
  std::optional<std::string> saved;
  Config::AddLayer(std::make_unique<SavedLayer>(&saved));
  struct ConfigBack
  {
    ~ConfigBack() { Config::Shutdown(); }
  } config_back;

  JoinWithBob();
  for (int i = 0; i < 5; ++i)
    Frame();
  const float base = m_frames.back().font_px;
  ASSERT_GT(base, 0.0f);
  EXPECT_EQ(Config::Get(Config::ONLINE_CHAT_TEXT_SIZE), 100);

  ClickItem(m_frames.back().options_menu);
  ASSERT_TRUE(m_frames.back().options_menu_open);
  for (int expected : {110, 125})
  {
    SCOPED_TRACE(testing::Message() << "increase to " << expected);
    ClickItem(m_frames.back().increase_item);
    EXPECT_TRUE(m_frames.back().options_menu_open);
    EXPECT_EQ(Config::Get(Config::ONLINE_CHAT_TEXT_SIZE), expected);
    EXPECT_NEAR(m_frames.back().font_px, base * expected / 100.0f, 0.01f);
  }
  ClickItem(m_frames.back().decrease_item);
  EXPECT_TRUE(m_frames.back().options_menu_open);
  EXPECT_EQ(Config::Get(Config::ONLINE_CHAT_TEXT_SIZE), 110);

  Click(1200, 700, 0);
  Frame();
  ASSERT_FALSE(Frame().options_menu_open);

  // Saved (to Dolphin.ini in Dolphin), and a new window starts at that size.
  EXPECT_EQ(saved, "110");
  UI::ResetForTesting();
  for (int i = 0; i < 3; ++i)
    Frame();
  EXPECT_NEAR(m_frames.back().font_px, base * 1.1f, 0.01f);

  // A value between two steps steps to the next one; one out of range is kept within them.
  Config::SetBaseOrCurrent(Config::ONLINE_CHAT_TEXT_SIZE, 117);
  ClickItem(m_frames.back().options_menu);
  ASSERT_TRUE(m_frames.back().options_menu_open);
  ClickItem(m_frames.back().decrease_item);
  EXPECT_EQ(Config::Get(Config::ONLINE_CHAT_TEXT_SIZE), 110);
  Config::SetBaseOrCurrent(Config::ONLINE_CHAT_TEXT_SIZE, 1000);
  Frame();
  EXPECT_NEAR(m_frames.back().font_px, base * 2.0f, 0.01f);
  Config::SetBaseOrCurrent(Config::ONLINE_CHAT_TEXT_SIZE, 0);
  Frame();
  EXPECT_NEAR(m_frames.back().font_px, base * 0.7f, 0.01f);
}

// The collapse arrow folds the window to its title bar at the top of the screen, right of the FPS
// box, saying how many messages came meanwhile; the arrow again opens it where it was. The fold is
// saved ([Online] ChatWindow), so the next game's window starts folded.
TEST_F(OnlineChatUITest, FoldingPutsTheTitleBarAtTheTopWithUnreadMessages)
{
  std::optional<std::string> text_size, window;
  Config::AddLayer(std::make_unique<SavedLayer>(&text_size, &window));
  struct ConfigBack
  {
    ~ConfigBack()
    {
      Config::Shutdown();
      UI::SetTopLeftBoxRight(0);
    }
  } config_back;
  UI::SetTopLeftBoxRight(100);

  JoinWithBob();
  for (int i = 0; i < 5; ++i)
    Frame();
  const UI::TestFrame open = m_frames.back();
  ASSERT_FALSE(open.collapsed);
  EXPECT_EQ(open.title, "Chat (T)");
  EXPECT_EQ(open.unread, 0);

  ClickArrow();
  UI::TestFrame f = m_frames.back();
  ASSERT_TRUE(f.collapsed);
  EXPECT_EQ(f.x, 108.0f);  // 8 px right of the FPS box
  EXPECT_EQ(f.y, 8.0f);
  EXPECT_LT(f.w, open.w / 2);
  EXPECT_LT(f.h, open.h / 2);
  EXPECT_EQ(f.title, "Chat (T)");
  ASSERT_TRUE(window);
  EXPECT_TRUE(window->ends_with(" 1")) << *window;
  const float narrow = f.w;

  BobSays("one");
  BobSays("two");
  Frame();
  f = Frame();
  EXPECT_TRUE(f.collapsed);
  EXPECT_EQ(f.unread, 2);
  EXPECT_EQ(f.title, "Chat (T) - 2 unread");
  EXPECT_EQ(f.x, 108.0f);
  EXPECT_GT(f.w, narrow);  // the whole title shows

  // With no FPS box, the corner itself, and the OSD messages go under it.
  UI::SetTopLeftBoxRight(0);
  f = Frame();
  EXPECT_EQ(f.x, 8.0f);
  EXPECT_EQ(UI::CornerBarBottom(), f.y + f.h);
  UI::SetTopLeftBoxRight(100);
  Frame();
  EXPECT_EQ(UI::CornerBarBottom(), 0.0f);

  // Dragging the folded bar doesn't move it.
  ImGuiIO& io = ImGui::GetIO();
  f = Frame();
  io.AddMousePosEvent(f.x + f.w - 4, f.y + f.h / 2);
  Frame();
  io.AddMouseButtonEvent(0, true);
  Frame();
  io.AddMousePosEvent(400, 300);
  Frame();
  Frame();
  io.AddMouseButtonEvent(0, false);
  Frame();
  f = Frame();
  EXPECT_TRUE(f.collapsed);
  EXPECT_EQ(f.x, 108.0f);
  EXPECT_EQ(f.y, 8.0f);

  ClickArrow();
  f = m_frames.back();
  EXPECT_FALSE(f.collapsed);
  EXPECT_EQ(f.x, open.x);
  EXPECT_EQ(f.y, open.y);
  EXPECT_EQ(f.w, open.w);
  EXPECT_EQ(f.h, open.h);
  EXPECT_EQ(f.unread, 0);
  EXPECT_EQ(f.title, "Chat (T)");
  EXPECT_TRUE(window->ends_with(" 0")) << *window;

  // Read: folding again counts from here.
  ClickArrow();
  f = m_frames.back();
  EXPECT_TRUE(f.collapsed);
  EXPECT_EQ(f.unread, 0);
  EXPECT_EQ(f.title, "Chat (T)");

  // The next game's window (the same Dolphin.ini) starts folded at the top, and opens where it was.
  UI::ResetForTesting();
  ImGui::DestroyContext();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  ImGui::GetIO().Fonts->AddFontDefault();
  for (int i = 0; i < 3; ++i)
    f = Frame();
  EXPECT_TRUE(f.collapsed);
  EXPECT_EQ(f.x, 108.0f);
  EXPECT_EQ(f.y, 8.0f);
  ClickArrow();
  f = m_frames.back();
  EXPECT_FALSE(f.collapsed);
  EXPECT_NEAR(f.x, open.x, 1.0f);
  EXPECT_NEAR(f.y, open.y, 1.0f);
  EXPECT_NEAR(f.w, open.w, 1.0f);
  EXPECT_NEAR(f.h, open.h, 1.0f);
}

// The chat key on a folded window opens it with the box focused; leaving the box folds it again,
// and the saved state stays folded. On an open window it changes nothing.
TEST_F(OnlineChatUITest, ChatKeyOpensAFoldedWindowUntilTypingEnds)
{
  std::optional<std::string> text_size, window;
  Config::AddLayer(std::make_unique<SavedLayer>(&text_size, &window));
  struct ConfigBack
  {
    ~ConfigBack() { Config::Shutdown(); }
  } config_back;

  JoinWithBob();
  for (int i = 0; i < 5; ++i)
    Frame();
  const UI::TestFrame open = m_frames.back();
  ClickArrow();
  ASSERT_TRUE(m_frames.back().collapsed);
  ASSERT_TRUE(window && window->ends_with(" 1"));

  ImGuiIO& io = ImGui::GetIO();
  const auto escape = [&] {
    io.AddKeyEvent(ImGuiKey_Escape, true);
    Frame();
    Frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    for (int i = 0; i < 4; ++i)
      Frame();
    return m_frames.back();
  };

  UI::Activate();
  UI::TestFrame f;
  for (int i = 0; i < 4; ++i)
    f = Frame();
  EXPECT_FALSE(f.collapsed);
  EXPECT_EQ(f.x, open.x);
  EXPECT_EQ(f.w, open.w);
  EXPECT_TRUE(f.typing);
  EXPECT_TRUE(window->ends_with(" 1")) << *window;

  f = escape();
  EXPECT_FALSE(f.typing);
  EXPECT_TRUE(f.collapsed);
  EXPECT_EQ(f.y, 8.0f);
  EXPECT_TRUE(window->ends_with(" 1")) << *window;

  // Open, the key leaves it open.
  ClickArrow();
  ASSERT_FALSE(m_frames.back().collapsed);
  EXPECT_TRUE(window->ends_with(" 0")) << *window;
  UI::Activate();
  for (int i = 0; i < 4; ++i)
    f = Frame();
  EXPECT_TRUE(f.typing);
  f = escape();
  EXPECT_FALSE(f.typing);
  EXPECT_FALSE(f.collapsed);
  EXPECT_TRUE(window->ends_with(" 0")) << *window;
}

// The old "Show during matches" turned off reads as folded, once.
TEST_F(OnlineChatUITest, ShowDuringMatchesOffStartsFolded)
{
  std::optional<std::string> text_size, window;
  Config::AddLayer(std::make_unique<SavedLayer>(&text_size, &window));
  struct ConfigBack
  {
    ~ConfigBack() { Config::Shutdown(); }
  } config_back;
  Config::SetBaseOrCurrent(Config::ONLINE_CHAT_IN_MATCHES, false);

  JoinWithBob();
  UI::TestFrame f;
  for (int i = 0; i < 3; ++i)
    f = Frame();
  EXPECT_TRUE(f.collapsed);
  EXPECT_EQ(f.unread, 1);  // Bob's hello
  EXPECT_EQ(f.title, "Chat (T) - 1 unread");
  EXPECT_TRUE(Config::Get(Config::ONLINE_CHAT_IN_MATCHES));  // the key is gone
  ASSERT_TRUE(window);
  EXPECT_TRUE(window->ends_with(" 1")) << *window;
}

TEST_F(OnlineChatUITest, TitleNamesTheChatKey)
{
  struct KeyBack
  {
    ~KeyBack() { UI::SetChatKey("T"); }
  } key_back;
  JoinWithBob();
  UI::SetChatKey("`RETURN`");
  EXPECT_EQ(Frame().title, "Chat (Enter)");
  UI::SetChatKey("");
  EXPECT_EQ(Frame().title, "Chat");
  UI::SetChatKey("`numbersign`");
  EXPECT_EQ(Frame().title, "Chat (#)");
}

TEST(OnlineChatKeyLabel, NamesTheKeyAsTheLauncherDoes)
{
  EXPECT_EQ(UI::KeyLabel("T"), "T");
  EXPECT_EQ(UI::KeyLabel("`T`"), "T");
  EXPECT_EQ(UI::KeyLabel("`t`"), "T");
  EXPECT_EQ(UI::KeyLabel(" `RETURN` "), "Enter");
  EXPECT_EQ(UI::KeyLabel("`Return`"), "Enter");
  EXPECT_EQ(UI::KeyLabel("`DInput/0/Keyboard Mouse:GRAVE`"), "`");
  EXPECT_EQ(UI::KeyLabel("`slash`"), "/");
  EXPECT_EQ(UI::KeyLabel("`/`"), "/");
  EXPECT_EQ(UI::KeyLabel("`numbersign`"), "#");
  EXPECT_EQ(UI::KeyLabel("`space`"), "Space");
  EXPECT_EQ(UI::KeyLabel("F5"), "F5");
  EXPECT_EQ(UI::KeyLabel(""), "");
  EXPECT_EQ(UI::KeyLabel("@(`Ctrl`+`T`)"), "@(`Ctrl`+`T`)");
}
