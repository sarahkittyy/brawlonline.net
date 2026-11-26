// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/Online/RoomWatch.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "BrawlOnline/Rollback/GameplayRollback.h"
#include "BrawlOnline/Rollback/GameplaySession.h"
#include "BrawlOnline/Rollback/PeerData.h"
#include "BrawlOnline/Rollback/ReplayFile.h"
#include "Core/Online/RoomWatchInput.h"
#include "Core/Rollback/ReplayPlayback.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Online::RoomWatch
{
namespace
{
namespace Replay = Gprb::Replay;
namespace Session = Gprb::Session;

constexpr size_t MAX_JSON = 1024;
// The game never left the room CSS for an armed watch (an older plugin, a menu kept open): this
// game is not watched.
constexpr Clock::duration LEAVE_CSS_TIMEOUT = std::chrono::seconds(10);
constexpr Clock::duration TOAST_EVERY = std::chrono::seconds(1);
constexpr u32 TOAST_MS = 1500;
constexpr char TOAST_TEXT[] = "Game ended, press B to return to CSS.";
// Out of frames this long (the players' upload paused: their connection is busy), it says so.
constexpr Clock::duration BUFFERING_TOAST_AFTER = std::chrono::seconds(1);
constexpr char BUFFERING_TEXT[] =
    "Buffering (the players' connection is busy), press B to return to CSS.";
// gfPadButtons (Rollback/GameplaySession.cpp HOLD_BUTTONS): START, B.
constexpr u16 BUTTON_START = 0x1000;
constexpr u16 BUTTON_B = 0x0200;
// Clips (RoomWatch.h): the kept games' folder and the files shared with the launcher, in <Online>/.
constexpr char KEPT_FOLDER[] = "RoomWatch";
constexpr char CLIPS_FILE[] = "room-watch-clips.json";
constexpr char OVERLAY_FILE[] = "room-watch-overlay.json";
constexpr u64 MAX_OVERLAY_SIZE = 16 << 10;
// (A run saves a few; as the playback Dolphin's.)
constexpr size_t MAX_SAVED_CLIPS = 500;
constexpr size_t MAX_KEPT_GAMES = 64;
constexpr size_t MAX_MARKS = 500;

void PutU16(std::vector<u8>& out, u16 v)
{
  out.push_back(static_cast<u8>(v));
  out.push_back(static_cast<u8>(v >> 8));
}

void PutU32(std::vector<u8>& out, u32 v)
{
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}

// A .brep's header (Replay::Writer::Open).
std::vector<u8> FileHeader(const std::array<u8, 16>& uid)
{
  std::vector<u8> h(Replay::MAGIC.begin(), Replay::MAGIC.end());
  PutU16(h, Replay::FORMAT_MAJOR);
  PutU16(h, Replay::FORMAT_MINOR);
  PutU32(h, Replay::HEADER_SIZE);
  h.insert(h.end(), uid.begin(), uid.end());
  return h;
}

std::optional<picojson::object> JsonOf(std::span<const u8> bytes)
{
  if (bytes.size() > MAX_JSON)
    return std::nullopt;
  const std::string text(bytes.begin(), bytes.end());
  picojson::value v;
  if (!Gprb::PeerData::JsonSafeToParse(text, 4) || !picojson::parse(v, text).empty() ||
      !v.is<picojson::object>())
  {
    return std::nullopt;
  }
  return v.get<picojson::object>();
}

std::optional<std::string> Str(const picojson::object& o, const char* key, size_t max)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<std::string>() || it->second.get<std::string>().size() > max)
    return std::nullopt;
  return it->second.get<std::string>();
}

std::optional<std::array<u8, 16>> Uid(const std::string& hex)
{
  if (hex.size() != 32)
    return std::nullopt;
  std::array<u8, 16> uid{};
  for (size_t i = 0; i < 32; ++i)
  {
    const char c = hex[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0)
      return std::nullopt;
    uid[i / 2] = static_cast<u8>(uid[i / 2] << 4 | d);
  }
  return uid;
}

u32 GetU32(const u8* p)
{
  return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
         static_cast<u32>(p[3]) << 24;
}
}  // namespace

