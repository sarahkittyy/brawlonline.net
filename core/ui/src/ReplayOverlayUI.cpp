// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/UI/ReplayOverlayUI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/WorkQueueThread.h"
#include "BrawlOnline/Config/OnlineSettings.h"
#include "BrawlOnline/Online/GameSetup.h"
#include "BrawlOnline/Rollback/GameplaySession.h"
#include "Core/Rollback/ReplayPlayback.h"
#include "BrawlOnline/UI/OnScreenDisplay.h"

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
// Clip mode (Clip, then "Save as highlight", Render or "Cancel clip"): the clip's first and last
// frames, the replay it is of (its title and first frame: another replay armed ends it), and the
// handle held (0: none, 1: the start, 2: the end): where it was pressed, its frame then, and whether
// it has been dragged (past HANDLE_DRAG_PX; until then a release is a click, which seeks there).
bool s_clip_mode = false;
s64 s_clip_start = 0;
s64 s_clip_end = 0;
std::string s_clip_title;
s64 s_clip_first = 0;
int s_clip_handle = 0;
// A live game's bar in clip mode: its end held at the live edge when Clip was clicked (or at the
// clip's end, once its handle is let go past that), so the clip does not drift left as the game
// goes on (Sarah's board, 2026-11-22: "the current latest live position keeps moving right, so the
// CLIP bounds keep slowly shifting to the left").
s64 s_clip_bar_end = 0;
float s_handle_press_x = 0;
s64 s_handle_press_frame = 0;
bool s_handle_moved = false;
// The session's clip mode end as last set (Session::SetPlaybackStop; -1: none).
s64 s_stop_sent = -1;
// A render just asked for (its replay and frames, when): until the launcher's overlay file has it,
// its row says it waits instead of offering Render again.
std::string s_pending_path;
s64 s_pending_start = -1;
s64 s_pending_end = -1;
std::chrono::steady_clock::time_point s_pending_since{};
constexpr auto PENDING_FOR = std::chrono::seconds(10);

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
// The -5 / +5 buttons (as the hotkeys' Jump).
constexpr double JUMP_SECONDS = 5;

// The look (the user's mockup, 2026-11-20): a bar across the bottom over a dark fade, the
// launcher's colour for what has played (<comm>.overlay.json; its plain grey without a launcher).
constexpr ImU32 TEXT = IM_COL32(242, 242, 242, 255);
constexpr ImU32 TEXT_2 = IM_COL32(180, 180, 180, 255);
constexpr ImU32 TEXT_DIM = IM_COL32(147, 147, 147, 255);
constexpr ImU32 ICON_OFF = IM_COL32(255, 255, 255, 77);
constexpr ImU32 HOVER_BG = IM_COL32(255, 255, 255, 36);
constexpr ImU32 GHOST_BG = IM_COL32(255, 255, 255, 31);
constexpr ImU32 GHOST_HOVER = IM_COL32(255, 255, 255, 56);
constexpr ImU32 TRACK = IM_COL32(255, 255, 255, 56);
constexpr ImU32 GOLD = IM_COL32(255, 200, 60, 255);
constexpr ImU32 GOLD_LIT = IM_COL32(255, 226, 122, 255);
constexpr ImU32 GOLD_SPAN = IM_COL32(255, 200, 60, 115);
constexpr ImU32 GOLD_SPAN_LIT = IM_COL32(255, 200, 60, 217);
// Clip mode's colour (the clip on the bar, Render, the star being watched): the launcher's, the
// colour opposite its own (Sarah's board, 2026-11-24: "should use color theory to be the
// complimentary color to the launcher color that's used as the scrubber"), this blue without it.
constexpr ImU32 DEFAULT_CLIP = IM_COL32(61, 139, 255, 255);
constexpr ImU32 DEFAULT_CLIP_HOVER = IM_COL32(90, 160, 255, 255);
ImU32 s_clip_col = DEFAULT_CLIP;
ImU32 s_clip_hover = DEFAULT_CLIP_HOVER;
constexpr ImU32 LIVE_RED = IM_COL32(210, 44, 44, 255);
constexpr ImU32 DESYNC_RED = IM_COL32(255, 77, 77, 255);
constexpr ImU32 PANEL = IM_COL32(15, 15, 15, 222);
constexpr ImU32 PANEL_BORDER = IM_COL32(255, 255, 255, 26);
constexpr ImU32 DEFAULT_ACCENT = IM_COL32(208, 208, 208, 255);

std::string Clock60(s64 frames)
{
  const s64 seconds = std::max<s64>(frames, 0) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

ImU32 WithAlpha(ImU32 col, int alpha)
{
  return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

// The overlay is drawn on the GPU thread, which must never wait on the session's lock (nor the
// comm file follower's, held while it takes the session's): the CPU thread holds it while it waits
// for the GPU (a match's start flushes it, StartRunning), so Clip clicked as a highlight loaded
// hung Dolphin (the user, 2026-10-24). What the overlay asks of them runs on this thread, in order.
// (Never joined: at exit it may still be waiting on the session.)
void Send(std::function<void()> f)
{
  static auto* const worker = new Common::AsyncWorkThread("Replay overlay");
  worker->Push(std::move(f));
}

void Control(Gprb::Session::PlaybackCommand command, double arg = 0)
{
  Send([command, arg] { Gprb::Session::PlaybackControl(command, arg); });
}

void SetStop(s64 end)
{
  if (end != s_stop_sent)
  {
    s_stop_sent = end;
    Send([end] { Gprb::Session::SetPlaybackStop(end); });
  }
}

void EndClipMode()
{
  s_clip_mode = false;
  s_clip_handle = 0;
  SetStop(-1);
}

// ---- Icons, drawn in a box of side `s` centred on `c`. ----

void IconPlay(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
  const float h = s * 0.62f;
  const float w = h * 0.86f;
  const float x = c.x - w * 0.4f;
  dl->AddTriangleFilled(ImVec2(x, c.y - h * 0.5f), ImVec2(x, c.y + h * 0.5f), ImVec2(x + w, c.y), col);
}

void IconPause(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
  const float h = s * 0.62f;
  const float w = s * 0.2f;
  const float gap = s * 0.16f;
  const float r = w * 0.25f;
  dl->AddRectFilled(ImVec2(c.x - gap * 0.5f - w, c.y - h * 0.5f), ImVec2(c.x - gap * 0.5f, c.y + h * 0.5f), col, r);
  dl->AddRectFilled(ImVec2(c.x + gap * 0.5f, c.y - h * 0.5f), ImVec2(c.x + gap * 0.5f + w, c.y + h * 0.5f), col, r);
}

// A circular arrow with a 5 in it: back (counter-clockwise) or forward.
void IconJump(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool forward, float font_px)
{
  const float r = s * 0.36f;
  const float t = std::max(1.5f, s * 0.085f);
  constexpr float PI = 3.14159265f;
  // An open ring, its gap at the top on the arrow's side.
  const float a0 = forward ? -PI * 0.5f + 0.35f : -PI * 0.5f - 0.35f;
  const float a1 = forward ? -PI * 0.5f - 1.65f * PI + 0.35f : -PI * 0.5f + 1.65f * PI - 0.35f;
  dl->PathArcTo(c, r, a0, a1, 24);
  dl->PathStroke(col, 0, t);
  // The arrowhead at the ring's start, pointing along it.
  const ImVec2 p(c.x + std::cos(a0) * r, c.y + std::sin(a0) * r);
  const float dir = forward ? 1.0f : -1.0f;
  const float k = s * 0.17f;
  dl->AddTriangleFilled(ImVec2(p.x + dir * k * 1.1f, p.y), ImVec2(p.x - dir * k * 0.2f, p.y - k),
                        ImVec2(p.x - dir * k * 0.2f, p.y + k), col);
  ImFont* font = ImGui::GetFont();
  const ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, "5");
  dl->AddText(font, font_px, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f + s * 0.02f), col, "5");
}

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

void StarOutline(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t)
{
  ImVec2 pts[10];
  for (int i = 0; i < 10; ++i)
  {
    const float a = -1.5707964f + i * 0.62831855f;
    const float k = i % 2 == 0 ? r : r * 0.45f;
    pts[i] = ImVec2(c.x + std::cos(a) * k, c.y + std::sin(a) * k);
  }
  dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, t);
}

