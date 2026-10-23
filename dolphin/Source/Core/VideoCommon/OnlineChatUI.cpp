// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/OnlineChatUI.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Timer.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Online/Chat.h"
#include "Core/Online/ChatText.h"
#include "Core/Online/Rooms.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "VideoCommon/Present.h"

namespace VideoCommon::OnlineChatUI
{
namespace
{
using Online::Chat::Line;

// In a match, an idle window fades to this after FADE_AFTER_MS without a new message.
constexpr float FADED_ALPHA = 0.35f;
constexpr u64 FADE_AFTER_MS = 8000;
u64 s_fade_after_ms = FADE_AFTER_MS;  // 0 in unit tests
constexpr u64 NOTICE_MS = 3000;
// The saved geometry: "2 x y w h collapsed", fractions of the window in 1/10000. The leading 2 is
// the format's version: layouts saved before it (five numbers, from when the text and the default
// window were about twice as big) are dropped for the new default.
constexpr int GEOMETRY_VERSION = 2;
constexpr int GEOMETRY_UNIT = 10000;
// Frames the box may take to get the keyboard back after a message is sent (SetKeyboardFocusHere
// activates it a frame or two later); the game's keyboard stays blocked meanwhile.
constexpr int REFOCUS_FRAMES = 4;
// The window's ImGui id (its title changes: "Chat (T) - 2 unread").
constexpr const char* WINDOW_ID = "###OnlineChat";

// Port colours (P1 red, P2 blue, P3 yellow, P4 green), by joining order.
constexpr std::array<ImU32, 4> NAME_COLORS = {
    IM_COL32(255, 110, 110, 255),
    IM_COL32(110, 160, 255, 255),
    IM_COL32(245, 215, 90, 255),
    IM_COL32(110, 215, 120, 255),
};
constexpr ImU32 SYSTEM_COLOR = IM_COL32(170, 170, 180, 255);
constexpr ImU32 SPOILER_COLOR = IM_COL32(58, 60, 68, 255);
constexpr ImU32 SPOILER_HOVER_COLOR = IM_COL32(78, 80, 90, 255);
// The chat's text height, in logical pixels (times the backbuffer scale, the screen's DPI scale):
// about standard ImGui's 13 px in a 720-line window, growing with the cube root-ish of the
// window's height (14.7 at 1080, 16 at 1440, 18 at 2160), so a big window doesn't get big text.
constexpr float FONT_BASE_PX = 13.0f;
constexpr float FONT_BASE_HEIGHT = 720.0f;
constexpr float FONT_GROWTH = 0.3f;
constexpr float MIN_FONT_PX = 12.0f;
constexpr float MAX_FONT_PX = 18.0f;
// The player's text size (Options > Increase / Decrease font size, [Online] ChatTextSize), a
// percentage of that height. A saved value between two steps goes to the next step from it.
constexpr std::array<int, 9> TEXT_SIZE_STEPS = {70, 80, 90, 100, 110, 125, 150, 175, 200};

#ifdef __APPLE__
constexpr const char* SHORTCUT_PREFIX = "Cmd+";
#else
constexpr const char* SHORTCUT_PREFIX = "Ctrl+";
#endif

std::atomic<bool> s_activate{false};
std::atomic<bool> s_typing{false};
ImFont* s_font = nullptr;

// The chat key's label for the title ("T": Dolphin's default binding, until LoadConfig says).
std::mutex s_key_mutex;
std::string s_key_label = "T";
// The FPS box's right edge this frame (0: none in the corner), and the folded title bar's bottom
// when it took the corner itself (ImGui lock).
float s_top_left_right = 0;
float s_corner_bar_bottom = 0;

// The clipboard (SetClipboard), and ImGui's own functions for when there is none.
std::mutex s_clipboard_mutex;
Clipboard s_clipboard;
const char* (*s_default_get_clipboard)(ImGuiContext*) = nullptr;
void (*s_default_set_clipboard)(ImGuiContext*, const char*) = nullptr;
std::string s_clipboard_text;  // what the last get returned (under the ImGui lock)

struct Geometry
{
  ImVec2 pos;
  ImVec2 size;
  bool collapsed = false;
};

// What the box's callback does once the box has focus again (its menu, or after a message).
enum class Edit : u8
{
  None,
  Cut,        // delete the selection (already copied)
  Paste,      // the clipboard over the selection
  SelectAll,  // select everything
  Restore,    // the selection as it was (Copy)
  End,        // the cursor at the end, nothing selected (after Enter)
};

struct UiState
{
  std::optional<Online::Chat::View> view;
  u64 serial = 0;
  std::set<u64> revealed;  // ids of ignored players' lines clicked open
  std::array<char, Online::ChatText::MAX_MESSAGE_BYTES + 1> input{};
  bool focus_input = false;
  int refocus_frames = 0;
  bool was_typing = false;
  bool hovered = false;
  bool at_bottom = true;
  bool scroll_to_bottom = false;
  u64 newest_line = 0;

  // The box: Ctrl+W this frame, the menu's edit waiting for focus, the selection when it last had
  // focus (bytes), and whether it had focus when its menu was opened.
  bool delete_word = false;
  Edit edit = Edit::None;
  int sel_start = 0;
  int sel_end = 0;
  bool menu_was_typing = false;

  // Geometry: placed for this window size; what was applied; the player moving it.
  bool placed = false;
  ImVec2 display{};
  Geometry applied;
  bool interacting = false;

  // Folded to the title bar (at the top of the screen): what the last frame drew; opened by the
  // chat key while folded, until the box lets go of the keyboard (the saved state stays folded);
  // folding back next frame. read_up_to: the newest line the open window showed.
  bool drawn_collapsed = false;
  bool peek = false;
  bool fold_back = false;
  u64 read_up_to = 0;

  // Collapsed because every other member is ignored: the group it was decided for, and whether the
  // window is collapsed by that (not by the player) so the next group can open it again.
  u64 group_number = 0;
  bool all_ignored = false;
  bool collapse = false;
  bool expand = false;
  bool auto_collapsed = false;