bool Assembler::OnPacket(std::span<const u8> data)
{
  if (data.empty())
    return false;
  const auto bad = [&](const char* why) {
    if (!m_games.empty() && !m_games.back().Done())
      m_games.back().bad = why;
    return false;
  };
  const u8 kind = data[0];
  const auto body = data.subspan(1);
  if (kind == KIND_BEGIN)
  {
    const auto o = JsonOf(body);
    if (!o)
      return bad("a game's start was malformed");
    const auto match_id = Str(*o, "matchId", 128);
    const auto uid_hex = Str(*o, "uid", 32);
    const auto uid = uid_hex ? Uid(*uid_hex) : std::nullopt;
    const auto number = o->find("number");
    if (!match_id || !uid)
      return bad("a game's start was malformed");
    if (!m_games.empty() && !m_games.back().Done())
      m_games.back().bad = "The rest of this game wasn't streamed.";
    Game g;
    g.match_id = *match_id;
    g.uid = *uid;
    if (number != o->end())
    {
      if (const auto n = Gprb::PeerData::JsonUInt(&number->second, 99))
        g.number = static_cast<u32>(*n);
    }
    g.bytes = FileHeader(g.uid);
    m_games.push_back(std::move(g));
    while (m_games.size() > MAX_GAMES)
      m_games.pop_front();
    return true;
  }
  if (kind == KIND_CHUNK)
  {
    if (m_games.empty() || m_games.back().Done())
      return false;
    Game& g = m_games.back();
    if (body.size() < 4 + Replay::CHUNK_HEADER_SIZE)
      return bad("a chunk was malformed");
    if (GetU32(body.data()) != g.chunks)
      return bad("a chunk of this game is missing");
    const auto chunk = body.subspan(4);
    if (g.bytes.size() + chunk.size() > MAX_GAME_BYTES)
      return bad("this game is too large");
    g.bytes.insert(g.bytes.end(), chunk.begin(), chunk.end());
    ++g.chunks;
    return true;
  }
  if (kind == KIND_END)
  {
    const auto o = JsonOf(body);
    const auto uid_hex = o ? Str(*o, "uid", 32) : std::nullopt;
    const auto uid = uid_hex ? Uid(*uid_hex) : std::nullopt;
    if (!uid || m_games.empty() || m_games.back().uid != *uid || m_games.back().Done())
      return false;
    Game& g = m_games.back();
    if (const auto why = Str(*o, "bad", 200))
      g.bad = why->empty() ? "The rest of this game wasn't streamed." : *why;
    else
      g.ended = true;
    return true;
  }
  return false;
}

namespace
{
struct State
{
  Assembler assembler;
  // What mm was told: `want` (none: not yet on this connection) and `watching`.
  std::optional<bool> want_sent;
  bool watching_sent = false;
  u64 connection = 0;
  // Left early (START / B, a launcher click): nothing more until the room's status changes.
  bool skip = false;
  bool skip_in_game = false;
  // The watched game's playback: armed (its serial), when, how much of its file it has, whether
  // the game left the room CSS for it, whether START / B were let go since.
  bool armed = false;
  u32 armed_serial = 0;
  std::array<u8, 16> armed_uid{};
  Clock::time_point armed_at{};
  size_t fed_size = 0;
  bool fed_done = false;
  bool left_css = false;
  bool pad_released = false;
  bool leaving = false;
  Clock::time_point next_toast{};
  // Since when the watched game has been out of frames (none: it is not).
  std::optional<Clock::time_point> buffering_since;
  u32 buffering_toasts_now = 0;  // ... and its toasts since
  bool played = false;           // it has played since its start's buffer
  std::string last_error;
  // For the harness.
  u32 watched = 0;      // games whose match started here
  u32 finished = 0;     // ... and ended by themselves (not left early)
  u32 left_early = 0;
  u32 toasts = 0;
  u32 buffering_toasts = 0;
  u32 bad_packets = 0;
  picojson::array results;  // each watched game's end (PlaybackStatus)

