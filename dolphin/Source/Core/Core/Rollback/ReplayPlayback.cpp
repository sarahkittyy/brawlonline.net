// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ReplayPlayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Online/GameBridge.h"
#include "Core/Rollback/ClipRecorder.h"
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
  // Highlights (`highlights`): each path plays from its clip's first frame to its last, then the
  // next one; the same replay as the one before goes on in the running match.
  bool highlights = false;
  std::vector<std::pair<s64, s64>> clips;
  // The launcher's list (`marks`, `nav`): its highlights of the replay, each with its place in the
  // list, marked instead of the queue's; the overlay's list buttons.
  struct ListMark
  {
    s64 start = 0;
    s64 end = 0;
    int position = 0;
  };
  bool has_marks = false;
  std::vector<ListMark> marks;
  Playback::NavView nav;
  // Rendering (`render`): highlights whose clips are written, one after another, to `output`.
  bool render = false;
  std::string output;
  // The video's height (Settings > Clips; ClipRecorder.h).
  int height = Clip::DEFAULT_HEIGHT;
};

// A spectated game's end: how long the last frame shows before Dolphin closes.
constexpr auto CLOSE_AFTER = std::chrono::seconds(5);
// A spectator whose first game already has more than 20 s at its barrier
// (Session::LIVE_JOIN_CATCH_UP_FRAMES) joined it well under way: it catches up to live. One there
// from about its start (the stream comes a few seconds behind the players) watches it from the
// start at normal speed, as every next game of the stream is (the user, 2026-10-14: tournaments
// watch the whole game; anyone can go live).