  // The name menu.
  bool open_menu = false;
  std::string menu_uid;
  bool confirm_report = false;
  // A name pressed (its item id and the button), opened on release over it.
  ImGuiID name_press_id = 0;
  int name_press_button = -1;

  std::string notice;
  u64 notice_until = 0;
};
UiState s;

// Harness and unit tests: what each frame drew, while recording.
constexpr size_t MAX_RECORDED_FRAMES = 600;
std::atomic<bool> s_recording{false};
TestFrame s_frame;  // the frame being drawn (ImGui lock)
std::mutex s_frames_mutex;
std::deque<TestFrame> s_frames;

void SetNotice(std::string text)
{
  s.notice = std::move(text);
  s.notice_until = Common::Timer::NowMs() + NOTICE_MS;
}

float FontPx(float display_h, float scale)
{
  const float dpi = scale > 0.0f ? scale : 1.0f;
  const float logical_h = std::max(display_h / dpi, 1.0f);
  return dpi * std::clamp(FONT_BASE_PX * std::pow(logical_h / FONT_BASE_HEIGHT, FONT_GROWTH),
                          MIN_FONT_PX, MAX_FONT_PX);
}

int TextSizePercent()
{
  return std::clamp(Config::Get(Config::ONLINE_CHAT_TEXT_SIZE), TEXT_SIZE_STEPS.front(),
                    TEXT_SIZE_STEPS.back());
}

// The step above (up) or below the current size, the same one at the end of the steps.
int NextTextSize(int percent, bool up)
{
  if (up)
  {
    const auto it = std::ranges::upper_bound(TEXT_SIZE_STEPS, percent);
    return it == TEXT_SIZE_STEPS.end() ? TEXT_SIZE_STEPS.back() : *it;
  }
  const auto it = std::ranges::lower_bound(TEXT_SIZE_STEPS, percent);
  return it == TEXT_SIZE_STEPS.begin() ? TEXT_SIZE_STEPS.front() : *std::prev(it);
}

void SetTextSize(int percent)
{
  Config::SetBaseOrCurrent(Config::ONLINE_CHAT_TEXT_SIZE, percent);
  Config::Save();
}

// The Options menu's text size items. They leave the menu open, so several clicks in a row step
// through the sizes.
void DrawTextSizeItems()
{
  const int percent = TextSizePercent();
  ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
  if (ImGui::MenuItem("Increase font size", nullptr, false, percent < TEXT_SIZE_STEPS.back()))
    SetTextSize(NextTextSize(percent, true));
  if (s_recording)
  {
    s_frame.increase_item = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                             ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y};
  }
  if (ImGui::MenuItem("Decrease font size", nullptr, false, percent > TEXT_SIZE_STEPS.front()))
    SetTextSize(NextTextSize(percent, false));
  if (s_recording)
  {
    s_frame.decrease_item = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                             ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y};
  }
  if (ImGui::MenuItem(fmt::format("Reset font size ({}%)", percent).c_str(), nullptr, false,
                      percent != 100))
  {
    SetTextSize(100);
  }
  ImGui::PopItemFlag();
}

std::optional<Geometry> LoadGeometry(ImVec2 display)
{
  const std::string text = Config::Get(Config::ONLINE_CHAT_WINDOW);
  int version, x, y, w, h, c;
  if (text.size() > 64 ||
      std::sscanf(text.c_str(), "%d %d %d %d %d %d", &version, &x, &y, &w, &h, &c) != 6 ||
      version != GEOMETRY_VERSION)
  {
    return std::nullopt;
  }
  const auto in_range = [](int v) { return v >= 0 && v <= GEOMETRY_UNIT; };
  if (!in_range(x) || !in_range(y) || !in_range(w) || !in_range(h) || w < GEOMETRY_UNIT / 50 ||
      h < GEOMETRY_UNIT / 50)
  {
    return std::nullopt;
  }
  const auto f = [](int v, float total) { return v * total / GEOMETRY_UNIT; };
  return Geometry{{f(x, display.x), f(y, display.y)}, {f(w, display.x), f(h, display.y)}, c != 0};
}

void SaveGeometry(const Geometry& g, ImVec2 display)
{
  if (display.x <= 0 || display.y <= 0)
    return;
  const auto u = [](float v, float total) {
    return std::clamp(static_cast<int>(v / total * GEOMETRY_UNIT + 0.5f), 0, GEOMETRY_UNIT);
  };
  Config::SetBaseOrCurrent(Config::ONLINE_CHAT_WINDOW,
                           fmt::format("{} {} {} {} {} {}", GEOMETRY_VERSION, u(g.pos.x, display.x),
                                       u(g.pos.y, display.y), u(g.size.x, display.x),
                                       u(g.size.y, display.y), g.collapsed ? 1 : 0));
  Config::Save();
}

// In the side bar when the game is 4:3 in a wider window, else the game's top-left corner. A few
// lines of a few words: it covers little of the game (the player can make it bigger).
Geometry DefaultGeometry(ImVec2 display, float scale, float font_px)
{
  MathUtil::Rectangle<int> r = g_presenter ? g_presenter->GetTargetRectangle() :
                                             MathUtil::Rectangle<int>{};
  if (r.right <= r.left || r.bottom <= r.top)
    r = {0, 0, static_cast<int>(display.x), static_cast<int>(display.y)};
  const float game_w = static_cast<float>(r.right - r.left);
  const float game_h = static_cast<float>(r.bottom - r.top);
  const float bar = static_cast<float>(r.left);
  Geometry g;
  if (bar >= 170.0f * scale)
  {
    g.pos = {8.0f * scale, r.top + 8.0f * scale};
    g.size = {std::min(bar - 16.0f * scale, 24.0f * font_px),
              std::min(16.0f * font_px, game_h * 0.45f)};
  }
  else
  {
    g.pos = {r.left + 12.0f * scale, r.top + 12.0f * scale};
    g.size = {std::min(22.0f * font_px, game_w * 0.32f), std::min(12.0f * font_px, game_h * 0.35f)};
  }
  return g;
}