// |< (previous replay) or >| (next replay).
void IconSkip(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool left)
{
  const float h = s * 0.56f;
  const float bar = s * 0.11f;
  const float w = h * 0.8f;
  const float total = bar + w;
  const float x0 = c.x - total * 0.5f;
  if (left)
  {
    dl->AddRectFilled(ImVec2(x0, c.y - h * 0.5f), ImVec2(x0 + bar, c.y + h * 0.5f), col, bar * 0.3f);
    dl->AddTriangleFilled(ImVec2(x0 + total, c.y - h * 0.5f), ImVec2(x0 + total, c.y + h * 0.5f),
                          ImVec2(x0 + bar, c.y), col);
  }
  else
  {
    dl->AddRectFilled(ImVec2(x0 + w, c.y - h * 0.5f), ImVec2(x0 + total, c.y + h * 0.5f), col, bar * 0.3f);
    dl->AddTriangleFilled(ImVec2(x0, c.y - h * 0.5f), ImVec2(x0, c.y + h * 0.5f), ImVec2(x0 + w, c.y), col);
  }
}

// <* (previous clip) or *> (next clip): a triangle and a small star on the far side.
void IconClipStep(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool left)
{
  const float h = s * 0.5f;
  const float w = h * 0.8f;
  const float sr = s * 0.2f;
  const float total = w + sr * 2.0f;
  const float x0 = c.x - total * 0.5f;
  if (left)
  {
    Star(dl, ImVec2(x0 + sr, c.y - h * 0.15f), sr, col);
    dl->AddTriangleFilled(ImVec2(x0 + total, c.y - h * 0.5f), ImVec2(x0 + total, c.y + h * 0.5f),
                          ImVec2(x0 + sr * 2.0f, c.y), col);
  }
  else
  {
    dl->AddTriangleFilled(ImVec2(x0, c.y - h * 0.5f), ImVec2(x0, c.y + h * 0.5f), ImVec2(x0 + w, c.y), col);
    Star(dl, ImVec2(x0 + w + sr, c.y - h * 0.15f), sr, col);
  }
}

void IconScissors(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
  const float t = std::max(1.5f, s * 0.11f);
  const float r = s * 0.15f;
  const ImVec2 a(c.x - s * 0.24f, c.y - s * 0.24f);
  const ImVec2 b(c.x - s * 0.24f, c.y + s * 0.24f);
  dl->AddCircle(a, r, col, 16, t);
  dl->AddCircle(b, r, col, 16, t);
  dl->AddLine(ImVec2(a.x + r * 0.75f, a.y + r * 0.75f), ImVec2(c.x + s * 0.38f, c.y + s * 0.38f), col, t);
  dl->AddLine(ImVec2(b.x + r * 0.75f, b.y - r * 0.75f), ImVec2(c.x + s * 0.38f, c.y - s * 0.38f), col, t);
}

void IconFilm(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
  const float t = std::max(1.2f, s * 0.09f);
  const ImVec2 a(c.x - s * 0.4f, c.y - s * 0.3f);
  const ImVec2 z(c.x + s * 0.4f, c.y + s * 0.3f);
  dl->AddRect(a, z, col, s * 0.08f, 0, t);
  const float y1 = a.y + (z.y - a.y) * 0.3f;
  const float y2 = z.y - (z.y - a.y) * 0.3f;
  dl->AddLine(ImVec2(a.x, y1), ImVec2(z.x, y1), col, t);
  dl->AddLine(ImVec2(a.x, y2), ImVec2(z.x, y2), col, t);
  for (float fx : {0.33f, 0.67f})
  {
    const float x = a.x + (z.x - a.x) * fx;
    dl->AddLine(ImVec2(x, a.y), ImVec2(x, y1), col, t);
    dl->AddLine(ImVec2(x, y2), ImVec2(x, z.y), col, t);
  }
}

// A chevron pointing down (fold) or up (unfold).
void IconChevron(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool up)
{
  const float w = s * 0.3f;
  const float h = s * 0.16f;
  const float t = std::max(1.5f, s * 0.12f);
  const float d = up ? -1.0f : 1.0f;
  const ImVec2 pts[3] = {ImVec2(c.x - w, c.y - d * h), ImVec2(c.x, c.y + d * h), ImVec2(c.x + w, c.y - d * h)};
  dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t);
}

// A folder (a finished render's: open the file explorer at its video).
void IconFolder(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
  const float t = std::max(1.3f, s * 0.1f);
  const float w = s * 0.42f;
  const float h = s * 0.3f;
  const ImVec2 pts[] = {ImVec2(c.x - w, c.y - h),          ImVec2(c.x - w * 0.25f, c.y - h),
                        ImVec2(c.x - w * 0.05f, c.y - h * 0.6f), ImVec2(c.x + w, c.y - h * 0.6f),
                        ImVec2(c.x + w, c.y + h),          ImVec2(c.x - w, c.y + h)};
  dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
  dl->AddLine(ImVec2(c.x - w, c.y - h * 0.2f), ImVec2(c.x + w, c.y - h * 0.2f), col, t);
}

void IconX(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
  const float k = s * 0.26f;
  const float t = std::max(1.5f, s * 0.11f);
  dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, t);
  dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, t);
}

// ---- Widgets ----

// An icon button at `p` (side `s`): a rounded light square under the mouse. True when clicked.
bool IconButton(const char* id, ImVec2 p, float s, bool enabled, const char* tip,
                const std::function<void(ImDrawList*, ImVec2, float, ImU32)>& icon, float scale)
{
  ImGui::SetCursorScreenPos(p);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(s, s));
  const bool over = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (over && enabled)
    dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), HOVER_BG, 8.0f * scale);
  icon(dl, ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.56f, enabled ? TEXT : ICON_OFF);
  if (over && tip)
    ImGui::SetTooltip("%s", tip);
  return pressed && enabled;
}

enum class ButtonLook
{
  Ghost,
  Solid,
};

float TextButtonWidth(const char* label, bool icon, float scale)
{
  return ImGui::CalcTextSize(label).x + 24.0f * scale + (icon ? 24.0f * scale : 0.0f);
}

// A text button (with an icon left of it), `h` tall at `p`. `round`: which corners are rounded.
bool TextButton(const char* id, const char* label, ImVec2 p, float h, ButtonLook look,
                const std::function<void(ImDrawList*, ImVec2, float, ImU32)>& icon, float scale,
                ImDrawFlags round = ImDrawFlags_RoundCornersAll)
{
  const float w = TextButtonWidth(label, static_cast<bool>(icon), scale);
  ImGui::SetCursorScreenPos(p);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool over = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = look == ButtonLook::Solid ? (over ? s_clip_hover : s_clip_col) : (over ? GHOST_HOVER : GHOST_BG);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 7.0f * scale, round);
  float x = p.x + 12.0f * scale;
  if (icon)
  {
    icon(dl, ImVec2(x + 8.0f * scale, p.y + h * 0.5f), 17.0f * scale, TEXT);
    x += 24.0f * scale;
  }
  const ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(x, p.y + (h - ts.y) * 0.5f), TEXT, label);
  return pressed;
}

// Render (w/ Music), with a chevron right of it that opens a menu with Render (No Music), its
// right edge at `right`: `render(music)` once one is chosen. Returns its left edge.
float RenderSplit(float right, float top, float h, float scale, const std::function<void(bool)>& render)
{
  constexpr const char* RENDER_MUSIC = "Render (w/ Music)";
  constexpr const char* RENDER_NO_MUSIC = "Render (No Music)";
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float caret_w = 28.0f * scale;
  const ImVec2 caret0(right - caret_w - 2.0f * scale, top);
  ImGui::SetCursorScreenPos(caret0);
  const bool caret = ImGui::InvisibleButton("##replayrendermenu", ImVec2(caret_w, h));
  const bool caret_over = ImGui::IsItemHovered();
  dl->AddRectFilled(caret0, ImVec2(caret0.x + caret_w, caret0.y + h), caret_over ? s_clip_hover : s_clip_col,
                    7.0f * scale, ImDrawFlags_RoundCornersRight);
  dl->AddRectFilled(ImVec2(caret0.x - 1.0f, caret0.y), ImVec2(caret0.x, caret0.y + h), IM_COL32(0, 0, 0, 90));
  IconChevron(dl, ImVec2(caret0.x + caret_w * 0.5f, caret0.y + h * 0.5f), 14.0f * scale, TEXT, false);
  if (caret)
    ImGui::OpenPopup("##replayrenderpopup");
  const auto film = [](ImDrawList* d, ImVec2 c, float s, ImU32 col) { IconFilm(d, c, s, col); };
  const float left = caret0.x - TextButtonWidth(RENDER_MUSIC, true, scale);
  if (TextButton("##replayrender", RENDER_MUSIC, ImVec2(left, top), h, ButtonLook::Solid, film, scale,
                 ImDrawFlags_RoundCornersLeft))
  {
    render(true);
  }
  // The menu, above the button (its right edge at the caret's).
  ImGui::SetNextWindowPos(ImVec2(caret0.x + caret_w, top - 6.0f * scale), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 4.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 6.0f * scale));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 31));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(255, 255, 255, 46));
  if (ImGui::BeginPopup("##replayrenderpopup"))
  {
    const ImVec2 item(std::max(TextButtonWidth(RENDER_NO_MUSIC, false, scale), 150.0f * scale),
                      ImGui::GetTextLineHeight() + 12.0f * scale);
    if (ImGui::Selectable(RENDER_MUSIC, false, 0, item))
      render(true);
    if (ImGui::Selectable(RENDER_NO_MUSIC, false, 0, item))
      render(false);
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
  return left;
}

