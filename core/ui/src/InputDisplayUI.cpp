// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/UI/InputDisplayUI.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "BrawlOnline/Config/OnlineSettings.h"
#include "BrawlOnline/Online/InputView.h"
#include "BrawlOnline/UI/Hooks.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

namespace BrawlOnline::UI::InputDisplay
{
namespace
{
using Online::InputView::Pad;
using Online::InputView::Type;

// Every layout is drawn on a 200 x 100 canvas, scaled to the box (its height is half its width).
// The launcher's preview (input_display_preview.tsx) draws the same tables.
constexpr float CANVAS_W = 200.0f;

enum class Kind : u8
{
  Body,     // the controller's outline (no fill), rect
  Circle,   // a button: cx, cy, r
  Pill,     // a button: rect
  DPad,     // cx, cy, arm (half length), thick
  Stick,    // cx, cy, gate radius; `which` 0 stick, 1 C-stick / right stick; `octagon`
  Trigger,  // rect, filled as far as it is pressed; `which` 0 L, 1 R; `mask` its digital press
};

struct Element
{
  Kind kind;
  float a, b, c, d;  // Circle: cx cy r; Pill, Body, Trigger: x0 y0 x1 y1; DPad: cx cy arm thick;
                     // Stick: cx cy r
  u32 mask = 0;
  const char* label = "";
  ImU32 color = 0;  // pressed fill (stick: the dot's)
  int which = 0;
  bool octagon = false;
};

constexpr ImU32 GREEN = IM_COL32(40, 200, 120, 255);
constexpr ImU32 RED = IM_COL32(232, 72, 72, 255);
constexpr ImU32 LIGHT = IM_COL32(222, 222, 222, 255);
constexpr ImU32 PURPLE = IM_COL32(150, 110, 235, 255);
constexpr ImU32 YELLOW = IM_COL32(246, 200, 40, 255);
constexpr ImU32 BLUE = IM_COL32(110, 180, 255, 255);

// The buttons (gfPadStatus +0x00 bits, Online/InputView.h): the D-pad's are a GameCube's on every
// controller (a sideways Wii Remote's already turned as the player holds it).
constexpr u32 D_LEFT = 0x1, D_RIGHT = 0x2, D_DOWN = 0x4, D_UP = 0x8;
constexpr u32 START = 0x1000;
constexpr u32 WII_1 = 0x10000, WII_2 = 0x20000, WII_C = 0x40000, WII_MINUS = 0x80000,
              WII_A = 0x200000, WII_B = 0x400000;

const Element GAMECUBE[] = {
    {Kind::Trigger, 12, 8, 62, 17, 0x40, "L", LIGHT, 0},
    {Kind::Trigger, 138, 8, 188, 17, 0x20, "R", LIGHT, 1},
    {Kind::Pill, 150, 21, 178, 28, 0x10, "Z", PURPLE},
    {Kind::Stick, 44, 56, 23, 0, 0, "", LIGHT, 0, true},
    {Kind::DPad, 80, 82, 8, 6},
    {Kind::Circle, 100, 46, 4.5f, 0, START, "", LIGHT},
    {Kind::Stick, 116, 82, 13, 0, 0, "", YELLOW, 1, true},
    {Kind::Circle, 160, 58, 11, 0, 0x100, "A", GREEN},
    {Kind::Circle, 140, 75, 7, 0, 0x200, "B", RED},
    {Kind::Circle, 182, 52, 7, 0, 0x400, "X", LIGHT},
    {Kind::Circle, 158, 36, 7, 0, 0x800, "Y", LIGHT},
};

const Element CLASSIC[] = {
    {Kind::Trigger, 12, 8, 62, 17, 0x40, "L", LIGHT, 0},
    {Kind::Trigger, 138, 8, 188, 17, 0x20, "R", LIGHT, 1},
    {Kind::Pill, 40, 21, 62, 28, 0x4000, "ZL", LIGHT},
    {Kind::Pill, 138, 21, 160, 28, 0x8000, "ZR", LIGHT},
    {Kind::DPad, 36, 50, 9, 7},
    {Kind::Circle, 86, 50, 4.5f, 0, WII_MINUS, "-", LIGHT},
    {Kind::Circle, 114, 50, 4.5f, 0, START, "+", LIGHT},
    {Kind::Circle, 164, 38, 7, 0, 0x400, "x", LIGHT},
    {Kind::Circle, 177, 51, 7, 0, 0x100, "a", LIGHT},
    {Kind::Circle, 164, 64, 7, 0, 0x200, "b", LIGHT},
    {Kind::Circle, 151, 51, 7, 0, 0x800, "y", LIGHT},
    {Kind::Stick, 76, 78, 15, 0, 0, "", LIGHT, 0},
    {Kind::Stick, 124, 78, 15, 0, 0, "", LIGHT, 1},
};

// Held sideways: the D-pad at the left, 1 and 2 at the right, B underneath.
const Element WII_REMOTE[] = {
    {Kind::Body, 10, 30, 190, 74},
    {Kind::DPad, 36, 52, 10, 7},
    {Kind::Circle, 68, 52, 8, 0, WII_A, "A", BLUE},
    {Kind::Circle, 94, 52, 4.5f, 0, WII_MINUS, "-", LIGHT},
    {Kind::Circle, 114, 52, 4.5f, 0, START, "+", LIGHT},
    {Kind::Circle, 146, 52, 7.5f, 0, WII_1, "1", LIGHT},
    {Kind::Circle, 170, 52, 7.5f, 0, WII_2, "2", LIGHT},
    {Kind::Pill, 54, 80, 82, 90, WII_B, "B", BLUE},
};

// The Nunchuk at the left, the Wii Remote upright at the right (B on its side).
const Element NUNCHUK[] = {
    {Kind::Body, 8, 14, 96, 90},
    {Kind::Stick, 44, 56, 22, 0, 0, "", LIGHT, 0},
    {Kind::Circle, 80, 31, 6, 0, WII_C, "C", LIGHT},
    {Kind::Pill, 71, 43, 89, 58, 0x10, "Z", LIGHT},
    {Kind::Body, 116, 6, 176, 94},
    {Kind::DPad, 146, 22, 8, 6},
    {Kind::Circle, 146, 44, 7, 0, WII_A, "A", BLUE},
    {Kind::Circle, 132, 62, 4, 0, WII_MINUS, "-", LIGHT},
    {Kind::Circle, 160, 62, 4, 0, START, "+", LIGHT},
    {Kind::Circle, 146, 74, 5.5f, 0, WII_1, "1", LIGHT},
    {Kind::Circle, 146, 87, 5.5f, 0, WII_2, "2", LIGHT},
    {Kind::Pill, 181, 36, 192, 56, WII_B, "B", BLUE},
};

std::span<const Element> Layout(Type type)
{
  switch (type)
  {
  case Type::Classic:
    return CLASSIC;
  case Type::WiiRemote:
    return WII_REMOTE;
  case Type::Nunchuk:
    return NUNCHUK;
  case Type::GameCube:
  default:
    return GAMECUBE;
  }
}

const char* TypeName(Type type)
{
  switch (type)
  {
  case Type::Classic:
    return "classic";
  case Type::WiiRemote:
    return "wiimote";
  case Type::Nunchuk:
    return "nunchuk";
  case Type::GameCube:
  default:
    return "gc";
  }
}

constexpr ImU32 BACKDROP = IM_COL32(0, 0, 0, 110);
constexpr ImU32 OUTLINE = IM_COL32(255, 255, 255, 200);
constexpr ImU32 BODY = IM_COL32(255, 255, 255, 70);
constexpr ImU32 IDLE = IM_COL32(0, 0, 0, 80);
constexpr ImU32 LABEL = IM_COL32(255, 255, 255, 230);
constexpr ImU32 LABEL_PRESSED = IM_COL32(20, 20, 20, 255);
// Port colours, as the game's (P1 red, P2 blue, P3 yellow, P4 green).
constexpr ImU32 PORT_COLORS[] = {IM_COL32(232, 72, 72, 255), IM_COL32(70, 130, 245, 255),
                                 IM_COL32(240, 200, 40, 255), IM_COL32(60, 190, 95, 255)};

struct Box
{
  ImVec2 min;
  float w = 0;
  Pad pad;
};

// What the last frame drew (the harness's input_display).
std::mutex s_status_mutex;
std::string s_status = R"({"enabled":false,"frames":0,"boxes":[]})";

// A stick's position as a fraction of its gate (-1..1, up positive): a GameCube controller's
// -128..127, a Wii one's -100..100; never past the gate.
ImVec2 StickAt(const Pad& p, int which)
{
  const float range = p.type == Type::GameCube ? 127.0f : 100.0f;
  float x = (which == 0 ? p.stick_x : p.sub_x) / range;
  float y = (which == 0 ? p.stick_y : p.sub_y) / range;
  const float len = std::sqrt(x * x + y * y);
  if (len > 1.0f)
  {
    x /= len;
    y /= len;
  }
  return {x, y};
}

void DrawLabel(ImDrawList* dl, ImVec2 center, float size, const char* text, ImU32 color)
{
  if (!text || !*text)
    return;
  ImFont* font = ImGui::GetFont();
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), color, text);
}