bool Differs(const Geometry& a, const Geometry& b)
{
  const auto apart = [](float x, float y) { return std::abs(x - y) > 1.0f; };
  return apart(a.pos.x, b.pos.x) || apart(a.pos.y, b.pos.y) || apart(a.size.x, b.size.x) ||
         apart(a.size.y, b.size.y) || a.collapsed != b.collapsed;
}

const Online::Chat::Member* FindMember(std::string_view uid)
{
  if (!s.view)
    return nullptr;
  const auto it = std::ranges::find(s.view->members, uid, &Online::Chat::Member::uid);
  return it == s.view->members.end() ? nullptr : &*it;
}

// Everyone else in the group is ignored (the opponent of a 1v1, every other player of a room).
bool AllOthersIgnored(const Online::Chat::View& v)
{
  bool anyone = false;
  for (const Online::Chat::Member& m : v.members)
  {
    if (m.is_me)
      continue;
    if (!m.hidden)
      return false;
    anyone = true;
  }
  return anyone;
}

// Other players' messages that came while the window was folded (an ignored player's don't count).
int UnreadCount()
{
  int unread = 0;
  for (auto it = s.view->lines.rbegin(); it != s.view->lines.rend() && it->id > s.read_up_to; ++it)
  {
    if (it->kind != Line::Kind::Message)
      continue;
    const Online::Chat::Member* m = FindMember(it->uid);
    if (!(m ? m->hidden : Online::Chat::IsHidden(it->uid)))
      ++unread;
  }
  return unread;
}

// "Chat (T)", and "Chat (T) - 2 unread" while folded with messages waiting ("Chat": no key).
std::string Title(int unread)
{
  std::string key;
  {
    std::lock_guard lk(s_key_mutex);
    key = s_key_label;
  }
  std::string title = key.empty() ? "Chat" : fmt::format("Chat ({})", key);
  if (unread > 0)
    title += fmt::format(" - {} unread", unread);
  return title;
}

ImU32 ColorOf(int color)
{
  return NAME_COLORS[static_cast<size_t>(color) % NAME_COLORS.size()];
}

// The Control key itself (ImGui swaps Control and Command on macOS, so there it reads as Super).
bool ControlKeyDown()
{
  const ImGuiIO& io = ImGui::GetIO();
  return io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
}

// A name drawn as text that can be clicked, left or right (an InvisibleButton under it: the name
// is never a widget label). Clicking another player's name opens their menu: on release over the
// name, whatever else holds ImGui's active item (the box keeps it after a message is sent) or
// whatever popup is open. The button covers the drawn text (drawn `y_offset` lower on the menu
// bar).
void NameButton(std::string_view text, ImU32 color, const std::string& uid, bool clickable,
                float y_offset = 0.0f)
{
  const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##name", ImVec2(std::max(size.x, 1.0f), size.y + y_offset),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const ImGuiID id = ImGui::GetItemID();
  // Also over a menu already open: clicking another name opens that player's menu instead of
  // only closing the open one.
  const bool hovered =
      clickable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                        ImGuiHoveredFlags_AllowWhenBlockedByPopup);
  bool clicked = false;
  for (const ImGuiMouseButton button : {ImGuiMouseButton_Left, ImGuiMouseButton_Right})
  {
    if (hovered && ImGui::IsMouseClicked(button))
    {
      s.name_press_id = id;
      s.name_press_button = button;
    }
    if (s.name_press_id == id && s.name_press_button == button && ImGui::IsMouseReleased(button))
    {
      s.name_press_id = 0;
      clicked = hovered;
    }
  }
  pos.y += y_offset;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  // Through GetColorU32: the window's alpha (faded in a match) applies to drawn text too.
  const ImU32 col = ImGui::GetColorU32(color);
  draw->AddText(pos, col, text.data(), text.data() + text.size());
  if (hovered)
    draw->AddLine({pos.x, pos.y + size.y}, {pos.x + size.x, pos.y + size.y}, col);
  if (s_recording && clickable)
  {
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    s_frame.names.push_back({uid, a.x, a.y, b.x, b.y});
  }
  if (clicked && clickable)
  {
    s.open_menu = true;
    s.menu_uid = uid;
    s.confirm_report = false;
  }
}

