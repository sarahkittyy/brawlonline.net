// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/UI/QuickChatUI.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "BrawlOnline/UI/Hooks.h"

namespace BrawlOnline::UI::QuickChatUI
{
namespace
{
using Online::QuickChat::View;
namespace QC = Online::QuickChat;

// Sizes in text heights (the chat's text, a little larger: it is read at a glance).
constexpr float TEXT_SCALE = 1.35f;
constexpr float ARM = 1.05f;       // a D-pad arm's width
constexpr float PAD = 1.0f;        // the panel's padding
constexpr float GAP = 0.8f;        // between the D-pad and the messages
constexpr float SIDE_MAX = 10.5f;  // a left or right message's widest line
constexpr float UPDOWN_MIN = 20.0f;
constexpr float MAX_WIDTH = 40.0f;
constexpr u64 APPEAR_MS = 120;

struct Wrapped
{
  std::vector<std::pair<const char*, const char*>> lines;
  float w = 0;
  float h = 0;
};

// `text` broken into lines at most `max_w` wide (at spaces where it can, else anywhere), each
// line's spaces at its ends left out.
Wrapped Wrap(ImFont* font, float size, const std::string& text, float max_w, float line_h)
{
  Wrapped out;
  const char* s = text.data();
  const char* const end = s + text.size();
  while (s < end)
  {
    const char* e = font->CalcWordWrapPosition(size, s, end, max_w);
    if (e <= s)
    {
      unsigned int c = 0;
      e = s + std::max(1, ImTextCharFromUtf8(&c, s, end));
    }
    const char* line_end = e;
    while (line_end > s && line_end[-1] == ' ')
      --line_end;
    out.w = std::max(out.w, font->CalcTextSizeA(size, FLT_MAX, 0.0f, s, line_end).x);
    out.lines.emplace_back(s, line_end);
    s = e;
    while (s < end && *s == ' ')
      ++s;
  }
  out.h = static_cast<float>(out.lines.size()) * line_h;
  return out;
}

ImU32 Fade(ImU32 color, float alpha)
{
  const float a =
      static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
  return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

enum class Align
{
  Left,
  Center,
  Right,
};

// Each line at `x` (its left, centre or right edge by `align`), from `top` down.
void DrawLines(ImDrawList* dl, ImFont* font, float size, const Wrapped& w, float x, float top,
               float line_h, Align align, ImU32 color)
{
  float y = top;
  for (const auto& [b, e] : w.lines)
  {
    const float lw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e).x;
    const float lx = align == Align::Left ? x : align == Align::Center ? x - lw * 0.5f : x - lw;
    dl->AddText(font, size, ImVec2(std::round(lx), std::round(y)), color, b, e);
    y += line_h;
  }
}
}  // namespace

Drawn Draw(const View& view, float scale, float font_px, ImU32 accent, const std::string& notice,
           u64 now_ms)
{
  Drawn drawn;
  if (view.state == View::State::Closed)
    return drawn;

  // Appearing.
  const float alpha = std::min(1.0f, static_cast<float>(now_ms - view.since_ms) / APPEAR_MS);

  const ImGuiIO& io = ImGui::GetIO();
  ImFont* const font = ImGui::GetFont();
  ImDrawList* const dl = ImGui::GetForegroundDrawList();

  const auto& game_rect = GetHooks().game_rect;
  Rect r = game_rect ? game_rect() : Rect{};
  if (r.right <= r.left || r.bottom <= r.top)
    r = {0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y)};
  const float game_w = static_cast<float>(r.right - r.left);
  const float cx = (r.left + r.right) * 0.5f;
  const float cy = (r.top + r.bottom) * 0.5f;

  const float t = std::round(font_px * TEXT_SCALE);
  const float line_h = std::ceil(font->CalcTextSizeA(t, FLT_MAX, 0.0f, "Ag").y);
  const float arm = std::round(ARM * t);
  const float dpad = 3.0f * arm;
  const float pad = PAD * t;
  const float gap = GAP * t;
  const float max_w = std::min(game_w * 0.78f, MAX_WIDTH * t);
  const float side_max =
      std::max(4.0f * t, std::min(SIDE_MAX * t, (max_w - 2 * pad - dpad - 2 * gap) * 0.5f));

