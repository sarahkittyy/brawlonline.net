// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/OnlineChatUI.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <imgui.h>

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
constexpr u64 NOTICE_MS = 3000;
// The saved geometry's unit: fractions of the window in 1/10000.
constexpr int GEOMETRY_UNIT = 10000;

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
// The chat's text height: a share of the window's height, so it reads the same at any size.
constexpr float FONT_SHARE = 1.0f / 54.0f;
constexpr float MIN_FONT_PX = 14.0f;
constexpr float MAX_FONT_PX = 44.0f;

std::atomic<bool> s_activate{false};
std::atomic<bool> s_typing{false};
ImFont* s_font = nullptr;

struct Geometry
{
  ImVec2 pos;
  ImVec2 size;
  bool collapsed = false;
};

struct UiState
{
  std::optional<Online::Chat::View> view;
  u64 serial = 0;
  std::set<u64> revealed;  // ids of hidden players' lines clicked open
  std::array<char, Online::ChatText::MAX_MESSAGE_BYTES + 1> input{};
  bool focus_input = false;
  bool was_typing = false;
  bool hovered = false;
  bool at_bottom = true;
  bool scroll_to_bottom = false;
  u64 newest_line = 0;

  // Geometry: placed for this window size; what was applied; the player moving it.
  bool placed = false;
  ImVec2 display{};
  Geometry applied;
  bool interacting = false;

  // The name menu.
  bool open_menu = false;
  std::string menu_uid;
  bool confirm_report = false;

  std::string notice;
  u64 notice_until = 0;
};
UiState s;

void SetNotice(std::string text)
{
  s.notice = std::move(text);
  s.notice_until = Common::Timer::NowMs() + NOTICE_MS;
}

std::optional<Geometry> LoadGeometry(ImVec2 display)
{
  const std::string text = Config::Get(Config::ONLINE_CHAT_WINDOW);
  int x, y, w, h, c;
  if (text.size() > 64 || std::sscanf(text.c_str(), "%d %d %d %d %d", &x, &y, &w, &h, &c) != 5)
    return std::nullopt;
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
                           fmt::format("{} {} {} {} {}", u(g.pos.x, display.x),
                                       u(g.pos.y, display.y), u(g.size.x, display.x),
                                       u(g.size.y, display.y), g.collapsed ? 1 : 0));
  Config::Save();
}