// "Clip 2:13 - 2:20 (7 s)": clip mode's own line, for the launcher's toast of a render.
std::string ClipLabel(s64 start, s64 end, s64 first)
{
  return fmt::format("Clip {} - {} ({} s)", Clock60(start - first), Clock60(end - first), (end - start + 30) / 60);
}

// A render's state on the row (the launcher's words: "Waiting", "Rendering 42%", "Saved"), with a
// folder button when `folder_id` is a finished one's, its right edge at `right`. Returns its left.
float RenderState(float right, float top, float h, float scale, const std::string& text, const std::string& folder_id)
{
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  const float pad = 12.0f * scale;
  const float fs = folder_id.empty() ? 0.0f : h;
  const float w = pad + ts.x + (fs > 0 ? 6.0f * scale + fs : pad);
  const float left = right - w;
  dl->AddRectFilled(ImVec2(left, top), ImVec2(right, top + h), WithAlpha(s_clip_col, 60), 7.0f * scale);
  dl->AddText(ImVec2(left + pad, top + (h - ts.y) * 0.5f), TEXT, text.c_str());
  if (fs > 0)
  {
    const ImVec2 f0(right - fs, top);
    ImGui::SetCursorScreenPos(f0);
    if (ImGui::InvisibleButton("##replayrenderfolder", ImVec2(fs, h)))
      Send([id = folder_id] { Gprb::Replay::Playback::ShowRenderFolder(id); });
    const bool over = ImGui::IsItemHovered();
    if (over)
      dl->AddRectFilled(f0, ImVec2(f0.x + fs, f0.y + h), HOVER_BG, 7.0f * scale, ImDrawFlags_RoundCornersRight);
    IconFolder(dl, ImVec2(f0.x + fs * 0.5f, f0.y + h * 0.5f), h * 0.62f, TEXT);
  }
  return left;
}

// A tooltip box drawn above `anchor` (centred on it), over everything.
void TipAbove(ImVec2 anchor, const std::string& text, float scale, float min_x, float max_x)
{
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  const ImVec2 pad(8.0f * scale, 4.0f * scale);
  const float w = ts.x + pad.x * 2.0f;
  const float h = ts.y + pad.y * 2.0f;
  const float x = std::clamp(anchor.x - w * 0.5f, min_x, std::max(min_x, max_x - w));
  const ImVec2 a(x, anchor.y - h);
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), PANEL, 6.0f * scale);
  dl->AddRect(a, ImVec2(a.x + w, a.y + h), PANEL_BORDER, 6.0f * scale);
  dl->AddText(ImVec2(a.x + pad.x, a.y + pad.y), TEXT, text.c_str());
}

// The fold button at the bar's right end: a chevron down that folds it to a thin line, a chevron up
// on that line that opens it again (the user, 2026-11-01: "so moving my mouse doesn't keep showing
// it all the time and i can hide it for my stream"). Kept in [Online] SeekBarCollapsed. True when
// clicked.
bool FoldButton(ImVec2 p, float s, float scale, bool collapsed)
{
  const bool pressed =
      IconButton("##replayfold", p, s, true, collapsed ? "Show the seek bar" : "Fold the seek bar to a thin bar",
                 [collapsed](ImDrawList* dl, ImVec2 c, float sz, ImU32 col) { IconChevron(dl, c, sz, col, collapsed); },
                 scale);
  if (pressed)
  {
    Config::SetBaseOrCurrent(Config::ONLINE_SEEK_BAR_COLLAPSED, !collapsed);
    Config::Save();
  }
  return pressed;
}

constexpr ImGuiWindowFlags WINDOW_FLAGS =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;

// The overlay's windows: no padding or border (only for the window: tooltips and menus keep theirs).
bool BeginBare(const char* name, ImGuiWindowFlags extra = 0)
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const bool open = ImGui::Begin(name, nullptr, WINDOW_FLAGS | extra);
  ImGui::PopStyleVar(2);
  return open;
}

// The launcher's renders asked for here ("Clip 2:13 - 2:20 (7 s)  Rendering 42%"), at the top
// right while the launcher shows them; a finished one has a folder button that opens the file
// explorer at its video (Sarah's board, 2026-11-20).
void Toasts(const Gprb::Replay::Playback::OverlayView& view, ImU32 accent, float scale)
{
  std::vector<const Gprb::Replay::Playback::OverlayToast*> shown;
  for (const auto& t : view.toasts)
  {
    if (t.toast)
      shown.push_back(&t);
  }
  if (shown.size() > 4)
    shown.erase(shown.begin(), shown.end() - 4);
  if (shown.empty())
    return;
  const ImGuiIO& io = ImGui::GetIO();
  const float w = 270.0f * scale;
  const float pad = 12.0f * scale;
  const float line = ImGui::GetTextLineHeight();
  const float h = pad * 2.0f + line + 12.0f * scale;
  const float gap = 8.0f * scale;
  const float total = shown.size() * (h + gap);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 20.0f * scale - w, 18.0f * scale), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, total), ImGuiCond_Always);
  if (BeginBare("##ReplayOverlayToasts"))
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = ImGui::GetWindowPos().y;
    const float x = ImGui::GetWindowPos().x;
    for (const auto* tp : shown)
    {
      const auto& t = *tp;
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), PANEL, 10.0f * scale);
      dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), PANEL_BORDER, 10.0f * scale);
      // The folder button at the right of a finished one's first line.
      float right = x + w - pad;
      if (t.folder && !t.id.empty())
      {
        const float fs = line + 6.0f * scale;
        const ImVec2 f0(right - fs + 3.0f * scale, y + pad - 3.0f * scale);
        ImGui::SetCursorScreenPos(f0);
        ImGui::PushID(t.id.c_str());
        if (ImGui::InvisibleButton("##toastfolder", ImVec2(fs, fs)))
          Send([id = t.id] { Gprb::Replay::Playback::ShowRenderFolder(id); });
        const bool over = ImGui::IsItemHovered();
        ImGui::PopID();
        if (over)
          dl->AddRectFilled(f0, ImVec2(f0.x + fs, f0.y + fs), HOVER_BG, 5.0f * scale);
        IconFolder(dl, ImVec2(f0.x + fs * 0.5f, f0.y + fs * 0.5f), fs * 0.8f, over ? TEXT : TEXT_2);
        right -= fs + 4.0f * scale;
      }
      const ImVec2 rs = ImGui::CalcTextSize(t.text.c_str());
      dl->PushClipRect(ImVec2(x + pad, y), ImVec2(right - rs.x - 8.0f * scale, y + h), true);
      dl->AddText(ImVec2(x + pad, y + pad), TEXT, t.label.c_str());
      dl->PopClipRect();
      dl->AddText(ImVec2(right - rs.x, y + pad), t.state == "error" ? DESYNC_RED : TEXT_2, t.text.c_str());
      const float by = y + pad + line + 6.0f * scale;
      const float bh = 4.0f * scale;
      dl->AddRectFilled(ImVec2(x + pad, by), ImVec2(x + w - pad, by + bh), IM_COL32(255, 255, 255, 38), bh * 0.5f);
      const float p = t.state == "done" ? 1.0f : t.progress;
      if (p > 0)
      {
        dl->AddRectFilled(ImVec2(x + pad, by), ImVec2(x + pad + (w - pad * 2.0f) * p, by + bh),
                          t.state == "error" ? DESYNC_RED : accent, bh * 0.5f);
      }
      y += h + gap;
    }
  }
  ImGui::End();
}

