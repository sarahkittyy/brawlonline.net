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
// held (0: none, 1: the start, 2: the end): where it was pressed, its frame then, and whether it
// has been dragged (past HANDLE_DRAG_PX; until then a release is a click, which seeks there).
bool s_clip_mode = false;
s64 s_clip_start = 0;
s64 s_clip_end = 0;
std::string s_clip_title;
s64 s_clip_first = 0;
int s_clip_handle = 0;
float s_handle_press_x = 0;
s64 s_handle_press_frame = 0;
bool s_handle_moved = false;
// The session's clip mode end as last set (Session::SetPlaybackStop; -1: none).
s64 s_stop_sent = -1;

constexpr auto SHOW_AFTER_MOUSE = std::chrono::seconds(3);
// A clip starts this long before the frame Clip was clicked on (as L+R+Z's, the launcher's
// CLIP_BEFORE_SECONDS) and is at least MIN_CLIP_FRAMES long.
constexpr s64 CLIP_BEFORE_FRAMES = 20 * 60;
constexpr s64 MIN_CLIP_FRAMES = 60;
// A press on a clip's handle moves it only once the mouse has gone this far (scaled); from then on
// the handle follows the mouse's movement from the press (it never jumps to the mouse), so it can
// still be nudged back by a pixel.
constexpr float HANDLE_DRAG_PX = 4.0f;
// < (previous clip) this far into a highlight plays it again from its start; nearer its start, the
// one before it (as media players' Previous).
constexpr s64 PREV_RESTARTS_AFTER = 60;

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

void SetStop(s64 end)
{
  if (end != s_stop_sent)
  {
    s_stop_sent = end;
    Gprb::Session::SetPlaybackStop(end);
  }
}

void EndClipMode()
{
  s_clip_mode = false;
  s_clip_handle = 0;
  SetStop(-1);
}

