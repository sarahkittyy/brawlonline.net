// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ReplayOverlayUI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
  // A click on the game (not on the overlay) pauses or plays, whether the overlay shows or not.
  // A double click (Dolphin's fullscreen) is two clicks: it toggles twice. Held at the end, where
  // Play ends the replay, a click does nothing (the button and Space still end it).
  if (io.MouseClicked[0] && !s_dragging && !info.at_end &&
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
    Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::TogglePause, 0);
  // Spectating: also while catching up, buffering, or with something to say (the banner).
  const bool wanted = info.paused || info.seeking || s_dragging || info.catching_up ||
                      info.buffering || !info.banner.empty() || now - s_mouse_moved < SHOW_AFTER_MOUSE;
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

    if (!info.banner.empty())
      ImGui::TextUnformatted(info.banner.c_str());

    const s64 span = std::max<s64>(info.last - info.first, 1);
    const s64 shown = std::clamp(info.shown, info.first, info.last);
    const bool live = info.live && !info.ended;
    std::string state = info.seeking ? "Seeking..." : info.paused ? "Paused" : "Playing";
    if (live && info.catching_up)
    {
      // "Catching up to live - 2:14 / 4:05 - about 20 s": the edge moves on at 1x, so it closes
      // at the catch-up rate less one.
      state = fmt::format("Catching up to live - {} / {}", Clock60(shown - info.first),
                          Clock60(info.live_edge - info.first));
      if (info.catch_rate > 0 && info.catch_rate < 1.5)
        state += " - your computer is catching up slowly";
      else if (info.catch_rate > 1.0)
        state += fmt::format(" - about {} s", static_cast<s64>(std::ceil(
                                                  (info.live_edge - shown) / ((info.catch_rate - 1.0) * 60.0))));
    }
    else if (live && info.buffering)
    {
      state = "Buffering...";
    }
    else if (live && info.at_live)
    {
      state = "Live";
    }
    if (info.speed != 1.0)
      state += fmt::format("   {}x", info.speed);
    const std::string time = fmt::format("{} / {}", Clock60(shown - info.first), Clock60(span));
    ImGui::TextUnformatted(state.c_str());
    ImGui::SameLine(width - ImGui::CalcTextSize(time.c_str()).x - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextUnformatted(time.c_str());

    // Play / pause, left of the bar: a triangle while paused (or held at the end), two bars while
    // playing.
    const float row_h = 24.0f * scale;
    const float bar_h = 14.0f * scale;
    {
      const ImVec2 b0 = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##replayplay", ImVec2(row_h, row_h)))
        Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::TogglePause, 0);
      const bool over = ImGui::IsItemHovered();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 col = over ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 230, 220);
      const float pad = 6.0f * scale;
      const ImVec2 a(b0.x + pad, b0.y + pad);
      const ImVec2 z(b0.x + row_h - pad, b0.y + row_h - pad);
      if (over)
        dl->AddRectFilled(b0, ImVec2(b0.x + row_h, b0.y + row_h), IM_COL32(255, 255, 255, 40), 4.0f * scale);
      if (info.paused || info.at_end)
      {
        dl->AddTriangleFilled(a, ImVec2(a.x, z.y), ImVec2(z.x, (a.y + z.y) * 0.5f), col);
      }
      else
      {
        const float w = (z.x - a.x) / 3.0f;
        dl->AddRectFilled(a, ImVec2(a.x + w, z.y), col);
        dl->AddRectFilled(ImVec2(z.x - w, a.y), z, col);
      }
      if (over)
        ImGui::SetTooltip("%s", info.paused || info.at_end ? "Play (Space)" : "Pause (Space)");
      ImGui::SameLine();
    }

    // Spectating: LIVE (red, playing at the live edge) or GO LIVE with how far behind, right of
    // the bar (End, or a click).
    std::string live_label;
    if (live)
    {
      const s64 behind = std::max<s64>(info.live_edge - shown, 0) / 60;
      live_label = info.at_live || behind == 0 ? std::string("LIVE") :
                                                 fmt::format("GO LIVE  -{} s", behind);
    }
    const float live_w =
        live ? ImGui::CalcTextSize(live_label.c_str()).x + 16.0f * scale + ImGui::GetStyle().ItemSpacing.x : 0.0f;

    // The bar: click or drag to seek (the whole row's height takes the click; the bar is drawn
    // in its middle).
    const ImVec2 row0 = ImGui::GetCursorScreenPos();
    const float bar_w = ImGui::GetContentRegionAvail().x - live_w;
    ImGui::InvisibleButton("##replayseek", ImVec2(bar_w, row_h));
    const ImVec2 p0(row0.x, row0.y + (row_h - bar_h) * 0.5f);
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
    if (live)
    {
      // The live edge: what is played live (the rest of the bar is the buffer received).
      const float x = p0.x + static_cast<float>(info.live_edge - info.first) / span * bar_w;
      dl->AddRectFilled(ImVec2(x - 1.0f * scale, p0.y - 2.0f * scale),
                        ImVec2(x + 1.0f * scale, p1.y + 2.0f * scale), IM_COL32(230, 60, 60, 255));
    }
    if (hovered || s_dragging)
    {
      const float hx = s_dragging ? s_drag_at : at(io.MousePos.x);
      ImGui::SetTooltip("%s", Clock60(static_cast<s64>(hx * span)).c_str());
    }
    if (live)
    {
      ImGui::SameLine();
      const ImVec2 b0 = ImGui::GetCursorScreenPos();
      const float w = live_w - ImGui::GetStyle().ItemSpacing.x;
      if (ImGui::InvisibleButton("##replaylive", ImVec2(w, row_h)) && !info.at_live)
        Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::GoLive, 0);
      const bool over = ImGui::IsItemHovered();
      const ImU32 bg = info.at_live ? IM_COL32(200, 40, 40, 230) :
                       over         ? IM_COL32(255, 255, 255, 70) :
                                      IM_COL32(255, 255, 255, 40);
      dl->AddRectFilled(b0, ImVec2(b0.x + w, b0.y + row_h), bg, 4.0f * scale);
      const ImVec2 ts = ImGui::CalcTextSize(live_label.c_str());
      dl->AddText(ImVec2(b0.x + (w - ts.x) * 0.5f, b0.y + (row_h - ts.y) * 0.5f),
                  IM_COL32(255, 255, 255, 255), live_label.c_str());
      if (over && !info.at_live)
        ImGui::SetTooltip("Go live (End)");
    }
  }
  ImGui::End();
  if (s_font)
    ImGui::PopFont();
}
}  // namespace VideoCommon::ReplayOverlayUI
