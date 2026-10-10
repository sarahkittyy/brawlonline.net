// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/Rooms.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

#include <enet/enet.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Online/Matchmaking.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/User.h"
#include "Core/Rollback/GameplaySession.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Online::Rooms
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int MM_CHANNELS = 3;
constexpr auto TICK_MS = 10;
constexpr auto CONNECT_TIMEOUT = 5s;     // ENet connect to mm
constexpr auto HELLO_TIMEOUT = 5s;       // hello-resp
constexpr auto BACKOFF_MAX = 30s;
constexpr auto STABLE_ONLINE = 60s;      // online this long: the backoff starts over
constexpr auto PENDING_TIMEOUT = 20s;    // a create/join waiting for the connection or answer
constexpr auto ERROR_SHOWN = 5s;         // an error's time on the line while in a room
constexpr auto AWAY_LEAVE = 5s;          // the game away from the room CSS: leave the room
constexpr auto FILE_POLL = 500ms;
constexpr auto STATUS_EVERY = 5s;
constexpr auto READY_RETRY = 1s;         // a room-ready not answered yet
constexpr auto READY_REFUSED = 3s;
constexpr auto MATCH_SCENE_GRACE = 5s;   // after a match, with no screen reported by the game
constexpr auto START_GUARD = 90s;        // room-start .. session connected
constexpr auto GAME_GUARD = 30min;       // a room game that never ends
constexpr s64 REQUEST_MAX_AGE_MS = 3 * 60 * 1000;
constexpr size_t MAX_REQUEST_FILE = 4096;

constexpr char TEXT_BUSY[] = "Finish your current game first.";
constexpr char TEXT_NOT_MENUS[] = "Go back to the menus first.";
constexpr char TEXT_LOGIN[] = "Log in in the launcher.";
constexpr char TEXT_NO_SERVER[] = "Can't reach the server.";
constexpr char TEXT_LOST[] = "Lost the connection to the server.";
constexpr char TEXT_PLAYERS_LOST[] = "Lost the connection to the other players.";
constexpr char TEXT_NOT_FOUND[] = "Room not found.";

enum class Conn
{
  Down,
  Connecting,
  Hello,
  Online,
};

const char* ConnName(Conn c)
{
  switch (c)
  {
  case Conn::Down:
    return "down";
  case Conn::Connecting:
    return "connecting";
  case Conn::Hello:
    return "hello";
  case Conn::Online:
    return "online";
  }
  return "?";
}

struct RoomGame
{
  bool active = false;
  std::string code;
  std::string match_id;
  int host = 0;
  bool teams = false;
  Clock::time_point started{};
  bool saw_session = false;
  bool saw_match = false;
  std::optional<Clock::time_point> match_ended;
  bool abort = false;  // the player left the room: end the room's game (no room-back)
};

struct Launch
{
  std::string id;
  std::string code;
};

struct State
{
  // The online connection (the thread's ENet host; these are its published state).
  Conn conn = Conn::Down;
  std::string conn_error;
  bool conn_hold = false;  // "Signed in from another game.": wait for the game to ask again
  int attempts = 0;
  Clock::time_point next_attempt{};
  Clock::time_point online_since{};
  std::string conn_uid;
  u64 connects = 0, hellos = 0, drops = 0, sent = 0, received = 0, invalid = 0;

  // The room.
  Phase phase = Phase::None;
  std::optional<View> view;
  std::optional<Request> pending;  // a create or join not answered yet
  bool pending_sent = false;
  Clock::time_point pending_since{};
  std::string pending_launch;      // the launcher request id that join answers
  std::string rejoin;              // the room the connection dropped out of
  std::deque<Request> outbox;

  // The line.
  std::string error;
  bool error_timed = false;
  Clock::time_point error_until{};
  u32 serial = 1;
  std::string shown;  // text + error of the last snapshot

  // The room's games.
  u32 game = 1;
  u8 pickers = 0;
  RoomGame rg;
  u64 games_started = 0, games_done = 0, games_failed = 0;
  std::string last_game_end;

  // The game.
  u8 screen = 0;
  std::optional<Clock::time_point> away_since;
  bool lock_ready = false;
  u32 lock_game = 0;
  u8 lock_char = 0xFF, lock_costume = 0;
  bool harness_lock = false;
  Clock::time_point next_ready{};

  // The launcher.
  std::optional<Launch> launch;
  u8 join_seq = 0;
  picojson::value last_request;  // null or {id, result, message}
  std::string status_written;
  Clock::time_point status_at{};
  u64 requests_seen = 0;

  std::deque<std::string> log;
};

std::mutex s_mutex;
State s;
std::thread s_thread;
std::atomic<bool> s_run{false};
std::mutex s_start_mutex;

void Log(std::string line)
{
  // under s_mutex
  NOTICE_LOG_FMT(NETPLAY, "Rooms: {}", line);
  s.log.push_back(std::move(line));
  while (s.log.size() > 40)
    s.log.pop_front();
}

void SetError(std::string text)
{
  // under s_mutex
  s.error = std::move(text);
  s.error_timed = s.phase == Phase::In;
  s.error_until = Clock::now() + ERROR_SHOWN;
  ++s.serial;
}

void ClearRoom()
{
  // under s_mutex
  s.view.reset();
  s.phase = s.pending ? Phase::Joining : Phase::None;
  s.away_since.reset();
  ++s.serial;
}