void DrawBox(ImDrawList* dl, const Box& box, float scale, bool label_port)
{
  const float k = box.w / CANVAS_W;
  const auto at = [&](float x, float y) { return ImVec2(box.min.x + x * k, box.min.y + y * k); };
  const float line = std::max(1.0f, 1.3f * scale);
  const Pad& p = box.pad;
  const auto pressed = [&](u32 mask) { return mask != 0 && (p.buttons & mask) == mask; };

  dl->AddRectFilled(box.min, at(CANVAS_W, CANVAS_W / 2), BACKDROP, 8.0f * k);
  if (label_port && p.port >= 0 && p.port < 4)
    DrawLabel(dl, at(14, 92), 11.0f * k, fmt::format("P{}", p.port + 1).c_str(), PORT_COLORS[p.port]);

  for (const Element& e : Layout(p.type))
  {
    switch (e.kind)
    {
    case Kind::Body:
      dl->AddRect(at(e.a, e.b), at(e.c, e.d), BODY, 12.0f * k, 0, line);
      break;
    case Kind::Circle:
    {
      const bool on = pressed(e.mask);
      dl->AddCircleFilled(at(e.a, e.b), e.c * k, on ? e.color : IDLE);
      dl->AddCircle(at(e.a, e.b), e.c * k, OUTLINE, 0, line);
      DrawLabel(dl, at(e.a, e.b), std::max(e.c * 1.25f, 7.0f) * k, e.label, on ? LABEL_PRESSED : LABEL);
      break;
    }
    case Kind::Pill:
    {
      const bool on = pressed(e.mask);
      const float r = std::min(e.c - e.a, e.d - e.b) * 0.5f * k;
      dl->AddRectFilled(at(e.a, e.b), at(e.c, e.d), on ? e.color : IDLE, r);
      dl->AddRect(at(e.a, e.b), at(e.c, e.d), OUTLINE, r, 0, line);
      DrawLabel(dl, at((e.a + e.c) / 2, (e.b + e.d) / 2), 7.5f * k, e.label, on ? LABEL_PRESSED : LABEL);
      break;
    }
    case Kind::DPad:
    {
      const float arm = e.c, t = e.d / 2;
      // up, down, left, right
      const std::array<std::pair<u32, std::array<float, 4>>, 4> arms = {{
          {D_UP, {e.a - t, e.b - arm, e.a + t, e.b - t}},
          {D_DOWN, {e.a - t, e.b + t, e.a + t, e.b + arm}},
          {D_LEFT, {e.a - arm, e.b - t, e.a - t, e.b + t}},
          {D_RIGHT, {e.a + t, e.b - t, e.a + arm, e.b + t}},
      }};
      dl->AddRectFilled(at(e.a - t, e.b - t), at(e.a + t, e.b + t), IDLE);
      for (const auto& [mask, r] : arms)
        dl->AddRectFilled(at(r[0], r[1]), at(r[2], r[3]), pressed(mask) ? LIGHT : IDLE, 1.5f * k);
      // The cross's outline.
      const ImVec2 pts[12] = {at(e.a - t, e.b - arm), at(e.a + t, e.b - arm), at(e.a + t, e.b - t),
                              at(e.a + arm, e.b - t), at(e.a + arm, e.b + t), at(e.a + t, e.b + t),
                              at(e.a + t, e.b + arm), at(e.a - t, e.b + arm), at(e.a - t, e.b + t),
                              at(e.a - arm, e.b + t), at(e.a - arm, e.b - t), at(e.a - t, e.b - t)};
      dl->AddPolyline(pts, 12, OUTLINE, ImDrawFlags_Closed, line);
      break;
    }
    case Kind::Stick:
    {
      const ImVec2 c = at(e.a, e.b);
      const float r = e.c * k;
      if (e.octagon)
      {
        dl->AddNgonFilled(c, r, IDLE, 8);
        dl->AddNgon(c, r, OUTLINE, 8, line);
      }
      else
      {
        dl->AddCircleFilled(c, r, IDLE);
        dl->AddCircle(c, r, OUTLINE, 0, line);
      }
      const ImVec2 s = StickAt(p, e.which);
      // The dot travels the gate less its own radius.
      const float dot = std::max(r * 0.3f, 2.0f);
      const ImVec2 d(c.x + s.x * (r - dot * 0.6f), c.y - s.y * (r - dot * 0.6f));
      dl->AddLine(c, d, e.color, line);
      dl->AddCircleFilled(d, dot, e.color);
      break;
    }
    case Kind::Trigger:
    {
      const u8 analog = e.which == 0 ? p.trigger_l : p.trigger_r;
      const bool digital = pressed(e.mask);
      const float frac = digital ? 1.0f : std::clamp(analog / 255.0f, 0.0f, 1.0f);
      const ImVec2 a = at(e.a, e.b), b = at(e.c, e.d);
      const float r = (b.y - a.y) * 0.5f;
      dl->AddRectFilled(a, b, IDLE, r);
      if (frac > 0.0f)
      {
        // L fills from its outer (left) end, R from its right end, as the triggers sit.
        const float w = (b.x - a.x) * frac;
        if (e.which == 0)
          dl->AddRectFilled(a, ImVec2(a.x + w, b.y), digital ? e.color : IM_COL32(222, 222, 222, 150), r);
        else
          dl->AddRectFilled(ImVec2(b.x - w, a.y), b, digital ? e.color : IM_COL32(222, 222, 222, 150), r);
      }
      dl->AddRect(a, b, OUTLINE, r, 0, line);
      DrawLabel(dl, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), 7.5f * k, e.label,
                digital ? LABEL_PRESSED : LABEL);
      break;
    }
    }
  }
}

