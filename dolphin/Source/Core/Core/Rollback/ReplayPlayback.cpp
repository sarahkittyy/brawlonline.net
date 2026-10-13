// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ReplayPlayback.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/ConfigManager.h"
#include "Core/Online/GameBridge.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/PeerData.h"
#include "Core/Rollback/ReplayFile.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Gprb::Replay::Playback
{
namespace
{
constexpr size_t MAX_COMM_SIZE = 256 << 10;
constexpr size_t MAX_QUEUE = 64;
constexpr auto POLL = std::chrono::milliseconds(200);

struct Request
{
  std::string command_id;
  std::vector<std::string> paths;
  bool play_anyway = false;
  // The last replay holds at its last frame (the launcher's), or ends its match (the harness).
  bool hold_at_end = true;
};

std::mutex s_mutex;
std::string s_comm_path;
std::thread s_thread;
std::atomic<bool> s_run{false};
// The request being played (under s_mutex).
Request s_request;
size_t s_index = 0;
bool s_armed = false;     // s_request's replay s_index is armed
u32 s_armed_serial = 0;   // the session's serial of it
std::string s_state = "idle";
std::string s_error;
bool s_needs_play_anyway = false;
bool s_wait_boot = false;  // a comm file request, armed once the game has booted
std::string s_last_status;

// The game is running with our plugin (GameBridge found its block). The launcher's playback
// Dolphin starts the comm file follower before it boots, and boots P+'s launcher DOL, whose game
// ID is 00000000, then "ID-Project+ Netplay Launcher", and the disc's (RSBE01) only once the disc
// runs; the plugin runs only in the disc's game.
bool GameBooted()
{
  return Online::GameBridge::SessionAddress() != 0;
}

// A path from the comm file: an existing regular file, no larger than a replay can be.
bool UsablePath(const std::string& path)
{
  if (path.empty() || path.size() > 4096 || !::File::Exists(path) || ::File::IsDirectory(path))
    return false;
  return ::File::GetSize(path) <= MAX_FILE_SIZE;
}

std::optional<Request> ReadComm(const std::string& path)
{
  ::File::IOFile f(path, "rb");
  if (!f.IsOpen())
    return std::nullopt;
  const u64 size = f.GetSize();
  if (size == 0 || size > MAX_COMM_SIZE)
    return std::nullopt;
  std::string text(size, '\0');
  if (!f.ReadBytes(text.data(), size))
    return std::nullopt;
  picojson::value v;
  if (!PeerData::JsonSafeToParse(text, 8) || !picojson::parse(v, text).empty() ||
      !v.is<picojson::object>())
  {
    return std::nullopt;
  }
  const auto& o = v.get<picojson::object>();
  const auto str = [&](const char* k) {
    const auto it = o.find(k);
    return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() :
                                                            std::string();
  };
  Request r;
  r.command_id = str("commandId").substr(0, 128);
  const auto anyway = o.find("playAnyway");
  r.play_anyway = anyway != o.end() && anyway->second.is<bool>() && anyway->second.get<bool>();
  if (str("mode") == "queue")
  {
    const auto q = o.find("queue");
    if (q != o.end() && q->second.is<picojson::array>())
    {
      for (const auto& item : q->second.get<picojson::array>())
      {
        if (r.paths.size() >= MAX_QUEUE || !item.is<picojson::object>())
          continue;
        const auto& io = item.get<picojson::object>();
        const auto p = io.find("path");
        if (p != io.end() && p->second.is<std::string>())
          r.paths.push_back(p->second.get<std::string>());
      }
    }
  }
  else if (!str("replay").empty())
  {
    r.paths.push_back(str("replay"));
  }
  return r;
}

// Loads and arms the request's replay `s_index` (under s_mutex).
void ArmCurrent()
{
  s_armed = false;
  s_needs_play_anyway = false;
  if (s_index >= s_request.paths.size())
  {
    s_state = "ended";
    return;
  }
  const std::string& path = s_request.paths[s_index];
  std::string error;
  std::shared_ptr<const File> file;
  if (!UsablePath(path))
  {
    error = "the replay file is missing";
  }
  else if (auto f = ReadFile(path, ReadOptions{}, &error))
  {
    // The disc must be the one it was recorded with (a revision may differ: Rev 1 and Rev 2 play
    // the same, docs/backend-design.md).
    picojson::value info;
    picojson::parse(info, f->info);
    const auto& io = info.get<picojson::object>();
    const auto build = io.find("build");
    std::string game;
    if (build != io.end() && build->second.is<picojson::object>())
    {
      const auto& bo = build->second.get<picojson::object>();
      if (const auto g = bo.find("game"); g != bo.end() && g->second.is<std::string>())
        game = g->second.get<std::string>();
    }
    const std::string here = SConfig::GetInstance().GetGameID();
    if (!game.empty() && GameBooted() && game != here && !s_request.play_anyway)
    {
      error = fmt::format("the replay was recorded with another game ({}, this is {})", game, here);
      s_needs_play_anyway = true;
    }
    else
    {
      file = std::make_shared<const File>(std::move(*f));
      const bool last = s_index + 1 == s_request.paths.size();
      if (const auto e = Session::ArmPlayback(file, s_request.play_anyway, last && s_request.hold_at_end))
      {
        error = *e;
        s_needs_play_anyway = e->find("another version") != std::string::npos ||
                              e->find("setup differs") != std::string::npos;
      }
    }
  }
  if (!error.empty())
  {
    s_state = "error";
    s_error = error;
    ERROR_LOG_FMT(BRAWLBACK, "replay: cannot play {}: {}", path, error);
    OSD::AddMessage(fmt::format("This replay cannot be played: {}", error),
                    OSD::Duration::VERY_LONG, OSD::Color::RED);
    return;
  }
  s_armed = true;
  s_error.clear();
  s_state = "waiting";
  const auto st = Session::PlaybackStatus();
  if (const auto it = st.find("serial"); it != st.end() && it->second.is<double>())
    s_armed_serial = static_cast<u32>(it->second.get<double>());
  NOTICE_LOG_FMT(BRAWLBACK, "replay: {} of {}: {}", s_index + 1, s_request.paths.size(), path);
}

picojson::object StatusLocked()
{
  picojson::object o;
  o["commandId"] = picojson::value(s_request.command_id);
  o["state"] = picojson::value(s_state);
  o["index"] = picojson::value(static_cast<double>(s_index));
  o["count"] = picojson::value(static_cast<double>(s_request.paths.size()));
  o["replay"] = picojson::value(s_index < s_request.paths.size() ? s_request.paths[s_index] : "");
  o["error"] = picojson::value(s_error);
  o["needsPlayAnyway"] = picojson::value(s_needs_play_anyway);
  const auto st = Session::PlaybackStatus();
  for (const char* k : {"frame", "first_frame", "last_frame", "desync_frame", "end_method"})
  {
    if (const auto it = st.find(k); it != st.end())
      o[k] = it->second;
  }
  return o;
}

// One step of the loop (under s_mutex): follow the session, go on with the queue.
void Step()
{
  if (!s_armed)
    return;
  const auto st = Session::PlaybackStatus();
  const auto get_bool = [&](const char* k) {
    const auto it = st.find(k);
    return it != st.end() && it->second.is<bool>() && it->second.get<bool>();
  };
  const auto serial = st.find("serial");
  if (!get_bool("active") || serial == st.end() || !serial->second.is<double>() ||
      static_cast<u32>(serial->second.get<double>()) != s_armed_serial)
  {
    s_armed = false;
    s_state = "ended";
    return;
  }
  const std::string phase = st.at("phase").is<std::string>() ? st.at("phase").get<std::string>() : "";
  if (phase == "error")
  {
    s_state = "error";
    s_error = st.at("error").is<std::string>() ? st.at("error").get<std::string>() : "";
    s_needs_play_anyway = s_error.find("setup differs") != std::string::npos;
    s_armed = false;
    return;
  }
  if (get_bool("finished"))
  {
    // The next replay of the queue once this one's match is over (the session takes it when the
    // game is back from the match).
    if (s_index + 1 < s_request.paths.size())
    {
      ++s_index;
      ArmCurrent();
    }
    else
    {
      s_state = "ended";
    }
    return;
  }
  s_state = get_bool("started") ? "playing" : "waiting";
}

void WriteStatus()
{
  if (s_comm_path.empty())
    return;
  const std::string text = picojson::value(StatusLocked()).serialize();
  if (text == s_last_status)
    return;
  s_last_status = text;
  ::File::WriteStringToFile(s_comm_path + ".status.json", text);
}

void Loop()
{
  Common::SetCurrentThreadName("Replay playback");
  std::string last_command;
  while (s_run)
  {
    {
      std::lock_guard lk(s_mutex);
      if (!s_comm_path.empty())
      {
        if (auto r = ReadComm(s_comm_path); r && r->command_id != last_command)
        {
          last_command = r->command_id;
          Session::StopPlayback();
          s_request = std::move(*r);
          s_index = 0;
          s_error.clear();
          s_state = "loading";
          if (s_request.paths.empty())
          {
            s_state = "error";
            s_error = "nothing to play";
          }
          else
          {
            s_wait_boot = true;
          }
        }
      }
      if (s_wait_boot && GameBooted())
      {
        s_wait_boot = false;
        ArmCurrent();
      }
      Step();
      WriteStatus();
    }
    std::this_thread::sleep_for(POLL);
  }
}

void EnsureThread()
{
  if (s_run.exchange(true))
    return;
  s_thread = std::thread(Loop);
}
}  // namespace

void SetCommFile(const std::string& path)
{
  {
    std::lock_guard lk(s_mutex);
    s_comm_path = path;
  }
  NOTICE_LOG_FMT(BRAWLBACK, "replay: following {}", path);
  EnsureThread();
}

std::optional<std::string> PlayFile(const std::string& path, bool play_anyway, bool hold_at_end)
{
  std::lock_guard lk(s_mutex);
  s_request = {};
  s_request.command_id = fmt::format("harness-{}", path);
  s_request.paths = {path};
  s_request.play_anyway = play_anyway;
  s_request.hold_at_end = hold_at_end;
  s_index = 0;
  s_error.clear();
  s_wait_boot = false;
  ArmCurrent();
  EnsureThread();
  if (!s_armed)
    return s_error.empty() ? std::string("cannot play") : s_error;
  return std::nullopt;
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  return StatusLocked();
}

void Shutdown()
{
  if (!s_run.exchange(false))
    return;
  if (s_thread.joinable())
    s_thread.join();
}
}  // namespace Gprb::Replay::Playback