void AnswerLaunch(const std::string& id, const char* result, const std::string& message)
{
  // under s_mutex
  picojson::object o;
  o["id"] = picojson::value(id);
  o["result"] = picojson::value(result);
  if (!message.empty())
    o["message"] = picojson::value(message);
  s.last_request = picojson::value(o);
  Log(fmt::format("launcher request {}: {}{}", id, result, message.empty() ? "" : " (" + message + ")"));
}

// ---- Time ----

s64 UnixMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
s64 DaysFromCivil(s64 y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const s64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<s64>(doe) - 719468;
}

// "2026-10-09T15:00:00.000Z" (the launcher's toISOString) -> ms since the epoch.
std::optional<s64> ParseIsoUtc(const std::string& t)
{
  int y, mo, d, h, mi;
  double sec;
  char z = 0;
  if (t.size() > 40 || std::sscanf(t.c_str(), "%4d-%2d-%2dT%2d:%2d:%lf%c", &y, &mo, &d, &h, &mi,
                                   &sec, &z) != 7 ||
      z != 'Z' || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || !(sec >= 0) ||
      !(sec < 61) || y < 2000 || y > 9999)
  {
    return std::nullopt;
  }
  const s64 days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return ((days * 24 + h) * 60 + mi) * 60000 + static_cast<s64>(sec * 1000);
}

std::string IsoUtcNow()
{
  const s64 ms = UnixMs();
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
}

u32 Pid()
{
#ifdef _WIN32
  return static_cast<u32>(GetCurrentProcessId());
#else
  return static_cast<u32>(getpid());
#endif
}

// ---- The line ----

std::pair<std::string, bool> Line(Clock::time_point now)
{
  // under s_mutex
  // An error stays until the next request; in a room, only for ERROR_SHOWN.
  if (!s.error.empty() && (!(s.error_timed && s.phase == Phase::In) || now < s.error_until))
    return {s.error, true};
  if (s.phase == Phase::Joining && s.pending)
  {
    const bool up = s.conn == Conn::Online && s.pending_sent;
    if (s.pending->op == Op::Create)
      return {up ? "Creating a room" : "Connecting to the server", false};
    return {(up ? "Joining room " : "Connecting to room ") + s.pending->code, false};
  }
  if (s.phase == Phase::In && s.view)
    return {s.view->status_text, false};
  return {std::string(), false};
}

// ---- The connection ----

void Send(ENetPeer* peer, const std::string& json)
{
  ENetPacket* packet =
      enet_packet_create(json.data(), json.size(), ENET_PACKET_FLAG_RELIABLE);
  if (enet_peer_send(peer, 0, packet) != 0)
    enet_packet_destroy(packet);
}

// What a message asks the thread to do outside the lock.
struct Actions
{
  bool start_game = false;
  RoomGame start;
  u8 pickers = 0;
  bool abort_game = false;  // the server cancelled the start
  bool drop = false;        // fatal error: close the connection
};

void Handle(const Message& m, Actions* a, Clock::time_point now)
{
  // under s_mutex
  ++s.received;
  switch (m.kind)
  {
  case Message::Kind::HelloResp:
    if (!m.error.empty())
    {
      Log(fmt::format("hello refused: {}", m.error));
      s.conn_error = m.error;
      if (!m.latest_version.empty())
        Client::GetUser().OverwriteLatestVersion(m.latest_version);
      if (s.pending)
      {
        s.pending.reset();
        s.phase = s.view ? Phase::In : Phase::None;
        SetError(m.error);
      }
      a->drop = true;
      return;
    }
    s.conn = Conn::Online;
    s.online_since = now;
    s.conn_error.clear();
    ++s.serial;
    Log("online");
    // Back into the room the connection dropped out of (once).
    if (!s.rejoin.empty() && !s.pending && s.phase == Phase::None)
    {
      Request r;
      r.op = Op::Join;
      r.code = s.rejoin;
      s.pending = r;
      s.pending_sent = false;
      s.pending_since = now;
      s.phase = Phase::Joining;
      Log(fmt::format("joining {} again after the connection dropped", s.rejoin));
    }
    s.rejoin.clear();
    return;
  case Message::Kind::RoomState:
  {
    const View& v = *m.view;
    const bool new_room = !s.view || s.view->code != v.code;
    s.view = v;
    s.phase = Phase::In;
    if (s.pending)
    {
      if (!s.pending_launch.empty() && s.pending->op == Op::Join && s.pending->code != v.code)
        Log(fmt::format("joined {} (asked for {})", v.code, s.pending->code));
      s.pending.reset();
      s.pending_launch.clear();
      s.error.clear();
    }
    if (new_room)
    {
      // A new room: its games start over (no stage pick carried from another room).
      s.pickers = 0;
      s.away_since.reset();
      Log(fmt::format("in room {} (slot {}, host {})", v.code, v.you, v.host));
    }
    // The server's ready is the truth: a send is due only if it differs from the game's.
    s.next_ready = std::min(s.next_ready, now + 100ms);
    ++s.serial;
    return;
  }
  case Message::Kind::RoomError:
    Log(fmt::format("room-error {}: {}", m.op, m.error));
    if (m.op == "room-create" || m.op == "room-join")
    {
      if (s.pending)
      {
        if (!s.pending_launch.empty())
          AnswerLaunch(s.pending_launch, "failed", m.error);
        s.pending.reset();
        s.pending_launch.clear();
      }
      s.phase = s.view ? Phase::In : Phase::None;
    }
    if (m.op == "room-ready")
      s.next_ready = now + READY_REFUSED;
    if (m.op == "room-start" && s.rg.active && !s.rg.saw_match)
      a->abort_game = true;
    SetError(m.error);
    return;
  case Message::Kind::RoomLeft:
    Log(fmt::format("room-left {}{}", m.code, m.reason.empty() ? "" : ": " + m.reason));
    if (s.view && (m.code.empty() || m.code == s.view->code))
    {
      ClearRoom();
      if (!m.reason.empty())
        SetError(m.reason);
    }
    return;
  case Message::Kind::RoomStart:
    Log(fmt::format("room-start {} {}", m.code, m.match_id));
    if (!s.view || s.view->code != m.code || s.phase != Phase::In)
    {
      Log("room-start for another room: ignored");
      return;
    }
    if (s.rg.active)
    {
      Log("room-start while a room game runs: ignored");
      return;
    }
    s.rg = RoomGame{};
    s.rg.active = true;
    s.rg.code = m.code;
    s.rg.match_id = m.match_id;
    s.rg.host = s.view->host;
    s.rg.teams = s.view->teams;
    s.rg.started = now;
    ++s.games_started;
    a->start_game = true;
    a->start = s.rg;
    a->pickers = s.pickers;
    ++s.serial;
    return;
  case Message::Kind::Error:
    Log(fmt::format("error: {}", m.error));
    s.conn_error = m.error;
    if (m.error == "Signed in from another game.")
      s.conn_hold = true;
    SetError(m.error.empty() ? std::string(TEXT_LOST) : m.error);
    a->drop = true;
    return;
  case Message::Kind::CreateTicketResp:
    Log(fmt::format("create-ticket-resp on the online connection: {}", m.error));
    return;
  case Message::Kind::Invalid:
    ++s.invalid;
    Log(fmt::format("invalid {} from mm: {}", m.type.empty() ? "message" : m.type, m.why));
    return;
  case Message::Kind::Unknown:
    return;
  }
}