  // Clips (RoomWatch.h). <Online>/ (Files; empty: not told yet) and this run's name for the
  // launcher; the watched game's file (empty: it cannot be clipped).
  std::string dir;
  std::string run;
  std::string armed_path;
  struct SavedClip
  {
    std::string path;
    s64 start = 0;
    s64 end = 0;
  };
  std::vector<SavedClip> saved_clips;
  struct RenderAsk
  {
    u64 seq = 0;
    std::string path;
    s64 start = 0;
    s64 end = 0;
    bool music = true;
    bool clip = true;
    std::string label;
  };
  std::vector<RenderAsk> render_asks;
  u64 show_seq = 0;
  std::string show_id;
  // The games a clip was saved of: their file, how much of the game has been taken for it, what is
  // still to be written (Files), and whether it was created.
  struct Kept
  {
    std::array<u8, 16> uid{};
    std::string path;
    size_t taken = 0;
    std::vector<u8> pending;
    bool created = false;
  };
  std::vector<Kept> kept;
};

std::mutex s_mutex;
State s;
std::atomic<bool> s_abort{false};
std::atomic<bool> s_watching{false};
std::atomic<bool> s_hold_feed{false};
// CanSaveClip and ClipPath, as of the last Service (the overlay asks every frame).
std::atomic<bool> s_can_clip{false};
std::mutex s_clip_path_mutex;
std::string s_clip_path;
// Files' own (the Rooms thread's): the clips file and the overlay file as last written / read.
std::string s_clips_written;
std::string s_overlay_read;

std::string HexOf(const std::array<u8, 16>& uid)
{
  std::string out;
  for (const u8 b : uid)
    out += fmt::format("{:02x}", b);
  return out;
}

// Under s_mutex: what has come of a kept game since it was last taken, to be written.
void TakeBytes(State::Kept& k, const Game& g)
{
  if (g.bytes.size() <= k.taken)
    return;
  k.pending.insert(k.pending.end(), g.bytes.begin() + static_cast<std::ptrdiff_t>(k.taken), g.bytes.end());
  k.taken = g.bytes.size();
}

// Under s_mutex: `g` is about to be dropped; if it is kept, the rest of it is taken first.
void TakeBeforeDrop(const Game& g)
{
  for (auto& k : s.kept)
  {
    if (k.uid == g.uid)
      TakeBytes(k, g);
  }
}

// Under s_mutex: the clips saved of the watched game as stars on its seek bar (a click plays one).
void ApplyMarks()
{
  std::vector<Session::PlaybackMark> marks;
  for (const auto& c : s.saved_clips)
  {
    if (c.path == s.armed_path && marks.size() < MAX_MARKS)
      marks.push_back({c.start, c.end, marks.size(), 0, true});
  }
  Session::SetPlaybackMarks(std::move(marks));
}

// Under s_mutex: the watched game plays here, as its clips need (the overlay's view may be a
// Service old).
bool ClipAllowedLocked()
{
  if (!s.armed || s.leaving || s.armed_path.empty())
    return false;
  const Session::PlaybackView pv = Session::GetPlaybackView();
  return pv.active && pv.room_watch && pv.serial == s.armed_serial;
}

// Under s_mutex: the CanSaveClip / ClipPath the overlay reads.
void UpdateClipView()
{
  const bool can = ClipAllowedLocked();
  s_can_clip = can;
  std::lock_guard pk(s_clip_path_mutex);
  s_clip_path = can ? s.armed_path : std::string();
}

// Under s_mutex: SaveClip's work.
bool SaveClipLocked(s64 start, s64 end)
{
  if (!ClipAllowedLocked() || end <= start || s.saved_clips.size() >= MAX_SAVED_CLIPS)
    return false;
  const bool known = std::any_of(s.saved_clips.begin(), s.saved_clips.end(), [&](const State::SavedClip& c) {
    return c.path == s.armed_path && c.start == start && c.end == end;
  });
  if (!known)
  {
    const bool kept = std::any_of(s.kept.begin(), s.kept.end(),
                                  [&](const State::Kept& k) { return k.uid == s.armed_uid; });
    if (!kept)
    {
      if (s.kept.size() >= MAX_KEPT_GAMES)
        return false;
      State::Kept k;
      k.uid = s.armed_uid;
      k.path = s.armed_path;
      s.kept.push_back(std::move(k));
    }
    s.saved_clips.push_back({s.armed_path, start, end});
    ApplyMarks();
  }
  NOTICE_LOG_FMT(NETPLAY, "RoomWatch: clip saved, frames {} to {} of {}", start, end, s.armed_path);
  return true;
}

picojson::object ClipsJsonLocked()
{
  picojson::object o;
  o["version"] = picojson::value(1.0);
  o["run"] = picojson::value(s.run);
  picojson::array clips;
  for (const auto& c : s.saved_clips)
  {
    picojson::object clip;
    clip["path"] = picojson::value(c.path);
    clip["startFrame"] = picojson::value(static_cast<double>(c.start));
    clip["endFrame"] = picojson::value(static_cast<double>(c.end));
    clips.emplace_back(std::move(clip));
  }
  o["savedClips"] = picojson::value(std::move(clips));
  picojson::array asks;
  for (const auto& a : s.render_asks)
  {
    picojson::object ask;
    ask["seq"] = picojson::value(static_cast<double>(a.seq));
    ask["path"] = picojson::value(a.path);
    ask["startFrame"] = picojson::value(static_cast<double>(a.start));
    ask["endFrame"] = picojson::value(static_cast<double>(a.end));
    ask["music"] = picojson::value(a.music);
    ask["clip"] = picojson::value(a.clip);
    if (!a.label.empty())
      ask["label"] = picojson::value(a.label);
    asks.emplace_back(std::move(ask));
  }
  o["renderRequests"] = picojson::value(std::move(asks));
  if (s.show_seq > 0)
  {
    picojson::object show;
    show["seq"] = picojson::value(static_cast<double>(s.show_seq));
    show["id"] = picojson::value(s.show_id);
    o["showRender"] = picojson::value(std::move(show));
  }
  return o;
}

std::string WatchJson(std::optional<bool> want, std::optional<bool> watching)
{
  picojson::object o;
  o["type"] = picojson::value("room-watch");
  if (want)
    o["want"] = picojson::value(*want);
  if (watching)
    o["watching"] = picojson::value(*watching);
  return picojson::value(o).serialize();
}

// Any controller's START or B (the local controllers' newest sample, never the replay's inputs);
// `standing_still`: the host's controllers now instead (Core/Online/RoomWatchInput.h: the engine reads
// them itself).
bool LeavePressed(bool* released, bool standing_still)
{
  bool down = false;
  if (standing_still)
  {
    down = Online::RoomWatchInput::HostLeaveDown();
  }
  else
  {
    const Gprb::RawPads latest = Gprb::PadsLatestRaw();
    for (u32 port = 0; port < 8; ++port)  // the GameCube ports and the Wii Remotes
    {
      const u8* pad = latest.data() + port * Gprb::Addr::PAD_STRIDE;
      if (pad[0x38] != 0)  // gfPadError: no controller
        continue;
      const u16 buttons = static_cast<u16>(pad[2] << 8 | pad[3]);
      down = down || (buttons & (BUTTON_START | BUTTON_B)) != 0;
    }
  }
  if (!down)
  {
    *released = true;
    return false;
  }
  return *released;
}

// Under s_mutex: the watched game's playback is over here (back on the room CSS, or it could not
// start); `done`: drop the game.
void EndWatch(std::vector<std::string>* out, bool done)
{
  if (s.armed)
  {
    const picojson::object st = Session::PlaybackStatus();
    picojson::object r;
    for (const char* k : {"started", "finished", "frames_checked", "desync_frame", "end_method",
                          "first_frame", "last_frame", "error"})
    {
      if (const auto it = st.find(k); it != st.end())
        r[k] = it->second;
    }
    r["left_early"] = picojson::value(s.leaving);
    // Whether it caught up to the live edge first (decided at its barrier).
    const auto joined = st.find("join_catch_up");
    r["catch_up"] = joined != st.end() ? joined->second : picojson::value(false);
    if (!s.assembler.Games().empty())
      r["number"] = picojson::value(static_cast<double>(s.assembler.Games().front().number));
    s.results.emplace_back(r);
    while (s.results.size() > 16)
      s.results.erase(s.results.begin());
    Session::StopPlayback();
  }
  s.armed = false;
  s.leaving = false;
  s_watching = false;
  s.armed_path.clear();
  // (A kept game's file gets the rest of it, whatever is dropped here.)
  if (!s.assembler.Games().empty() && s.assembler.Games().front().uid == s.armed_uid)
    TakeBeforeDrop(s.assembler.Games().front());
  if (done && !s.assembler.Games().empty() && s.assembler.Games().front().uid == s.armed_uid)
    s.assembler.Games().pop_front();
  if (s.watching_sent)
  {
    out->push_back(WatchJson(std::nullopt, false));
    s.watching_sent = false;
  }
}

// Under s_mutex: START / B, a launcher click.
void LeaveEarly(std::vector<std::string>* out, const RoomInfo& room, const char* why)
{
  INFO_LOG_FMT(NETPLAY, "RoomWatch: left the watched game ({})", why);
  ++s.left_early;
  s.leaving = true;
  Session::StopPlayback();
  s.skip = true;
  s.skip_in_game = room.room_in_game;
  // Nothing more of it: mm stops sending, and what came of the next games is dropped too.
  out->push_back(WatchJson(false, std::nullopt));
  s.want_sent = false;
  while (s.assembler.Games().size() > 1)
    s.assembler.Games().pop_back();
}

std::optional<Replay::File> ParseGame(const Game& g)
{
  std::string error;
  return Replay::Parse(g.bytes, Replay::ReadOptions{}, &error);
}
}  // namespace