  // The group's four, by direction; an empty one is a short dash in its place.
  std::array<Wrapped, QC::DIRECTIONS> w;
  const auto message = [&](int d) -> const std::string& {
    return view.messages[4 * view.group + d];
  };
  w[QC::Left] = Wrap(font, t, message(QC::Left), side_max, line_h);
  w[QC::Right] = Wrap(font, t, message(QC::Right), side_max, line_h);
  const float dash = 1.2f * t;
  const float side_w = std::max({w[QC::Left].w, w[QC::Right].w, dash});
  const float middle_w = 2 * side_w + 2 * gap + dpad;
  const float updown_max = std::max(middle_w, std::min(max_w - 2 * pad, UPDOWN_MIN * t));
  w[QC::Up] = Wrap(font, t, message(QC::Up), updown_max, line_h);
  w[QC::Down] = Wrap(font, t, message(QC::Down), updown_max, line_h);

  const float notice_t = std::round(t * 0.85f);
  const float notice_line_h = std::ceil(font->CalcTextSizeA(notice_t, FLT_MAX, 0.0f, "Ag").y);
  const float inner_w = std::max({middle_w, w[QC::Up].w, w[QC::Down].w});
  const Wrapped notice_w = Wrap(font, notice_t, notice, inner_w, notice_line_h);

  const float up_h = std::max(w[QC::Up].h, line_h);
  const float down_h = std::max(w[QC::Down].h, line_h);
  const float middle_h = std::max({dpad, w[QC::Left].h, w[QC::Right].h});
  const float timer_h = std::max(2.0f * scale, std::round(0.12f * t));
  float inner_h = up_h + gap + middle_h + gap + down_h;
  if (!notice.empty())
    inner_h += gap * 0.8f + notice_w.h;

  const float panel_w = std::ceil(inner_w + 2 * pad);
  const float panel_h = std::ceil(inner_h + 2 * pad);
  float x0 = std::round(cx - panel_w * 0.5f);
  float y0 = std::round(cy - panel_h * 0.5f);
  x0 = std::clamp(x0, 0.0f, std::max(0.0f, io.DisplaySize.x - panel_w));
  y0 = std::clamp(y0, 0.0f, std::max(0.0f, io.DisplaySize.y - panel_h));
  const float x1 = x0 + panel_w;
  const float y1 = y0 + panel_h;
  const float radius = std::round(0.55f * t);

