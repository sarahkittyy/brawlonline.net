// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/SpeedCheck.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Online/Matchmaking.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/System.h"

namespace Online::SpeedCheck
{
namespace
{
using Clock = std::chrono::steady_clock;

// The room's first 2 s of running are left out: its loads and first draws (shaders) stutter on
// any PC. Then windows of 300 frames (5 s, as the match's own check, UpdateSlowNotice), at most
// four. A gap of over 1 s between two frames (Dolphin paused, the PC asleep) starts the window
// again: that is no frame rate.
constexpr u32 WARMUP_FRAMES = 120;
constexpr u32 WINDOW_FRAMES = 300;
constexpr size_t MAX_WINDOWS = 4;
constexpr auto MAX_GAP = std::chrono::seconds(1);

// CPU thread only.
struct Room
{
  bool in = false;
  bool done = false;  // reported (four windows in): nothing more until the next room
  Matchmaking::Mode mode = Matchmaking::Mode::Unranked;
  u32 window_frames = 0;
  Clock::time_point window_start{};
  Clock::time_point last{};
  std::vector<double> windows;
  u32 gaps = 0;
};
Room s_room;

std::mutex s_mutex;  // guards s_outbox
std::vector<std::string> s_outbox;

void Report(Room& r)
{
  r.done = true;
  if (r.windows.empty())
  {
    INFO_LOG_FMT(NETPLAY, "Speed check: the room ended before a whole window ({} gaps)", r.gaps);
    return;
  }
  picojson::array windows;
  double sum = 0;
  for (const double fps : r.windows)
  {
    windows.emplace_back(std::round(fps * 100) / 100);
    sum += fps;
  }
  const double fps = sum / static_cast<double>(r.windows.size());
  picojson::object o{{"type", picojson::value("speed-check")},
                     {"mode", picojson::value(Matchmaking::ModeName(r.mode))},
                     {"windows", picojson::value(std::move(windows))},
                     {"gaps", picojson::value(static_cast<double>(r.gaps))},
                     {"backend", picojson::value(Config::Get(Config::MAIN_GFX_BACKEND))}};
  INFO_LOG_FMT(NETPLAY, "Speed check ({}): {:.2f} frames a second over {} windows ({:.2f}), {} gaps",
               Matchmaking::ModeName(r.mode), fps, r.windows.size(), fmt::join(r.windows, ", "),
               r.gaps);
  std::lock_guard lk(s_mutex);
  // Kept until a connection takes them; a few searches' at most.
  if (s_outbox.size() >= 4)
    s_outbox.erase(s_outbox.begin());
  s_outbox.push_back(picojson::value(std::move(o)).serialize());
}
}  // namespace

void OnLoopTop(const Core::CPUThreadGuard& guard)
{
  Room& r = s_room;
  const std::optional<Matchmaking::Mode> mode = Client::SearchMode();
  const bool queue = mode == Matchmaking::Mode::Unranked || mode == Matchmaking::Mode::Ranked;
  const Gprb::Guest g(guard.GetSystem().GetMemory());
  const bool in_room = queue && Gprb::CurrentSceneName(g) == "scMelee" && Gprb::IsWaitingRoom(g);
  if (!in_room)
  {
    if (r.in && !r.done)
      Report(r);
    r = {};
    return;
  }
  const auto now = Clock::now();
  if (!r.in)
  {
    r.in = true;
    r.mode = *mode;
  }
  if (r.done)
    return;
  // The match frame counter: 1 on the room's first simulation frame.
  if (g.U32(Gprb::Addr::GAME_FRAME + 4).value_or(0) < WARMUP_FRAMES)
    return;
  if (r.window_frames == 0 || now - r.last > MAX_GAP)
  {
    if (r.window_frames != 0)
      ++r.gaps;
    r.window_frames = 0;
    r.window_start = now;
  }
  r.last = now;
  if (++r.window_frames <= WINDOW_FRAMES)
    return;
  // 300 frames since the window's first one.
  const double seconds = std::chrono::duration<double>(now - r.window_start).count();
  r.windows.push_back(seconds > 0 ? WINDOW_FRAMES / seconds : 0.0);
  r.window_frames = 1;
  r.window_start = now;
  if (r.windows.size() >= MAX_WINDOWS)
    Report(r);
}

std::vector<std::string> TakeOutbox()
{
  std::lock_guard lk(s_mutex);
  return std::exchange(s_outbox, {});
}
}  // namespace Online::SpeedCheck