// The connection dropped (or was closed): the server took us out of any room.
void OnDropped(Clock::time_point now, const std::string& why, bool count)
{
  // under s_mutex
  if (s.conn == Conn::Online && now - s.online_since > STABLE_ONLINE)
    s.attempts = 0;
  const bool was_online = s.conn == Conn::Online;
  s.conn = Conn::Down;
  if (count)
    ++s.drops;
  if (!why.empty())
    s.conn_error = why;
  const int shift = std::min(s.attempts, 5);
  auto delay = std::chrono::duration_cast<Clock::duration>(1s * (1 << shift));
  if (delay > BACKOFF_MAX)
    delay = BACKOFF_MAX;
  static std::mt19937 rng{std::random_device{}()};
  delay += std::chrono::milliseconds(rng() % 1000);
  s.next_attempt = now + delay;
  ++s.attempts;
  if (s.view)
  {
    s.rejoin = s.view->code;
    ClearRoom();
    if (was_online && s.error.empty())
      SetError(TEXT_LOST);
  }
  if (s.pending)
    s.pending_sent = false;
  s.outbox.clear();
  ++s.serial;
  Log(fmt::format("connection down ({}); next try in {} ms", why,
                  std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()));
}

// ---- The launcher hand-off ----

std::string OnlineDir()
{
  return File::GetUserPath(D_ONLINE_IDX);
}

// join-room.json: read it, delete it, keep the request if it is valid and fresh.
void PollJoinRequest()
{
  const std::string path = OnlineDir() + "join-room.json";
  if (!File::Exists(path))
    return;
  std::string text;
  const u64 size = File::GetSize(path);
  const bool read = size <= MAX_REQUEST_FILE && File::ReadFileToString(path, text);
  File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
  std::lock_guard lk(s_mutex);
  ++s.requests_seen;
  if (!read)
  {
    Log("join-room.json unreadable or too large: ignored");
    return;
  }
  picojson::value v;
  if (!picojson::parse(v, text).empty() || !v.is<picojson::object>())
  {
    Log("join-room.json is not a JSON object: ignored");
    return;
  }
  const auto& o = v.get<picojson::object>();
  const auto get = [&](const char* k) -> const picojson::value* {
    const auto it = o.find(k);
    return it == o.end() ? nullptr : &it->second;
  };
  const picojson::value* version = get("version");
  const picojson::value* id = get("id");
  const picojson::value* code = get("code");
  const picojson::value* created = get("createdAt");
  if (!version || !version->is<double>() || version->get<double>() != 1 || !id ||
      !id->is<std::string>() || id->get<std::string>().empty() ||
      id->get<std::string>().size() > 100 || !code || !code->is<std::string>() || !created ||
      !created->is<std::string>())
  {
    Log("join-room.json: not version 1 with an id, code and createdAt: ignored");
    return;
  }
  const auto room = NormalizeRoomCode(code->get<std::string>());
  if (!room)
  {
    Log("join-room.json: not a room code: ignored");
    return;
  }
  const auto at = ParseIsoUtc(created->get<std::string>());
  const s64 age = at ? UnixMs() - *at : REQUEST_MAX_AGE_MS + 1;
  if (age > REQUEST_MAX_AGE_MS)
  {
    Log(fmt::format("join-room.json for {} is {} s old: ignored", *room, age / 1000));
    return;
  }
  s.launch = Launch{CleanText(id->get<std::string>(), 100), *room};
  Log(fmt::format("launcher asks to join {} (request {})", *room, s.launch->id));
}