// In the side bar when the game is 4:3 in a wider window, else the game's top-left corner.
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
    g.size = {bar - 16.0f * scale, std::max(160.0f * scale, game_h * 0.5f)};
  }
  else
  {
    g.pos = {r.left + 12.0f * scale, r.top + 12.0f * scale};
    g.size = {std::min(24.0f * font_px, game_w * 0.4f), std::min(12.0f * font_px, game_h * 0.4f)};
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

ImU32 ColorOf(int color)
{
  return NAME_COLORS[static_cast<size_t>(color) % NAME_COLORS.size()];
}

// A name drawn as text that can be clicked (an InvisibleButton under it: the name is never a
// widget label). Clicking another player's name opens their menu.
void NameButton(std::string_view text, ImU32 color, const std::string& uid, bool clickable)
{
  const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##name", ImVec2(std::max(size.x, 1.0f), size.y));
  const bool hovered = clickable && ImGui::IsItemHovered();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  // Through GetColorU32: the window's alpha (faded in a match) applies to drawn text too.
  const ImU32 col = ImGui::GetColorU32(color);
  draw->AddText(pos, col, text.data(), text.data() + text.size());
  if (hovered)
    draw->AddLine({pos.x, pos.y + size.y}, {pos.x + size.x, pos.y + size.y}, col);
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
  const bool hidden = !own && (m ? m->hidden : Online::Chat::IsHidden(line.uid)) &&
                      !s.revealed.contains(line.id);
  if (hidden)
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

void DrawHeader(float scale)
{
  const auto& v = *s.view;
  bool first = true;
  if (v.room)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, SYSTEM_COLOR);
    ImGui::TextUnformatted(v.title.data(), v.title.data() + v.title.size());
    ImGui::PopStyleColor();
    first = false;
  }
  bool anyone = false;
  for (const Online::Chat::Member& m : v.members)
  {
    if (m.is_me)
      continue;
    anyone = true;
    if (!first)
      ImGui::SameLine(0.0f, 10.0f * scale);
    first = false;
    ImGui::PushID(m.uid.c_str());
    std::string label = m.code.empty() ? m.name : fmt::format("{} ({})", m.name, m.code);
    NameButton(label, ColorOf(m.color), m.uid, true);
    if (m.hidden || !m.has_key)
    {
      ImGui::SameLine(0.0f, 4.0f * scale);
      ImGui::PushStyleColor(ImGuiCol_Text, SYSTEM_COLOR);
      ImGui::TextUnformatted(m.hidden ? "(hidden)" : "(connecting)");
      ImGui::PopStyleColor();
    }
    ImGui::PopID();
  }
  if (!anyone)
  {
    if (!first)
      ImGui::SameLine(0.0f, 10.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_Text, SYSTEM_COLOR);
    ImGui::TextUnformatted("Nobody else here yet");
    ImGui::PopStyleColor();
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
  if (!m)
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
    if (ImGui::MenuItem(m->hidden ? "Show messages" : "Hide messages"))
    {
      // Hiding starts from all of their messages covered again.
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

int LimitInput(ImGuiInputTextCallbackData* data)
{
  // At most 100 code points (the cleaner's limit), cut on a code point boundary.
  const std::string_view text(data->Buf, static_cast<size_t>(data->BufTextLen));
  const std::string_view kept =
      Online::ChatText::Truncate(text, Online::ChatText::MAX_MESSAGE_CODE_POINTS,
                                 Online::ChatText::MAX_MESSAGE_BYTES);
  if (kept.size() < text.size())
  {
    data->DeleteChars(static_cast<int>(kept.size()), static_cast<int>(text.size() - kept.size()));
  }
  return 0;
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
  s_typing = false;
  ciface::Core::SetKeyboardMouseBlocked(false);
}
}  // namespace

void SetFont(ImFont* font)
{
  s_font = font;
}

void Activate()
{
  s_activate = true;
}

bool IsTyping()
{
  return s_typing;
}

void Display(float scale)
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
  }
  const bool in_match = Online::Rooms::GetScreen() == Online::Rooms::Screen::Match;
  const bool shown = s.view && s.view->active &&
                     (!in_match || Config::Get(Config::ONLINE_CHAT_IN_MATCHES));
  if (!shown)
  {
    s_activate = false;
    StopTyping();
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  const u64 now = Common::Timer::NowMs();
  const float font_px = std::clamp(io.DisplaySize.y * FONT_SHARE, MIN_FONT_PX, MAX_FONT_PX);

  // Where: the saved place (fractions of the window), else the default; again when the window
  // changes size.
  if (!s.placed || io.DisplaySize.x != s.display.x || io.DisplaySize.y != s.display.y)
  {
    s.display = io.DisplaySize;
    s.applied =
        LoadGeometry(io.DisplaySize).value_or(DefaultGeometry(io.DisplaySize, scale, font_px));
    ImGui::SetNextWindowPos(s.applied.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(s.applied.size, ImGuiCond_Always);
    ImGui::SetNextWindowCollapsed(s.applied.collapsed, ImGuiCond_Always);
    s.placed = true;
  }
  if (s_activate.exchange(false))
  {
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
    // The hotkey's own key press must not land in the box.
    ImGui::GetIO().ClearInputKeys();
    s.focus_input = true;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(8.0f * font_px, 5.0f * font_px), io.DisplaySize);

  const bool faded = in_match && !s.was_typing && !s.hovered &&
                     now - s.view->last_message_ms > FADE_AFTER_MS &&
                     !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
  if (faded)
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, FADED_ALPHA);
  ImGui::SetNextWindowBgAlpha(in_match ? 0.55f : 0.85f);
  // PushFont's size is before the style's scale (the backbuffer scale).
  const float main_scale = ImGui::GetStyle().FontScaleMain > 0 ? ImGui::GetStyle().FontScaleMain : 1.0f;
  ImGui::PushFont(s_font, font_px / main_scale);

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                                     ImGuiWindowFlags_NoNav;
  bool typing = false;
  const bool open = ImGui::Begin("Chat###OnlineChat", nullptr, flags);
  if (open)
  {
    if (ImGui::BeginMenuBar())
    {
      if (ImGui::BeginMenu("Options"))
      {
        bool in_matches = Config::Get(Config::ONLINE_CHAT_IN_MATCHES);
        if (ImGui::MenuItem("Show during matches", nullptr, &in_matches))
        {
          Config::SetBaseOrCurrent(Config::ONLINE_CHAT_IN_MATCHES, in_matches);
          Config::Save();
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    DrawHeader(scale);
    ImGui::Separator();

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
    const bool enter = ImGui::InputTextWithHint(
        "##input", "Message", s.input.data(), s.input.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit, LimitInput);
    typing = ImGui::IsItemActive();
    if (enter)
    {
      Submit();
      // Back to the game: the box lets go of the keyboard.
      ImGui::SetWindowFocus(nullptr);
      typing = false;
    }
    DrawMemberMenu();
  }

  // The player moving, resizing or collapsing the window: saved once the mouse is let go.
  const Geometry now_geometry{ImGui::GetWindowPos(), ImGui::GetWindowSize(),
                              ImGui::IsWindowCollapsed()};
  s.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
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

  // The keyboard stays the box's until Enter or Esc is let go, so the key that ended typing
  // doesn't reach the game (Enter is START in the default keyboard mapping).
  const bool guard = s.was_typing && !typing &&
                     (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
                      ImGui::IsKeyDown(ImGuiKey_Escape));
  s.was_typing = typing || guard;
  s_typing = s.was_typing;
  ciface::Core::SetKeyboardMouseBlocked(s.was_typing);
}
}  // namespace VideoCommon::OnlineChatUI