void OnPacket(std::span<const u8> data)
{
  std::lock_guard lk(s_mutex);
  // (A game's start can drop the oldest game, MAX_GAMES: a kept one has the rest of it first.)
  if (!data.empty() && data[0] == KIND_BEGIN && s.assembler.Games().size() >= MAX_GAMES)
    TakeBeforeDrop(s.assembler.Games().front());
  if (!s.assembler.OnPacket(data))
  {
    ++s.bad_packets;
    WARN_LOG_FMT(NETPLAY, "RoomWatch: a watch packet was dropped (kind {}, {} bytes)",
                 data.empty() ? -1 : data[0], data.size());
  }
}

std::vector<std::string> Service(Clock::time_point now, const RoomInfo& room)
{
  std::vector<std::string> out;
  std::lock_guard lk(s_mutex);
  // (Whichever way it returns: the overlay's CanSaveClip as things are now.)
  struct ClipViewOnReturn
  {
    ~ClipViewOnReturn() { UpdateClipView(); }
  } clip_view;
  const bool abort = s_abort.exchange(false);
  if (!room.online || !room.in_room)
  {
    // Out of the room (or its connection): mm forgets the watch with the membership.
    if (s.armed)
      EndWatch(&out, true);
    out.clear();
    s.assembler.Clear();
    s.want_sent.reset();
    s.watching_sent = false;
    s.skip = false;
    return out;
  }
  if (room.connection != s.connection)
  {
    s.connection = room.connection;
    s.want_sent.reset();
    s.watching_sent = false;
  }
  if (s.skip && room.room_in_game != s.skip_in_game)
    s.skip = false;
  // Not watching yet: this game is not watched (a launcher click, the harness).
  if (abort && !s.armed)
  {
    s.skip = true;
    s.skip_in_game = room.room_in_game;
    s.assembler.Clear();
  }
  const bool want = !s.skip;
  if (s.want_sent != want)
  {
    out.push_back(WatchJson(want, std::nullopt));
    s.want_sent = want;
  }

  const Session::PlaybackView pv = Session::GetPlaybackView();
  if (s.armed)
  {
    if (!pv.active || !pv.room_watch || pv.serial != s.armed_serial)
    {
      // Ended by something else (a launcher replay, the session's own stop).
      s.armed = false;
      EndWatch(&out, true);
      return out;
    }
    if (room.screen != static_cast<u8>(3))  // Rooms::Screen::Room
      s.left_css = true;
    auto& games = s.assembler.Games();
    Game* g = !games.empty() && games.front().uid == s.armed_uid ? &games.front() : nullptr;
    if (g && !s.leaving && !s_hold_feed &&
        (g->bytes.size() != s.fed_size || (g->Done() && !s.fed_done)))
    {
      if (auto f = ParseGame(*g))
      {
        Session::ExtendPlayback(std::make_shared<const Replay::File>(std::move(*f)), false,
                                !g->bad.empty());
      }
      s.fed_size = g->bytes.size();
      s.fed_done = g->Done();
    }
    const bool in_match = pv.in_match || (pv.starting && s.left_css);
    if (!s.leaving && (abort || room.own_game))
      LeaveEarly(&out, room, abort ? "asked" : "this player's own game starts");
    else if (!s.leaving && in_match && LeavePressed(&s.pad_released, pv.buffering))
      LeaveEarly(&out, room, pv.buffering ? "START or B, buffering" : "START or B");
    // The players' game is over while it still plays here.
    const bool players_done = !room.room_in_game || s.assembler.Games().size() > 1;
    if (!s.leaving && pv.in_match && players_done && now >= s.next_toast)
    {
      OSD::AddTypedMessage(OSD::MessageType::RoomWatch, TOAST_TEXT, TOAST_MS, OSD::Color::CYAN);
      s.next_toast = now + TOAST_EVERY;
      ++s.toasts;
    }
    // The players' upload paused (players first: their connection is busy) and the frames ran
    // out: it says why it stands still, and how to leave. Not the wait for a buffer's frames at
    // the start (the upload trickles then), only once it has played.
    if (pv.in_match && !pv.buffering)
      s.played = true;
    if (pv.in_match && pv.buffering && s.played && !s.leaving)
    {
      if (!s.buffering_since)
        s.buffering_since = now;
      if (!players_done && now - *s.buffering_since >= BUFFERING_TOAST_AFTER &&
          now >= s.next_toast)
      {
        if (s.buffering_toasts_now++ == 0)
          INFO_LOG_FMT(NETPLAY, "RoomWatch: buffering: out of the players' frames");
        OSD::AddTypedMessage(OSD::MessageType::RoomWatch, BUFFERING_TEXT, TOAST_MS,
                             OSD::Color::YELLOW);
        s.next_toast = now + TOAST_EVERY;
        ++s.buffering_toasts;
      }
    }
    else if (s.buffering_since)
    {
      if (s.buffering_toasts_now != 0)
      {
        INFO_LOG_FMT(NETPLAY, "RoomWatch: frames again after {:.1f} s buffering",
                     std::chrono::duration<double>(now - *s.buffering_since).count());
      }
      s.buffering_since.reset();
      s.buffering_toasts_now = 0;
    }
    if (pv.starting && !s.left_css && now - s.armed_at > LEAVE_CSS_TIMEOUT)
    {
      WARN_LOG_FMT(NETPLAY, "RoomWatch: the game did not leave the room CSS for the watched game");
      EndWatch(&out, true);
      return out;
    }
    // Back on the room CSS after its match (or after leaving it).
    if (s.left_css && room.screen == 3 && !pv.in_match && !pv.starting && !pv.stopping)
    {
      if (!s.leaving)
        ++s.finished;
      EndWatch(&out, true);
    }
    return out;
  }

  // Games over before they could be watched here, while the room is past them, are not watched.
  auto& games = s.assembler.Games();
  while (!games.empty() && games.front().Done() && (games.size() > 1 || !room.room_in_game))
    games.pop_front();
  if (games.empty() || s.skip || room.screen != 3 || room.own_game || room.me_in_game || pv.active ||
      Session::GetLobby().active)
  {
    return out;
  }
  const Game& g = games.front();
  auto f = ParseGame(g);
  // Armed once its setup is here (it comes in the players' countdown): the match loads while its
  // first frames come, and the playback's barrier waits for them, as spectating's mirror does
  // (Rollback/ReplayPlayback.cpp). A game cut before any frames is not watched.
  if (!f || f->setup.empty() || (f->pads.empty() && g.Done()))
    return out;
  // A game joined well into (more than Session::LIVE_JOIN_CATCH_UP_FRAMES at its barrier) catches
  // up to the live edge first; one watched from about its start plays from its start.
  const bool catch_up = f->endg.empty();
  const size_t frames = f->pads.size();
  if (const auto e = Session::ArmPlayback(std::make_shared<const Replay::File>(std::move(*f)), false,
                                          false, true, catch_up, true))
  {
    if (*e != s.last_error)
      WARN_LOG_FMT(NETPLAY, "RoomWatch: cannot watch the room's game {}: {}", g.number, *e);
    s.last_error = *e;
    // A game that cannot be played here never will be (another version, a damaged setup).
    if (e->find("running") == std::string::npos && e->find("playing") == std::string::npos &&
        e->find("ending") == std::string::npos)
    {
      games.pop_front();
    }
    return out;
  }
  s.last_error.clear();
  s.armed = true;
  s.armed_serial = Session::GetPlaybackView().serial;
  s.armed_uid = g.uid;
  // Where its file goes once a clip of it is saved (RoomWatch.h).
  s.armed_path = s.dir.empty() ? std::string() : s.dir + KEPT_FOLDER + "/" + HexOf(g.uid) + ".brep";
  s.armed_at = now;
  s.fed_size = g.bytes.size();
  s.fed_done = g.Done();
  s.left_css = false;
  s.pad_released = false;
  s.leaving = false;
  s.next_toast = {};
  s.buffering_since.reset();
  s.buffering_toasts_now = 0;
  s.played = false;
  ++s.watched;
  s_watching = true;
  INFO_LOG_FMT(NETPLAY, "RoomWatch: watching the room's game {} ({} frames so far)", g.number,
               frames);
  out.push_back(WatchJson(std::nullopt, true));
  s.watching_sent = true;
  return out;
}