bool BusyScreen(u8 screen)
{
  return screen >= static_cast<u8>(Screen::OnlineBusy);
}

// Dolphin's own view: a search, a session or a room game make the game busy.
struct DolphinBusy
{
  bool searching = false;
  bool in_match = false;
  bool session = false;
};

DolphinBusy QueryBusy()
{
  DolphinBusy b;
  const Client::MatchState ms = Client::GetMatchState();
  b.searching = ms.state == Matchmaking::State::Initializing ||
                ms.state == Matchmaking::State::Matchmaking ||
                ms.state == Matchmaking::State::OpponentConnecting;
  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  b.session = lobby.active || (ms.state == Matchmaking::State::ConnectionSuccess);
  b.in_match = lobby.in_match;
  return b;
}

void DecideLaunch(const DolphinBusy& busy, bool logged_in, Clock::time_point now)
{
  // under s_mutex
  if (!s.launch)
    return;
  const Launch l = *s.launch;
  const Screen screen = static_cast<Screen>(s.screen);
  const bool dolphin_busy = busy.searching || busy.session || busy.in_match || s.rg.active;
  if (dolphin_busy || BusyScreen(s.screen))
  {
    s.launch.reset();
    AnswerLaunch(l.id, "refused",
                 screen == Screen::Other && !dolphin_busy ? TEXT_NOT_MENUS : TEXT_BUSY);
    return;
  }
  // At start-up: kept until the game reaches its menus (and the user is read).
  if (screen == Screen::Unknown || !logged_in)
    return;
  s.launch.reset();
  AnswerLaunch(l.id, "accepted", "");
  Request r;
  r.op = Op::Join;
  r.code = l.code;
  s.error.clear();
  s.pending = r;
  s.pending_sent = false;
  s.pending_since = now;
  s.pending_launch = l.id;
  s.conn_hold = false;
  if (s.phase != Phase::In)
    s.phase = Phase::Joining;
  ++s.join_seq;
  s.away_since.reset();
  ++s.serial;
}

void WriteGameStatus(const DolphinBusy& busy, Clock::time_point now, bool force)
{
  picojson::object o;
  std::string content_key;
  {
    std::lock_guard lk(s_mutex);
    const Screen screen = static_cast<Screen>(s.screen);
    const char* name = "other";
    switch (screen)
    {
    case Screen::Menus:
      name = "menus";
      break;
    case Screen::OnlineCss:
      name = "online-css";
      break;
    case Screen::Room:
      name = "room";
      break;
    case Screen::OnlineBusy:
      name = "searching";
      break;
    case Screen::Match:
      name = "match";
      break;
    case Screen::Offline:
      name = "single-player";
      break;
    default:
      break;
    }
    bool is_busy = screen == Screen::Unknown || BusyScreen(s.screen);
    if (busy.in_match)
      name = "match", is_busy = true;
    else if (busy.searching || s.rg.active)
      name = "searching", is_busy = true;
    else if (busy.session)
      is_busy = true;
    o["version"] = picojson::value(1.0);
    o["state"] = picojson::value(is_busy ? "busy" : "idle");
    o["screen"] = picojson::value(name);
    o["room"] = s.phase == Phase::In && s.view ? picojson::value(s.view->code) : picojson::value();
    o["pid"] = picojson::value(static_cast<double>(Pid()));
    o["lastRequest"] = s.last_request;
    content_key = picojson::value(o).serialize();
    if (!force && content_key == s.status_written && now - s.status_at < STATUS_EVERY)
      return;
    s.status_written = content_key;
    s.status_at = now;
  }
  o["updatedAt"] = picojson::value(IsoUtcNow());
  const std::string dir = OnlineDir();
  File::CreateFullPath(dir);
  const std::string tmp = dir + "game-status.json.tmp";
  const std::string path = dir + "game-status.json";
  if (!File::WriteStringToFile(tmp, picojson::value(o).serialize(true)) || !File::Rename(tmp, path))
    WARN_LOG_FMT(NETPLAY, "Rooms: cannot write {}", path);
}

// ---- A room's game ----

void StartGame(const RoomGame& g, u8 pickers)
{
  Client::SearchOptions o;
  o.settings.mode = Matchmaking::Mode::Teams;
  o.settings.connect_code = g.code;
  o.hand_off = true;
  o.room_host_port = g.host;
  o.room_teams = g.teams;
  o.room_pickers = pickers;
  NOTICE_LOG_FMT(NETPLAY, "Rooms: room {} starts: ticket in mode 3, host P{}, teams {}, pickers {:#x}",
                 g.code, g.host, g.teams, pickers);
  auto error = Client::FindMatch(o);
  if (error)
  {
    // A search or connection left over (Direct): the room's game replaces it.
    Client::Cleanup();
    error = Client::FindMatch(o);
  }
  if (error)
  {
    std::lock_guard lk(s_mutex);
    Log(fmt::format("cannot start the room's game: {}", *error));
  }
}

