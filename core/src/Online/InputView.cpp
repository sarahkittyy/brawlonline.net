// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/Online/InputView.h"

#include <cstdlib>
#include <mutex>
#include <optional>

#include <picojson.h>

#include "BrawlOnline/Config/OnlineSettings.h"
#include "BrawlOnline/Rollback/GameplayRollback.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace Online::InputView
{
namespace
{
std::mutex s_mutex;
View s_view;
// The pad this machine's player last pressed (shown outside a gameplay session); -1: none yet.
int s_active_pad = -1;

// A stick this far from the middle counts as a press (both kinds' units: GameCube -128..127, Wii
// -100..100), so a resting stick's drift does not take the display to its controller.
constexpr int STICK_PRESS = 24;
constexpr int TRIGGER_PRESS = 60;
constexpr size_t MAX_FILE = 4096;

u32 BE32(const u8* p)
{
  return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

bool Pressed(const Pad& p)
{
  return p.buttons != 0 || std::abs(p.stick_x) > STICK_PRESS || std::abs(p.stick_y) > STICK_PRESS ||
         std::abs(p.sub_x) > STICK_PRESS || std::abs(p.sub_y) > STICK_PRESS ||
         p.trigger_l > TRIGGER_PRESS || p.trigger_r > TRIGGER_PRESS;
}
}  // namespace

bool Decode(const u8* r, Pad* out)
{
  // gfPadStatus +0x38: the error (s8; 0 NONE); +0x3C: the type, a u32 0-3.
  if (r[0x38] != 0 || r[0x3C] != 0 || r[0x3D] != 0 || r[0x3E] != 0 || r[0x3F] > 3)
    return false;
  out->type = static_cast<Type>(r[0x3F]);
  out->buttons = BE32(r);
  out->stick_x = static_cast<s8>(r[0x30]);
  out->stick_y = static_cast<s8>(r[0x31]);
  out->sub_x = static_cast<s8>(r[0x32]);
  out->sub_y = static_cast<s8>(r[0x33]);
  out->trigger_l = r[0x34];
  out->trigger_r = r[0x35];
  return true;
}

void OnFrameEnd(const u8* pads, u32 ports)
{
  View v;
  std::array<Pad, PADS> decoded{};
  std::array<bool, PADS> has{};
  for (u32 i = 0; i < PADS; ++i)
  {
    has[i] = Decode(pads + i * PAD_SIZE, &decoded[i]);
    decoded[i].pad = static_cast<int>(i);
  }
  std::lock_guard lk(s_mutex);
  if (ports != 0)
  {
    // Each port's record is in its GameCube or its Wii Remote pad (the other has no controller).
    for (u32 port = 0; port < MAX_SHOWN; ++port)
    {
      if (!(ports & (1u << port)))
        continue;
      const u32 wii = port + 4;
      const u32 pick = has[port] ? port : has[wii] ? wii : PADS;
      if (pick == PADS)
        continue;
      Pad p = decoded[pick];
      p.port = static_cast<int>(port);
      v.pads[v.count++] = p;
    }
  }
  else
  {
    // This machine's controllers: the one pressed last (a press on another takes the display
    // there). While it has no controller (a Wii Remote reconnecting, its extension plugged in)
    // the first that has one is shown, and it comes back with its controller.
    for (u32 i = 0; i < PADS; ++i)
    {
      if (has[i] && Pressed(decoded[i]) &&
          (s_active_pad < 0 || !has[s_active_pad] || !Pressed(decoded[s_active_pad])))
      {
        s_active_pad = static_cast<int>(i);
        break;
      }
    }
    int shown = s_active_pad;
    if (shown < 0 || !has[shown])
    {
      shown = -1;
      for (u32 i = 0; i < PADS && shown < 0; ++i)
      {
        if (has[i])
          shown = static_cast<int>(i);
      }
      if (s_active_pad < 0)
        s_active_pad = shown;
    }
    if (shown >= 0)
      v.pads[v.count++] = decoded[shown];
  }
  v.frames = s_view.frames + 1;
  s_view = v;
}

void SampleGuest(u32 ports)
{
  auto& memory = Core::System::GetInstance().GetMemory();
  const Gprb::Guest g(memory);
  // gfPadSystem made (its pointer set): the pads are its, not whatever the boot left there.
  if (g.Ptr32(Gprb::Addr::PAD_SYSTEM_PTR) != Gprb::Addr::PAD_STATUS - 0x40)
    return;
  const u8* pads = g.Ptr(Gprb::Addr::PAD_STATUS, PADS * PAD_SIZE);
  if (pads)
    OnFrameEnd(pads, ports);
}

View Get()
{
  std::lock_guard lk(s_mutex);
  return s_view;
}

void Reset()
{
  std::lock_guard lk(s_mutex);
  s_view = View{};
  s_active_pad = -1;
}

void PollLauncherFile(const std::string& online_dir)
{
  const std::string path = online_dir + "input-display.json";
  if (!File::Exists(path))
    return;
  std::string text;
  const bool read = File::GetSize(path) <= MAX_FILE && File::ReadFileToString(path, text);
  File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
  picojson::value v;
  if (!read || !picojson::parse(v, text).empty() || !v.is<picojson::object>())
  {
    WARN_LOG_FMT(BRAWLBACK, "input-display.json unreadable or not a JSON object: ignored");
    return;
  }
  const auto& o = v.get<picojson::object>();
  const auto number = [&](const char* k, int lo, int hi) -> std::optional<int> {
    const auto it = o.find(k);
    if (it == o.end() || !it->second.is<double>())
      return std::nullopt;
    const double d = it->second.get<double>();
    if (!(d >= lo && d <= hi))
      return std::nullopt;
    return static_cast<int>(d);
  };
  const auto version = number("version", 1, 1);
  const auto enabled = o.find("enabled");
  const auto x = number("x", 0, 1000);
  const auto y = number("y", 0, 1000);
  const auto width = number("width", Config::INPUT_DISPLAY_MIN_WIDTH, Config::INPUT_DISPLAY_MAX_WIDTH);
  if (!version || enabled == o.end() || !enabled->second.is<bool>() || !x || !y || !width)
  {
    WARN_LOG_FMT(BRAWLBACK, "input-display.json: not version 1 with enabled, x, y and width: ignored");
    return;
  }
  INFO_LOG_FMT(BRAWLBACK, "launcher: input display {} at {},{} width {}",
               enabled->second.get<bool>() ? "on" : "off", *x, *y, *width);
  Config::SetBaseOrCurrent(Config::ONLINE_INPUT_DISPLAY, enabled->second.get<bool>());
  Config::SetBaseOrCurrent(Config::ONLINE_INPUT_DISPLAY_X, *x);
  Config::SetBaseOrCurrent(Config::ONLINE_INPUT_DISPLAY_Y, *y);
  Config::SetBaseOrCurrent(Config::ONLINE_INPUT_DISPLAY_WIDTH, *width);
  if (const auto everyone = o.find("everyone"); everyone != o.end() && everyone->second.is<bool>())
    Config::SetBaseOrCurrent(Config::ONLINE_INPUT_DISPLAY_EVERYONE, everyone->second.get<bool>());
}
}  // namespace Online::InputView