std::mutex s_mutex;
std::string s_comm_path;
// s_comm_path is set (Following): read on every pad poll, so without s_mutex.
std::atomic<bool> s_following{false};
std::thread s_thread;
std::atomic<bool> s_run{false};
// The request being played (under s_mutex).
Request s_request;
size_t s_index = 0;
bool s_armed = false;     // s_request's replay s_index is armed
u32 s_armed_serial = 0;   // the session's serial of it
std::string s_armed_path;  // its file
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
bool s_close_saved = false;  // the settings were saved for the close
// Mirror: the game waiting for its first frames (its index; SIZE_MAX: none), since when, and
// since when it is known to be cut without any.
size_t s_wait_index = SIZE_MAX;
std::chrono::steady_clock::time_point s_wait_since;
std::optional<std::chrono::steady_clock::time_point> s_wait_cut_since;
// Waiting this long for a game's first frames says why it may take a while.
constexpr auto WAIT_EXPLAINED_AFTER = std::chrono::seconds(15);
// Highlights: the armed replay's clip was given to the session; the frame its fast-forward
// started from (-1: not seen yet), for the progress.
bool s_clip_applied = false;
s64 s_seek_from = -1;
// Highlights: the clicks on the seek bar's marks already followed (the session's count, from 0 for
// each replay armed).
u64 s_mark_picks = 0;
// Rendering: the output is open; the clip that ended, its frames still coming (the frame dumper
// hands them over a frame late; while paused the last one is shown again) since when, and how many
// had come; clips written.
bool s_render_open = false;
bool s_render_draining = false;
std::chrono::steady_clock::time_point s_drain_since;
u64 s_drain_frames = 0;
size_t s_rendered = 0;
// A clip's frames have all come once none has for this long.
constexpr auto DRAIN_QUIET = std::chrono::milliseconds(500);
// Manual clips saved in this run (the overlay's "Save as highlight"): the replay, its frames.
struct SavedClip
{
  std::string path;
  s64 start = 0;
  s64 end = 0;
};
std::vector<SavedClip> s_saved_clips;
// CanSaveClip, as of the loop's last step (the overlay asks every frame, without s_mutex).
std::atomic<bool> s_can_save_clip{false};
// The marks given to the session (their index is the session's `item`): a queue item, one of the
// list's, or a clip saved here.
struct MarkSource
{
  s64 start = 0;
  s64 end = 0;
  size_t queue_item = SIZE_MAX;  // SIZE_MAX: not the queue's
  int position = 0;
  bool saved = false;
};
std::vector<MarkSource> s_marks;
// For the launcher, counted from 1 for each commandId: the last star (or < >) played here, and the
// last list button that needs it.
u64 s_pick_seq = 0;
s64 s_pick_start = 0, s_pick_end = 0;
u64 s_nav_seq = 0;
std::string s_nav_action;
s64 s_nav_frame = 0;
// GetNav's view, as of the loop's last step.
std::mutex s_nav_mutex;
Playback::NavView s_nav_view;
constexpr size_t MAX_MARKS = 500;
void ApplyMarks();
// (A run saves a few; this many is far more than anyone makes, and keeps the status small.)
constexpr size_t MAX_SAVED_CLIPS = 500;

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
  r.render = str("mode") == "render";
  r.highlights = str("mode") == "highlights" || r.render;
  if (r.render)
  {
    r.output = str("output");
    if (const auto h = o.find("height"); h != o.end() && h->second.is<double>())
      r.height = static_cast<int>(h->second.get<double>());
    if (r.output.size() > 4096)
      r.output.clear();
  }
  if (r.highlights)
  {
    const auto frame = [](const picojson::object& io, const char* k) -> std::optional<s64> {
      const auto it = io.find(k);
      if (it == io.end() || !it->second.is<double>())
        return std::nullopt;
      const double v = it->second.get<double>();
      if (!std::isfinite(v) || v < 0 || v > static_cast<double>(MAX_FRAMES))
        return std::nullopt;
      return static_cast<s64>(v);
    };
    const auto q = o.find("queue");
    if (q != o.end() && q->second.is<picojson::array>())
    {
      for (const auto& item : q->second.get<picojson::array>())
      {
        if (r.paths.size() >= MAX_QUEUE || !item.is<picojson::object>())
          continue;
        const auto& io = item.get<picojson::object>();
        const auto p = io.find("path");
        const auto start = frame(io, "startFrame");
        const auto end = frame(io, "endFrame");
        if (p == io.end() || !p->second.is<std::string>() || !start || !end || *end < *start)
          continue;
        r.paths.push_back(p->second.get<std::string>());
        r.clips.emplace_back(*start, *end);
      }
    }
    const auto m = o.find("marks");
    if (!r.render && m != o.end() && m->second.is<picojson::array>())
    {
      r.has_marks = true;
      for (const auto& item : m->second.get<picojson::array>())
      {
        if (r.marks.size() >= MAX_MARKS || !item.is<picojson::object>())
          continue;
        const auto& io = item.get<picojson::object>();
        const auto start = frame(io, "startFrame");
        const auto end = frame(io, "endFrame");
        const auto pos = io.find("position");
        if (!start || !end || *end < *start)
          continue;
        const double p = pos != io.end() && pos->second.is<double>() ? pos->second.get<double>() : 0;
        r.marks.push_back({*start, *end, std::isfinite(p) ? static_cast<int>(std::clamp(p, 0.0, 1e6)) : 0});
      }
    }
    const auto n = o.find("nav");
    if (!r.render && n != o.end() && n->second.is<picojson::object>())
    {
      const auto& no = n->second.get<picojson::object>();
      const auto num = [&](const char* k) {
        const auto it = no.find(k);
        const double v = it != no.end() && it->second.is<double>() ? it->second.get<double>() : 0;
        return std::isfinite(v) ? static_cast<int>(std::clamp(v, 0.0, 1e6)) : 0;
      };
      const auto yes = [&](const char* k) {
        const auto it = no.find(k);
        return it != no.end() && it->second.is<bool>() && it->second.get<bool>();
      };
      r.nav.shown = true;
      r.nav.position = num("position");
      r.nav.total = num("total");
      r.nav.prev_replay = yes("hasPrevReplay");
      r.nav.next_replay = yes("hasNextReplay");
    }
  }
  else if (str("mode") == "queue" || r.mirror)
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
  s_clip_applied = false;
  s_seek_from = -1;
  s_mark_picks = 0;
  s_needs_play_anyway = false;
  s_mirror_pending = false;
  s_close_at.reset();
  s_close_cancelled = false;
  s_close_saved = false;
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
    // A live game's file fills from its start: until it has the setup, wait (the launcher shows
    // that it is getting the game). With the setup it is armed: its match loads while its first
    // frames come, and the barrier waits for them.
    std::string why;
    std::optional<File> f;
    if (UsablePath(path))
      f = ReadFile(path, ReadOptions{}, &why);
    const bool cut_now = s_index < s_request.cut.size() && s_request.cut[s_index];
    if (!f || f->setup.empty() || (cut_now && f->pads.empty()))
    {
      const bool cut = cut_now;
      // A game cut before its first frames came: on to the next one, if it has come.
      if (cut && s_index + 1 < s_request.paths.size())
      {
        ++s_index;
        ArmCurrent();
        return;
      }
      s_mirror_pending = true;
      const auto now = std::chrono::steady_clock::now();
      if (s_wait_index != s_index)
      {
        s_wait_index = s_index;
        s_wait_since = now;
        s_wait_cut_since.reset();
      }
      // Retried every poll: the line stays while this waits and goes a second after.
      const auto show = [](const std::string& text) {
        OSD::AddTypedMessage(OSD::MessageType::SpectateWaiting, text, 1000, OSD::Color::CYAN);
      };
      if (!cut)
      {
        // The setup comes in the players' countdown; much later, the players' uploads are paused
        // for their game's sake.
        s_state = "loading";
        show(now - s_wait_since < WAIT_EXPLAINED_AFTER ?
                 "Game found, waiting for data..." :
                 "Waiting for data... The stream pauses while the players' connection is busy.");
        return;
      }
      // Cut with nothing to show (production 2026-10-14: "Game found, waiting for data..." stayed
      // 4 minutes on a room game whose upload was cut 30 s in): the set's next game, or the end
      // of the stream as after a game that played (Auto play goes on, or Dolphin closes).
      if (!s_wait_cut_since)
        s_wait_cut_since = now;
      if (!s_request.stream_over)
      {
        s_state = "between";
        show("This game wasn't streamed. Waiting for the next game...");
        return;
      }
      if (!s_request.close_on_end)
      {
        s_state = "ended";
        show(s_request.banner.empty() ? std::string("This game wasn't streamed.") : s_request.banner);
        return;
      }
      const auto left =
          std::chrono::ceil<std::chrono::seconds>(*s_wait_cut_since + CLOSE_AFTER - now).count();
      if (left <= 0)
      {
        // As MirrorAtEnd closes: what was set while watching is saved first.
        if (s_state != "close")
          Config::Save();
        s_state = "close";
        return;
      }
      s_state = "ended";
      show(fmt::format("This game wasn't streamed. Closing in {} s", left));
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
      // (A highlight's replay holds at its end: the next highlight may be of the same replay.)
      const bool hold = (last || s_request.highlights) && s_request.hold_at_end;
      // (A game that has already ended, e.g. a finished stream watched afterwards, plays from its
      // start. Whether it catches up is decided at its barrier, from the frames there by then.)
      const bool catch_up = s_request.mirror && !s_mirror_started && file->endg.empty();
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
    if (s_render_open)
    {
      Clip::Close(true);
      s_render_open = false;
    }
    s_state = "error";
    s_error = error;
    ERROR_LOG_FMT(BRAWLBACK, "replay: cannot play {}: {}", path, error);
    OSD::AddMessage(fmt::format("This replay cannot be played: {}", error),
                    OSD::Duration::VERY_LONG, OSD::Color::RED);
    return;
  }
  s_armed = true;
  s_armed_path = path;
  s_error.clear();
  s_state = "waiting";
  const auto st = Session::PlaybackStatus();
  if (const auto it = st.find("serial"); it != st.end() && it->second.is<double>())
    s_armed_serial = static_cast<u32>(it->second.get<double>());
  // (Highlights: once its clip is given, StepHighlights. Replays and spectated games: the clips
  // saved of it in this run.)
  if (!s_request.highlights)
    ApplyMarks();
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
  if (s_nav_seq > 0)
  {
    picojson::object nav;
    nav["seq"] = picojson::value(static_cast<double>(s_nav_seq));
    nav["action"] = picojson::value(s_nav_action);
    nav["frame"] = picojson::value(static_cast<double>(s_nav_frame));
    o["navRequest"] = picojson::value(std::move(nav));
  }
  if (s_pick_seq > 0)
  {
    picojson::object pick;
    pick["seq"] = picojson::value(static_cast<double>(s_pick_seq));
    pick["startFrame"] = picojson::value(static_cast<double>(s_pick_start));
    pick["endFrame"] = picojson::value(static_cast<double>(s_pick_end));
    o["highlightPick"] = picojson::value(std::move(pick));
  }
  if (!s_saved_clips.empty())
  {
    picojson::array clips;
    for (const auto& c : s_saved_clips)
    {
      picojson::object clip;
      clip["path"] = picojson::value(c.path);
      clip["startFrame"] = picojson::value(static_cast<double>(c.start));
      clip["endFrame"] = picojson::value(static_cast<double>(c.end));
      clips.emplace_back(std::move(clip));
    }
    o["savedClips"] = picojson::value(std::move(clips));
  }
  if (s_request.highlights && s_index < s_request.clips.size())
  {
    o["highlights"] = picojson::value(true);
    o["clipStart"] = picojson::value(static_cast<double>(s_request.clips[s_index].first));
    o["clipEnd"] = picojson::value(static_cast<double>(s_request.clips[s_index].second));
    o["seekFrom"] = picojson::value(static_cast<double>(s_seek_from));
  }
  if (s_request.render)
  {
    o["render"] = picojson::value(true);
    o["output"] = picojson::value(s_request.output);
    o["rendered"] = picojson::value(static_cast<double>(s_rendered));
    o["framesWritten"] = picojson::value(static_cast<double>(Clip::FramesWritten()));
  }
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
    // The launcher closes Dolphin, by ending its process (on Windows nothing of Dolphin's runs
    // after that): what was set while watching, e.g. the resolution with the Settings window still
    // open, is saved first (Dolphin saves it when that window or Dolphin itself closes).
    if (!s_close_saved)
      Config::Save();
    s_close_saved = true;
    s_state = "close";
    Session::SetPlaybackBanner("");
    return;
  }
  Session::SetPlaybackBanner(
      fmt::format("Closing in {} s - press any replay key to stay", std::max<long long>(left, 1)));
}