// The room's game is over (played, or failed): the session ends, mm hears room-back.
void EndGame(bool played, const std::string& error, std::optional<u8> pickers, bool back)
{
  Client::Cleanup();
  std::lock_guard lk(s_mutex);
  if (!s.rg.active)
    return;
  s.rg = RoomGame{};
  ++s.game;
  if ((s.game & 0xFF) == 0)  // the game's lock-in carries the number in a byte; 0 is none
    ++s.game;
  if (pickers)
    s.pickers = *pickers & 0x0F;
  if (played)
    ++s.games_done;
  else
    ++s.games_failed;
  if (back)
  {
    Request r;
    r.op = Op::Back;
    s.outbox.push_back(r);
  }
  if (!error.empty())
    SetError(error);
  s.last_game_end = played ? "played" : error;
  ++s.serial;
  Log(fmt::format("room game {} ({}); next game {}, pickers {:#x}", played ? "over" : "failed",
                  error.empty() ? "ok" : error, s.game, s.pickers));
}

void SuperviseGame(Clock::time_point now)
{
  RoomGame g;
  u8 screen;
  {
    std::lock_guard lk(s_mutex);
    if (!s.rg.active)
      return;
    g = s.rg;
    screen = s.screen;
  }
  if (g.abort)
  {
    EndGame(false, "", std::nullopt, false);
    return;
  }
  const Client::MatchState ms = Client::GetMatchState();
  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  if (!g.saw_session && ms.state == Matchmaking::State::ErrorEncountered)
  {
    EndGame(false, ms.error.empty() ? std::string("Could not connect to players.") : ms.error,
            std::nullopt, true);
    return;
  }
  if (!g.saw_session && ms.handoff == "failed")
  {
    EndGame(false, "Could not start the game.", std::nullopt, true);
    return;
  }
  if (lobby.active)
    g.saw_session = true;
  if (lobby.in_match)
    g.saw_match = true;
  // The match is over once the game has left its scene (the game shows DISCONNECTED or DESYNC
  // DETECTED from LOCAL, which the cleanup would clear).
  const bool match_over =
      g.saw_match && !lobby.in_match && !(lobby.active && lobby.after_match);
  if (match_over && !g.match_ended)
    g.match_ended = now;
  const bool scene_left =
      screen == static_cast<u8>(Screen::Unknown) ?
          (g.match_ended && now - *g.match_ended > MATCH_SCENE_GRACE) :
          screen != static_cast<u8>(Screen::Match);
  {
    std::lock_guard lk(s_mutex);
    if (s.rg.active && s.rg.match_id == g.match_id)
    {
      s.rg.saw_session = g.saw_session;
      s.rg.saw_match = g.saw_match;
      s.rg.match_ended = g.match_ended;
    }
  }
  if (match_over && scene_left)
  {
    EndGame(true, "", lobby.active ? std::optional<u8>(lobby.stage_pickers) : std::nullopt, true);
    return;
  }
  if (g.saw_session && !g.saw_match && !lobby.active)
  {
    EndGame(false, TEXT_PLAYERS_LOST, std::nullopt, true);
    return;
  }
  if ((!g.saw_session && now - g.started > START_GUARD) || now - g.started > GAME_GUARD)
    EndGame(false, "Could not connect to players.", std::nullopt, true);
}

// ---- The thread ----