bool Watching()
{
  return s_watching;
}

void Abort()
{
  s_abort = true;
}

void HoldFeed(bool hold)
{
  s_hold_feed = hold;
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  picojson::object o;
  o["armed"] = picojson::value(s.armed);
  o["want"] = picojson::value(s.want_sent.value_or(false));
  o["watching"] = picojson::value(s.watching_sent);
  o["skip"] = picojson::value(s.skip);
  o["left_css"] = picojson::value(s.left_css);
  o["watched"] = picojson::value(static_cast<double>(s.watched));
  o["finished"] = picojson::value(static_cast<double>(s.finished));
  o["left_early"] = picojson::value(static_cast<double>(s.left_early));
  o["toasts"] = picojson::value(static_cast<double>(s.toasts));
  o["buffering_toasts"] = picojson::value(static_cast<double>(s.buffering_toasts));
  o["bad_packets"] = picojson::value(static_cast<double>(s.bad_packets));
  o["hold"] = picojson::value(s_hold_feed.load());
  o["buffering"] = picojson::value(s.buffering_since.has_value());
  o["error"] = picojson::value(s.last_error);
  picojson::array games;
  for (const Game& g : s.assembler.Games())
  {
    picojson::object go;
    go["match_id"] = picojson::value(g.match_id);
    go["number"] = picojson::value(static_cast<double>(g.number));
    go["chunks"] = picojson::value(static_cast<double>(g.chunks));
    go["bytes"] = picojson::value(static_cast<double>(g.bytes.size()));
    go["ended"] = picojson::value(g.ended);
    go["bad"] = picojson::value(g.bad);
    games.emplace_back(go);
  }
  o["games"] = picojson::value(games);
  o["results"] = picojson::value(s.results);
  // Clips (RoomWatch.h).
  o["can_clip"] = picojson::value(s_can_clip.load());
  o["clip_path"] = picojson::value(s.armed_path);
  o["clips"] = picojson::value(ClipsJsonLocked());
  picojson::array kept;
  for (const auto& k : s.kept)
  {
    picojson::object ko;
    ko["path"] = picojson::value(k.path);
    ko["taken"] = picojson::value(static_cast<double>(k.taken));
    ko["pending"] = picojson::value(static_cast<double>(k.pending.size()));
    ko["created"] = picojson::value(k.created);
    kept.emplace_back(std::move(ko));
  }
  o["kept"] = picojson::value(std::move(kept));
  return o;
}