void NextHighlight();

// Rendering (under s_mutex): the output file is written out (or deleted, `error`), and the state
// says how it went.
void RenderFinish(const std::string& error)
{
  if (!s_render_open)
    return;
  s_render_open = false;
  s_render_draining = false;
  const auto close_error = Clip::Close(!error.empty());
  if (!error.empty() || close_error)
  {
    s_state = "error";
    s_error = !error.empty() ? error : *close_error;
    ERROR_LOG_FMT(BRAWLBACK, "replay: render to {} failed: {}", s_request.output, s_error);
    return;
  }
  s_state = "ended";
  NOTICE_LOG_FMT(BRAWLBACK, "replay: rendered {} clips to {}", s_rendered, s_request.output);
}

// Rendering (under s_mutex): a clip that ended, once its frames have all come: its segment ends
// and the next clip plays.
void RenderDrain()
{
  const u64 frames = Clip::SegmentFrames();
  const auto now = std::chrono::steady_clock::now();
  if (frames != s_drain_frames)
  {
    s_drain_frames = frames;
    s_drain_since = now;
  }
  if (const std::string e = Clip::LastError(); !e.empty())
  {
    RenderFinish(e);
    return;
  }
  if (now - s_drain_since < DRAIN_QUIET)
    return;
  s_render_draining = false;
  Clip::EndSegment();
  ++s_rendered;
  NextHighlight();
}