void DrawText(const std::string& text, ImU32 color)
{
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

void DrawLine(const Line& line, float scale)
{
  if (line.kind == Line::Kind::System)
  {
    DrawText(line.text, SYSTEM_COLOR);
    return;
  }
  ImGui::PushID(static_cast<int>(line.id));
  const bool own = line.kind == Line::Kind::Own;
  NameButton(line.name + ":", ColorOf(line.color), line.uid, !own);
  ImGui::SameLine(0.0f, ImGui::CalcTextSize(" ").x);

  // A sender who has left the group is no member any more: ask Chat (rare).
  const Online::Chat::Member* m = FindMember(line.uid);
  const bool ignored = !own && (m ? m->hidden : Online::Chat::IsHidden(line.uid)) &&
                       !s.revealed.contains(line.id);
  if (ignored)
  {
    // The spoiler: a box the size the text would take; a click shows this message only.
    const float avail = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    ImVec2 size = ImGui::CalcTextSize(line.text.data(), line.text.data() + line.text.size(), false,
                                      avail);
    size.x = std::max(size.x, 8.0f * scale);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##spoiler", size))
      s.revealed.insert(line.id);
    ImGui::GetWindowDrawList()->AddRectFilled(
        pos, {pos.x + size.x, pos.y + size.y},
        ImGui::GetColorU32(ImGui::IsItemHovered() ? SPOILER_HOVER_COLOR : SPOILER_COLOR),
        4.0f * scale);
  }
  else
  {
    DrawText(line.text, ImGui::GetColorU32(ImGuiCol_Text));
  }
  ImGui::PopID();
}

// A padlock drawn from shapes (no icon font), the height of a text line, at the cursor; hovering
// it says what it means.
void LockIcon(float scale)
{
  const float h = ImGui::GetTextLineHeight();
  const float w = 0.75f * h;
  const ImVec2 item = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##lock", ImVec2(w, h));
  if (ImGui::IsItemHovered())
  {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("Chat is E2E encrypted.");
    ImGui::EndTooltip();
  }
  // On the menu bar's text line, as the names.
  const ImVec2 p{item.x, item.y + ImGui::GetStyle().FramePadding.y};
  const ImU32 col = ImGui::GetColorU32(SYSTEM_COLOR);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float body_top = p.y + 0.45f * h;
  const float cx = p.x + 0.5f * w;
  const float r = 0.3f * w;
  const float thick = std::max(1.5f * scale, 0.1f * h);
  // The shackle: one path, left leg, round top, right leg (the arc starts where the left leg
  // ends; adding that point twice drew a nub at the join).
  draw->PathLineTo({cx - r, body_top});
  draw->PathArcTo({cx, p.y + 0.3f * h}, r, 3.14159265f, 2.0f * 3.14159265f);
  draw->PathLineTo({cx + r, body_top});
  draw->PathStroke(col, ImDrawFlags_None, thick);
  // The body and its keyhole.
  draw->AddRectFilled({p.x, body_top}, {p.x + w, p.y + h}, col, 0.12f * h);
  draw->AddCircleFilled({cx, body_top + 0.27f * h}, 0.09f * h,
                        ImGui::GetColorU32(ImGuiCol_MenuBarBg));
}

// In the menu bar after Options: the other players' names (click: their menu), then the room's
// code (or the opponent's connect code) at the right with the padlock after it.
void DrawMenuBarPlayers(float scale)
{
  const auto& v = *s.view;
  bool anyone = false;
  for (const Online::Chat::Member& m : v.members)
  {
    if (m.is_me)
      continue;
    anyone = true;
    ImGui::PushID(m.uid.c_str());
    // On the menu bar's text line (Options' label sits FramePadding.y below the item's top).
    NameButton(m.name, ColorOf(m.color), m.uid, true, ImGui::GetStyle().FramePadding.y);
    if (m.hidden || !m.has_key)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, SYSTEM_COLOR);
      ImGui::TextUnformatted(m.hidden ? "(ignored)" : "(connecting)");
      ImGui::PopStyleColor();
    }
    ImGui::PopID();
  }
  if (!anyone)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, SYSTEM_COLOR);
    ImGui::TextUnformatted("Nobody else here yet");
    ImGui::PopStyleColor();
  }
  const float gap = 6.0f * scale;
  const float lock_w = 0.75f * ImGui::GetTextLineHeight();
  const float code_w =
      v.code.empty() ? 0.0f : ImGui::CalcTextSize(v.code.data(), v.code.data() + v.code.size()).x;
  const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  const float start = right - lock_w - (v.code.empty() ? 0.0f : code_w + gap);
  if (start > ImGui::GetCursorPosX() + 8.0f * scale)
  {
    ImGui::SetCursorPosX(start);
    if (!v.code.empty())
    {
      ImGui::PushStyleColor(ImGuiCol_Text, SYSTEM_COLOR);
      ImGui::TextUnformatted(v.code.data(), v.code.data() + v.code.size());
      ImGui::PopStyleColor();
      ImGui::SameLine(0.0f, gap);
    }
    LockIcon(scale);
  }
}

