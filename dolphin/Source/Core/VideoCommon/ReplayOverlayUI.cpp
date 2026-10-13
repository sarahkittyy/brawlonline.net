// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ReplayOverlayUI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include "Core/Online/GameSetup.h"
#include "Core/Rollback/GameplaySession.h"

namespace VideoCommon::ReplayOverlayUI
{
namespace
{
using Clock = std::chrono::steady_clock;

std::atomic<bool> s_hidden{false};
ImFont* s_font = nullptr;
ImVec2 s_last_mouse{-1, -1};
Clock::time_point s_mouse_moved{};
// A drag on the bar: where it would seek (the seek happens on release).
bool s_dragging = false;
float s_drag_at = 0;

constexpr auto SHOW_AFTER_MOUSE = std::chrono::seconds(3);

std::string Clock60(s64 frames)
{
  const s64 seconds = std::max<s64>(frames, 0) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

std::string Players(const Gprb::Session::PlaybackInfo& info)
{
  std::string out;
  for (size_t port = 0; port < info.names.size(); ++port)
  {
    if (info.names[port].empty())
      continue;
    if (!out.empty())
      out += "  vs  ";
    out += fmt::format("{} ({})", info.names[port],
                       Online::GameSetup::CharacterName(info.chars[port]));
  }
  return out;
}
}  // namespace

void ToggleHidden()
{
  s_hidden = !s_hidden.load();
}

void SetFont(ImFont* font)
{
  s_font = font;
}

void Display(float scale)
{
  const Gprb::Session::PlaybackInfo info = Gprb::Session::GetPlaybackInfo();
  if (!info.active)
  {
    s_dragging = false;
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const auto now = Clock::now();
  if (io.MousePos.x != s_last_mouse.x || io.MousePos.y != s_last_mouse.y)
  {
    s_last_mouse = io.MousePos;
    s_mouse_moved = now;
  }
  const bool wanted = info.paused || info.seeking || s_dragging || now - s_mouse_moved < SHOW_AFTER_MOUSE;
  if (s_hidden || !wanted)
    return;

  if (s_font)
    ImGui::PushFont(s_font);
  const float width = std::min(io.DisplaySize.x * 0.8f, 900.0f * scale);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 12.0f * scale),
                          ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.7f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("##ReplayOverlay", nullptr, flags))
  {
    // What is playing (display text from the file: drawn, never formatted or used as a label).
    ImGui::TextUnformatted(info.title.c_str());
    const std::string players = Players(info);
    if (!players.empty())
    {
      ImGui::SameLine();
      ImGui::TextDisabled("|");
      ImGui::SameLine();
      ImGui::TextUnformatted(players.c_str());
    }

    const s64 span = std::max<s64>(info.last - info.first, 1);
    const s64 shown = std::clamp(info.shown, info.first, info.last);
    std::string state = info.seeking ? "Seeking..." : info.paused ? "Paused" : "Playing";
    if (info.speed != 1.0)
      state += fmt::format("   {}x", info.speed);
    const std::string time = fmt::format("{} / {}", Clock60(shown - info.first), Clock60(span));
    ImGui::TextUnformatted(state.c_str());
    ImGui::SameLine(width - ImGui::CalcTextSize(time.c_str()).x - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextUnformatted(time.c_str());

    // The bar: click or drag to seek.
    const float bar_h = 14.0f * scale;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float bar_w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##replayseek", ImVec2(bar_w, bar_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const auto at = [&](float x) { return std::clamp((x - p0.x) / std::max(bar_w, 1.0f), 0.0f, 1.0f); };
    if (active)
    {
      s_dragging = true;
      s_drag_at = at(io.MousePos.x);
    }
    else if (s_dragging)
    {
      s_dragging = false;
      Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::Seek,
                                     static_cast<double>(info.first + s_drag_at * span));
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + bar_w, p0.y + bar_h);
    dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 50), 3.0f * scale);
    const float done = s_dragging ? s_drag_at : static_cast<float>(shown - info.first) / span;
    dl->AddRectFilled(p0, ImVec2(p0.x + done * bar_w, p1.y), IM_COL32(110, 200, 120, 220),
                      3.0f * scale);
    if (info.desync_frame >= info.first)
    {
      // Where the replay stopped matching what was played.
      const float x = p0.x + static_cast<float>(info.desync_frame - info.first) / span * bar_w;
      dl->AddRectFilled(ImVec2(x - 1.0f * scale, p0.y), ImVec2(x + 1.0f * scale, p1.y),
                        IM_COL32(230, 60, 60, 255));
    }
    if (hovered || s_dragging)
    {
      const float hx = s_dragging ? s_drag_at : at(io.MousePos.x);
      ImGui::SetTooltip("%s", Clock60(static_cast<s64>(hx * span)).c_str());
    }
  }
  ImGui::End();
  if (s_font)
    ImGui::PopFont();
}
}  // namespace VideoCommon::ReplayOverlayUI