  // The panel: a soft shadow, the body, a hairline edge.
  for (int i = 3; i >= 1; --i)
  {
    const float grow = i * 2.0f * scale;
    dl->AddRectFilled(ImVec2(x0 - grow, y0 - grow + 2 * scale),
                      ImVec2(x1 + grow, y1 + grow + 2 * scale), Fade(IM_COL32(0, 0, 0, 40), alpha),
                      radius + grow);
  }
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Fade(IM_COL32(17, 18, 23, 238), alpha), radius);
  dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), Fade(IM_COL32(255, 255, 255, 26), alpha), radius, 0,
              std::max(1.0f, scale));

  // The D-pad, its group's arm in the player's colour, a notch pointing out of each arm.
  const float gx = std::round(x0 + pad + inner_w * 0.5f);
  const float gy = std::round(y0 + pad + up_h + gap + middle_h * 0.5f);
  const float h = arm * 0.5f;
  const float big = arm * 1.5f;
  const float round = std::round(0.22f * arm);
  const ImU32 arm_color = Fade(IM_COL32(64, 68, 80, 255), alpha);
  dl->AddRectFilled(ImVec2(gx - h, gy - big), ImVec2(gx + h, gy + big), arm_color, round);
  dl->AddRectFilled(ImVec2(gx - big, gy - h), ImVec2(gx + big, gy + h), arm_color, round);
  struct Arm
  {
    ImVec2 a, b;
    ImDrawFlags corners;
    ImVec2 dir;
  };
  const std::array<Arm, QC::DIRECTIONS> arms = {{
      {{gx - h, gy - big}, {gx + h, gy - h}, ImDrawFlags_RoundCornersTop, {0, -1}},
      {{gx + h, gy - h}, {gx + big, gy + h}, ImDrawFlags_RoundCornersRight, {1, 0}},
      {{gx - h, gy + h}, {gx + h, gy + big}, ImDrawFlags_RoundCornersBottom, {0, 1}},
      {{gx - big, gy - h}, {gx - h, gy + h}, ImDrawFlags_RoundCornersLeft, {-1, 0}},
  }};
  const float notch = 0.3f * arm;
  for (int d = 0; d < QC::DIRECTIONS; ++d)
  {
    const Arm& a = arms[d];
    const bool lit = d == view.group;
    if (lit)
      dl->AddRectFilled(a.a, a.b, Fade(accent, alpha), round, a.corners);
    const ImVec2 c((a.a.x + a.b.x) * 0.5f + a.dir.x * 0.08f * arm,
                   (a.a.y + a.b.y) * 0.5f + a.dir.y * 0.08f * arm);
    const ImVec2 tip(c.x + a.dir.x * notch * 0.6f, c.y + a.dir.y * notch * 0.6f);
    const ImVec2 side(-a.dir.y * notch * 0.7f, a.dir.x * notch * 0.7f);
    const ImVec2 base(c.x - a.dir.x * notch * 0.4f, c.y - a.dir.y * notch * 0.4f);
    dl->AddTriangleFilled(
        tip, ImVec2(base.x + side.x, base.y + side.y), ImVec2(base.x - side.x, base.y - side.y),
        Fade(lit ? IM_COL32(17, 18, 23, 220) : IM_COL32(150, 155, 172, 255), alpha));
  }
  dl->AddCircleFilled(ImVec2(gx, gy), 0.2f * arm, Fade(IM_COL32(44, 47, 56, 255), alpha));

  // The messages, each on its direction's side.
  const ImU32 text_color = IM_COL32(236, 238, 244, 255);
  const ImU32 dash_color = IM_COL32(86, 90, 104, 255);
  struct Place
  {
    float x, top;
    Align align;
  };
  std::array<Place, QC::DIRECTIONS> places;
  places[QC::Up] = {gx, y0 + pad + up_h - std::max(w[QC::Up].h, line_h), Align::Center};
  places[QC::Down] = {gx, y0 + pad + up_h + gap + middle_h + gap, Align::Center};
  places[QC::Left] = {gx - big - gap, gy - std::max(w[QC::Left].h, line_h) * 0.5f, Align::Right};
  places[QC::Right] = {gx + big + gap, gy - std::max(w[QC::Right].h, line_h) * 0.5f, Align::Left};
  for (int d = 0; d < QC::DIRECTIONS; ++d)
  {
    const Place& p = places[d];
    const Wrapped& m = w[d];
    if (m.lines.empty())
    {
      const float dx = p.align == Align::Left   ? p.x :
                       p.align == Align::Center ? p.x - dash * 0.5f :
                                                  p.x - dash;
      const float dy = std::round(p.top + line_h * 0.5f);
      const float thick = std::max(2.0f * scale, std::round(0.1f * t));
      dl->AddRectFilled(ImVec2(dx, dy - thick * 0.5f), ImVec2(dx + dash, dy + thick * 0.5f),
                        Fade(dash_color, alpha), thick * 0.5f);
      continue;
    }
    DrawLines(dl, font, t, m, p.x, p.top, line_h, p.align, Fade(text_color, alpha));
  }

  if (!notice.empty())
  {
    const float top = y0 + pad + up_h + gap + middle_h + gap + down_h + gap * 0.8f;
    DrawLines(dl, font, notice_t, notice_w, gx, top, notice_line_h, Align::Center,
              Fade(IM_COL32(255, 176, 128, 255), alpha));
  }

  // How long the open group stays: a thin line along the bottom edge, running out.
  {
    const float left = x0 + radius;
    const float right = x1 - radius;
    const float y = y1 - std::round(pad * 0.42f);
    const float left_ms =
        static_cast<float>(QC::OPEN_MS - std::min(now_ms - view.last_press_ms, QC::OPEN_MS));
    const float fill = left_ms / QC::OPEN_MS;
    dl->AddRectFilled(ImVec2(left, y - timer_h * 0.5f), ImVec2(right, y + timer_h * 0.5f),
                      Fade(IM_COL32(255, 255, 255, 18), alpha), timer_h * 0.5f);
    const float mid = (left + right) * 0.5f;
    const float half = (right - left) * 0.5f * fill;
    dl->AddRectFilled(ImVec2(mid - half, y - timer_h * 0.5f),
                      ImVec2(mid + half, y + timer_h * 0.5f), Fade(accent, alpha * 0.85f),
                      timer_h * 0.5f);
  }

  drawn.shown = true;
  drawn.rect = {x0, y0, x1, y1};
  return drawn;
}
}  // namespace BrawlOnline::UI::QuickChatUI