// Rendering: opens the request's output (under s_mutex); the error when it could not. Frames must
// carry the time of the VI field they were shown in: Immediately Present XFB (on by default, for
// latency) stamps them with an estimate of the next field instead, wrong whenever the GPU thread
// is ahead of or behind the CPU's (on macOS most frames came with an earlier one's time). It is
// turned off in the command line's layer, which every frontend has from the start, and in the
// game's run once there is one (a request comes before the game boots, when there is none).
std::optional<std::string> OpenRender()
{
  if (s_request.output.empty())
    return "no output file";
  for (const auto layer : {Config::LayerType::CommandLine, Config::LayerType::CurrentRun})
  {
    if (Config::GetLayer(layer))
      Config::Set(layer, Config::GFX_HACK_IMMEDIATE_XFB, false);
  }
  if (auto e = Clip::Open(s_request.output, s_request.height))
    return e;
  s_render_open = true;
  return std::nullopt;
}

// Highlights (under s_mutex): the line on screen while the next one loads or is fast-forwarded to.
void ShowHighlightProgress(const std::string& what)
{
  const std::string of =
      s_request.paths.size() > 1 ? fmt::format(" ({} of {})", s_index + 1, s_request.paths.size()) : "";
  OSD::AddTypedMessage(OSD::MessageType::HighlightSeek, fmt::format("{}{}", what, of), 600,
                       OSD::Color::CYAN);
}

