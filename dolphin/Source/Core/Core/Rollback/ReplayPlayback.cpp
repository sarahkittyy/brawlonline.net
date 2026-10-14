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
  // Mirror: the games whose rest won't come (`"cut": true` in the queue).
  std::vector<bool> cut;
  bool play_anyway = false;
  // The last replay holds at its last frame (the launcher's), or ends its match (the harness).
  bool hold_at_end = true;
  // Spectating (`mirror`, docs/design/spectating.md §6.2): the files are a live stream's games,
  // still growing; the launcher adds a set's next game to `paths` under the same commandId and
  // says when the stream is over. Then, at its end: close (the launcher closes Dolphin) after a
  // countdown, unless any control is used; or show `banner` (Auto play's wait).
  bool mirror = false;
  bool stream_over = false;
  bool close_on_end = false;
  std::string banner;
};

// A spectated game's end: how long the last frame shows before Dolphin closes.
constexpr auto CLOSE_AFTER = std::chrono::seconds(5);
// A spectator whose first game already has more than this much (20 s) when it starts joined it
// well under way: it catches up to live. One there from about its start (the stream comes about
// 10 s behind the players) watches it from the start at normal speed, as every next game of the
// stream is (the user, 2026-10-14: tournaments watch the whole game; anyone can go live).
constexpr size_t JOIN_CATCH_UP_FRAMES = 20 * 60;

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
// Mirror: the armed file's size when last read, and the hold it was given; the game's wait for
// its first frames; the close countdown (and the controls count it started at).
u64 s_mirror_size = 0;
bool s_mirror_hold = true;
bool s_mirror_cut = false;
bool s_mirror_pending = false;
bool s_mirror_started = false;  // a game of this request was armed (the next ones never catch up)
std::optional<std::chrono::steady_clock::time_point> s_close_at;
double s_close_controls = 0;
bool s_close_cancelled = false;

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
  const auto flag = [&](const char* k) {
    const auto it = o.find(k);
    return it != o.end() && it->second.is<bool>() && it->second.get<bool>();
  };
  r.play_anyway = flag("playAnyway");
  r.mirror = str("mode") == "mirror";
  if (r.mirror)
  {
    // A live game never plays "anyway": it would only lose sync.
    r.play_anyway = false;
    r.stream_over = flag("streamOver");
    r.close_on_end = flag("closeOnEnd");
    r.banner = str("banner").substr(0, 120);
  }
  if (str("mode") == "queue" || r.mirror)
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
        {
          r.paths.push_back(p->second.get<std::string>());
          const auto c = io.find("cut");
          r.cut.push_back(c != io.end() && c->second.is<bool>() && c->second.get<bool>());
        }
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
  s_mirror_pending = false;
  s_close_at.reset();
  s_close_cancelled = false;
  if (s_index >= s_request.paths.size())
  {
    s_state = "ended";
    return;
  }
  const std::string& path = s_request.paths[s_index];
  std::string error;
  std::shared_ptr<const File> file;
  const u64 size = ::File::Exists(path) ? ::File::GetSize(path) : 0;
  if (s_request.mirror)
  {
    // A live game's file fills from its start: until it has the setup and a first second of
    // frames, wait (the launcher shows that it is getting the game).
    std::string why;
    std::optional<File> f;
    if (UsablePath(path))
      f = ReadFile(path, ReadOptions{}, &why);
    if (!f || f->setup.empty() || f->pads.empty())
    {
      // A game cut before its first frames came: on to the next one, if it has come.
      if (s_index < s_request.cut.size() && s_request.cut[s_index] &&
          s_index + 1 < s_request.paths.size())
      {
        ++s_index;
        ArmCurrent();
        return;
      }
      s_mirror_pending = true;
      s_state = "loading";
      // Retried every poll: the line stays while this waits and goes a second after (the upload
      // holds a game's first 10 s, so a game picked at its start waits about that long).
      OSD::AddTypedMessage(OSD::MessageType::SpectateWaiting, "Game found, waiting for data...", 1000,
                           OSD::Color::CYAN);
      return;
    }
  }
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
      const bool hold = last && s_request.hold_at_end;
      // (A game that has already ended, e.g. a finished stream watched afterwards, plays from its
      // start.)
      const bool catch_up = s_request.mirror && !s_mirror_started &&
                            file->pads.size() > JOIN_CATCH_UP_FRAMES && file->endg.empty();
      if (const auto e =
              Session::ArmPlayback(file, s_request.play_anyway, hold, s_request.mirror, catch_up))
      {
        error = *e;
        s_needs_play_anyway = !s_request.mirror && (e->find("another version") != std::string::npos ||
                                                    e->find("setup differs") != std::string::npos);
      }
      s_mirror_size = size;
      s_mirror_hold = hold;
      s_mirror_cut = false;
      if (s_request.mirror)
        s_mirror_started = true;
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
  o["mirror"] = picojson::value(s_request.mirror);
  if (s_close_at)
  {
    o["closeIn"] = picojson::value(std::max(0.0, std::chrono::duration<double>(
                                                     *s_close_at - std::chrono::steady_clock::now())
                                                     .count()));
  }
  // (Not armed yet: what the session has is the last request's.)
  if (s_wait_boot)
    return o;
  const auto st = Session::PlaybackStatus();
  for (const char* k : {"frame", "first_frame", "last_frame", "desync_frame", "end_method",
                        "at_end", "live_edge", "buffering", "catching_up", "catch_rate", "complete"})
  {
    if (const auto it = st.find(k); it != st.end())
      o[k] = it->second;
  }
  return o;
}