// The list buttons' arrows: `count` triangles pointing left or right, centred in [a, z].
void Arrows(ImDrawList* dl, ImVec2 a, ImVec2 z, int count, bool left, ImU32 col)
{
  const float h = (z.y - a.y) * 0.55f;
  const float w = h * 0.75f;
  const float total = w * count;
  float x = (a.x + z.x - total) * 0.5f;
  const float cy = (a.y + z.y) * 0.5f;
  for (int i = 0; i < count; ++i, x += w)
  {
    if (left)
      dl->AddTriangleFilled(ImVec2(x + w, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f), ImVec2(x, cy), col);
    else
      dl->AddTriangleFilled(ImVec2(x, cy - h * 0.5f), ImVec2(x, cy + h * 0.5f), ImVec2(x + w, cy), col);
  }
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
    EndClipMode();
    return;
  }
  // (A clip is of one replay: another one armed, e.g. a spectated set's next game, ends it.)
  if (s_clip_mode && (info.title != s_clip_title || info.first != s_clip_first))
    EndClipMode();
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
    // The launcher's list (highlights, not spectating): "3/40  << < > >>" right of the title (the
    // user, 2026-10-24). < > go to the previous / next highlight from the frame shown, in this
    // replay at once (its marks), past them in the replay before / after it (the launcher picks
    // it); << >> to the replay before / after.
    const Gprb::Replay::Playback::NavView nav = Gprb::Replay::Playback::GetNav();
    const float line_h = ImGui::GetTextLineHeight();
    const float nav_button = line_h * 1.4f;
    // (On the way to a highlight, it is where the frame shown is going.)
    const s64 cursor = info.seeking && info.clip_armed ? info.clip_start : info.shown;
    int prev_mark = -1, next_mark = -1, playing_position = 0;
    for (size_t i = 0; i < info.marks.size(); ++i)
    {
      const auto& m = info.marks[i];
      if (m.position <= 0)
        continue;
      if (m.start < cursor - PREV_RESTARTS_AFTER && (prev_mark < 0 || m.start > info.marks[prev_mark].start))
        prev_mark = static_cast<int>(i);
      if (m.start > cursor && (next_mark < 0 || m.start < info.marks[next_mark].start))
        next_mark = static_cast<int>(i);
      if (m.start == info.clip_start && m.end == info.clip_end)
        playing_position = m.position;
    }
    const int position = playing_position > 0 ? playing_position : nav.position;
    const std::string count =
        nav.shown ? fmt::format("{}/{}", position > 0 ? std::to_string(position) : std::string("-"), nav.total) :
                    std::string();
    const float nav_w = nav.shown ? ImGui::CalcTextSize(count.c_str()).x + ImGui::GetStyle().ItemSpacing.x +
                                        nav_button * 4.0f + 2.0f * scale * 3.0f :
                                    0.0f;
    const ImVec2 title0 = ImGui::GetCursorScreenPos();
    if (nav.shown)
    {
      ImGui::PushClipRect(title0, ImVec2(title0.x + std::max(0.0f, ImGui::GetContentRegionAvail().x - nav_w - 8.0f * scale),
                                         title0.y + line_h + 2.0f), true);
    }
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
    if (nav.shown)
    {
      ImGui::PopClipRect();
      ImGui::SameLine(width - ImGui::GetStyle().WindowPadding.x - nav_w);
      ImGui::TextUnformatted(count.c_str());
      using Gprb::Replay::Playback::NavAction;
      struct Button
      {
        const char* id;
        const char* tip;
        int arrows;
        bool left;
        bool enabled;
        NavAction action;
        int mark;  // a mark of this replay to play instead (-1: ask the launcher)
      };
      const Button buttons[] = {
          {"##navprevreplay", "Previous replay", 2, true, nav.prev_replay, NavAction::PrevReplay, -1},
          {"##navprevclip", "Previous clip", 1, true, prev_mark >= 0 || nav.prev_replay, NavAction::PrevClip,
           prev_mark},
          {"##navnextclip", "Next clip", 1, false, next_mark >= 0 || nav.next_replay, NavAction::NextClip,
           next_mark},
          {"##navnextreplay", "Next replay", 2, false, nav.next_replay, NavAction::NextReplay, -1},
      };
      for (const Button& b : buttons)
      {
        ImGui::SameLine(0.0f, 2.0f * scale);
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        const ImVec2 b1(b0.x + nav_button, b0.y + line_h);
        const bool pressed = ImGui::InvisibleButton(b.id, ImVec2(nav_button, line_h));
        const bool over = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (over && b.enabled)
          dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 255, 40), 3.0f * scale);
        Arrows(dl, b0, b1, b.arrows, b.left,
               !b.enabled ? IM_COL32(230, 230, 230, 70) :
               over       ? IM_COL32(255, 255, 255, 255) :
                            IM_COL32(230, 230, 230, 220));
        if (over)
          ImGui::SetTooltip("%s", b.tip);
        if (pressed && b.enabled)
        {
          if (b.mark >= 0)
            Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::PlayHighlight, b.mark);
          else
            Gprb::Replay::Playback::RequestNav(b.action, cursor);
        }
      }
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
      EndClipMode();
    // Clip mode's end pauses playing as a highlight's does (the user, 2026-10-24).
    if (s_clip_mode)
      SetStop(s_clip_end);
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
          // Ends at the frame shown, starts 20 s before it; paused at its start (the user,
          // 2026-10-24: "creating a clip should seek to the start of the clip"), so Play plays it
          // and pauses at its end.
          s_clip_mode = true;
          s_clip_handle = 0;
          s_clip_title = info.title;
          s_clip_first = info.first;
          s_clip_end = std::clamp(shown, info.first + MIN_CLIP_FRAMES, std::max(end, info.first + MIN_CLIP_FRAMES));
          s_clip_start = std::max(info.first, s_clip_end - CLIP_BEFORE_FRAMES);
          if (!info.paused)
            Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::Pause, 0);
          Gprb::Session::PlaybackControl(Gprb::Session::PlaybackCommand::Seek, static_cast<double>(s_clip_start));
          SetStop(s_clip_end);
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
          EndClipMode();
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
          EndClipMode();
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
      if (handle_hovered)
      {
        s_handle_press_x = io.MousePos.x;
        s_handle_press_frame = handle_hovered == 1 ? s_clip_start : s_clip_end;
        s_handle_moved = false;
      }
    }
    if (s_clip_mode && (handle_hovered || s_clip_handle))
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (active && s_clip_handle)
    {
      // Moved only once clearly dragged; then by the mouse's movement since the press.
      const float dx = io.MousePos.x - s_handle_press_x;
      if (!s_handle_moved && std::abs(dx) >= HANDLE_DRAG_PX * scale)
        s_handle_moved = true;
      if (s_handle_moved)
      {
        const s64 frame = s_handle_press_frame +
                          static_cast<s64>(std::lround(dx / std::max(bar_w, 1.0f) * static_cast<float>(span)));
        if (s_clip_handle == 1)
        {
          s_clip_start = std::clamp(frame, info.first, s_clip_end - MIN_CLIP_FRAMES);
        }
        else
        {
          s_clip_end = std::clamp(frame, s_clip_start + MIN_CLIP_FRAMES, std::max(end, s_clip_start + MIN_CLIP_FRAMES));
          SetStop(s_clip_end);
        }
      }
    }
    else if (!active && s_clip_handle)
    {
      // A click on a handle goes there; a drag shows the frame it was left at.
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
    // Where a highlight ends: the one playing while its end is ahead (it pauses there), the
    // hovered star's while hovered (the user, 2026-10-24).
    const auto end_line = [&](s64 frame, ImU32 col) {
      const float x = bar_x + bar_frac(frame) * bar_w;
      dl->AddRectFilled(ImVec2(x - 1.0f * scale, p0.y - 3.0f * scale), ImVec2(x + 1.0f * scale, p1.y + 3.0f * scale),
                        col);
    };
    if (info.clip_armed && info.clip_end >= 0)
      end_line(info.clip_end, IM_COL32(255, 255, 255, 230));
    if (mark_hovered >= 0 && static_cast<size_t>(mark_hovered) < info.marks.size())
      end_line(info.marks[mark_hovered].end, IM_COL32(255, 230, 120, 230));
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