// Highlights (under s_mutex, the armed replay running): its clip to the session, then on to the
// next one once it is done.
void StepHighlights(const picojson::object& st)
{
  const auto get_bool = [&](const char* k) {
    const auto it = st.find(k);
    return it != st.end() && it->second.is<bool>() && it->second.get<bool>();
  };
  const auto get_num = [&](const char* k) {
    const auto it = st.find(k);
    return it != st.end() && it->second.is<double>() ? static_cast<s64>(it->second.get<double>()) : -1;
  };
  // (A render whose file is written, or was given up: nothing more to do.)
  if (s_request.render && !s_render_open)
    return;
  if (s_render_draining)
  {
    RenderDrain();
    return;
  }
  // A click on a highlight's mark on the seek bar: the session plays it already, and the queue
  // goes on from there (the launcher's list follows `index`).
  // (One of the launcher's list, or a clip saved here: it says which, and the queue stays.)
  if (const s64 picks = get_num("highlight_picks"); picks > static_cast<s64>(s_mark_picks))
  {
    s_mark_picks = static_cast<u64>(picks);
    const s64 mark = get_num("highlight_pick");
    if (!s_request.render && mark >= 0 && static_cast<size_t>(mark) < s_marks.size())
    {
      const MarkSource& m = s_marks[static_cast<size_t>(mark)];
      const size_t item = m.queue_item;
      if (item < s_request.clips.size() && s_request.paths[item] == s_request.paths[s_index])
      {
        s_index = item;
        s_clip_applied = true;
        s_seek_from = -1;
        s_state = "seeking";
        return;
      }
      if (item == SIZE_MAX)
      {
        // (Now the request's clip: the status' clipStart / clipEnd follow it.)
        ++s_pick_seq;
        s_pick_start = m.start;
        s_pick_end = m.end;
        s_request.clips[s_index] = {m.start, m.end};
        s_seek_from = -1;
        s_state = "seeking";
        return;
      }
    }
  }
  const auto [start, end] = s_request.clips[s_index];
  if (!s_clip_applied)
  {
    s_clip_applied = Session::SetPlaybackClip(start, end, s_request.render);
    s_seek_from = -1;
    if (!s_clip_applied)
    {
      s_state = "waiting";
      return;
    }
    // Watched (not rendered): the highlights of this replay asked for, marked on the seek bar.
    ApplyMarks();
    // (The session takes it at its next loop top: until then it is on the way there.)
    s_state = get_bool("started") && get_num("shown") >= 0 ? "seeking" : "waiting";
    return;
  }
  if (get_bool("finished"))
  {
    // Played on past its held end (the replay controls): the match is over.
    s_state = "ended";
    if (s_request.render)
      RenderFinish("the replay ended before its clip");
    return;
  }
  const bool started = get_bool("started");
  const bool seeking = get_bool("seeking");
  const s64 frame = get_num("frame");
  // Until the match's frames run (its loading and countdown), `frame` is still the last match's.
  if (!started || get_num("shown") < 0)
  {
    s_state = "waiting";
    ShowHighlightProgress("Loading the highlight's replay...");
    return;
  }
  if (seeking)
  {
    if (s_seek_from < 0 || frame < s_seek_from)
      s_seek_from = std::max<s64>(frame, 0);
    s_state = "seeking";
    const double span = static_cast<double>(std::max<s64>(start - s_seek_from, 1));
    const int pct = static_cast<int>(std::clamp((frame - s_seek_from) / span, 0.0, 1.0) * 100);
    ShowHighlightProgress(fmt::format("Fast-forwarding to the highlight... {}%", pct));
    return;
  }
  if (!get_bool("clip_done") && !get_bool("at_end"))
  {
    s_state = "playing";
    return;
  }
  if (s_request.render)
  {
    // The clip's last frames are still on their way to the file: on once they are in.
    s_render_draining = true;
    s_drain_since = std::chrono::steady_clock::now();
    s_drain_frames = Clip::SegmentFrames();
    return;
  }
  NextHighlight();
}

