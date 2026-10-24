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
#include "Core/Rollback/ReplayPlayback.h"
#include "VideoCommon/OnScreenDisplay.h"

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
// A press on a highlight's mark in the bar: which (-1: none); it plays on release, never seeks.
int s_mark_held = -1;
// Clip mode (Clip, then "Save as highlight" or "Cancel clip"): the clip's first and last frames,
// the replay it is of (its title and first frame: another replay armed ends it), and the handle
// held (0: none, 1: the start, 2: the end).
bool s_clip_mode = false;
s64 s_clip_start = 0;
s64 s_clip_end = 0;
std::string s_clip_title;
s64 s_clip_first = 0;
int s_clip_handle = 0;

constexpr auto SHOW_AFTER_MOUSE = std::chrono::seconds(3);
// A clip starts this long before the frame Clip was clicked on (as L+R+Z's, the launcher's
// CLIP_BEFORE_SECONDS) and is at least MIN_CLIP_FRAMES long.
constexpr s64 CLIP_BEFORE_FRAMES = 20 * 60;
constexpr s64 MIN_CLIP_FRAMES = 60;

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

// A highlight's star (the seek bar's marks), its points at radius r around c.
void Star(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
  ImVec2 pts[10];
  for (int i = 0; i < 10; ++i)
  {
    const float a = -1.5707964f + i * 0.62831855f;
    const float k = i % 2 == 0 ? r : r * 0.45f;
    pts[i] = ImVec2(c.x + std::cos(a) * k, c.y + std::sin(a) * k);
  }
  dl->AddConcavePolyFilled(pts, 10, col);
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
    s_mark_held = -1;
    s_clip_mode = false;
    s_clip_handle = 0;
    return;
  }
  // (A clip is of one replay: another one armed, e.g. a spectated set's next game, ends it.)
  if (s_clip_mode && (info.title != s_clip_title || info.first != s_clip_first))
  {
    s_clip_mode = false;
    s_clip_handle = 0;
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
  // Play ends the replay, a click does nothing (the button and Space still end it). Catching up to
  // live, a click stops the catch-up and plays on from there (the user, 2026-10-21: "Click to
  // watch"), not paused.
  const bool catching_up = info.live && !info.ended && info.catching_up;
  if (io.MouseClicked[0] && !s_dragging && !info.at_end &&
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
  {
    Gprb::Session::PlaybackControl(catching_up ? Gprb::Session::PlaybackCommand::Play :
                                                 Gprb::Session::PlaybackCommand::TogglePause,
                                   0);
  }
  // The catch-up's progress at the top, as a highlight's fast-forward has it.
  if (catching_up)
  {
    const s64 from = info.catch_from >= 0 ? info.catch_from : info.shown;
    const double span = static_cast<double>(std::max<s64>(info.live_edge - from, 1));
    const int pct = static_cast<int>(std::clamp((info.shown - from) / span, 0.0, 1.0) * 100);
    OSD::AddTypedMessage(OSD::MessageType::SpectateCatchUp,
                         fmt::format("Catching up to live ({}%). Click to watch", pct), 600,
                         OSD::Color::CYAN);
  }
  // Spectating: also while catching up, buffering, or with something to say (the banner).
  const bool wanted = info.paused || info.seeking || s_dragging || s_mark_held >= 0 || s_clip_mode || info.catching_up ||
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

    // A live game's bar ends at the live edge (the frames past it are the 3 s buffer, not there to
    // be played yet), so live is the bar's very end; a seek there is Go Live.
    const bool live = info.live && !info.ended;
    const s64 end = live ? std::max(info.live_edge, info.first) : info.last;
    const s64 span = std::max<s64>(end - info.first, 1);
    const s64 shown = std::clamp(info.shown, info.first, end);
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
    // Clip mode: the clip's place and length instead (the user, 2026-10-24: Clip shows a left and
    // a right point, ending where it was clicked and starting 20 s earlier).
    if (s_clip_mode)
    {
      s_clip_end = std::clamp(s_clip_end, info.first + MIN_CLIP_FRAMES, std::max(end, info.first + MIN_CLIP_FRAMES));
      s_clip_start = std::clamp(s_clip_start, info.first, s_clip_end - MIN_CLIP_FRAMES);
      state = fmt::format("Clip {} - {} ({} s): drag its ends", Clock60(s_clip_start - info.first),
                          Clock60(s_clip_end - info.first), (s_clip_end - s_clip_start + 30) / 60);
    }
    const std::string time = fmt::format("{} / {}", Clock60(shown - info.first), Clock60(span));
    ImGui::TextUnformatted(state.c_str());
    // Clip, or Cancel clip and Save as highlight, left of the time; only when the launcher keeps
    // what is saved (ReplayPlayback.h, manual clips).
    const bool can_clip = Gprb::Replay::Playback::CanSaveClip();
    if (!can_clip)
    {
      s_clip_mode = false;
      s_clip_handle = 0;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto button_w = [&](const char* label) {
      return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    };
    constexpr const char* CLIP = "Clip";
    constexpr const char* CANCEL = "Cancel clip";
    constexpr const char* SAVE = "Save as highlight";
    const float time_x = width - ImGui::CalcTextSize(time.c_str()).x - style.WindowPadding.x;
    if (can_clip)
    {
      const float buttons =
          s_clip_mode ? button_w(CANCEL) + style.ItemSpacing.x + button_w(SAVE) : button_w(CLIP);
      ImGui::SameLine(time_x - style.ItemSpacing.x * 2.0f - buttons);
      if (!s_clip_mode)
      {
        if (ImGui::SmallButton(CLIP))
        {
          // Ends at the frame shown, starts 20 s before it; paused there to set its ends.
          s_clip_mode = true;
          s_clip_handle = 0;
          s_clip_title = info.title;
          s_clip_first = info.first;
          s_clip_end = std::clamp(shown, info.first + MIN_CLIP_FRAMES, std::max(end, info.first + MIN_CLIP_FRAMES));
          s_clip_start = std::max(info.first, s_clip_end - CLIP_BEFORE_FRAMES);
          if (!info.paused)
            Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::Pause, 0);
        }
        if (ImGui::IsItemHovered())
        {
          ImGui::SetTooltip("Clip the last %d seconds, then drag its ends",
                            static_cast<int>(CLIP_BEFORE_FRAMES / 60));
        }
      }
      else
      {
        if (ImGui::SmallButton(CANCEL))
        {
          s_clip_mode = false;
          s_clip_handle = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(SAVE))
        {
          if (Gprb::Replay::Playback::SaveClip(s_clip_start, s_clip_end))
          {
            OSD::AddMessage("Clip saved to Replays > Highlights > Manual Clips", OSD::Duration::NORMAL,
                            OSD::Color::GREEN);
          }
          else
          {
            OSD::AddMessage("The clip could not be saved", OSD::Duration::NORMAL, OSD::Color::RED);
          }
          s_clip_mode = false;
          s_clip_handle = 0;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Keep it in the launcher's Replays > Highlights > Manual Clips");
      }
    }
    ImGui::SameLine(time_x);
    ImGui::TextUnformatted(time.c_str());

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

    // Play / pause, left of the bar: a triangle while paused (or held at the end), two bars while
    // playing.
    const float row_h = 24.0f * scale;
    const float bar_h = 14.0f * scale;
    // Where the bar goes (right of the button, left of Go Live).
    const float bar_x = ImGui::GetCursorScreenPos().x + row_h + ImGui::GetStyle().ItemSpacing.x;
    const float bar_w = ImGui::GetContentRegionAvail().x - row_h - ImGui::GetStyle().ItemSpacing.x - live_w;
    const auto bar_frac = [&](s64 frame) {
      return std::clamp(static_cast<float>(frame - info.first) / span, 0.0f, 1.0f);
    };

    // Highlights: each one asked for in this replay is a line on the bar with a star above it; a
    // click on either plays it from its start (the user, 2026-10-24: a click on the bar leaves the
    // highlight for the replay, this goes back).
    const float mark_r = 6.0f * scale;
    const float mark_grab = 7.0f * scale;
    const auto mark_at = [&](float x) {
      int best = -1;
      float best_d = mark_grab;
      for (size_t i = 0; i < info.marks.size(); ++i)
      {
        const float d = std::abs(bar_x + bar_frac(info.marks[i].start) * bar_w - x);
        if (d <= best_d)
        {
          best = static_cast<int>(i);
          best_d = d;
        }
      }
      return best;
    };
    int mark_hovered = -1;
    float stars_y = 0;
    if (!info.marks.empty())
    {
      const ImVec2 s0 = ImGui::GetCursorScreenPos();
      stars_y = s0.y + mark_r;
      ImGui::SetCursorScreenPos(ImVec2(bar_x, s0.y));
      if (ImGui::InvisibleButton("##replaymarks", ImVec2(std::max(bar_w, 1.0f), mark_r * 2.0f)))
      {
        if (const int m = mark_at(io.MousePos.x); m >= 0)
          Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::PlayHighlight, m);
      }
      if (ImGui::IsItemHovered())
        mark_hovered = mark_at(io.MousePos.x);
    }

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

    // The bar: click or drag to seek (the whole row's height takes the click; the bar is drawn
    // in its middle).
    const ImVec2 row0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##replayseek", ImVec2(std::max(bar_w, 1.0f), row_h));
    const ImVec2 p0(row0.x, row0.y + (row_h - bar_h) * 0.5f);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const auto at = [&](float x) { return std::clamp((x - p0.x) / std::max(bar_w, 1.0f), 0.0f, 1.0f); };
    // Clip mode: the clip's ends are handles on the bar (the nearer one within reach; a press
    // elsewhere seeks as always). A handle let go shows its frame.
    const float handle_grab = 8.0f * scale;
    const auto frame_at = [&](float x) { return info.first + static_cast<s64>(std::lround(at(x) * span)); };
    int handle_hovered = 0;
    if (s_clip_mode && hovered && !s_dragging && s_clip_handle == 0)
    {
      const float ds = std::abs(bar_x + bar_frac(s_clip_start) * bar_w - io.MousePos.x);
      const float de = std::abs(bar_x + bar_frac(s_clip_end) * bar_w - io.MousePos.x);
      if (std::min(ds, de) <= handle_grab)
        handle_hovered = de <= ds ? 2 : 1;
    }
    if (hovered && !s_dragging && mark_hovered < 0 && handle_hovered == 0 && s_clip_handle == 0)
      mark_hovered = mark_at(io.MousePos.x);
    // A press on a mark's line plays its highlight once released (no drag, no seek); on a clip's
    // handle, it moves that end.
    if (ImGui::IsItemActivated())
    {
      s_clip_handle = handle_hovered;
      s_mark_held = handle_hovered ? -1 : mark_hovered;
    }
    if (s_clip_mode && (handle_hovered || s_clip_handle))
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (active && s_clip_handle == 1)
    {
      s_clip_start = std::clamp(frame_at(io.MousePos.x), info.first, s_clip_end - MIN_CLIP_FRAMES);
    }
    else if (active && s_clip_handle == 2)
    {
      s_clip_end = std::clamp(frame_at(io.MousePos.x), s_clip_start + MIN_CLIP_FRAMES,
                              std::max(end, s_clip_start + MIN_CLIP_FRAMES));
    }
    else if (!active && s_clip_handle)
    {
      Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::Seek,
                                     static_cast<double>(s_clip_handle == 1 ? s_clip_start : s_clip_end));
      s_clip_handle = 0;
    }
    else if (active && s_mark_held < 0)
    {
      s_dragging = true;
      s_drag_at = at(io.MousePos.x);
    }
    else if (!active && s_mark_held >= 0)
    {
      Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::PlayHighlight, s_mark_held);
      s_mark_held = -1;
    }
    else if (!active && s_dragging)
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
    if (info.desync_frame >= info.first && info.desync_frame <= end)
    {
      // Where the replay stopped matching what was played.
      const float x = p0.x + static_cast<float>(info.desync_frame - info.first) / span * bar_w;
      dl->AddRectFilled(ImVec2(x - 1.0f * scale, p0.y), ImVec2(x + 1.0f * scale, p1.y),
                        IM_COL32(230, 60, 60, 255));
    }
    if (s_clip_mode)
    {
      // The clip: its span over the bar, a handle at each end (taller than the bar).
      const float xs = bar_x + bar_frac(s_clip_start) * bar_w;
      const float xe = bar_x + bar_frac(s_clip_end) * bar_w;
      dl->AddRectFilled(ImVec2(xs, p0.y), ImVec2(xe, p1.y), IM_COL32(90, 170, 255, 110));
      dl->AddRect(ImVec2(xs, p0.y), ImVec2(xe, p1.y), IM_COL32(90, 170, 255, 255), 0.0f, 0, 1.5f * scale);
      for (int h = 1; h <= 2; ++h)
      {
        const float x = h == 1 ? xs : xe;
        const bool lit = h == handle_hovered || h == s_clip_handle;
        const float hw = (lit ? 4.0f : 3.0f) * scale;
        const ImVec2 a(x - hw, row0.y + 1.0f * scale);
        const ImVec2 z(x + hw, row0.y + row_h - 1.0f * scale);
        dl->AddRectFilled(a, z, lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 235, 255, 240), 2.0f * scale);
        dl->AddRect(a, z, IM_COL32(30, 90, 170, 255), 2.0f * scale, 0, 1.0f * scale);
      }
    }
    for (size_t i = 0; i < info.marks.size(); ++i)
    {
      const auto& mark = info.marks[i];
      const float x = bar_x + bar_frac(mark.start) * bar_w;
      const bool lit = static_cast<int>(i) == mark_hovered || static_cast<int>(i) == s_mark_held;
      const bool playing = info.clip_end >= 0 && mark.end == info.clip_end;
      const ImU32 col = lit ? IM_COL32(255, 230, 120, 255) :
                        playing ? IM_COL32(255, 200, 50, 255) :
                                  IM_COL32(255, 200, 50, 200);
      dl->AddRectFilled(ImVec2(x - 1.0f * scale, stars_y + mark_r * 0.5f), ImVec2(x + 1.0f * scale, p1.y), col);
      Star(dl, ImVec2(x, stars_y), lit ? mark_r * 1.2f : mark_r, col);
    }
    if (s_clip_mode && (handle_hovered || s_clip_handle))
    {
      const int h = s_clip_handle ? s_clip_handle : handle_hovered;
      ImGui::SetTooltip("%s (%s)", h == 1 ? "Clip start" : "Clip end",
                        Clock60((h == 1 ? s_clip_start : s_clip_end) - info.first).c_str());
    }
    else if (mark_hovered >= 0 && !s_dragging)
    {
      ImGui::SetTooltip("Play this highlight (%s)",
                        Clock60(info.marks[mark_hovered].start - info.first).c_str());
    }
    else if (hovered || s_dragging)
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