std::string PressedNames(const Pad& p)
{
  std::string out;
  for (const Element& e : Layout(p.type))
  {
    if ((e.kind == Kind::Circle || e.kind == Kind::Pill || e.kind == Kind::Trigger) && e.mask != 0 &&
        (p.buttons & e.mask) == e.mask)
    {
      const std::string name = e.mask == START && !*e.label ? "START" : e.label;
      out += fmt::format("{}\"{}\"", out.empty() ? "" : ",", name);
    }
  }
  const std::pair<u32, const char*> dpad[] = {{D_UP, "UP"}, {D_DOWN, "DOWN"}, {D_LEFT, "LEFT"}, {D_RIGHT, "RIGHT"}};
  for (const auto& [mask, name] : dpad)
  {
    if (p.buttons & mask)
      out += fmt::format("{}\"{}\"", out.empty() ? "" : ",", name);
  }
  return out;
}
}  // namespace

void Display(float scale)
{
  const bool enabled = Config::Get(Config::ONLINE_INPUT_DISPLAY);
  const Online::InputView::View view = Online::InputView::Get();
  std::vector<Box> boxes;
  if (enabled && view.frames != 0 && view.count != 0)
  {
    // The game's picture (the whole window when the engine does not say).
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    Rect rect;
    if (const auto& game_rect = GetHooks().game_rect)
      rect = game_rect();
    if (rect.right <= rect.left || rect.bottom <= rect.top)
      rect = Rect{0, 0, static_cast<int>(display.x), static_cast<int>(display.y)};
    const float gw = static_cast<float>(rect.right - rect.left);
    const float gh = static_cast<float>(rect.bottom - rect.top);
    const auto setting = [](const Config::Info<int>& info, int lo, int hi) {
      return std::clamp(Config::Get(info), lo, hi) / 1000.0f;
    };
    float w = gw * setting(Config::ONLINE_INPUT_DISPLAY_WIDTH, Config::INPUT_DISPLAY_MIN_WIDTH,
                           Config::INPUT_DISPLAY_MAX_WIDTH);
    // Every player's box side by side (a replay), a gap of GAP widths between: smaller when they
    // would not fit.
    constexpr float GAP = 0.04f;
    const float n = static_cast<float>(view.count);
    w = std::min(w, gw / (n * (1 + GAP) - GAP));
    const float h = w / 2;
    const float total = w * (n * (1 + GAP) - GAP);
    float x = rect.left + gw * setting(Config::ONLINE_INPUT_DISPLAY_X, 0, 1000);
    float y = rect.top + gh * setting(Config::ONLINE_INPUT_DISPLAY_Y, 0, 1000);
    // Inside the picture; several boxes grow towards its middle from where the one box is.
    const bool leftwards = x + w / 2 > rect.left + gw / 2;
    if (leftwards)
      x = x + w - total;
    x = std::clamp(x, static_cast<float>(rect.left), std::max<float>(rect.left, rect.right - total));
    y = std::clamp(y, static_cast<float>(rect.top), std::max<float>(rect.top, rect.bottom - h));
    for (u32 i = 0; i < view.count; ++i)
      boxes.push_back(Box{ImVec2(x + i * w * (1 + GAP), y), w, view.pads[i]});
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const Box& b : boxes)
      DrawBox(dl, b, scale, view.count > 1);
  }

  std::string status = fmt::format(R"({{"enabled":{},"frames":{},"boxes":[)", enabled, view.frames);
  for (size_t i = 0; i < boxes.size(); ++i)
  {
    const Box& b = boxes[i];
    const Pad& p = b.pad;
    status += fmt::format(
        R"({}{{"x":{:.1f},"y":{:.1f},"w":{:.1f},"h":{:.1f},"port":{},"pad":{},"type":"{}","buttons":{},)"
        R"("stick":[{},{}],"sub":[{},{}],"triggers":[{},{}],"pressed":[{}]}})",
        i ? "," : "", b.min.x, b.min.y, b.w, b.w / 2, p.port, p.pad, TypeName(p.type), p.buttons,
        p.stick_x, p.stick_y, p.sub_x, p.sub_y, p.trigger_l, p.trigger_r, PressedNames(p));
  }
  status += "]}";
  std::lock_guard lk(s_status_mutex);
  s_status = std::move(status);
}

std::string Status()
{
  std::lock_guard lk(s_status_mutex);
  return s_status;
}
}  // namespace BrawlOnline::UI::InputDisplay