// Above the bar: the stream's line (the launcher's banner, "Closing in 5 s ..."), or Auto play's
// countdown "Next game in 3 s." with an X that stays on this game and a ring of the time left (the
// user, 2026-11-20). `bottom`: the pill's bottom edge.
void Pill(const Gprb::Session::PlaybackInfo& info, const Gprb::Replay::Playback::NextCountdown& next, float bottom,
          float scale)
{
  std::string text;
  if (next.shown)
  {
    if (next.waiting)
    {
      // Ran out: the launcher's line while it waits for a live game, else still the countdown's
      // (never the X and the ring alone).
      text = info.banner.empty() ? std::string("Next game in 1 s.") : info.banner;
    }
    else
    {
      const double left = std::chrono::duration<double>(next.deadline - Clock::now()).count();
      text = fmt::format("Next game in {} s.", std::max<long long>(static_cast<long long>(std::ceil(left)), 1));
    }
  }
  else
  {
    text = info.banner;
  }
  if (text.empty() && !next.shown)
    return;
  const ImGuiIO& io = ImGui::GetIO();
  const float h = 34.0f * scale;
  const float icon = 20.0f * scale;
  const float pad = 14.0f * scale;
  const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  const float x_w = next.shown ? icon + 10.0f * scale : 0.0f;
  const float ring_w = next.shown ? icon + 10.0f * scale : 0.0f;
  const float w = pad * 2.0f + x_w + ts.x + ring_w;
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f, bottom - h), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  if (BeginBare("##ReplayOverlayPill"))
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), PANEL, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), PANEL_BORDER, h * 0.5f);
    float x = p.x + pad;
    if (next.shown)
    {
      // The X: stay on this game.
      const ImVec2 b0(x - 4.0f * scale, p.y + (h - icon - 8.0f * scale) * 0.5f);
      ImGui::SetCursorScreenPos(b0);
      const ImVec2 bs(icon + 8.0f * scale, icon + 8.0f * scale);
      if (ImGui::InvisibleButton("##replaynextcancel", bs))
        Send([] { Gprb::Replay::Playback::CancelNext(); });
      const bool over = ImGui::IsItemHovered();
      if (over)
        dl->AddRectFilled(b0, ImVec2(b0.x + bs.x, b0.y + bs.y), HOVER_BG, bs.x * 0.5f);
      IconX(dl, ImVec2(b0.x + bs.x * 0.5f, b0.y + bs.y * 0.5f), icon * 0.8f, over ? TEXT : TEXT_2);
      x += x_w;
    }
    dl->AddText(ImVec2(x, p.y + (h - ts.y) * 0.5f), TEXT, text.c_str());
    x += ts.x + 10.0f * scale;
    if (next.shown)
    {
      // The ring: the time gone of the countdown (spinning while the launcher gets the next game).
      const ImVec2 c(x + icon * 0.5f, p.y + h * 0.5f);
      const float r = icon * 0.42f;
      const float t = 2.5f * scale;
      dl->AddCircle(c, r, IM_COL32(255, 255, 255, 51), 32, t);
      constexpr float PI = 3.14159265f;
      if (next.waiting)
      {
        const float a = static_cast<float>(
            std::fmod(std::chrono::duration<double>(Clock::now().time_since_epoch()).count() * 5.0, 2.0 * PI));
        dl->PathArcTo(c, r, a, a + PI * 0.6f, 16);
      }
      else
      {
        const double left = std::chrono::duration<double>(next.deadline - Clock::now()).count();
        const float done = static_cast<float>(std::clamp(1.0 - left / next.total_seconds, 0.0, 1.0));
        dl->PathArcTo(c, r, -PI * 0.5f, -PI * 0.5f + std::max(done, 0.01f) * 2.0f * PI, 32);
      }
      dl->PathStroke(TEXT, 0, t);
    }
  }
  ImGui::End();
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
  // (The session busy for a moment, e.g. a match's start or a seek's step, is not the replay gone:
  // the last info stands in, so clip mode, a drag and the overlay stay.)
  static Gprb::Session::PlaybackInfo s_last_info;
  Gprb::Session::PlaybackInfo info = Gprb::Session::GetPlaybackInfo();
  if (info.busy)
    info = s_last_info;
  else
    s_last_info = info;
  if (!info.active)
  {
    s_dragging = false;
    s_mark_held = -1;
    EndClipMode();
    Gprb::Replay::Playback::SetClipMode(false);
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
  // Not a click on another window (the chat, drawn before this, has already made the item under it
  // active, which IsWindowHovered alone takes as not hovered), nor one that closes a menu.
  const bool catching_up = info.live && !info.ended && info.catching_up;
  if (io.MouseClicked[0] && !s_dragging && !info.at_end &&
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                              ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
  {
    Control(catching_up ? Gprb::Session::PlaybackCommand::Play :
                          Gprb::Session::PlaybackCommand::TogglePause);
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
  // Clip mode holds Auto play's countdown at a stream's end.
  Gprb::Replay::Playback::SetClipMode(s_clip_mode);
  const auto next = Gprb::Replay::Playback::GetNextCountdown();
  const auto view = Gprb::Replay::Playback::GetOverlayView();
  const ImU32 accent =
      view.has_accent ? IM_COL32(view.accent[0], view.accent[1], view.accent[2], 255) : DEFAULT_ACCENT;
  // (Hovered: a fifth of the way to white.)
  const auto lighter = [&view](int i) { return view.clip[i] + (255 - view.clip[i]) / 5; };
  s_clip_col = view.has_clip ? IM_COL32(view.clip[0], view.clip[1], view.clip[2], 255) : DEFAULT_CLIP;
  s_clip_hover = view.has_clip ? IM_COL32(lighter(0), lighter(1), lighter(2), 255) : DEFAULT_CLIP_HOVER;
  if (s_hidden)
    return;

  // (The OSD's font size is the user's; the overlay's text is the chat's font at a fixed size.)
  const float main_scale = ImGui::GetStyle().FontScaleMain > 0 ? ImGui::GetStyle().FontScaleMain : 1.0f;
  ImGui::PushFont(s_font, 14.5f * scale / main_scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 4.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, PANEL);
  ImGui::PushStyleColor(ImGuiCol_Border, PANEL_BORDER);
  ImGui::PushStyleColor(ImGuiCol_Text, TEXT);
  const auto pop = [] {
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
  };

  // The renders asked for here, while the launcher has them (also with the bar hidden).
  Toasts(view, accent, scale);

  // Spectating: also while catching up, buffering, or with something to say (the banner, Auto
  // play's countdown).
  const bool wanted = info.paused || info.seeking || s_dragging || s_mark_held >= 0 || s_clip_mode ||
                      info.catching_up || info.buffering || !info.banner.empty() || next.shown ||
                      now - s_mouse_moved < SHOW_AFTER_MOUSE;
  if (!wanted)
  {
    pop();
    return;
  }

  const float W = io.DisplaySize.x;
  const float H = io.DisplaySize.y;
  const float margin = 28.0f * scale;
  const float bottom_pad = 12.0f * scale;
  const float ctl_h = 40.0f * scale;
  const float track_h = 36.0f * scale;
  const float btn = 34.0f * scale;
  const bool live = info.live && !info.ended;
  const s64 end = live ? std::max(info.live_edge, info.first) : info.last;
  const s64 bar_end = live && s_clip_mode ? std::clamp(s_clip_bar_end, info.first, end) : end;
  const s64 span = std::max<s64>(bar_end - info.first, 1);
  const s64 shown = std::clamp(info.shown, info.first, end);

  // Folded: a thin line of how far it has played along the bottom, and a chevron up to open it,
  // shown when the overlay would be. Nothing else of it (clip mode ends with the fold).
  if (Config::Get(Config::ONLINE_SEEK_BAR_COLLAPSED))
  {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float line = 3.0f * scale;
    const float done = std::clamp(static_cast<float>(shown - info.first) / static_cast<float>(span), 0.0f, 1.0f);
    bg->AddRectFilled(ImVec2(0, H - line), ImVec2(W, H), IM_COL32(255, 255, 255, 46));
    bg->AddRectFilled(ImVec2(0, H - line), ImVec2(W * done, H), accent);
    const ImVec2 bs(30.0f * scale, 24.0f * scale);
    const ImVec2 p0(12.0f * scale, H - line - 8.0f * scale - bs.y);
    ImGui::SetNextWindowPos(p0, ImGuiCond_Always);
    ImGui::SetNextWindowSize(bs, ImGuiCond_Always);
    if (BeginBare("##ReplayOverlayFolded"))
    {
      ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + bs.x, p0.y + bs.y), IM_COL32(15, 15, 15, 180),
                                                6.0f * scale);
      ImGui::SetCursorScreenPos(p0);
      if (ImGui::InvisibleButton("##replayunfold", bs))
      {
        Config::SetBaseOrCurrent(Config::ONLINE_SEEK_BAR_COLLAPSED, false);
        Config::Save();
      }
      const bool over = ImGui::IsItemHovered();
      if (over)
      {
        ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + bs.x, p0.y + bs.y), HOVER_BG, 6.0f * scale);
        ImGui::SetTooltip("%s", "Show the seek bar");
      }
      IconChevron(ImGui::GetWindowDrawList(), ImVec2(p0.x + bs.x * 0.5f, p0.y + bs.y * 0.5f), 16.0f * scale, TEXT,
                  true);
    }
    ImGui::End();
    Pill(info, next, p0.y - 10.0f * scale, scale);
    pop();
    return;
  }

  const float win_h = bottom_pad + ctl_h + track_h;
  const float win_y = std::floor(H - win_h);
  // The fade under it, over the game's bottom (behind every window, the chat too).
  {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float fade = win_h + 70.0f * scale;
    const float mid = H - fade * 0.55f;
    bg->AddRectFilledMultiColor(ImVec2(0, H - fade), ImVec2(W, mid), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 140), IM_COL32(0, 0, 0, 140));
    bg->AddRectFilledMultiColor(ImVec2(0, mid), ImVec2(W, H), IM_COL32(0, 0, 0, 140), IM_COL32(0, 0, 0, 140),
                                IM_COL32(0, 0, 0, 210), IM_COL32(0, 0, 0, 210));
  }
  Pill(info, next, win_y - 8.0f * scale, scale);

  ImGui::SetNextWindowPos(ImVec2(0, win_y), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(W, win_h), ImGuiCond_Always);
  if (BeginBare("##ReplayOverlay"))
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar_x = std::round(margin);
    const float bar_w = std::max(std::round(W - margin * 2.0f), 1.0f);
    const float track_y = std::round(win_y + 24.0f * scale);  // the track's centre line
    const float row_y = win_y + track_h;          // the controls' row
    const auto bar_frac = [&](s64 frame) {
      return std::clamp(static_cast<float>(frame - info.first) / span, 0.0f, 1.0f);
    };
    const auto x_of = [&](s64 frame) { return std::round(bar_x + bar_frac(frame) * bar_w); };

    // The launcher's list (highlights, not spectating): "3/40  << < > >>" (the user, 2026-10-24).
    // < > go to the previous / next star from the frame shown, in this replay at once (its marks),
    // past them in the replay before / after it (the launcher picks it); << >> to the replay
    // before / after. The Replays tab's replays: "12/340  << >>", and < > while it has stars.
    const Gprb::Replay::Playback::NavView nav = Gprb::Replay::Playback::GetNav();
    // (On the way to a highlight, it is where the frame shown is going.)
    const s64 cursor = info.seeking && info.clip_armed ? info.clip_start : info.shown;
    int prev_mark = -1, next_mark = -1, playing_position = 0;
    for (size_t i = 0; i < info.marks.size(); ++i)
    {
      const auto& m = info.marks[i];
      if (nav.shown && nav.clips && m.position <= 0)
        continue;
      if (m.start < cursor - PREV_RESTARTS_AFTER && (prev_mark < 0 || m.start > info.marks[prev_mark].start))
        prev_mark = static_cast<int>(i);
      if (m.start > cursor && (next_mark < 0 || m.start < info.marks[next_mark].start))
        next_mark = static_cast<int>(i);
      if (m.start == info.clip_start && m.end == info.clip_end)
        playing_position = m.position;
    }
    const int position = nav.clips && playing_position > 0 ? playing_position : nav.position;
    const std::string count =
        nav.shown ? fmt::format("{}/{}", position > 0 ? std::to_string(position) : std::string("-"), nav.total) :
                    std::string();
    const bool clip_buttons = (nav.shown && nav.clips) || !info.marks.empty();

    // Clip mode: its frames kept within the replay (a live game's: the live edge).
    const bool can_clip = Gprb::Replay::Playback::CanSaveClip();
    if (!can_clip)
      EndClipMode();
    if (s_clip_mode)
    {
      s_clip_end = std::clamp(s_clip_end, info.first + MIN_CLIP_FRAMES, std::max(end, info.first + MIN_CLIP_FRAMES));
      s_clip_start = std::clamp(s_clip_start, info.first, s_clip_end - MIN_CLIP_FRAMES);
      // Clip mode's end pauses playing as a highlight's does (the user, 2026-10-24).
      SetStop(s_clip_end);
    }

    // A star's highlight being watched: reached (not on the way there: Sarah's board, 2026-11-20,
    // "the render button appears when im skimming to a clip even though i havent reached it yet"),
    // playing or paused at its end, until it is left or played past. Its star is outlined in blue,
    // and the row has its Render (or that render's state).
    int watching = -1;
    if (!s_clip_mode && can_clip && !info.seeking && info.clip_start >= 0 && info.clip_end > info.clip_start &&
        (info.clip_armed || (info.paused && info.shown == info.clip_end)) && info.shown >= info.clip_start - 1 &&
        info.shown <= info.clip_end)
    {
      for (size_t i = 0; i < info.marks.size(); ++i)
      {
        if (info.marks[i].start == info.clip_start && info.marks[i].end == info.clip_end)
          watching = static_cast<int>(i);
      }
    }
    // The launcher's newest render of frames `start` to `end` of this replay (nullptr: none).
    const auto render_of = [&](s64 start, s64 stop) -> const Gprb::Replay::Playback::OverlayToast* {
      const Gprb::Replay::Playback::OverlayToast* found = nullptr;
      for (const auto& t : view.toasts)
      {
        if (t.start == start && t.end == stop && !nav.armed_path.empty() && t.path == nav.armed_path)
          found = &t;
      }
      return found;
    };

    // ---- The bar ----
    // Highlights: each one of this replay is a gold span on the bar with a star above its start; a
    // click on the star or the span's start plays it from its start (the user, 2026-10-24: a click
    // on the bar leaves the highlight for the replay, this goes back).
    const float mark_r = 8.0f * scale;
    const float mark_grab = 8.0f * scale;
    const auto mark_at = [&](float x) {
      int best = -1;
      float best_d = mark_grab;
      for (size_t i = 0; i < info.marks.size(); ++i)
      {
        const float d = std::abs(x_of(info.marks[i].start) - x);
        if (d <= best_d)
        {
          best = static_cast<int>(i);
          best_d = d;
        }
      }
      return best;
    };
    const float stars_y = track_y - 14.0f * scale;

    // Clip mode: the clip's ends are grips on the bar, each its own button over the whole grip
    // drawn (and a little past its sides), submitted first so it wins over the stars and the track
    // under it. (Sarah's board, 2026-11-24: "i tried to clip something at the start of spectate and
    // i couldn't drag the left hand scrubber ... it took me like a few tries": only the part of a
    // grip over the seek area took the press, so a grip at the bar's end, as a clip's start is in a
    // game's first 20 s, had its outer half and top dead.) Grips nearer each other than their reach
    // share the space between them at its middle.
    const float handle_grab = 9.0f * scale;
    const float handle_reach_y = 15.0f * scale;
    int handle_hovered = 0, handle_active = 0, handle_activated = 0;
    if (s_clip_mode)
    {
      const float xs = x_of(s_clip_start);
      const float xe = x_of(s_clip_end);
      const float mid = (xs + xe) * 0.5f;
      for (int h = 1; h <= 2; ++h)
      {
        const float x0 = h == 1 ? xs - handle_grab : std::max(xe - handle_grab, mid);
        const float x1 = h == 1 ? std::min(xs + handle_grab, mid) : xe + handle_grab;
        if (x1 - x0 < 1.0f)
          continue;
        ImGui::SetCursorScreenPos(ImVec2(x0, track_y - handle_reach_y));
        ImGui::InvisibleButton(h == 1 ? "##clipstart" : "##clipend", ImVec2(x1 - x0, handle_reach_y * 2.0f));
        if (ImGui::IsItemHovered())
          handle_hovered = h;
        if (ImGui::IsItemActive())
          handle_active = h;
        if (ImGui::IsItemActivated())
          handle_activated = h;
      }
    }

    int mark_hovered = -1;
    if (!info.marks.empty())
    {
      ImGui::SetCursorScreenPos(ImVec2(bar_x - mark_grab, stars_y - mark_r * 1.3f));
      if (ImGui::InvisibleButton("##replaymarks", ImVec2(bar_w + mark_grab * 2.0f, mark_r * 2.2f)))
      {
        if (const int m = mark_at(io.MousePos.x); m >= 0)
          Control(Gprb::Session::PlaybackCommand::PlayHighlight, m);
      }
      if (ImGui::IsItemHovered())
        mark_hovered = mark_at(io.MousePos.x);
    }

    // The seek area: the track and some height around it, and the knob's reach past its ends
    // (click or drag to seek; a press past an end goes to that end).
    const float seek_top = track_y - 8.0f * scale;
    const float seek_h = 18.0f * scale;
    ImGui::SetCursorScreenPos(ImVec2(bar_x - handle_grab, seek_top));
    ImGui::InvisibleButton("##replayseek", ImVec2(bar_w + handle_grab * 2.0f, seek_h));
    const bool hovered = ImGui::IsItemHovered() || handle_hovered != 0;
    const bool active = ImGui::IsItemActive();
    const auto at = [&](float x) { return std::clamp((x - bar_x) / bar_w, 0.0f, 1.0f); };
    if (ImGui::IsItemHovered() && !s_dragging && mark_hovered < 0 && s_clip_handle == 0)
      mark_hovered = mark_at(io.MousePos.x);
    // A press on a mark's start plays its highlight once released (no drag, no seek); on a clip's
    // grip, it moves that end (a grip let go shows its frame).
    if (ImGui::IsItemActivated())
      s_mark_held = mark_hovered;
    if (handle_activated)
    {
      s_clip_handle = handle_activated;
      s_mark_held = -1;
      s_handle_press_x = io.MousePos.x;
      s_handle_press_frame = handle_activated == 1 ? s_clip_start : s_clip_end;
      s_handle_moved = false;
    }
    if (s_clip_mode && (handle_hovered || s_clip_handle))
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (s_clip_handle && handle_active == s_clip_handle)
    {
      // Moved only once clearly dragged; then by the mouse's movement since the press.
      const float dx = io.MousePos.x - s_handle_press_x;
      if (!s_handle_moved && std::abs(dx) >= HANDLE_DRAG_PX * scale)
        s_handle_moved = true;
      if (s_handle_moved)
      {
        const s64 frame =
            s_handle_press_frame + static_cast<s64>(std::lround(dx / bar_w * static_cast<float>(span)));
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
    else if (s_clip_handle)
    {
      // A click on a handle goes there; a drag shows the frame it was left at.
      Control(Gprb::Session::PlaybackCommand::Seek,
              static_cast<double>(s_clip_handle == 1 ? s_clip_start : s_clip_end));
      s_clip_handle = 0;
      s_clip_bar_end = std::max(s_clip_bar_end, s_clip_end);
    }
    else if (active && s_mark_held < 0)
    {
      s_dragging = true;
      s_drag_at = at(io.MousePos.x);
    }
    else if (!active && s_mark_held >= 0)
    {
      Control(Gprb::Session::PlaybackCommand::PlayHighlight, s_mark_held);
      s_mark_held = -1;
    }
    else if (!active && s_dragging)
    {
      s_dragging = false;
      Control(Gprb::Session::PlaybackCommand::Seek, static_cast<double>(info.first + s_drag_at * span));
    }

    // Drawn: the track (thicker under the mouse), what has played in the launcher's colour, the
    // highlights' spans, where the replay stopped matching, the clip, the stars, the knob.
    const bool track_lit = hovered || s_dragging;
    const float th = (track_lit ? 7.0f : 5.0f) * scale;
    const ImVec2 t0(bar_x, track_y - th * 0.5f);
    const ImVec2 t1(bar_x + bar_w, track_y + th * 0.5f);
    dl->AddRectFilled(t0, t1, TRACK, th * 0.5f);
    const float done = s_dragging ? s_drag_at : bar_frac(shown);
    if (done > 0)
      dl->AddRectFilled(t0, ImVec2(bar_x + done * bar_w, t1.y), accent, th * 0.5f);
    for (size_t i = 0; i < info.marks.size(); ++i)
    {
      const auto& mark = info.marks[i];
      const bool lit = static_cast<int>(i) == mark_hovered || static_cast<int>(i) == s_mark_held ||
                       (info.clip_end >= 0 && mark.end == info.clip_end && mark.start == info.clip_start);
      const float xs = x_of(mark.start);
      const float xe = std::max(x_of(mark.end), xs + 2.0f * scale);
      dl->AddRectFilled(ImVec2(xs, t0.y), ImVec2(xe, t1.y), lit ? GOLD_SPAN_LIT : GOLD_SPAN);
    }
    if (info.desync_frame >= info.first && info.desync_frame <= end)
    {
      const float x = x_of(info.desync_frame);
      dl->AddRectFilled(ImVec2(x - 1.5f * scale, t0.y - 3.0f * scale), ImVec2(x + 1.5f * scale, t1.y + 3.0f * scale),
                        DESYNC_RED, 1.0f * scale);
    }
    // Where the highlight playing ends while it is ahead (it pauses there). Not in clip mode: only
    // the clip's end pauses then (GitHub #41).
    if (!s_clip_mode && info.clip_armed && info.clip_end >= 0)
    {
      const float x = x_of(info.clip_end);
      dl->AddRectFilled(ImVec2(x - 1.0f * scale, t0.y - 4.0f * scale), ImVec2(x + 1.0f * scale, t1.y + 4.0f * scale),
                        IM_COL32(255, 255, 255, 230));
    }
    if (s_clip_mode)
    {
      // The clip: the bar outside it dimmed, a box over it, a grip at each end.
      const float xs = x_of(s_clip_start);
      const float xe = x_of(s_clip_end);
      dl->AddRectFilled(ImVec2(bar_x, t0.y), ImVec2(xs, t1.y), IM_COL32(0, 0, 0, 115));
      dl->AddRectFilled(ImVec2(xe, t0.y), ImVec2(bar_x + bar_w, t1.y), IM_COL32(0, 0, 0, 115));
      // (Filled rectangles on whole pixels, not a stroked rounded one: its anti-aliased edges
      // stepped a pixel partway along at fractional positions, Sarah's board 2026-11-20: "the
      // seek rectangle is ... askew a little bit". Its sides are under the grips.)
      const float bt = std::max(1.0f, std::round(2.0f * scale));
      const ImVec2 b0(xs, track_y - std::round(9.0f * scale));
      const ImVec2 b1(xe, track_y + std::round(9.0f * scale));
      dl->AddRectFilled(b0, b1, WithAlpha(s_clip_col, 56));
      dl->AddRectFilled(b0, ImVec2(b1.x, b0.y + bt), s_clip_col);
      dl->AddRectFilled(ImVec2(b0.x, b1.y - bt), b1, s_clip_col);
      for (int h = 1; h <= 2; ++h)
      {
        const float x = h == 1 ? xs : xe;
        const bool lit = h == handle_hovered || h == s_clip_handle;
        const float hw = std::round((lit ? 6.0f : 5.0f) * scale);
        const float hh = std::round((lit ? 13.0f : 12.0f) * scale);
        const ImVec2 a(x - hw, track_y - hh);
        const ImVec2 z(x + hw, track_y + hh);
        dl->AddRectFilled(ImVec2(a.x - 2.0f * scale, a.y - 2.0f * scale), ImVec2(z.x + 2.0f * scale, z.y + 2.0f * scale),
                          s_clip_col, 5.0f * scale);
        dl->AddRectFilled(a, z, IM_COL32(255, 255, 255, 255), 4.0f * scale);
        for (float gx : {-1.5f, 1.5f})
        {
          dl->AddLine(ImVec2(x + gx * scale, track_y - 4.5f * scale), ImVec2(x + gx * scale, track_y + 4.5f * scale),
                      IM_COL32(120, 120, 120, 255), 1.0f * scale);
        }
      }
    }
    for (size_t i = 0; i < info.marks.size(); ++i)
    {
      const auto& mark = info.marks[i];
      const bool lit = static_cast<int>(i) == mark_hovered || static_cast<int>(i) == s_mark_held;
      const float r = lit ? mark_r * 1.3f : mark_r;
      const ImVec2 c(x_of(mark.start), stars_y);
      // (Sarah's board, 2026-11-20: "a blue outline on the star on the last clip you selected
      // that you're currently on / rendering"; clip mode's colour since.)
      const auto* job = render_of(mark.start, mark.end);
      const bool outlined =
          static_cast<int>(i) == watching || (job && (job->state == "queued" || job->state == "rendering"));
      if (outlined)
        Star(dl, c, r * 1.45f, s_clip_col);
      else
        Star(dl, ImVec2(c.x, c.y + 1.0f * scale), r * 1.12f, IM_COL32(0, 0, 0, 140));
      Star(dl, c, r, lit ? GOLD_LIT : GOLD);
    }
    if (!s_clip_mode)
    {
      // The knob: where it is (or would seek to), ringed in the launcher's colour.
      const ImVec2 k(bar_x + done * bar_w, track_y);
      const float kr = (track_lit ? 8.5f : 7.5f) * scale;
      dl->AddCircleFilled(k, kr + 4.0f * scale, WithAlpha(accent, 90), 24);
      dl->AddCircleFilled(k, kr, IM_COL32(255, 255, 255, 255), 24);
    }
    const float tip_y = stars_y - mark_r * 1.6f;
    if (s_clip_mode && (handle_hovered || s_clip_handle))
    {
      const int h = s_clip_handle ? s_clip_handle : handle_hovered;
      const s64 f = h == 1 ? s_clip_start : s_clip_end;
      TipAbove(ImVec2(x_of(f), tip_y), fmt::format("{} ({})", h == 1 ? "Clip start" : "Clip end", Clock60(f - info.first)),
               scale, 4.0f * scale, W - 4.0f * scale);
    }
    else if (mark_hovered >= 0 && !s_dragging)
    {
      const auto& m = info.marks[mark_hovered];
      TipAbove(ImVec2(x_of(m.start), tip_y), fmt::format("Play this highlight ({})", Clock60(m.start - info.first)),
               scale, 4.0f * scale, W - 4.0f * scale);
    }
    else if (hovered || s_dragging)
    {
      const float hx = s_dragging ? s_drag_at : at(io.MousePos.x);
      TipAbove(ImVec2(bar_x + hx * bar_w, tip_y), Clock60(static_cast<s64>(hx * span)), scale, 4.0f * scale,
               W - 4.0f * scale);
    }

    // ---- The controls' row ----
    const float by = row_y + (ctl_h - btn) * 0.5f;
    const float text_y = row_y + (ctl_h - ImGui::GetTextLineHeight()) * 0.5f;
    float x = bar_x - 6.0f * scale;
    // Play / pause.
    {
      const bool show_play = info.paused || info.at_end;
      if (IconButton("##replayplay", ImVec2(x, by), btn, true, show_play ? "Play (Space)" : "Pause (Space)",
                     [show_play](ImDrawList* d, ImVec2 c, float s, ImU32 col) {
                       show_play ? IconPlay(d, c, s, col) : IconPause(d, c, s, col);
                     },
                     scale))
      {
        Control(Gprb::Session::PlaybackCommand::TogglePause);
      }
      x += btn + 2.0f * scale;
    }
    // -5 s / +5 s (the user, 2026-11-20).
    const float small_px = 9.0f * scale;
    if (IconButton("##replayback5", ImVec2(x, by), btn, true, nullptr,
                   [small_px](ImDrawList* d, ImVec2 c, float s, ImU32 col) { IconJump(d, c, s * 1.1f, col, false, small_px); },
                   scale))
    {
      Control(Gprb::Session::PlaybackCommand::Jump, -JUMP_SECONDS);
    }
    x += btn + 2.0f * scale;
    if (IconButton("##replayfwd5", ImVec2(x, by), btn, true, nullptr,
                   [small_px](ImDrawList* d, ImVec2 c, float s, ImU32 col) { IconJump(d, c, s * 1.1f, col, true, small_px); },
                   scale))
    {
      Control(Gprb::Session::PlaybackCommand::Jump, JUMP_SECONDS);
    }
    x += btn + 10.0f * scale;

    // The right side, laid out from the right edge: the fold, the list's buttons, Clip (or clip
    // mode's buttons).
    float rx = bar_x + bar_w + 6.0f * scale;
    rx -= btn;
    if (FoldButton(ImVec2(rx, by), btn, scale, false))
    {
      s_dragging = false;
      s_mark_held = -1;
      EndClipMode();
    }
    const auto separator = [&] {
      rx -= 9.0f * scale;
      dl->AddRectFilled(ImVec2(rx + 4.0f * scale, row_y + ctl_h * 0.5f - 10.0f * scale),
                        ImVec2(rx + 5.0f * scale, row_y + ctl_h * 0.5f + 10.0f * scale), IM_COL32(255, 255, 255, 46));
    };
    separator();
    const float bh = 30.0f * scale;
    const float bty = row_y + (ctl_h - bh) * 0.5f;
    if (s_clip_mode)
    {
      // Cancel clip, Save as highlight, and Render (w/ Music) with a menu for Render (No Music)
      // (the user, 2026-11-20: "rendering should be an option from Clip mode only").
      constexpr const char* CANCEL = "Cancel clip";
      constexpr const char* SAVE = "Save as highlight";
      rx = RenderSplit(rx, bty, bh, scale, [first = info.first](bool music) {
        Send([start = s_clip_start, end = s_clip_end, music, label = ClipLabel(s_clip_start, s_clip_end, first)] {
          if (!Gprb::Replay::Playback::RequestRender(start, end, music, true, label))
            OSD::AddMessage("The clip could not be saved", OSD::Duration::NORMAL, OSD::Color::RED);
        });
        EndClipMode();
      });
      rx -= 8.0f * scale;
      const auto star = [](ImDrawList* d, ImVec2 c, float s, ImU32 col) {
        StarOutline(d, c, s * 0.5f, col, std::max(1.3f, s * 0.09f));
      };
      const float sw = TextButtonWidth(SAVE, true, scale);
      rx -= sw;
      if (TextButton("##replaysaveclip", SAVE, ImVec2(rx, bty), bh, ButtonLook::Ghost, star, scale))
      {
        Send([start = s_clip_start, end = s_clip_end] {
          if (Gprb::Replay::Playback::SaveClip(start, end))
          {
            OSD::AddMessage("Clip saved to Replays > Highlights > Manual Clips", OSD::Duration::NORMAL,
                            OSD::Color::GREEN);
          }
          else
          {
            OSD::AddMessage("The clip could not be saved", OSD::Duration::NORMAL, OSD::Color::RED);
          }
        });
        EndClipMode();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep it in the launcher's Replays > Highlights > Manual Clips");
      rx -= 8.0f * scale;
      const float cw = TextButtonWidth(CANCEL, false, scale);
      rx -= cw;
      if (TextButton("##replaycancelclip", CANCEL, ImVec2(rx, bty), bh, ButtonLook::Ghost, nullptr, scale))
        EndClipMode();
    }
    else
    {
      if (nav.shown)
      {
        using Gprb::Replay::Playback::NavAction;
        struct Button
        {
          const char* id;
          const char* tip;
          bool clip;
          bool left;
          bool enabled;
          NavAction action;
          int mark;  // a mark of this replay to play instead (-1: ask the launcher)
        };
        // (A replay list's < > only go from star to star of this replay.)
        const bool ask_clip = nav.clips;
        const Button buttons[] = {
            {"##navnextreplay", "Next replay", false, false, nav.next_replay, NavAction::NextReplay, -1},
            {"##navnextclip", "Next clip", true, false, next_mark >= 0 || (ask_clip && nav.next_replay),
             NavAction::NextClip, next_mark},
            {"##navprevclip", "Previous clip", true, true, prev_mark >= 0 || (ask_clip && nav.prev_replay),
             NavAction::PrevClip, prev_mark},
            {"##navprevreplay", "Previous replay", false, true, nav.prev_replay, NavAction::PrevReplay, -1},
        };
        for (const Button& b : buttons)
        {
          if (b.clip && !clip_buttons)
            continue;
          rx -= btn;
          const bool clip = b.clip;
          const bool left = b.left;
          if (IconButton(b.id, ImVec2(rx, by), btn, b.enabled, b.tip,
                         [clip, left](ImDrawList* d, ImVec2 c, float s, ImU32 col) {
                           clip ? IconClipStep(d, c, s, col, left) : IconSkip(d, c, s, col, left);
                         },
                         scale))
          {
            if (b.mark >= 0)
              Control(Gprb::Session::PlaybackCommand::PlayHighlight, b.mark);
            else
              Send([action = b.action, cursor] { Gprb::Replay::Playback::RequestNav(action, cursor); });
          }
        }
        const ImVec2 cs = ImGui::CalcTextSize(count.c_str());
        rx -= cs.x + 6.0f * scale;
        dl->AddText(ImVec2(rx, text_y), TEXT_2, count.c_str());
        separator();
      }
      else if (clip_buttons)
      {
        // (No list, e.g. spectating with stars: < > between them.)
        for (int i = 0; i < 2; ++i)
        {
          const bool left = i == 1;
          const int mark = left ? prev_mark : next_mark;
          rx -= btn;
          if (IconButton(left ? "##navprevclip" : "##navnextclip", ImVec2(rx, by), btn, mark >= 0,
                         left ? "Previous clip" : "Next clip",
                         [left](ImDrawList* d, ImVec2 c, float s, ImU32 col) { IconClipStep(d, c, s, col, left); },
                         scale))
          {
            Control(Gprb::Session::PlaybackCommand::PlayHighlight, mark);
          }
        }
        separator();
      }
      if (can_clip)
      {
        // A star's highlight being watched (played from its star or the launcher's list, on its
        // way there, playing, or paused at its end): Render too, until it is left or played past
        // (Sarah's board, 2026-11-20: "the render button should appear when you click on a star
        // ... when our ui state machine knows for sure the user is watching a specific starred
        // replay"). Asked once per highlight shown.
        // Once rendered (or rendering), its state instead ("Rendering 42%", "Saved" and a folder
        // button: Sarah's board, 2026-11-20); Render again once it failed or was cancelled.
        if (watching >= 0)
        {
          const s64 start = info.clip_start;
          const s64 end_frame = info.clip_end;
          const auto* job = render_of(start, end_frame);
          const bool pending = !job && s_pending_path == nav.armed_path && s_pending_start == start &&
                               s_pending_end == end_frame && now - s_pending_since < PENDING_FOR;
          if (job && job->state != "error")
          {
            rx = RenderState(rx, bty, bh, scale, job->text, job->folder ? job->id : std::string());
          }
          else if (pending)
          {
            rx = RenderState(rx, bty, bh, scale, "Waiting", {});
          }
          else
          {
            const std::string path = nav.armed_path;
            const std::string label = ClipLabel(start, end_frame, info.first);
            rx = RenderSplit(rx, bty, bh, scale, [start, end_frame, path, label](bool music) {
              s_pending_path = path;
              s_pending_start = start;
              s_pending_end = end_frame;
              s_pending_since = Clock::now();
              Send([start, end_frame, music, label] {
                Gprb::Replay::Playback::RequestRender(start, end_frame, music, false, label);
              });
            });
          }
          rx -= 8.0f * scale;
        }
        constexpr const char* CLIP = "Clip";
        const float cw = TextButtonWidth(CLIP, true, scale);
        rx -= cw + 4.0f * scale;
        const auto scissors = [](ImDrawList* d, ImVec2 c, float s, ImU32 col) { IconScissors(d, c, s, col); };
        if (TextButton("##replayclip", CLIP, ImVec2(rx, bty), bh, ButtonLook::Ghost, scissors, scale))
        {
          // Ends at the frame shown, starts 20 s before it; paused at its start (the user,
          // 2026-10-24: "creating a clip should seek to the start of the clip"), so Play plays it
          // and pauses at its end.
          s_clip_mode = true;
          s_clip_handle = 0;
          s_clip_title = info.title;
          s_clip_first = info.first;
          s_clip_end = std::clamp(shown, info.first + MIN_CLIP_FRAMES, std::max(end, info.first + MIN_CLIP_FRAMES));
          s_clip_bar_end = std::max(end, s_clip_end);
          s_clip_start = std::max(info.first, s_clip_end - CLIP_BEFORE_FRAMES);
          if (!info.paused)
            Control(Gprb::Session::PlaybackCommand::Pause);
          Control(Gprb::Session::PlaybackCommand::Seek, static_cast<double>(s_clip_start));
          SetStop(s_clip_end);
        }
        if (ImGui::IsItemHovered())
        {
          ImGui::SetTooltip("Clip the last %d seconds, then move the start and end position",
                            static_cast<int>(CLIP_BEFORE_FRAMES / 60));
        }
      }
    }
    const float right_edge = rx - 12.0f * scale;

    // Left side after the buttons: the time (or the clip's frames), live, what it is doing, the
    // title.
    if (s_clip_mode)
    {
      const std::string label =
          fmt::format("Clip {} - {} ({} s)", Clock60(s_clip_start - info.first), Clock60(s_clip_end - info.first),
                      (s_clip_end - s_clip_start + 30) / 60);
      // (Clipped before the buttons in a narrow window.)
      dl->PushClipRect(ImVec2(x, row_y), ImVec2(std::max(x, right_edge), row_y + ctl_h), true);
      dl->AddText(ImVec2(x, text_y), TEXT, label.c_str());
      dl->PopClipRect();
    }
    else
    {
      const std::string now_s = Clock60(shown - info.first);
      const std::string total_s = fmt::format(" / {}", Clock60(span));
      dl->AddText(ImVec2(x, text_y), TEXT, now_s.c_str());
      x += ImGui::CalcTextSize(now_s.c_str()).x;
      dl->AddText(ImVec2(x, text_y), TEXT_2, total_s.c_str());
      x += ImGui::CalcTextSize(total_s.c_str()).x + 12.0f * scale;

      // Spectating: LIVE (red, playing at the live edge) or GO LIVE with how far behind (End, or
      // a click).
      if (live)
      {
        const s64 behind = std::max<s64>(info.live_edge - shown, 0) / 60;
        const bool at_live = info.at_live || behind == 0;
        const std::string label = at_live ? std::string("LIVE") : fmt::format("GO LIVE  -{} s", behind);
        ImGui::PushFont(nullptr, 12.0f * scale / main_scale);
        const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
        const float dot = 7.0f * scale;
        const ImVec2 lp(x, row_y + (ctl_h - 24.0f * scale) * 0.5f);
        const ImVec2 lsz(ls.x + dot + 26.0f * scale, 24.0f * scale);
        ImGui::SetCursorScreenPos(lp);
        if (ImGui::InvisibleButton("##replaylive", lsz) && !at_live)
          Control(Gprb::Session::PlaybackCommand::GoLive);
        const bool over = ImGui::IsItemHovered();
        dl->AddRectFilled(lp, ImVec2(lp.x + lsz.x, lp.y + lsz.y),
                          at_live ? LIVE_RED : over ? GHOST_HOVER : GHOST_BG, 6.0f * scale);
        dl->AddCircleFilled(ImVec2(lp.x + 10.0f * scale + dot * 0.5f, lp.y + lsz.y * 0.5f), dot * 0.5f,
                            at_live ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 90, 90, 255), 12);
        dl->AddText(ImVec2(lp.x + 16.0f * scale + dot, lp.y + (lsz.y - ls.y) * 0.5f), TEXT, label.c_str());
        ImGui::PopFont();
        if (over && !at_live)
          ImGui::SetTooltip("Go live (End)");
        x += lsz.x + 12.0f * scale;
      }
      // What it is doing, when not just playing or paused (the button says that): seeking,
      // buffering, catching up; and the speed.
      std::string state;
      if (info.seeking)
      {
        state = "Seeking...";
      }
      else if (live && info.catching_up)
      {
        // "Catching up to live - about 20 s": the edge moves on at 1x, so it closes at the
        // catch-up rate less one.
        state = "Catching up to live";
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
      if (!state.empty() && x < right_edge)
      {
        dl->PushClipRect(ImVec2(x, row_y), ImVec2(right_edge, row_y + ctl_h), true);
        dl->AddText(ImVec2(x, text_y), TEXT_DIM, state.c_str());
        dl->PopClipRect();
        x += ImGui::CalcTextSize(state.c_str()).x + 12.0f * scale;
      }
      if (info.speed != 1.0 && x < right_edge)
      {
        const std::string sp = fmt::format("{}x", info.speed);
        ImGui::PushFont(nullptr, 12.0f * scale / main_scale);
        const ImVec2 ss = ImGui::CalcTextSize(sp.c_str());
        const ImVec2 cp(x, row_y + (ctl_h - ss.y - 6.0f * scale) * 0.5f);
        dl->AddRectFilled(cp, ImVec2(cp.x + ss.x + 14.0f * scale, cp.y + ss.y + 6.0f * scale), GHOST_HOVER,
                          5.0f * scale);
        dl->AddText(ImVec2(cp.x + 7.0f * scale, cp.y + 3.0f * scale), TEXT, sp.c_str());
        ImGui::PopFont();
        x += ss.x + 14.0f * scale + 12.0f * scale;
      }
      // What is playing (display text from the file: drawn, never formatted or used as a label):
      // the title, then the players, "vs" dimmed.
      if (x < right_edge)
      {
        dl->PushClipRect(ImVec2(x, row_y), ImVec2(right_edge, row_y + ctl_h), true);
        const auto put = [&](const std::string& s, ImU32 col, float after) {
          dl->AddText(ImVec2(x, text_y), col, s.c_str());
          x += ImGui::CalcTextSize(s.c_str()).x + after;
        };
        put(info.title, TEXT, 10.0f * scale);
        bool first = true;
        for (size_t port = 0; port < info.names.size(); ++port)
        {
          if (info.names[port].empty())
            continue;
          if (!first)
            put("vs", TEXT_DIM, 6.0f * scale);
          first = false;
          put(fmt::format("{} ({})", info.names[port], Online::GameSetup::CharacterName(info.chars[port])), TEXT_2,
              6.0f * scale);
        }
        dl->PopClipRect();
      }
    }
  }
  ImGui::End();
  pop();
}
}  // namespace VideoCommon::ReplayOverlayUI
