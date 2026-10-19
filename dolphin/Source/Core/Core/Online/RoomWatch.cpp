// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/RoomWatch.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/PeerData.h"
#include "Core/Rollback/ReplayFile.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Online::RoomWatch
{
namespace
{
namespace Replay = Gprb::Replay;
namespace Session = Gprb::Session;

constexpr size_t MAX_JSON = 1024;
// A game joined further in than this catches up to the live edge first (as spectating's mirror
// does, Rollback/ReplayPlayback.cpp); one watched from about its start plays from its start.
constexpr size_t JOIN_CATCH_UP_FRAMES = 20 * 60;
// The game never left the room CSS for an armed watch (an older plugin, a menu kept open): this
// game is not watched.
constexpr Clock::duration LEAVE_CSS_TIMEOUT = std::chrono::seconds(10);
constexpr Clock::duration TOAST_EVERY = std::chrono::seconds(1);
constexpr u32 TOAST_MS = 1500;
constexpr char TOAST_TEXT[] = "Game ended, press B to return to CSS.";
// gfPadButtons (Rollback/GameplaySession.cpp HOLD_BUTTONS): START, B.
constexpr u16 BUTTON_START = 0x1000;
constexpr u16 BUTTON_B = 0x0200;

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
  bool armed_catch_up = false;
  Clock::time_point armed_at{};
  size_t fed_size = 0;
  bool fed_done = false;
  bool left_css = false;
  bool pad_released = false;
  bool leaving = false;
  Clock::time_point next_toast{};
  std::string last_error;
  // For the harness.
  u32 watched = 0;      // games whose match started here
  u32 finished = 0;     // ... and ended by themselves (not left early)
  u32 left_early = 0;
  u32 toasts = 0;
  u32 bad_packets = 0;
  picojson::array results;  // each watched game's end (PlaybackStatus)
};

std::mutex s_mutex;
State s;
std::atomic<bool> s_abort{false};
std::atomic<bool> s_watching{false};

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

// Any controller's START or B (the local controllers' newest sample, never the replay's inputs).
bool LeavePressed(bool* released)
{
  const Gprb::PadSlots latest = Gprb::PadsLatestRaw();
  bool down = false;
  for (u32 port = 0; port < 4; ++port)
  {
    const u8* pad = latest.data() + port * Gprb::Addr::PAD_STRIDE;
    if (pad[0x38] != 0)  // gfPadError: no controller
      continue;
    const u16 buttons = static_cast<u16>(pad[2] << 8 | pad[3]);
    down = down || (buttons & (BUTTON_START | BUTTON_B)) != 0;
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
    r["catch_up"] = picojson::value(s.armed_catch_up);
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
    if (g && !s.leaving && (g->bytes.size() != s.fed_size || (g->Done() && !s.fed_done)))
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
    else if (!s.leaving && in_match && LeavePressed(&s.pad_released))
      LeaveEarly(&out, room, "START or B");
    // The players' game is over while it still plays here.
    const bool players_done = !room.room_in_game || s.assembler.Games().size() > 1;
    if (!s.leaving && pv.in_match && players_done && now >= s.next_toast)
    {
      OSD::AddTypedMessage(OSD::MessageType::RoomWatch, TOAST_TEXT, TOAST_MS, OSD::Color::CYAN);
      s.next_toast = now + TOAST_EVERY;
      ++s.toasts;
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
  if (!f || f->setup.empty() || f->pads.empty())
    return out;
  const bool catch_up = f->pads.size() > JOIN_CATCH_UP_FRAMES && f->endg.empty();
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
  s.armed_catch_up = catch_up;
  s.armed_at = now;
  s.fed_size = g.bytes.size();
  s.fed_done = g.Done();
  s.left_css = false;
  s.pad_released = false;
  s.leaving = false;
  s.next_toast = {};
  ++s.watched;
  s_watching = true;
  INFO_LOG_FMT(NETPLAY, "RoomWatch: watching the room's game {} ({} frames so far{})", g.number,
               frames, catch_up ? ", catching up" : "");
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
  o["bad_packets"] = picojson::value(static_cast<double>(s.bad_packets));
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
  return o;
}
}  // namespace Online::RoomWatch