void Thread()
{
  Common::SetCurrentThreadName("Rooms");
  if (enet_initialize() != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "Rooms: enet_initialize failed");
    return;
  }
  ENetHost* host = nullptr;
  ENetPeer* peer = nullptr;
  Clock::time_point conn_started{};
  Clock::time_point next_file{};
  bool was_logged_in = false;

  const auto close = [&](bool polite) {
    if (!host)
      return;
    if (peer && polite)
    {
      enet_peer_disconnect(peer, 0);
      ENetEvent ev;
      const auto until = Clock::now() + 300ms;
      while (Clock::now() < until && enet_host_service(host, &ev, 20) >= 0)
      {
        if (ev.type == ENET_EVENT_TYPE_RECEIVE)
          enet_packet_destroy(ev.packet);
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
          break;
      }
    }
    enet_host_destroy(host);
    host = nullptr;
    peer = nullptr;
  };

  while (s_run)
  {
    const auto now = Clock::now();
    User& user = Client::GetUser();
    const UserInfo info = user.GetUserInfo();
    const bool logged_in = user.IsLoggedIn() && !info.uid.empty() && !info.play_key.empty();

    // Logged out (or another account): close; the room is gone with the connection.
    bool want = false;
    {
      std::lock_guard lk(s_mutex);
      if (host && (!logged_in || info.uid != s.conn_uid))
      {
        OnDropped(now, logged_in ? "another account" : "logged out", false);
        s.rejoin.clear();
        s.attempts = 0;
        s.next_attempt = now;
        if (!logged_in && s.rg.active)
          Log("logged out during a room game");
        close(true);
      }
      if (!logged_in && was_logged_in)
      {
        s.pending.reset();
        s.pending_launch.clear();
        ClearRoom();
      }
      want = logged_in && !s.conn_hold;
      // A create/join that waited too long for the connection: tell the player why.
      if (s.pending && now - s.pending_since > PENDING_TIMEOUT)
      {
        const std::string why = !logged_in              ? TEXT_LOGIN :
                                !s.conn_error.empty() && s.conn != Conn::Online ? s.conn_error :
                                                          std::string(TEXT_NO_SERVER);
        if (!s.pending_launch.empty())
          AnswerLaunch(s.pending_launch, "failed", why);
        s.pending.reset();
        s.pending_launch.clear();
        s.phase = s.view ? Phase::In : Phase::None;
        SetError(why);
      }
    }
    was_logged_in = logged_in;

    // Connect.
    if (want && !host)
    {
      bool due;
      {
        std::lock_guard lk(s_mutex);
        due = now >= s.next_attempt;
      }
      if (due)
      {
        ENetAddress addr{};
        const std::string mm_host = Config::GetMatchmakingHost();
        host = enet_host_create(nullptr, 1, MM_CHANNELS, 0, 0);
        if (host)
        {
          host->maximumPacketSize = 64 * 1024;
          host->maximumWaitingData = 256 * 1024;
        }
        std::lock_guard lk(s_mutex);
        if (!host || enet_address_set_host(&addr, mm_host.c_str()) != 0)
        {
          close(false);
          OnDropped(now, TEXT_NO_SERVER, false);
        }
        else
        {
          addr.port = static_cast<enet_uint16>(Config::GetMatchmakingPort());
          peer = enet_host_connect(host, &addr, MM_CHANNELS, 0);
          if (!peer)
          {
            close(false);
            OnDropped(now, TEXT_NO_SERVER, false);
          }
          else
          {
            enet_peer_timeout(peer, 32, 5000, 10000);
            s.conn = Conn::Connecting;
            s.conn_uid = info.uid;
            ++s.connects;
            conn_started = now;
            Log(fmt::format("connecting to {}:{}", mm_host, Config::GetMatchmakingPort()));
          }
        }
      }
    }

    // Network.
    if (host)
    {
      ENetEvent ev;
      int n;
      Actions actions;
      while (host && (n = enet_host_service(host, &ev, 0)) > 0)
      {
        std::lock_guard lk(s_mutex);
        if (ev.type == ENET_EVENT_TYPE_CONNECT)
        {
          Send(peer, HelloJson(info.uid, info.play_key, APP_VERSION));
          s.conn = Conn::Hello;
          ++s.hellos;
          conn_started = now;
        }
        else if (ev.type == ENET_EVENT_TYPE_RECEIVE)
        {
          const std::string text(reinterpret_cast<const char*>(ev.packet->data),
                                 ev.packet->dataLength);
          enet_packet_destroy(ev.packet);
          Handle(Parse(text), &actions, now);
          if (actions.drop)
            break;
        }
        else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
        {
          close(false);
          OnDropped(now, s.conn_error.empty() ? TEXT_LOST : s.conn_error, true);
        }
      }
      if (actions.drop && host)
      {
        close(true);
        std::lock_guard lk(s_mutex);
        OnDropped(now, s.conn_error, true);
      }
      if (actions.start_game)
        StartGame(actions.start, actions.pickers);
      if (actions.abort_game)
        EndGame(false, "", std::nullopt, false);

      std::lock_guard lk(s_mutex);
      if (host && (s.conn == Conn::Connecting || s.conn == Conn::Hello) &&
          now - conn_started > (s.conn == Conn::Connecting ? CONNECT_TIMEOUT : HELLO_TIMEOUT))
      {
        close(false);
        OnDropped(now, TEXT_NO_SERVER, false);
      }
      if (host && s.conn == Conn::Online)
      {
        // A create or join waiting for the connection goes first; then the queued ops.
        if (s.pending && !s.pending_sent)
        {
          if (const auto json = RequestJson(*s.pending))
          {
            Send(peer, *json);
            ++s.sent;
            Log(fmt::format("sent {} {}", OpName(s.pending->op), s.pending->code));
          }
          s.pending_sent = true;
          s.pending_since = now;
          ++s.serial;
        }
        while (!s.outbox.empty())
        {
          const Request r = s.outbox.front();
          s.outbox.pop_front();
          if (const auto json = RequestJson(r))
          {
            Send(peer, *json);
            ++s.sent;
            Log(fmt::format("sent {}", *json));
          }
        }
        // The lock-in -> room-ready, when it differs from what the server shows.
        if (s.phase == Phase::In && s.view && !s.rg.active && now >= s.next_ready)
        {
          const View& v = *s.view;
          const auto& me = v.slots[v.you - 1].player;
          const bool may = v.status == Status::Waiting ||
                           (v.status == Status::InGame && me && !me->in_game);
          const bool want_ready =
              s.lock_ready && s.lock_game == (s.game & 0xFF) && s.lock_char != 0xFF;
          if (may && me &&
              (want_ready != me->ready ||
               (want_ready && (me->character != s.lock_char || me->costume != s.lock_costume))))
          {
            Request r;
            r.op = Op::Ready;
            r.flag = want_ready;
            r.character = s.lock_char;
            r.costume = s.lock_costume;
            if (const auto json = RequestJson(r))
            {
              Send(peer, *json);
              ++s.sent;
              Log(fmt::format("sent {}", *json));
            }
            s.next_ready = now + READY_RETRY;
          }
        }
        // The game left the room CSS without leaving the room: leave for it.
        if (s.phase == Phase::In && s.view && !s.rg.active && s.view->status == Status::Waiting &&
            s.away_since && now - *s.away_since > AWAY_LEAVE)
        {
          Request r;
          r.op = Op::Leave;
          Send(peer, *RequestJson(r));
          ++s.sent;
          s.away_since.reset();
          Log("the game is away from the room CSS: left the room");
        }
      }
    }

    SuperviseGame(now);

    // Files.
    if (now >= next_file)
    {
      next_file = now + FILE_POLL;
      PollJoinRequest();
      const DolphinBusy busy = QueryBusy();
      {
        std::lock_guard lk(s_mutex);
        DecideLaunch(busy, logged_in, now);
      }
      WriteGameStatus(busy, now, false);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
  }

  // Exit: closing the connection takes us out of the room (mm drops a member whose connection
  // is gone).
  close(true);
  enet_deinitialize();
}
}  // namespace