bool CanSaveClip()
{
  return s_can_clip.load();
}

bool SaveClip(s64 start, s64 end)
{
  std::lock_guard lk(s_mutex);
  return SaveClipLocked(start, end);
}

bool RequestRender(s64 start, s64 end, bool music, bool save, const std::string& label)
{
  std::lock_guard lk(s_mutex);
  // (Clip mode's is saved first, as Save as highlight: the launcher renders the Manual Clip it
  // keeps of it. A star's is one of the clips saved here already.)
  if (s.render_asks.size() >= MAX_SAVED_CLIPS || end <= start ||
      !(save ? SaveClipLocked(start, end) : ClipAllowedLocked()))
  {
    return false;
  }
  const u64 seq = s.render_asks.size() + 1;
  s.render_asks.push_back({seq, s.armed_path, start, end, music, save, label.substr(0, 120)});
  NOTICE_LOG_FMT(NETPLAY, "RoomWatch: render {} asked for, frames {} to {} ({})", seq, start, end,
                 music ? "music" : "no music");
  return true;
}

void ShowRenderFolder(const std::string& id)
{
  std::lock_guard lk(s_mutex);
  ++s.show_seq;
  s.show_id = id.substr(0, 128);
}

std::string ClipPath()
{
  std::lock_guard pk(s_clip_path_mutex);
  return s_clip_path;
}