void DrawMemberMenu()
{
  if (s.open_menu)
  {
    ImGui::OpenPopup("##member");
    s.open_menu = false;
  }
  if (!ImGui::BeginPopup("##member"))
    return;
  const Online::Chat::Member* m = FindMember(s.menu_uid);
  if (s_recording)
  {
    s_frame.member_menu_open = true;
    s_frame.menu_uid = s.menu_uid;
  }
  if (!m || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
  {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const std::string who = m->code.empty() ? m->name : fmt::format("{} ({})", m->name, m->code);
  ImGui::PushStyleColor(ImGuiCol_Text, ColorOf(m->color));
  ImGui::TextUnformatted(who.data(), who.data() + who.size());
  ImGui::PopStyleColor();
  ImGui::Separator();
  if (!s.confirm_report)
  {
    const bool toggled = ImGui::MenuItem(m->hidden ? "Unignore user" : "Ignore user");
    if (s_recording)
    {
      s_frame.ignore_item = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                             ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y};
    }
    if (toggled)
    {
      // Ignoring starts from all of their messages covered again.
      if (!m->hidden)
        s.revealed.clear();
      Online::Chat::SetHidden(m->uid, !m->hidden);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Report..."))
      s.confirm_report = true;
  }
  else
  {
    ImGui::TextUnformatted("Send their recent messages to the moderators?");
    if (ImGui::Button("Report"))
    {
      if (!Online::Chat::Report(m->uid, ""))
        SetNotice("Nothing to report.");
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// Clipboard text as the box takes it: line breaks and tabs become spaces, other controls and
// invalid UTF-8 are dropped, and at most `max_code_points` code points and `max_bytes` bytes.
std::string ForTheBox(std::string_view clip, size_t max_code_points, size_t max_bytes)
{
  std::string out;
  size_t code_points = 0;
  size_t pos = 0;
  while (pos < clip.size() && code_points < max_code_points)
  {
    const size_t start = pos;
    const auto c = Online::ChatText::DecodeUtf8(clip, &pos);
    if (!c)
    {
      ++pos;
      continue;
    }
    std::string_view bytes = clip.substr(start, pos - start);
    if (*c == '\n' || *c == '\r' || *c == '\t')
      bytes = " ";
    else if (*c < 0x20 || *c == 0x7F)
      continue;
    if (out.size() + bytes.size() > max_bytes)
      break;
    out.append(bytes);
    ++code_points;
  }
  return out;
}

// The box's callback, every frame it has focus: the menu's edit, Ctrl+W, the length limit, and
// the selection kept for the menu.
int BoxCallback(ImGuiInputTextCallbackData* data)
{
  namespace CT = Online::ChatText;
  const auto text = [data] {
    return std::string_view(data->Buf, static_cast<size_t>(data->BufTextLen));
  };
  if (s.edit != Edit::None)
  {
    const int a = std::clamp(std::min(s.sel_start, s.sel_end), 0, data->BufTextLen);
    const int b = std::clamp(std::max(s.sel_start, s.sel_end), 0, data->BufTextLen);
    switch (s.edit)
    {
    case Edit::Cut:
      data->DeleteChars(a, b - a);
      break;
    case Edit::Paste:
    {
      const char* clip = ImGui::GetClipboardText();
      data->DeleteChars(a, b - a);
      data->CursorPos = a;
      const size_t used_bytes = text().size();
      const size_t used_code_points = CT::CountCodePoints(text());
      const std::string paste =
          ForTheBox(clip ? clip : "",
                    CT::MAX_MESSAGE_CODE_POINTS - std::min(used_code_points,
                                                           CT::MAX_MESSAGE_CODE_POINTS),
                    CT::MAX_MESSAGE_BYTES - std::min(used_bytes, CT::MAX_MESSAGE_BYTES));
      data->InsertChars(a, paste.data(), paste.data() + paste.size());
      break;
    }
    case Edit::SelectAll:
      data->SelectAll();
      data->CursorPos = data->BufTextLen;
      break;
    case Edit::Restore:
      data->SelectionStart = a;
      data->SelectionEnd = b;
      data->CursorPos = b;
      break;
    case Edit::End:
      data->CursorPos = data->BufTextLen;
      data->SelectionStart = data->SelectionEnd = data->CursorPos;
      break;
    case Edit::None:
      break;
    }
    s.edit = Edit::None;
  }
  if (s.delete_word)
  {
    // Ctrl+W, as in a terminal: the selection if there is one, else back to the word's start.
    s.delete_word = false;
    if (data->HasSelection())
    {
      const int a = std::min(data->SelectionStart, data->SelectionEnd);
      data->DeleteChars(a, std::abs(data->SelectionEnd - data->SelectionStart));
    }
    else
    {
      const int cursor = std::clamp(data->CursorPos, 0, data->BufTextLen);
      const int start =
          static_cast<int>(CT::WordRuboutStart(text(), static_cast<size_t>(cursor)));
      data->DeleteChars(start, cursor - start);
    }
  }
  // At most 100 code points (the cleaner's limit), cut on a code point boundary.
  const std::string_view kept =
      CT::Truncate(text(), CT::MAX_MESSAGE_CODE_POINTS, CT::MAX_MESSAGE_BYTES);
  if (kept.size() < text().size())
  {
    data->DeleteChars(static_cast<int>(kept.size()),
                      static_cast<int>(text().size() - kept.size()));
  }
  s.sel_start = data->SelectionStart;
  s.sel_end = data->SelectionEnd;
  return 0;
}

// The box's right-click menu. Returns whether it is open. Must follow the box (the last item).
bool DrawBoxMenu()
{
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    s.menu_was_typing = s.was_typing;
  if (!ImGui::BeginPopupContextItem("##boxmenu"))
    return false;
  const std::string_view text(s.input.data());
  const int len = static_cast<int>(text.size());
  const int a = std::clamp(std::min(s.sel_start, s.sel_end), 0, len);
  const int b = std::clamp(std::max(s.sel_start, s.sel_end), 0, len);
  // The selection only counts if the box had focus (it isn't drawn otherwise).
  const bool selection = s.menu_was_typing && a < b;
  const std::string selected(text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
  const char* clip = ImGui::GetClipboardText();
  const bool can_paste = clip && clip[0] != '\0' && len < Online::ChatText::MAX_MESSAGE_BYTES;
  const auto item = [](const char* label, char key, bool enabled) {
    return ImGui::MenuItem(label, fmt::format("{}{}", SHORTCUT_PREFIX, key).c_str(), false,
                           enabled);
  };
  Edit edit = Edit::None;
  bool chosen = false;
  if (item("Cut", 'X', selection))
  {
    ImGui::SetClipboardText(selected.c_str());
    edit = Edit::Cut;
    chosen = true;
  }
  if (item("Copy", 'C', selection))
  {
    ImGui::SetClipboardText(selected.c_str());
    edit = Edit::Restore;
    chosen = true;
  }
  if (item("Paste", 'V', can_paste))
  {
    if (!s.menu_was_typing)
      s.sel_start = s.sel_end = len;  // at the end, where the cursor shows when the box opens
    edit = Edit::Paste;
    chosen = true;
  }
  ImGui::Separator();
  if (item("Select all", 'A', len > 0))
  {
    edit = Edit::SelectAll;
    chosen = true;
  }
  if (chosen)
  {
    // Done by the box's callback once it has focus again (the menu took it).
    s.edit = edit;
    s.focus_input = true;
    s.refocus_frames = REFOCUS_FRAMES;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  return true;
}

void Submit()
{
  const std::string text(s.input.data());
  switch (Online::Chat::Send(text))
  {
  case Online::Chat::SendResult::Sent:
  case Online::Chat::SendResult::Empty:
    s.input.fill(0);
    break;
  case Online::Chat::SendResult::NoGroup:
    SetNotice("Not in a room or match.");
    break;
  case Online::Chat::SendResult::NoReaders:
    SetNotice("Nobody can read chat yet.");
    break;
  case Online::Chat::SendResult::TooFast:
    SetNotice("You're sending messages too fast.");
    break;
  }
}

void StopTyping()
{
  s.was_typing = false;
  s.refocus_frames = 0;
  s.edit = Edit::None;
  s_typing = false;
  ciface::Core::SetKeyboardMouseBlocked(false);
}

// A new view: what changed for the ignored-players collapse.
void OnNewView()
{
  const bool ignored = AllOthersIgnored(*s.view);
  if (s.view->group_number != s.group_number)
  {
    // A new group: collapsed if everyone else in it is ignored, else open again if it was only
    // collapsed for the last group's ignored players.
    s.group_number = s.view->group_number;
    s.collapse = ignored;
    s.expand = !ignored && s.auto_collapsed;
  }
  else if (ignored && !s.all_ignored)
  {
    // The player (here or in the launcher) ignored the last one they were hearing from.
    s.collapse = true;
    s.expand = false;
  }
  s.all_ignored = ignored;
}
}  // namespace

void SetClipboard(Clipboard clipboard)
{
  std::lock_guard lk(s_clipboard_mutex);
  s_clipboard = std::move(clipboard);
}

void InstallClipboard()
{
  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  if (pio.Platform_GetClipboardTextFn != nullptr && s_default_get_clipboard == nullptr)
  {
    s_default_get_clipboard = pio.Platform_GetClipboardTextFn;
    s_default_set_clipboard = pio.Platform_SetClipboardTextFn;
  }
  pio.Platform_GetClipboardTextFn = [](ImGuiContext* ctx) -> const char* {
    std::function<std::string()> get;
    {
      std::lock_guard lk(s_clipboard_mutex);
      get = s_clipboard.get;
    }
    if (!get)
      return s_default_get_clipboard ? s_default_get_clipboard(ctx) : nullptr;
    s_clipboard_text = get();
    return s_clipboard_text.c_str();
  };
  pio.Platform_SetClipboardTextFn = [](ImGuiContext* ctx, const char* text) {
    std::function<void(const std::string&)> set;
    {
      std::lock_guard lk(s_clipboard_mutex);
      set = s_clipboard.set;
    }
    if (set)
      set(text ? text : "");
    else if (s_default_set_clipboard)
      s_default_set_clipboard(ctx, text);
  };
}

void SetFont(ImFont* font)
{
  s_font = font;
}

void Activate()
{
  s_activate = true;
}

std::string KeyLabel(std::string_view expression)
{
  constexpr std::string_view SPACES = " \t\r\n";
  const size_t first = expression.find_first_not_of(SPACES);
  if (first == std::string_view::npos)
    return "";
  expression = expression.substr(first, expression.find_last_not_of(SPACES) - first + 1);
  // One key: `name` or a bare name, its device qualifier ("DInput/0/Keyboard Mouse:") dropped.
  std::string_view name;
  if (expression.size() >= 2 && expression.front() == '`' && expression.back() == '`' &&
      expression.substr(1, expression.size() - 2).find('`') == std::string_view::npos)
  {
    name = expression.substr(1, expression.size() - 2);
  }
  else if (std::ranges::all_of(expression, [](char c) {
             return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_';
           }))
  {
    name = expression;
  }
  else
  {
    return std::string(expression);
  }
  name = name.substr(name.rfind(':') + 1);
  if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));
  // DirectInput's (Windows), Quartz's (macOS) and XInput2's (Linux, XKeysymToString) names; a name
  // that is its own label ("-", "F5", "ESCAPE") is shown as it is.
  using Name = std::pair<std::string_view, std::string_view>;
  static constexpr Name NAMES[] = {
      {"RETURN", "Enter"},   {"Return", "Enter"},      {"TAB", "Tab"},
      {"SPACE", "Space"},    {"space", "Space"},       {"GRAVE", "`"},
      {"Tilde", "`"},        {"grave", "`"},           {"MINUS", "-"},
      {"minus", "-"},        {"EQUALS", "="},          {"equal", "="},
      {"LBRACKET", "["},     {"bracketleft", "["},     {"RBRACKET", "]"},
      {"bracketright", "]"}, {"BACKSLASH", "\\"},      {"backslash", "\\"},
      {"SEMICOLON", ";"},    {"semicolon", ";"},       {"APOSTROPHE", "'"},
      {"apostrophe", "'"},   {"COMMA", ","},           {"comma", ","},
      {"PERIOD", "."},       {"period", "."},          {"SLASH", "/"},
      {"slash", "/"},        {"exclam", "!"},          {"quotedbl", "\""},
      {"numbersign", "#"},   {"dollar", "$"},          {"percent", "%"},
      {"ampersand", "&"},    {"parenleft", "("},       {"parenright", ")"},
      {"asterisk", "*"},     {"plus", "+"},            {"colon", ":"},
      {"less", "<"},         {"greater", ">"},         {"question", "?"},
      {"at", "@"},           {"asciicircum", "^"},     {"underscore", "_"},
      {"braceleft", "{"},    {"bar", "|"},             {"braceright", "}"},
      {"asciitilde", "~"},
  };
  const auto it = std::ranges::find(NAMES, name, &Name::first);
  return std::string(it != std::end(NAMES) ? it->second : name);
}

void SetChatKey(std::string_view expression)
{
  std::string label = KeyLabel(expression);
  // Never "##" in the window's title (ImGui reads it as an id).
  if (label.find("##") != std::string::npos)
    label.clear();
  std::lock_guard lk(s_key_mutex);
  s_key_label = std::move(label);
}

void SetTopLeftBoxRight(float right)
{
  s_top_left_right = right;
}

float CornerBarBottom()
{
  return s_corner_bar_bottom;
}

bool IsTyping()
{
  return s_typing;
}

void ResetForTesting()
{
  s = UiState{};
  s_fade_after_ms = 0;
  TakeRecordedFrames();
  SetRecording(true);
}

TestFrame LastFrameForTesting()
{
  std::lock_guard lk(s_frames_mutex);
  return s_frames.empty() ? TestFrame{} : s_frames.back();
}

void SetRecording(bool on)
{
  s_recording = on;
}

std::vector<TestFrame> TakeRecordedFrames()
{
  std::lock_guard lk(s_frames_mutex);
  std::vector<TestFrame> out(s_frames.begin(), s_frames.end());
  s_frames.clear();
  return out;
}

namespace
{
void DisplayFrame(float scale);
}

void Display(float scale)
{
  if (!s_recording)
  {
    DisplayFrame(scale);
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  s_frame = {};
  s_frame.frame = ImGui::GetFrameCount();
  s_frame.display_w = io.DisplaySize.x;
  s_frame.display_h = io.DisplaySize.y;
  s_frame.mouse_x = io.MousePos.x;
  s_frame.mouse_y = io.MousePos.y;
  for (int i = 0; i < 3; ++i)
    s_frame.mouse_down |= io.MouseDown[i] ? 1 << i : 0;
  DisplayFrame(scale);
  s_frame.typing = s.was_typing;
  std::lock_guard lk(s_frames_mutex);
  s_frames.push_back(s_frame);
  while (s_frames.size() > MAX_RECORDED_FRAMES)
    s_frames.pop_front();
}

namespace
{
void DisplayFrame(float scale)
{
  if (auto view = Online::Chat::GetView(s.serial))
  {
    s.serial = view->serial;
    s.view = std::move(view);
    std::erase_if(s.revealed, [](u64 id) {
      return std::ranges::none_of(s.view->lines, [id](const Line& l) { return l.id == id; });
    });
    const u64 newest = s.view->lines.empty() ? 0 : s.view->lines.back().id;
    if (newest != s.newest_line)
    {
      s.newest_line = newest;
      if (s.at_bottom)
        s.scroll_to_bottom = true;
    }
    OnNewView();
  }
  const bool in_match = Online::Rooms::GetScreen() == Online::Rooms::Screen::Match;
  s_corner_bar_bottom = 0;
  const bool shown = s.view && s.view->active && !Config::Get(Config::ONLINE_DISABLE_CHAT);
  if (!shown)
  {
    s_activate = false;
    StopTyping();
    // Opened by the chat key when it went: folded again when it comes back.
    if (s.peek)
    {
      s.peek = false;
      s.fold_back = true;
    }
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  const u64 now = Common::Timer::NowMs();
  const float font_px = FontPx(io.DisplaySize.y, scale) * TextSizePercent() / 100.0f;
  // PushFont's size is before the style's scale (the backbuffer scale).
  const float main_scale = ImGui::GetStyle().FontScaleMain > 0 ? ImGui::GetStyle().FontScaleMain : 1.0f;
  ImGui::PushFont(s_font, font_px / main_scale);

  // Folded or not as ImGui will draw it unless told otherwise: a click on the collapse arrow last
  // frame takes effect in this Begin. That is the player's: the chat key's opening ends.
  const ImGuiWindow* const window = ImGui::FindWindowByName(WINDOW_ID);
  bool collapsed = window && window->Collapsed != window->WantCollapseToggle;
  if (window && window->WantCollapseToggle)
    s.peek = false;

  // Where: the saved place (fractions of the window), else the default; again when the window
  // changes size.
  bool place = false;
  if (!s.placed || io.DisplaySize.x != s.display.x || io.DisplaySize.y != s.display.y)
  {
    s.display = io.DisplaySize;
    s.applied =
        LoadGeometry(io.DisplaySize).value_or(DefaultGeometry(io.DisplaySize, scale, font_px));
    if (!Config::Get(Config::ONLINE_CHAT_IN_MATCHES))
    {
      // "Show during matches" was turned off, an option there is no more: folded instead.
      s.applied.collapsed = true;
      Config::DeleteKey(Config::GetActiveLayerForConfig(Config::ONLINE_CHAT_IN_MATCHES),
                        Config::ONLINE_CHAT_IN_MATCHES);
      SaveGeometry(s.applied, io.DisplaySize);
    }
    collapsed = (s.applied.collapsed || s.auto_collapsed) && !s.peek;
    s.placed = true;
    place = true;
  }
  if (s.fold_back)
  {
    collapsed = true;
    s.fold_back = false;
  }
  if (s.collapse)
  {
    // Not saved: the next group opens as the player left it.
    collapsed = true;
    s.auto_collapsed = true;
    s.collapse = false;
    s.peek = false;
  }
  else if (s.expand)
  {
    collapsed = s.applied.collapsed && !s.peek;
    s.auto_collapsed = false;
    s.expand = false;
  }
  if (s_activate.exchange(false))
  {
    // Folded: open until the box lets go of the keyboard.
    if (collapsed)
      s.peek = true;
    collapsed = false;
    // The hotkey's own key press must not land in the box.
    ImGui::GetIO().ClearInputKeys();
    s.focus_input = true;
    s.refocus_frames = REFOCUS_FRAMES;
  }

  const int unread = collapsed ? UnreadCount() : 0;
  const std::string title = Title(unread);
  ImGui::SetNextWindowCollapsed(collapsed, ImGuiCond_Always);
  if (collapsed)
  {
    // The title bar alone, as wide as its text, at the top of the screen right of the FPS box
    // (PerformanceMetrics' 8 px from the edges), as a taskbar's entry.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = 8.0f * scale;
    const float w = 3.0f * style.FramePadding.x + ImGui::GetFontSize() + style.ItemInnerSpacing.x +
                    ImGui::CalcTextSize(title.c_str()).x;
    ImGui::SetNextWindowPos(ImVec2(s_top_left_right > 0 ? s_top_left_right + pad : pad, pad),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::ceil(w), ImGui::GetFrameHeight()), ImGuiCond_Always);
  }
  else
  {
    // Opened: back where the player put it.
    if (place || s.drawn_collapsed)
    {
      ImGui::SetNextWindowPos(s.applied.pos, ImGuiCond_Always);
      ImGui::SetNextWindowSize(s.applied.size, ImGuiCond_Always);
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(8.0f * font_px, 5.0f * font_px), io.DisplaySize);
  }

  // Not while the mouse is over the window or the player holds it (a press on a name, the
  // scrollbar or the resize grip, or a drag that left the window), nor folded.
  const bool faded = in_match && !collapsed && !s.was_typing && !s.hovered && !s.interacting &&
                     now - s.view->last_message_ms >= s_fade_after_ms &&
                     !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
  if (faded)
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, FADED_ALPHA);
  ImGui::SetNextWindowBgAlpha(in_match ? 0.55f : 0.85f);

  // Folded, it stays at the top.
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNav |
      (collapsed ? ImGuiWindowFlags_NoMove : ImGuiWindowFlags_None);
  bool typing = false;
  const bool open = ImGui::Begin(fmt::format("{}{}", title, WINDOW_ID).c_str(), nullptr, flags);
  // A double click on the title bar folds or unfolds it in this Begin, not foreseen above (the
  // player's too). Unfolded from the top: where the player put it (its size from the next frame).
  const bool now_collapsed = ImGui::IsWindowCollapsed();
  if (now_collapsed != collapsed)
  {
    s.peek = false;
    if (!now_collapsed)
    {
      ImGui::SetWindowPos(s.applied.pos, ImGuiCond_Always);
      ImGui::SetWindowSize(s.applied.size, ImGuiCond_Always);
    }
  }
  if (open)
  {
    // Opened by the player (or the hotkey, for good): it is theirs again.
    if (!s.peek)
      s.auto_collapsed = false;
    s.read_up_to = s.newest_line;

    if (ImGui::BeginMenuBar())
    {
      if (s_recording)
      {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 label = ImGui::CalcTextSize("Options");
        s_frame.options_menu = {at.x, at.y, at.x + label.x, at.y + ImGui::GetFrameHeight()};
      }
      if (ImGui::BeginMenu("Options"))
      {
        if (s_recording)
          s_frame.options_menu_open = true;
        DrawTextSizeItems();
        ImGui::EndMenu();
      }
      DrawMenuBarPlayers(scale);
      ImGui::EndMenuBar();
    }

    const bool notice = now < s.notice_until;
    const float input_h = ImGui::GetFrameHeightWithSpacing() +
                          (notice ? ImGui::GetTextLineHeightWithSpacing() : 0.0f);
    if (ImGui::BeginChild("##lines", ImVec2(0.0f, -input_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoNav))
    {
      for (const Line& line : s.view->lines)
        DrawLine(line, scale);
      if (s.scroll_to_bottom)
      {
        ImGui::SetScrollHereY(1.0f);
        s.scroll_to_bottom = false;
      }
      s.at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    }
    ImGui::EndChild();

    if (notice)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, SYSTEM_COLOR);
      ImGui::TextUnformatted(s.notice.data(), s.notice.data() + s.notice.size());
      ImGui::PopStyleColor();
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (s.focus_input)
    {
      ImGui::SetKeyboardFocusHere();
      s.focus_input = false;
    }
    // Ctrl+W (the Control key on every platform, not Alt Gr): the callback deletes the word.
    s.delete_word =
        ControlKeyDown() && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_W, true);
    const bool enter = ImGui::InputTextWithHint(
        "##input", "Message", s.input.data(), s.input.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit |
            ImGuiInputTextFlags_CallbackAlways,
        BoxCallback);
    s.delete_word = false;
    const bool active = ImGui::IsItemActive();
    if (active)
      s.refocus_frames = 0;
    else if (s.refocus_frames > 0 && --s.refocus_frames == 0)
      s.edit = Edit::None;  // it didn't get focus back (clicked away): forget the edit
    typing = active || s.refocus_frames > 0;
    if (DrawBoxMenu() && s.menu_was_typing)
      typing = true;  // the menu took the focus from the box: still typing
    if (enter)
    {
      if (!Online::ChatText::Clean(s.input.data()))
      {
        // Enter on an empty box: back to the game, the box lets go of the keyboard.
        s.input.fill(0);
        ImGui::SetWindowFocus(nullptr);
        s.refocus_frames = 0;
        s.edit = Edit::None;
        typing = false;
      }
      else
      {
        // Sent (or kept, with a notice saying why): the box keeps the keyboard for the next one.
        Submit();
        s.focus_input = true;
        s.refocus_frames = REFOCUS_FRAMES;
        s.edit = Edit::End;
        typing = true;
      }
    }
    DrawMemberMenu();
  }

  // The player moving, resizing or collapsing the window: saved once the mouse is let go. A
  // collapse for ignored players is not the player's: the saved state stays as they left it.
  if (s_recording)
  {
    s_frame.x = ImGui::GetWindowPos().x;
    s_frame.y = ImGui::GetWindowPos().y;
    s_frame.w = ImGui::GetWindowSize().x;
    s_frame.h = ImGui::GetWindowSize().y;
    s_frame.collapsed = ImGui::IsWindowCollapsed();
    s_frame.shown = true;
    s_frame.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    s_frame.alpha = ImGui::GetStyle().Alpha;
    s_frame.font_px = font_px;
    s_frame.title = title;
    s_frame.unread = unread;
  }
  // Folded, the window is at the top: the place it opens at stays the saved one. The fold is saved
  // at once (a click on the arrow is no drag), unless it is for ignored players or the chat key
  // opened it.
  Geometry now_geometry = s.applied;
  if (!now_collapsed)
  {
    now_geometry.pos = ImGui::GetWindowPos();
    now_geometry.size = ImGui::GetWindowSize();
  }
  if (!s.auto_collapsed && !s.peek && now_collapsed != s.applied.collapsed)
  {
    s.applied.collapsed = now_collapsed;
    SaveGeometry(s.applied, io.DisplaySize);
  }
  now_geometry.collapsed = s.applied.collapsed;
  s.drawn_collapsed = now_collapsed;
  if (now_collapsed && s_top_left_right <= 0)
    s_corner_bar_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
  // Hovered also while one of its items is held: without that, a press on a name (or anywhere that
  // makes an item active) read as the mouse leaving, and a faded window flashed back to faded
  // for as long as the button was down.
  s.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                     ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                     ImGuiHoveredFlags_AllowWhenBlockedByPopup);
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (s.hovered || s.interacting))
  {
    s.interacting = true;
  }
  else if (s.interacting)
  {
    s.interacting = false;
    if (Differs(now_geometry, s.applied))
    {
      s.applied = now_geometry;
      SaveGeometry(now_geometry, io.DisplaySize);
    }
  }
  ImGui::End();
  ImGui::PopFont();
  if (faded)
    ImGui::PopStyleVar();

  if (!typing)
    s.refocus_frames = 0;
  // The keyboard stays the box's until Enter or Esc is let go, so the key that ended typing
  // doesn't reach the game (Enter is START in the default keyboard mapping).
  const bool guard = s.was_typing && !typing &&
                     (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
                      ImGui::IsKeyDown(ImGuiKey_Escape));
  const bool was_typing = s.was_typing;
  s.was_typing = typing || guard;
  s_typing = s.was_typing;
  // The box let go of the keyboard after the chat key opened the folded window: folded again,
  // unless that was a click in the window (a name, the lines: the player wants it open).
  if (s.peek && was_typing && !s.was_typing)
  {
    s.peek = false;
    s.fold_back = !(s.hovered && (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                  ImGui::IsMouseReleased(ImGuiMouseButton_Left)));
  }
  ciface::Core::SetKeyboardMouseBlocked(s.was_typing);
}
}  // namespace
}  // namespace VideoCommon::OnlineChatUI