void Start()
{
  std::lock_guard lk(s_start_mutex);
  if (s_run)
    return;
  s_run = true;
  s_thread = std::thread(Thread);
}

void Shutdown()
{
  {
    std::lock_guard lk(s_start_mutex);
    if (!s_run)
      return;
    s_run = false;
    if (s_thread.joinable())
      s_thread.join();
  }
  File::Delete(OnlineDir() + "game-status.json", File::IfAbsentBehavior::NoConsoleWarning);
}

void Submit(const Request& request)
{
  std::lock_guard lk(s_mutex);
  const auto now = Clock::now();
  s.error.clear();
  ++s.serial;
  switch (request.op)
  {
  case Op::Poll:
    return;
  case Op::Create:
  case Op::Join:
  {
    if (request.op == Op::Join && !NormalizeRoomCode(request.code))
    {
      Log(fmt::format("join '{}': not a room code", request.code));
      SetError(TEXT_NOT_FOUND);
      return;
    }
    Request r = request;
    if (r.op == Op::Join)
      r.code = *NormalizeRoomCode(request.code);
    s.pending = r;
    s.pending_sent = false;
    s.pending_since = now;
    s.pending_launch.clear();
    s.conn_hold = false;  // the player asks: try again even after "Signed in from another game."
    s.rejoin.clear();
    if (s.phase != Phase::In)
      s.phase = Phase::Joining;
    Log(fmt::format("game asks to {} {}", OpName(r.op), r.code));
    return;
  }
  case Op::Leave:
    if (s.rg.active)
      s.rg.abort = true;
    s.pending.reset();
    s.pending_launch.clear();
    s.rejoin.clear();
    if (s.view)
      s.outbox.push_back(request);
    ClearRoom();
    Log("game leaves the room");
    return;
  default:
    if (!s.view || s.conn != Conn::Online)
    {
      SetError("You are not in a room.");
      return;
    }
    s.outbox.push_back(request);
    return;
  }
}

void SetScreen(u8 screen)
{
  std::lock_guard lk(s_mutex);
  const u8 v = screen <= static_cast<u8>(Screen::Other) ? screen : static_cast<u8>(Screen::Other);
  if (v != s.screen)
  {
    s.screen = v;
    const bool away = v == static_cast<u8>(Screen::Menus) ||
                      v == static_cast<u8>(Screen::OnlineCss) ||
                      v == static_cast<u8>(Screen::OnlineBusy) ||
                      v == static_cast<u8>(Screen::Offline);
    if (away)
      s.away_since = Clock::now();
    else
      s.away_since.reset();
  }
}

void SetLocalLock(bool ready, u32 game, u8 char_kind, u8 costume)
{
  std::lock_guard lk(s_mutex);
  if (s.harness_lock)
    return;
  s.lock_ready = ready;
  s.lock_game = game;
  s.lock_char = char_kind;
  s.lock_costume = costume;
}