void Files(const std::string& online_dir)
{
  struct Write
  {
    std::string path;
    std::vector<u8> bytes;
    bool create = false;
  };
  std::vector<Write> writes;
  std::string clips_text;
  bool first = false;
  {
    std::lock_guard lk(s_mutex);
    if (s.dir != online_dir)
    {
      first = s.dir.empty();
      s.dir = online_dir;
      s.run = fmt::format("{}", std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
    }
    for (auto& k : s.kept)
    {
      for (const Game& g : s.assembler.Games())
      {
        if (g.uid == k.uid)
          TakeBytes(k, g);
      }
      if (k.pending.empty())
        continue;
      writes.push_back({k.path, std::move(k.pending), !k.created});
      k.pending.clear();
      k.created = true;
    }
    if (!s.saved_clips.empty() || !s.render_asks.empty() || s.show_seq > 0)
      clips_text = picojson::value(ClipsJsonLocked()).serialize();
  }
  // The last run's kept games and clips: the launcher has its copies of them by now.
  if (first)
  {
    File::DeleteDirRecursively(online_dir + KEPT_FOLDER);
    File::Delete(online_dir + CLIPS_FILE, File::IfAbsentBehavior::NoConsoleWarning);
    s_clips_written.clear();
    s_overlay_read.clear();
  }
  // The games first: a clip the launcher reads is of a file that is there.
  for (const Write& w : writes)
  {
    File::CreateFullPath(w.path);
    File::IOFile f(w.path, w.create ? "wb" : "ab");
    if (!f.IsOpen() || !f.WriteBytes(w.bytes.data(), w.bytes.size()))
      WARN_LOG_FMT(NETPLAY, "RoomWatch: cannot write the kept game {}", w.path);
  }
  if (!clips_text.empty() && clips_text != s_clips_written)
  {
    const std::string path = online_dir + CLIPS_FILE;
    const std::string tmp = path + ".tmp";
    if (File::WriteStringToFile(tmp, clips_text) && File::Rename(tmp, path))
      s_clips_written = clips_text;
    else
      WARN_LOG_FMT(NETPLAY, "RoomWatch: cannot write {}", path);
  }
  // The launcher's side of the seek bar.
  std::string overlay;
  const std::string overlay_path = online_dir + OVERLAY_FILE;
  if (File::Exists(overlay_path) && File::GetSize(overlay_path) <= MAX_OVERLAY_SIZE)
    File::ReadFileToString(overlay_path, overlay);
  if (overlay != s_overlay_read)
  {
    s_overlay_read = overlay;
    Gprb::Replay::Playback::SetRoomWatchOverlay(overlay);
  }
}
}  // namespace Online::RoomWatch