// Mirror (under s_mutex): the armed game's file as it grows, and the set's next game once it is
// queued (the armed one then ends its match at its end instead of holding there).
void FollowMirror()
{
  const std::string& path = s_request.paths[s_index];
  const bool hold = s_index + 1 >= s_request.paths.size();
  const bool cut = s_index < s_request.cut.size() && s_request.cut[s_index];
  const u64 size = ::File::Exists(path) ? ::File::GetSize(path) : 0;
  if (size == s_mirror_size && hold == s_mirror_hold && cut == s_mirror_cut)
    return;
  std::shared_ptr<const File> file;
  if (size != s_mirror_size && size <= MAX_FILE_SIZE)
  {
    std::string error;
    if (auto f = ReadFile(path, ReadOptions{}, &error))
      file = std::make_shared<const File>(std::move(*f));
    else
      return;  // read again at the next poll
  }
  s_mirror_size = size;
  s_mirror_hold = hold;
  s_mirror_cut = cut;
  Session::ExtendPlayback(file, hold, cut);
}

// Mirror, held at the armed game's end (under s_mutex): the set's next game is coming, or the
// stream is over: then Auto play's banner, or the close countdown, which any control cancels.
void MirrorAtEnd(const picojson::object& st)
{
  using std::chrono::steady_clock;
  if (!s_request.stream_over)
  {
    s_state = "between";
    Session::SetPlaybackBanner("The next game starts soon");
    return;
  }
  s_state = "ended";
  if (!s_request.close_on_end || s_close_cancelled)
  {
    s_close_at.reset();
    Session::SetPlaybackBanner(s_request.banner);
    return;
  }
  const auto c = st.find("controls");
  const double controls = c != st.end() && c->second.is<double>() ? c->second.get<double>() : 0;
  const auto now = steady_clock::now();
  if (!s_close_at)
  {
    s_close_at = now + CLOSE_AFTER;
    s_close_controls = controls;
  }
  else if (controls != s_close_controls)
  {
    // Any control: it stays, held at the end like a finished replay.
    s_close_cancelled = true;
    s_close_at.reset();
    Session::SetPlaybackBanner("");
    return;
  }
  const auto left = std::chrono::ceil<std::chrono::seconds>(*s_close_at - now).count();
  if (left <= 0)
  {
    // The launcher closes Dolphin.
    s_state = "close";
    Session::SetPlaybackBanner("");
    return;
  }
  Session::SetPlaybackBanner(
      fmt::format("Closing in {} s - press any replay key to stay", std::max<long long>(left, 1)));
}

// One step of the loop (under s_mutex): follow the session, go on with the queue.
void Step()
{
  if (s_request.mirror && s_mirror_pending)
  {
    ArmCurrent();
    return;
  }
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
  if (s_request.mirror)
    FollowMirror();
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
      // A spectated stream's next game may still come: it is armed when the launcher adds it.
      s_state = s_request.mirror && !s_request.stream_over ? "between" : "ended";
    }
    return;
  }
  if (s_request.mirror && get_bool("at_end"))
  {
    MirrorAtEnd(st);
    return;
  }
  if (s_request.mirror)
  {
    s_close_at.reset();
    Session::SetPlaybackBanner("");
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
        auto r = ReadComm(s_comm_path);
        if (r && r->mirror && s_request.mirror && r->command_id == last_command)
        {
          // The same stream: a set's next game added, the stream over, the settings changed.
          // Games are only ever added at the end.
          if (r->paths.size() >= s_request.paths.size() &&
              std::equal(s_request.paths.begin(), s_request.paths.end(), r->paths.begin()))
          {
            s_request.paths = std::move(r->paths);
            s_request.cut = std::move(r->cut);
          }
          s_request.stream_over = r->stream_over;
          s_request.close_on_end = r->close_on_end;
          s_request.banner = std::move(r->banner);
        }
        else if (r && r->command_id != last_command)
        {
          last_command = r->command_id;
          Session::StopPlayback();
          s_request = std::move(*r);
          s_index = 0;
          s_mirror_started = false;
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
      // Another request while a replay played (spectating: another game picked): its match ends
      // first and the game leaves it; armed before that, the new replay would take over the
      // old match and never start.
      const bool stopping = Session::PlaybackStopping();
      if (s_wait_boot && GameBooted() && !stopping)
      {
        s_wait_boot = false;
        ArmCurrent();
      }
      if (!stopping)
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

bool Following()
{
  std::lock_guard lk(s_mutex);
  return !s_comm_path.empty();
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