Snapshot GetSnapshot()
{
  std::lock_guard lk(s_mutex);
  Snapshot snap;
  snap.phase = s.phase;
  snap.view = s.view;
  snap.game = s.game;
  snap.pickers = s.pickers;
  snap.join_seq = s.join_seq;
  snap.game_active = s.rg.active;
  auto [text, error] = Line(Clock::now());
  const std::string shown = text + (error ? "\x01" : "\x02");
  if (shown != s.shown)
  {
    s.shown = shown;
    ++s.serial;
  }
  snap.text = std::move(text);
  snap.error = error;
  snap.serial = s.serial;
  return snap;
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  const auto now = Clock::now();
  picojson::object o;
  o["running"] = picojson::value(s_run.load());
  o["connection"] = picojson::value(ConnName(s.conn));
  o["connection_error"] = picojson::value(s.conn_error);
  o["connection_hold"] = picojson::value(s.conn_hold);
  o["attempts"] = picojson::value(static_cast<double>(s.attempts));
  o["connects"] = picojson::value(static_cast<double>(s.connects));
  o["hellos"] = picojson::value(static_cast<double>(s.hellos));
  o["drops"] = picojson::value(static_cast<double>(s.drops));
  o["sent"] = picojson::value(static_cast<double>(s.sent));
  o["received"] = picojson::value(static_cast<double>(s.received));
  o["invalid"] = picojson::value(static_cast<double>(s.invalid));
  o["phase"] = picojson::value(static_cast<double>(s.phase));
  o["pending"] = s.pending ? picojson::value(fmt::format("{} {}", OpName(s.pending->op),
                                                         s.pending->code)) :
                             picojson::value();
  const auto [text, error] = Line(now);
  o["text"] = picojson::value(text);
  o["error"] = picojson::value(error);
  o["serial"] = picojson::value(static_cast<double>(s.serial));
  o["game"] = picojson::value(static_cast<double>(s.game));
  o["pickers"] = picojson::value(static_cast<double>(s.pickers));
  o["join_seq"] = picojson::value(static_cast<double>(s.join_seq));
  o["screen"] = picojson::value(static_cast<double>(s.screen));
  picojson::object lock;
  lock["ready"] = picojson::value(s.lock_ready);
  lock["game"] = picojson::value(static_cast<double>(s.lock_game));
  lock["char_kind"] = picojson::value(static_cast<double>(s.lock_char));
  lock["costume"] = picojson::value(static_cast<double>(s.lock_costume));
  lock["harness"] = picojson::value(s.harness_lock);
  o["lock"] = picojson::value(lock);
  picojson::object rg;
  rg["active"] = picojson::value(s.rg.active);
  rg["code"] = picojson::value(s.rg.code);
  rg["match_id"] = picojson::value(s.rg.match_id);
  rg["host"] = picojson::value(static_cast<double>(s.rg.host));
  rg["teams"] = picojson::value(s.rg.teams);
  rg["saw_session"] = picojson::value(s.rg.saw_session);
  rg["saw_match"] = picojson::value(s.rg.saw_match);
  rg["started"] = picojson::value(static_cast<double>(s.games_started));
  rg["done"] = picojson::value(static_cast<double>(s.games_done));
  rg["failed"] = picojson::value(static_cast<double>(s.games_failed));
  rg["last_end"] = picojson::value(s.last_game_end);
  o["room_game"] = picojson::value(rg);
  if (s.view)
  {
    const View& v = *s.view;
    picojson::object vo;
    vo["code"] = picojson::value(v.code);
    vo["public"] = picojson::value(v.is_public);
    vo["teams"] = picojson::value(v.teams);
    vo["mode"] = picojson::value(ModeName(v.mode));
    vo["status"] = picojson::value(StatusName(v.status));
    vo["you"] = picojson::value(static_cast<double>(v.you));
    vo["host"] = picojson::value(static_cast<double>(v.host));
    vo["status_text"] = picojson::value(v.status_text);
    picojson::array slots;
    for (const Slot& sl : v.slots)
    {
      picojson::object so;
      so["open"] = picojson::value(sl.open);
      so["host"] = picojson::value(sl.host);
      if (sl.player)
      {
        picojson::object p;
        p["name"] = picojson::value(sl.player->name);
        p["code"] = picojson::value(sl.player->code);
        p["ready"] = picojson::value(sl.player->ready);
        p["team"] = picojson::value(static_cast<double>(sl.player->team));
        p["character"] = sl.player->character ?
                             picojson::value(static_cast<double>(*sl.player->character)) :
                             picojson::value();
        p["costume"] = sl.player->costume ?
                           picojson::value(static_cast<double>(*sl.player->costume)) :
                           picojson::value();
        p["in_game"] = picojson::value(sl.player->in_game);
        so["player"] = picojson::value(p);
      }
      else
      {
        so["player"] = picojson::value();
      }
      slots.emplace_back(so);
    }
    vo["slots"] = picojson::value(slots);
    o["view"] = picojson::value(vo);
  }
  else
  {
    o["view"] = picojson::value();
  }
  o["launch_pending"] = s.launch ? picojson::value(s.launch->code) : picojson::value();
  o["last_request"] = s.last_request;
  o["requests_seen"] = picojson::value(static_cast<double>(s.requests_seen));
  picojson::array log;
  for (const auto& l : s.log)
    log.emplace_back(l);
  o["log"] = picojson::value(log);
  return o;
}

std::optional<std::string> HarnessRequest(const picojson::object& args)
{
  const auto str = [&](const char* k) -> std::string {
    const auto it = args.find(k);
    return it != args.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "";
  };
  const auto num = [&](const char* k, double def) -> double {
    const auto it = args.find(k);
    return it != args.end() && it->second.is<double>() ? it->second.get<double>() : def;
  };
  const auto flag = [&](const char* k, bool def) -> bool {
    const auto it = args.find(k);
    return it != args.end() && it->second.is<bool>() ? it->second.get<bool>() : def;
  };
  const std::string op = str("op");
  Request r;
  if (op == "create")
  {
    r.op = Op::Create;
    r.flag = flag("public", true);
  }
  else if (op == "join")
  {
    r.op = Op::Join;
    r.code = str("code");
  }
  else if (op == "leave")
  {
    r.op = Op::Leave;
  }
  else if (op == "slot")
  {
    r.op = Op::Slot;
    r.slot = static_cast<int>(num("slot", 0));
    r.flag = flag("open", true);
    if (r.slot < 1 || r.slot > ROOM_SLOTS)
      return "slot must be 1-4";
  }
  else if (op == "teams")
  {
    r.op = Op::Teams;
    r.flag = flag("on", true);
  }
  else if (op == "public")
  {
    r.op = Op::Public;
    r.flag = flag("public", true);
  }
  else if (op == "team")
  {
    r.op = Op::Team;
    const double t = num("team", -1);
    if (t < 0 || t > 2)
      return "team must be 0-2";
    r.team = static_cast<u8>(t);
  }
  else if (op == "ready")
  {
    std::lock_guard lk(s_mutex);
    s.harness_lock = true;
    s.lock_ready = flag("ready", true);
    s.lock_game = s.game & 0xFF;
    s.lock_char = static_cast<u8>(num("character", 0));
    s.lock_costume = static_cast<u8>(num("costume", 0));
    return std::nullopt;
  }
  else if (op == "release_lock")
  {
    std::lock_guard lk(s_mutex);
    s.harness_lock = false;
    return std::nullopt;
  }
  else if (op == "screen")
  {
    SetScreen(static_cast<u8>(num("screen", 0)));
    return std::nullopt;
  }
  else
  {
    return "op must be create, join, leave, slot, teams, public, team, ready, release_lock or "
           "screen";
  }
  Submit(r);
  return std::nullopt;
}
}  // namespace Online::Rooms