// Highlights (under s_mutex): the queue's next one, or the end.
void NextHighlight()
{
  if (s_index + 1 >= s_request.paths.size())
  {
    s_state = "ended";
    if (s_request.render)
      RenderFinish("");
    return;
  }
  ++s_index;
  s_clip_applied = false;
  s_seek_from = -1;
  if (s_request.paths[s_index] == s_request.paths[s_index - 1])
  {
    s_state = "seeking";
    return;
  }
  // Another replay: this match ends first (the loop arms the next once the game is out of it).
  Session::StopPlayback();
  s_armed = false;
  s_state = "loading";
  s_wait_boot = true;
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
    if (s_request.render)
      RenderFinish("the replay stopped before its clips were rendered");
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
    if (s_render_open)
    {
      Clip::Close(true);
      s_render_open = false;
    }
    return;
  }
  if (s_request.highlights)
  {
    // (Nothing left of a request that failed at once: its output could not be opened.)
    if (s_index < s_request.clips.size())
      StepHighlights(st);
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

// Under s_mutex: GetNav's view as the request and the armed replay are now.
void UpdateNavView()
{
  Playback::NavView v;
  if (s_request.highlights && !s_request.render && s_request.nav.shown && s_armed)
    v = s_request.nav;
  std::lock_guard nk(s_nav_mutex);
  s_nav_view = v;
}

// Under s_mutex: the armed replay's marks to the session: the launcher's list's (or, without one,
// the queue's of this replay), then the clips saved of it in this run that aren't among them.
void ApplyMarks()
{
  if (!s_armed || s_request.render)
    return;
  s_marks.clear();
  if (s_request.highlights && s_request.has_marks)
  {
    for (const auto& m : s_request.marks)
      s_marks.push_back({m.start, m.end, SIZE_MAX, m.position, false});
  }
  else if (s_request.highlights)
  {
    for (size_t i = 0; i < s_request.clips.size(); ++i)
    {
      if (s_request.paths[i] == s_armed_path)
        s_marks.push_back({s_request.clips[i].first, s_request.clips[i].second, i, 0, false});
    }
  }
  for (const auto& c : s_saved_clips)
  {
    if (c.path != s_armed_path || s_marks.size() >= MAX_MARKS)
      continue;
    const bool listed = std::any_of(s_marks.begin(), s_marks.end(), [&](const MarkSource& m) {
      return m.start == c.start && m.end == c.end;
    });
    if (!listed)
      s_marks.push_back({c.start, c.end, SIZE_MAX, 0, true});
  }
  std::vector<Session::PlaybackMark> marks;
  for (size_t i = 0; i < s_marks.size(); ++i)
    marks.push_back({s_marks[i].start, s_marks[i].end, i, s_marks[i].position, s_marks[i].saved});
  Session::SetPlaybackMarks(std::move(marks));
}

// Under s_mutex: the armed replay is the session's (not one that ended, nor the next one's yet).
bool CanSaveClipLocked()
{
  if (s_comm_path.empty() || !s_armed || s_request.render || s_armed_path.empty())
    return false;
  const auto st = Session::PlaybackStatus();
  const auto serial = st.find("serial");
  return serial != st.end() && serial->second.is<double>() &&
         static_cast<u32>(serial->second.get<double>()) == s_armed_serial;
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
        else if (r && r->command_id == last_command && r->highlights && !r->render &&
                 s_request.highlights && !s_request.render)
        {
          // The same request written again: the launcher's list changed (its marks, the count).
          const auto same = [](const Request::ListMark& a, const Request::ListMark& b) {
            return a.start == b.start && a.end == b.end && a.position == b.position;
          };
          const bool marks_changed =
              r->has_marks != s_request.has_marks ||
              !std::equal(r->marks.begin(), r->marks.end(), s_request.marks.begin(), s_request.marks.end(), same);
          const auto& a = r->nav;
          const auto& b = s_request.nav;
          if (a.shown != b.shown || a.position != b.position || a.total != b.total ||
              a.prev_replay != b.prev_replay || a.next_replay != b.next_replay)
          {
            s_request.nav = r->nav;
          }
          if (marks_changed)
          {
            s_request.has_marks = r->has_marks;
            s_request.marks = std::move(r->marks);
            ApplyMarks();
          }
        }
        else if (r && r->command_id != last_command && r->highlights && !r->paths.empty() &&
                 s_armed && !s_request.mirror && !s_render_open && s_index < s_request.paths.size() &&
                 s_request.paths[s_index] == r->paths[0] && !Session::PlaybackStopping())
        {
          // Highlights of the replay already playing (the next row, Play all; the next render of
          // the render Dolphin, once the last one's file is written): its match goes on, and the
          // session seeks (forward: on from where it is; back: a keyframe put back).
          last_command = r->command_id;
          s_request = std::move(*r);
          s_index = 0;
          s_clip_applied = false;
          s_seek_from = -1;
          s_pick_seq = 0;
          s_nav_seq = 0;
          // (The stars clicked before this request, e.g. while it played as a replay, are not its.)
          {
            const auto st = Session::PlaybackStatus();
            const auto picks = st.find("highlight_picks");
            s_mark_picks = picks != st.end() && picks->second.is<double>() ?
                               static_cast<u64>(picks->second.get<double>()) :
                               0;
          }
          s_error.clear();
          s_render_draining = false;
          s_rendered = 0;
          s_state = "seeking";
          if (s_request.render)
          {
            if (auto e = OpenRender())
            {
              s_state = "error";
              s_error = *e;
              s_request.paths.clear();
              s_request.clips.clear();
            }
          }
        }
        else if (r && r->command_id != last_command)
        {
          last_command = r->command_id;
          Session::StopPlayback();
          // (A render still going is given up: its file deleted.)
          if (s_render_open)
          {
            Clip::Close(true);
            s_render_open = false;
          }
          s_render_draining = false;
          s_rendered = 0;
          s_request = std::move(*r);
          s_index = 0;
          s_pick_seq = 0;
          s_nav_seq = 0;
          s_mirror_started = false;
          s_wait_index = SIZE_MAX;
          s_error.clear();
          s_state = "loading";
          if (s_request.paths.empty())
          {
            s_state = "error";
            s_error = "nothing to play";
          }
          else if (s_request.render)
          {
            if (auto e = OpenRender())
            {
              s_state = "error";
              s_error = *e;
            }
            else
            {
              s_wait_boot = true;
            }
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
      s_can_save_clip.store(CanSaveClipLocked(), std::memory_order_relaxed);
      UpdateNavView();
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
  s_following = !path.empty();
  NOTICE_LOG_FMT(BRAWLBACK, "replay: following {}", path);
  EnsureThread();
}

bool Following()
{
  return s_following.load(std::memory_order_relaxed);
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

bool CanSaveClip()
{
  return s_can_save_clip.load(std::memory_order_relaxed);
}

NavView GetNav()
{
  std::lock_guard nk(s_nav_mutex);
  return s_nav_view;
}

void RequestNav(NavAction action, s64 frame)
{
  std::lock_guard lk(s_mutex);
  if (!s_request.highlights || s_request.render || !s_request.nav.shown)
    return;
  static constexpr const char* NAMES[] = {"prevReplay", "prevClip", "nextClip", "nextReplay"};
  ++s_nav_seq;
  s_nav_action = NAMES[static_cast<int>(action)];
  s_nav_frame = frame;
  NOTICE_LOG_FMT(BRAWLBACK, "replay: the list's {} asked for at frame {}", s_nav_action, frame);
  WriteStatus();
}

bool SaveClip(s64 start, s64 end)
{
  std::lock_guard lk(s_mutex);
  if (!CanSaveClipLocked() || end <= start || s_saved_clips.size() >= MAX_SAVED_CLIPS)
    return false;
  // (The same frames saved again are one clip.)
  const bool known = std::any_of(s_saved_clips.begin(), s_saved_clips.end(), [&](const SavedClip& c) {
    return c.path == s_armed_path && c.start == start && c.end == end;
  });
  if (!known)
  {
    s_saved_clips.push_back({s_armed_path, start, end});
    // Its star at once (the user, 2026-10-24).
    ApplyMarks();
  }
  NOTICE_LOG_FMT(BRAWLBACK, "replay: clip saved, frames {} to {} of {}", start, end, s_armed_path);
  WriteStatus();
  return true;
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
  // (A render that did not finish: its partial file is deleted.)
  std::lock_guard lk(s_mutex);
  if (s_render_open)
  {
    Clip::Close(true);
    s_render_open = false;
  }
}
}  // namespace Gprb::Replay::Playback
