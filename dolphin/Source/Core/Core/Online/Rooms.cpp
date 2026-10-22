// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/Rooms.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <utility>

#include <enet/enet.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Core.h"
#include "Core/Online/Chat.h"
#include "Core/Online/GameAlive.h"
#include "Core/Online/GameSetup.h"
#include "Core/Online/Matchmaking.h"
#include "Core/Online/ModsRoom.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/RoomWatch.h"
#include "Core/Online/SpeedCheck.h"
#include "Core/Online/Spectate.h"
#include "Core/Online/User.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/MachineSession.h"
#include "VideoCommon/OnScreenDisplay.h"

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
constexpr auto CONNECT_ERROR_SHOWN = 15s;  // ... one that names who could not connect
constexpr auto AWAY_LEAVE = 5s;          // the game away from the room CSS: leave the room
// ... on the online stage select or a loading screen, where a room's set game leaves the game
// when its session ends (the game goes back to the room CSS from there).
constexpr auto AWAY_LEAVE_BUSY = 30s;
constexpr auto FILE_POLL = 500ms;
constexpr auto STATUS_EVERY = 5s;
constexpr auto READY_RETRY = 1s;         // a room-ready not answered yet
constexpr auto READY_REFUSED = 3s;
// A coin placed on the CSS goes to the others once it has stayed put this long (a player sweeping
// the coin across the grid sends one pick, not one per portrait).
constexpr auto PICK_SETTLE = 250ms;
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
  bool team_colors = false;  // View::team_colors
  Clock::time_point started{};
  bool saw_session = false;
  bool saw_match = false;
  std::optional<Clock::time_point> match_ended;
  // Who picks the next stage, read when the match ended (the other players' cleanup may end
  // this session before this game leaves the match scene).
  std::optional<u8> pickers;
  bool abort = false;  // the player left the room: end the room's game (no room-back)
  bool set = false;    // the session plays a set: it goes on after a game until the set is won
  u32 games = 0;       // the matches it played
};

enum class LaunchKind
{
  Room,    // kind absent or "room": join the room `code`
  Direct,  // kind "direct": a Direct invite, `code` is the other player's connect code
  Create,  // kind "create": a new room, as WITH FRIENDS > Create Room (no code)
  Queue,   // kind "queue": the Unranked or Ranked CSS, `code` is "unranked" or "ranked"
};

const char* LaunchKindName(LaunchKind k)
{
  return k == LaunchKind::Direct ? "direct" :
         k == LaunchKind::Create ? "create" :
         k == LaunchKind::Queue  ? "queue" :
                                   "room";
}

struct Launch
{
  std::string id;
  LaunchKind kind = LaunchKind::Room;
  std::string code;       // the room code (Room), the connect code (Direct, canonical), "" (Create),
                          // "unranked" or "ranked" (Queue)
  std::string ruleset;    // Create: the launcher box's ruleset code (normalised), "" none
  s64 created_ms = 0;     // its createdAt
  bool answered = false;  // "accepted" already (held during the game's boot)
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
  // The connection's mm takes `game-alive` (hello-resp): an older one answers it with a
  // room-error, which would be shown.
  bool mm_game_alive = false;
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

  // The room's games: `game` is the next one's number, counted from 1 in the room `game_room`.
  u32 game = 1;
  std::string game_room;
  u8 pickers = 0;
  RoomGame rg;
  u64 games_started = 0, games_done = 0, games_failed = 0;
  std::string last_game_end;
  std::string last_match_id;  // the last room game's, once it ended
  // The pairs of players (match id and slots) named already: each is shown once.
  std::set<std::string> unreachable_shown;

  // The game.
  u8 screen = 0;
  // The game has reported a menu screen (menus, an idle online CSS, the room CSS) since it booted.
  // Before that (the boot, the title, the boot redirect) nothing it reports means "busy".
  bool reached_menus = false;
  std::optional<Clock::time_point> away_since;
  bool lock_ready = false;
  u32 lock_game = 0;
  u8 lock_char = 0xFF, lock_costume = 0, lock_stocks = 0;
  Clock::time_point pick_changed{};  // the lock-in's character or costume last changed
  bool harness_lock = false;
  bool harness_screen = false;  // tests: the harness's screen, not the game's
  Clock::time_point next_ready{};

  // The launcher.
  std::optional<Launch> launch;
  u8 join_seq = 0;
  u8 direct_seq = 0;        // bumped on an accepted Direct invite (LOCAL.directJoin)
  std::string direct_code;  // its connect code (LOCAL.directPrefix / directNumber)
  // Or the launcher's Play unranked / Play ranked (join-room.json kind "queue"), with direct_seq
  // and no code: 1 Unranked, 2 Ranked, 0 a Direct invite (LOCAL.directNumber, zero prefix).
  u8 direct_queue = 0;
  u8 create_seq = 0;        // bumped on an accepted launcher create (LOCAL.roomCreate)
  // A launcher create from a room: room-leave goes out right before the pending create (the
  // outbox would send it after, and leave the new room).
  bool leave_before_pending = false;
  std::string create_left;  // the room a launcher create left: its late pushes are not the answer
  // The Ruleset line's list (room-rulesets): what mm last sent, and when it was asked for.
  ListState list_state = ListState::None;
  u32 list_serial = 0;
  std::vector<RulesetEntry> list;
  std::optional<Clock::time_point> list_asked;
  picojson::value last_request;  // null or {id, result, message}
  std::string delay_request;     // the id of the last input-delay.json taken
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
std::once_flag s_stop_hook_once;
Common::EventHook s_stop_hook;

void Log(std::string line)
{
  // under s_mutex
  NOTICE_LOG_FMT(NETPLAY, "Rooms: {}", line);
  s.log.push_back(std::move(line));
  while (s.log.size() > 40)
    s.log.pop_front();
}

bool AwayScreen(u8 screen)
{
  return screen == static_cast<u8>(Screen::Menus) || screen == static_cast<u8>(Screen::OnlineCss) ||
         screen == static_cast<u8>(Screen::OnlineBusy) || screen == static_cast<u8>(Screen::Offline);
}

// The game away from the room CSS (menus, another CSS, offline) counts from now.
void RestartAway()
{
  // under s_mutex
  if (AwayScreen(s.screen))
    s.away_since = Clock::now();
  else
    s.away_since.reset();
}

void SetError(std::string text, std::chrono::seconds shown = ERROR_SHOWN)
{
  // under s_mutex
  s.error = std::move(text);
  s.error_timed = s.phase == Phase::In;
  s.error_until = Clock::now() + shown;
  ++s.serial;
}

void ClearRoom()
{
  // under s_mutex
  s.view.reset();
  s.phase = s.pending ? Phase::Joining : Phase::None;
  RestartAway();
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

// Reliable bytes to mm not acknowledged yet: on the wire (reliableDataInTransit) or still queued
// in ENet (its outgoing lists), which the spectate uploader keeps small.
size_t UnackedBytes(ENetPeer* peer)
{
  size_t n = peer->reliableDataInTransit;
  for (ENetList* list : {&peer->outgoingCommands, &peer->outgoingSendReliableCommands})
  {
    for (ENetListIterator it = enet_list_begin(list); it != enet_list_end(list);
         it = enet_list_next(it))
    {
      const auto* c = reinterpret_cast<const ENetOutgoingCommand*>(it);
      if (c->packet)
        n += c->fragmentLength ? c->fragmentLength : c->packet->dataLength;
    }
  }
  return n;
}

// A spectate data packet (docs/spectate-protocol.md §1): reliable, on channel 1.
constexpr enet_uint8 SPECTATE_CHANNEL = 1;
void SendData(ENetPeer* peer, const std::vector<u8>& data)
{
  ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
  if (enet_peer_send(peer, SPECTATE_CHANNEL, packet) != 0)
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
  std::string fail_game;    // the room's game cannot start: end it with this text (room-back)
  std::vector<std::string> notices;  // the room chat's system lines (added outside the lock)
};

// The CSS line's name for slot 1-4 of the room (its player's, else "P<slot>").
std::string SlotName(int slot)
{
  // under s_mutex
  if (s.view && slot >= 1 && slot <= ROOM_SLOTS && s.view->slots[slot - 1].player &&
      !s.view->slots[slot - 1].player->name.empty())
  {
    return s.view->slots[slot - 1].player->name;
  }
  return fmt::format("P{}", slot);
}

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
    s.mm_game_alive = m.game_alive;
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
    if (s.pending && s.pending->op == Op::Create && !s.create_left.empty() && v.code == s.create_left)
    {
      Log(fmt::format("room-state for {} (left for the launcher's create): ignored", v.code));
      return;
    }
    s.create_left.clear();
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
      // A new room: its games start over (no stage pick carried from another room). The count
      // too, unless it is the same room again (back after the connection dropped): it went on
      // from the last room's, so a player who came from another room numbered the room's games
      // differently from the others (prod 2026-10-14, room TLSZ: 4 against 1), and the number is
      // in the game's id (replays, spectating).
      s.pickers = 0;
      if (v.code != s.game_room)
      {
        s.game = 1;
        s.game_room = v.code;
      }
      RestartAway();
      Log(fmt::format("in room {} (slot {}, host {})", v.code, v.you, v.host));
    }
    // The server's ready is the truth: a send is due only if it differs from the game's.
    s.next_ready = std::min(s.next_ready, now + 100ms);
    ++s.serial;
    return;
  }
  case Message::Kind::RulesetList:
    s.list = m.rulesets;
    s.list_state = ListState::Ready;
    ++s.list_serial;
    Log(fmt::format("ruleset list: {} rulesets", s.list.size()));
    return;
  case Message::Kind::RoomError:
    Log(fmt::format("room-error {}: {}", m.op, m.error));
    if (m.op == "room-rulesets")
    {
      s.list_state = ListState::Failed;
      ++s.list_serial;
      return;
    }
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
    // Sitting this game out (SIT OUT): no ticket; the room's line says it plays. (mm sends
    // room-start only to the players; an older mm names none, and everyone plays.)
    if (!m.start_players.empty() &&
        std::find(m.start_players.begin(), m.start_players.end(), s.view->you) ==
            m.start_players.end())
    {
      Log("room-start: sitting this game out");
      return;
    }
    s.rg = RoomGame{};
    s.rg.active = true;
    s.rg.code = m.code;
    s.rg.match_id = m.match_id;
    // The game's host is one of its players: the room's host, or another when they sit out.
    s.rg.host = m.start_host ? m.start_host : s.view->host;
    s.rg.teams = s.view->teams;
    s.rg.team_colors = s.view->team_colors;
    s.rg.started = now;
    ++s.games_started;
    a->start_game = true;
    a->start = s.rg;
    // The server's pickers (the loser, or a player drawn when the loser sits out), else this
    // machine's own (an older mm).
    a->pickers = m.start_pickers ? *m.start_pickers :
                 s.view && !s.view->ruleset.LoserPicks() ? 0 :
                                                           s.pickers;
    ++s.serial;
    return;
  case Message::Kind::RoomUnreachable:
  {
    // Players of the room's game that could not connect to each other (one of them told the
    // server). Every member sees who, on the line and in the room's chat, once per pair: the
    // others' game cannot start either, and should not look broken.
    std::string first;
    for (const auto& pair : m.pairs)
    {
      if (!s.unreachable_shown.insert(fmt::format("{} {} {}", m.match_id, pair[0], pair[1])).second)
        continue;
      const std::string a_name = SlotName(pair[0]), b_name = SlotName(pair[1]);
      Log(fmt::format("room-unreachable {}: P{} ({}) and P{} ({})", m.match_id, pair[0], a_name,
                      pair[1], b_name));
      a->notices.push_back(fmt::format("{} and {} couldn't connect to each other. A VPN or "
                                       "firewall can block it.",
                                       a_name, b_name));
      if (first.empty())
        first = fmt::format("No connection between {} and {}.", a_name, b_name);
    }
    if (first.empty())
      return;
    if (s.rg.active && s.rg.match_id == m.match_id && !s.rg.saw_match)
    {
      if (a->fail_game.empty())  // (several messages in one pass: the first pair named)
        a->fail_game = first;
    }
    else if (!s.rg.active && s.last_match_id == m.match_id)
      SetError(first, CONNECT_ERROR_SHOWN);
    return;
  }
  case Message::Kind::Error:
    Log(fmt::format("error: {}", m.error));
    s.conn_error = m.error;
    if (m.error == "Signed in from another game.")
    {
      s.conn_hold = true;
      ModsRoom::OnSignedInElsewhere();
    }
    else if (m.error == "This account is banned.")
    {
      // mm's BANNED (`pp-admin user ban`): mm already took this account out of its room, queue
      // and chat. The game closes, as when its window closes (a Ranked set is abandoned, the
      // match's session and a mod room are left on the way), and the connection is not tried
      // again: the play key is no longer valid, and the launcher logs the account out.
      s.conn_hold = true;
      OSD::AddMessage("This account is banned.", 10000, OSD::Color::RED);
      Core::QueueHostJob([](Core::System& system) { Core::Stop(system); }, false);
    }
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
  s.leave_before_pending = false;  // the server took us out of the room with the connection
  s.outbox.clear();
  Chat::OnConnectionLost();
  Spectate::OnConnectionLost();
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
  const picojson::value* kind = get("kind");
  const picojson::value* created = get("createdAt");
  if (!version || !version->is<double>() || version->get<double>() != 1 || !id ||
      !id->is<std::string>() || id->get<std::string>().empty() ||
      id->get<std::string>().size() > 100 || !created || !created->is<std::string>())
  {
    Log("join-room.json: not version 1 with an id and createdAt: ignored");
    return;
  }
  // "kind": absent or "room" is a room to join ("code"); "direct" a Direct invite
  // ("connectCode", the other player's connect code); "create" a new room (no code); "queue"
  // the Unranked or Ranked CSS ("queue": "unranked" or "ranked"). docs/rooms-protocol.md §5.
  if (kind && (!kind->is<std::string>() ||
               (kind->get<std::string>() != "room" && kind->get<std::string>() != "direct" &&
                kind->get<std::string>() != "create" && kind->get<std::string>() != "queue")))
  {
    Log("join-room.json: kind is not \"room\", \"direct\", \"create\" or \"queue\": ignored");
    return;
  }
  LaunchKind launch_kind = LaunchKind::Room;
  if (kind && kind->get<std::string>() == "direct")
    launch_kind = LaunchKind::Direct;
  else if (kind && kind->get<std::string>() == "create")
    launch_kind = LaunchKind::Create;
  else if (kind && kind->get<std::string>() == "queue")
    launch_kind = LaunchKind::Queue;
  std::optional<std::string> target;
  std::string ruleset;
  if (launch_kind == LaunchKind::Create)
  {
    target = std::string();
    // The launcher's ruleset box (docs/rooms-game-interface.md): the room plays it.
    if (const picojson::value* rs = get("ruleset"); rs && !rs->is<picojson::null>())
    {
      const auto code = rs->is<std::string>() ? NormalizeRulesetCode(rs->get<std::string>()) :
                                                std::nullopt;
      if (!code)
      {
        Log("join-room.json: a create with a ruleset that is not a ruleset code: ignored");
        return;
      }
      ruleset = *code;
    }
  }
  else if (launch_kind == LaunchKind::Queue)
  {
    const picojson::value* q = get("queue");
    if (q && q->is<std::string>() &&
        (q->get<std::string>() == "unranked" || q->get<std::string>() == "ranked"))
      target = q->get<std::string>();
    if (!target)
    {
      Log("join-room.json: a queue request without \"unranked\" or \"ranked\": ignored");
      return;
    }
  }
  else if (launch_kind == LaunchKind::Direct)
  {
    const picojson::value* cc = get("connectCode");
    if (cc && cc->is<std::string>())
      target = NormalizeConnectCode(cc->get<std::string>());
    if (!target)
    {
      Log("join-room.json: a direct request without a valid connectCode: ignored");
      return;
    }
  }
  else
  {
    const picojson::value* code = get("code");
    if (!code || !code->is<std::string>())
    {
      Log("join-room.json: a room request without a code: ignored");
      return;
    }
    target = NormalizeRoomCode(code->get<std::string>());
    if (!target)
    {
      Log("join-room.json: not a room code: ignored");
      return;
    }
  }
  const auto at = ParseIsoUtc(created->get<std::string>());
  const s64 age = at ? UnixMs() - *at : REQUEST_MAX_AGE_MS + 1;
  if (age > REQUEST_MAX_AGE_MS)
  {
    Log(fmt::format("join-room.json ({} {}) is {} s old: ignored", LaunchKindName(launch_kind),
                    *target, age / 1000));
    return;
  }
  s.launch = Launch{CleanText(id->get<std::string>(), 100), launch_kind, *target, ruleset, *at};
  const char* what = launch_kind == LaunchKind::Direct ? "play Direct with " :
                     launch_kind == LaunchKind::Create ? "create a room" :
                     launch_kind == LaunchKind::Queue  ? "go to the CSS of " :
                                                         "join ";
  Log(fmt::format("launcher asks to {}{} (request {})", what, *target, s.launch->id));
}

// input-delay.json (docs/rooms-protocol.md §5): the player changed the launcher's Input Delay
// while this Dolphin runs. The setting is Dolphin's from now on (Dolphin.ini, which the launcher
// writes too: Dolphin's own save of its settings keeps it), and a running game takes it from its
// next frame, a P+ game or an Other Games room's session alike. Read and deleted.
void PollInputDelayRequest()
{
  const std::string path = OnlineDir() + "input-delay.json";
  if (!File::Exists(path))
    return;
  std::string text;
  const bool read = File::GetSize(path) <= MAX_REQUEST_FILE && File::ReadFileToString(path, text);
  File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
  std::unique_lock lk(s_mutex);
  picojson::value v;
  if (!read || !picojson::parse(v, text).empty() || !v.is<picojson::object>())
  {
    Log("input-delay.json unreadable or not a JSON object: ignored");
    return;
  }
  const auto& o = v.get<picojson::object>();
  const auto get = [&](const char* k) -> const picojson::value* {
    const auto it = o.find(k);
    return it == o.end() ? nullptr : &it->second;
  };
  const picojson::value* version = get("version");
  const picojson::value* id = get("id");
  const picojson::value* delay = get("inputDelay");
  if (!version || !version->is<double>() || version->get<double>() != 1 || !id ||
      !id->is<std::string>() || id->get<std::string>().empty() ||
      id->get<std::string>().size() > 100 || !delay || !delay->is<double>() ||
      delay->get<double>() != static_cast<int>(delay->get<double>()) ||
      delay->get<double>() < 0 || delay->get<double>() > Gprb::Session::MAX_INPUT_DELAY)
  {
    Log("input-delay.json: not version 1 with an id and an inputDelay of 0-9: ignored");
    return;
  }
  const int setting = static_cast<int>(delay->get<double>());
  s.delay_request = CleanText(id->get<std::string>(), 100);
  Log(setting == 0 ? fmt::format("launcher: input delay automatic (request {})", s.delay_request) :
                     fmt::format("launcher: input delay {} (request {})", setting, s.delay_request));
  // Outside s_mutex: the config's change callbacks run here.
  lk.unlock();
  Config::SetBaseOrCurrent(Config::ONLINE_INPUT_DELAY, setting);
  Gprb::Session::SetInputDelay(setting);
  Machine::SetInputDelay(setting == 0 ? Machine::AUTO_DELAY : setting);
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

void DecideLaunch(const DolphinBusy& busy, bool logged_in, const std::string& own_code,
                  Clock::time_point now)
{
  // under s_mutex
  if (!s.launch)
    return;
  const Launch l = *s.launch;
  const Screen screen = static_cast<Screen>(s.screen);
  const bool dolphin_busy = busy.searching || busy.session || busy.in_match || s.rg.active;
  // The game is still booting (docs/rooms-protocol.md §5 step 2, a click that launched the game,
  // or one during its boot): the request is accepted at once and kept until the game first shows
  // its menus, at most REQUEST_MAX_AGE_MS from its createdAt. What the game reports before that
  // (the boot's scenes) is not "busy".
  if (!s.reached_menus && !dolphin_busy)
  {
    if (UnixMs() - l.created_ms > REQUEST_MAX_AGE_MS)
    {
      s.launch.reset();
      AnswerLaunch(l.id, "failed", "The game did not reach its menus in time.");
    }
    else if (!l.answered)
    {
      s.launch->answered = true;
      AnswerLaunch(l.id, "accepted", "");
    }
    return;
  }
  // Watching the room's game is not busy (the user's, 2026-10-19): the watch ends first, and the
  // click goes ahead once the game is back on the room CSS.
  if (RoomWatch::Watching())
  {
    RoomWatch::Abort();
    if (!l.answered)
    {
      s.launch->answered = true;
      AnswerLaunch(l.id, "accepted", "");
    }
    return;
  }
  if (dolphin_busy || BusyScreen(s.screen))
  {
    s.launch.reset();
    AnswerLaunch(l.id, "refused",
                 screen == Screen::Other && !dolphin_busy ? TEXT_NOT_MENUS : TEXT_BUSY);
    return;
  }
  // Kept until the user is read (user.json at start-up).
  if (screen == Screen::Unknown || !logged_in)
    return;
  s.launch.reset();
  if (l.kind == LaunchKind::Direct || l.kind == LaunchKind::Queue)
  {
    if (l.kind == LaunchKind::Direct && !own_code.empty() &&
        NormalizeConnectCode(own_code) == l.code)
    {
      AnswerLaunch(l.id, l.answered ? "failed" : "refused", "That's your own code.");
      return;
    }
    if (!l.answered)
      AnswerLaunch(l.id, "accepted", "");
    // A Direct invite: out of any room first (as a room join from a room), then the game goes to
    // the Direct CSS with the code and searches it on lock-in (LOCAL.directJoin). Play unranked /
    // Play ranked the same way, to that mode's CSS (no code; LOCAL.directNumber says the mode).
    s.error.clear();
    s.rejoin.clear();
    if (s.view || s.pending)
    {
      s.pending.reset();
      s.pending_launch.clear();
      if (s.view)
      {
        Request leave;
        leave.op = Op::Leave;
        s.outbox.push_back(leave);
      }
      ClearRoom();
      Log(l.kind == LaunchKind::Queue ? "left the room for the launcher's Play" :
                                        "left the room for the Direct invite");
    }
    if (l.kind == LaunchKind::Queue)
    {
      s.direct_code.clear();
      s.direct_queue = l.code == "ranked" ? 2 : 1;
      ++s.direct_seq;
      Log(fmt::format("launcher Play: the game goes to the {} CSS (direct {})", l.code,
                      s.direct_seq));
      ++s.serial;
      return;
    }
    s.direct_code = l.code;
    s.direct_queue = 0;
    ++s.direct_seq;
    Log(fmt::format("Direct invite: the game plays {} (direct {})", l.code, s.direct_seq));
    ++s.serial;
    return;
  }
  if (!l.answered)
    AnswerLaunch(l.id, "accepted", "");
  if (l.kind == LaunchKind::Create)
  {
    // As WITH FRIENDS > Create Room (public, the in-game default): out of any room first, then
    // room-create; the game goes to the new room's CSS (LOCAL.roomCreate) and does not send a
    // create of its own. mm's refusal ("Too many rooms. Wait a moment.", "No rooms free. Try
    // later.") answers the request `failed` (Handle, RoomError).
    Request r;
    r.op = Op::Create;
    r.flag = true;
    r.ruleset = l.ruleset;
    s.error.clear();
    s.rejoin.clear();
    s.leave_before_pending = s.view.has_value() && s.conn == Conn::Online;
    s.create_left = s.view ? s.view->code : std::string();
    if (s.view)
      Log(fmt::format("leaving {} for the launcher's create", s.view->code));
    s.pending = r;
    s.pending_sent = false;
    s.pending_since = now;
    s.pending_launch = l.id;
    s.conn_hold = false;
    ClearRoom();  // phase Joining (the pending create)
    ++s.create_seq;
    RestartAway();
    ++s.serial;
    return;
  }
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
  RestartAway();
  ++s.serial;
}

void WriteGameStatus(const DolphinBusy& busy, Clock::time_point now, bool force)
{
  picojson::object o;
  std::string content_key;
  const int delay_setting = Config::Get(Config::ONLINE_INPUT_DELAY);
  const int machine_delay = Machine::CurrentInputDelay();
  const int current = machine_delay >= 0 ? machine_delay : Gprb::Session::CurrentInputDelay();
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
    // Booting (no menu seen yet): idle for the launcher, whose click is then held (DecideLaunch).
    bool is_busy = s.reached_menus && (screen == Screen::Unknown || BusyScreen(s.screen));
    if (!s.reached_menus)
      name = "other";
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
    // The Input Delay setting this Dolphin plays with (0: automatic), the running game's delay
    // (null: none runs), and the last input-delay.json taken.
    picojson::object delay;
    delay["setting"] = picojson::value(static_cast<double>(delay_setting));
    delay["current"] = current >= 0 ? picojson::value(static_cast<double>(current)) : picojson::value();
    delay["request"] = s.delay_request.empty() ? picojson::value() : picojson::value(s.delay_request);
    o["inputDelay"] = picojson::value(delay);
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
  o.room_team_colors = g.teams && g.team_colors;
  o.room_pickers = pickers;
  NOTICE_LOG_FMT(NETPLAY,
                 "Rooms: room {} starts: ticket in mode 3, host P{}, teams {}{}, pickers {:#x}",
                 g.code, g.host, g.teams, g.teams && g.team_colors ? " (team colours)" : "", pickers);
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
void EndGame(bool played, const std::string& error, std::optional<u8> pickers, bool back,
             std::chrono::seconds shown = ERROR_SHOWN)
{
  Client::Cleanup();
  std::lock_guard lk(s_mutex);
  if (!s.rg.active)
    return;
  // A set's session played several games: the room's game numbers go on after all of them.
  const u32 games = std::max<u32>(1, s.rg.games);
  s.last_match_id = s.rg.match_id;
  s.rg = RoomGame{};
  // The time away from the room CSS counts from the game's end (a set's session can end with
  // the game on the stage select).
  RestartAway();
  s.game += games;
  if ((s.game & 0xFF) == 0)  // the game's lock-in carries the number in a byte; 0 is none
    ++s.game;
  if (pickers)
    s.pickers = *pickers & 0x0F;
  // A ruleset that strikes, draws or plays a set leaves no stage to the last game's loser.
  if (s.view && !s.view->ruleset.LoserPicks())
    s.pickers = 0;
  if (played)
    ++s.games_done;
  else
    ++s.games_failed;
  if (back)
  {
    Request r;
    r.op = Op::Back;
    // Who picks the next stage as this game saw it end: mm carries it to the next game's
    // players, who may not have played this one.
    r.pickers = s.pickers;
    s.outbox.push_back(r);
  }
  if (!error.empty())
    SetError(error, shown);
  s.last_game_end = played ? "played" : error;
  ++s.serial;
  Log(fmt::format("room game {} ({}); next game {}, pickers {:#x}", played ? "over" : "failed",
                  error.empty() ? "ok" : error, s.game, s.pickers));
}

void ReportSetSteps(const GameSetup::StepReport& step)
{
  Request r;
  r.op = Op::Step;
  r.game = step.set_game;
  r.actions = step.actions;
  r.flag = step.match_started;
  std::lock_guard lk(s_mutex);
  if (!s.view || !RequestJson(r))
    return;
  s.outbox.push_back(r);
  Log(fmt::format("set game {} steps: {}+{}{}", step.set_game, step.actions[0].size(),
                  step.actions[1].size(), step.match_started ? ", match started" : ""));
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
    // The players this one could not reach: the server names each pair to the whole room.
    // (PPR_TEST_ROOM_NO_UNREACHABLE: tests of the others' own timeout.)
    if (ms.match && ms.match->match_id == g.match_id && !ms.match->unreachable_ports.empty() &&
        !std::getenv("PPR_TEST_ROOM_NO_UNREACHABLE"))
    {
      Request r;
      r.op = Op::Unreachable;
      r.match_id = g.match_id;
      r.ports = ms.match->unreachable_ports;
      std::lock_guard lk(s_mutex);
      if (s.view && RequestJson(r))
      {
        s.outbox.push_back(r);
        Log(fmt::format("could not connect to slot(s) {}", fmt::join(r.ports, ", ")));
      }
    }
    EndGame(false, ms.error.empty() ? std::string("Could not connect to players.") : ms.error,
            std::nullopt, true, CONNECT_ERROR_SHOWN);
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
  // A set's steps so far, kept by the server for a session that starts again (room-rules.md A).
  if (g.set)
  {
    if (const auto step = GameSetup::TakeStepReport())
      ReportSetSteps(*step);
  }
  // The host ended the set (RULES > SET > End set) between its games: the set is gone from the
  // room's state. The session ends with it and everyone is back on the room's CSS (never in a
  // match: the server refuses End set once the game's match started).
  bool set_ended = false;
  {
    std::lock_guard lk(s_mutex);
    set_ended = g.set && s.view && !s.view->set && s.rg.active && s.rg.match_id == g.match_id;
  }
  if (set_ended && !lobby.in_match)
  {
    {
      std::lock_guard lk(s_mutex);
      Log("the host ended the set: the session ends");
    }
    EndGame(true, "The host ended the set.", std::nullopt, true);
    return;
  }
  // The match is over once the game has left its scene (the game shows DISCONNECTED or DESYNC
  // DETECTED from LOCAL, which the cleanup would clear).
  const bool match_over =
      g.saw_match && !lobby.in_match && !(lobby.active && lobby.after_match);
  if (match_over && !g.match_ended)
    g.match_ended = now;
  if (g.saw_match && !lobby.in_match && lobby.active && !g.pickers)
    g.pickers = lobby.stage_pickers;
  // This player quit the match (the pause screen's quit, 3-4 players) and left before the game
  // ended: it is not placed, so it never picks the next stage, and the others' result (which this
  // machine does not see) decides who does.
  if (lobby.quit_left && !g.pickers)
    g.pickers = 0;
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
      s.rg.pickers = g.pickers;
    }
  }
  if (match_over && scene_left && g.set && !GameSetup::SetDone() && lobby.active)
  {
    // A set's game is over and the set is not: the session goes on to the next game (its steps,
    // then the match), watched like the first.
    std::lock_guard lk(s_mutex);
    if (s.rg.active && s.rg.match_id == g.match_id)
    {
      ++s.rg.games;
      s.rg.saw_match = false;
      s.rg.match_ended.reset();
      s.rg.pickers.reset();
      Log(fmt::format("set game over; the set goes on ({} played)", s.rg.games));
    }
    return;
  }
  if (match_over && scene_left && !g.set && lobby.active && lobby.setup_ready)
  {
    // A 1v1's salty runback (L+R+Y with the quit) outside a set: the session has set its next
    // game up as the same match on every machine, and the game goes from the match straight into
    // it (docs/game-code.md §11). The session goes on, watched like the first game; ending it
    // here left both games playing the runback alone, each local pad driving P1 (prod
    // 2026-10-13). Nothing else sets a next game up in a room game without a set.
    std::lock_guard lk(s_mutex);
    if (s.rg.active && s.rg.match_id == g.match_id)
    {
      ++s.rg.games;
      s.rg.saw_match = false;
      s.rg.match_ended.reset();
      s.rg.pickers.reset();
      Log(fmt::format("game run back: the session plays it again ({} played)", s.rg.games));
    }
    return;
  }
  if (match_over && scene_left)
  {
    {
      std::lock_guard lk(s_mutex);
      if (s.rg.active)
        ++s.rg.games;
    }
    EndGame(true, "", g.pickers, true);
    return;
  }
  if (g.saw_session && !g.saw_match && !lobby.active)
  {
    // The session gave up on players it never heard from (their own connect to another player
    // failed; the server's room-unreachable, which names the pair, normally comes first).
    std::string text = TEXT_PLAYERS_LOST;
    if (lobby.unheard != 0)
    {
      std::lock_guard lk(s_mutex);
      std::vector<std::string> names;
      for (int port = 0; port < ROOM_SLOTS; ++port)
      {
        if (lobby.unheard & (1 << port))
          names.push_back(SlotName(port + 1));
      }
      text = fmt::format("Could not connect to players: {}", fmt::join(names, ", "));
    }
    EndGame(false, text, std::nullopt, true, lobby.unheard != 0 ? CONNECT_ERROR_SHOWN : ERROR_SHOWN);
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
  // The chat identity (docs/chat-protocol.md §1), read or made once, before any lock is held.
  const std::string chat_key = Chat::IdentityPublicKeyHex();

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
        std::unique_lock lk(s_mutex);
        if (ev.type == ENET_EVENT_TYPE_CONNECT)
        {
          Send(peer, HelloJson(info.uid, info.play_key, APP_VERSION, APP_PLATFORM, chat_key));
          s.conn = Conn::Hello;
          ++s.hellos;
          conn_started = now;
        }
        else if (ev.type == ENET_EVENT_TYPE_RECEIVE && ev.channelID == RoomWatch::CHANNEL)
        {
          // The room's game this player watches (Online/RoomWatch.h): binary, outside the lock.
          const std::vector<u8> data(ev.packet->data, ev.packet->data + ev.packet->dataLength);
          enet_packet_destroy(ev.packet);
          ++s.received;
          lk.unlock();
          RoomWatch::OnPacket(data);
          lk.lock();
        }
        else if (ev.type == ENET_EVENT_TYPE_RECEIVE)
        {
          const std::string text(reinterpret_cast<const char*>(ev.packet->data),
                                 ev.packet->dataLength);
          enet_packet_destroy(ev.packet);
          // Chat checks and decrypts outside the room lock (the CPU thread reads the room);
          // spectating and Brawl Mods rooms (ModsRoom.h) take their own messages there too.
          lk.unlock();
          const bool chat = Chat::OnServerMessage(text) || Spectate::OnServerMessage(text) ||
                            ModsRoom::OnServerMessage(text);
          lk.lock();
          if (chat)
          {
            ++s.received;
            continue;
          }
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
      if (!actions.fail_game.empty())
        EndGame(false, actions.fail_game, std::nullopt, true, CONNECT_ERROR_SHOWN);
      for (const std::string& line : actions.notices)
        Chat::AddNotice(line);

      // Chat's outgoing messages, signed and encrypted outside the room lock; a Brawl Mods
      // room's requests; the waiting room's speed checks.
      bool online;
      bool game_alive;
      u64 connection;
      {
        std::lock_guard lk(s_mutex);
        online = host && s.conn == Conn::Online;
        game_alive = online && s.mm_game_alive;
        connection = s.connects;
      }
      std::vector<std::string> chat_out =
          online ? Chat::TakeOutbox() : std::vector<std::string>{};
      if (online)
      {
        for (std::string& m : ModsRoom::TakeOutbox(connection))
          chat_out.push_back(std::move(m));
        for (std::string& m : SpeedCheck::TakeOutbox())
          chat_out.push_back(std::move(m));
      }
      // While the game runs (Online/GameAlive.h); none once it froze, with this thread still up.
      if (auto alive = GameAlive::Service(now, game_alive, connection))
        chat_out.push_back(std::move(*alive));
      // Spectating's upload (Online/Spectate.h): it backs off on the game's health and on what
      // is still unacknowledged to mm, before the game feels anything. The round trip is ENet's
      // smoothed one (roundTripTime); lastRoundTripTime is the lowest of the last 5 s throttle
      // window and lags a queue that builds.
      const Gprb::Session::LinkHealth lh = Gprb::Session::GetLinkHealth();
      const Spectate::Outgoing spectate_out = Spectate::Service(
          now, online, online && peer ? UnackedBytes(peer) : 0,
          Spectate::Health{lh.running, lh.late_frames, lh.rollback_avg, lh.ping_ms,
                           online && peer ? static_cast<double>(peer->roundTripTime) : -1.0});
      // Watching the room's game from the room CSS (Online/RoomWatch.h).
      RoomWatch::RoomInfo watch;
      {
        std::lock_guard lk(s_mutex);
        watch.online = online;
        watch.connection = connection;
        watch.in_room = s.phase == Phase::In && s.view.has_value();
        if (watch.in_room)
        {
          const View& v = *s.view;
          watch.room_in_game = v.status == Status::InGame;
          const auto& me = v.slots[v.you - 1].player;
          watch.me_in_game = me && me->in_game;
        }
        watch.own_game = s.rg.active;
        watch.screen = s.screen;
      }
      const std::vector<std::string> watch_out = RoomWatch::Service(now, watch);

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
          if (s.leave_before_pending)
          {
            Request leave;
            leave.op = Op::Leave;
            Send(peer, *RequestJson(leave));
            ++s.sent;
            Log("sent room-leave (before the launcher's create)");
            s.leave_before_pending = false;
          }
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
        for (const std::string& json : chat_out)
        {
          Send(peer, json);
          ++s.sent;
        }
        for (const std::string& json : spectate_out.json)
        {
          Send(peer, json);
          ++s.sent;
        }
        for (const std::vector<u8>& data : spectate_out.data)
          SendData(peer, data);
        for (const std::string& json : watch_out)
        {
          Send(peer, json);
          ++s.sent;
          Log(fmt::format("sent {}", json));
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
          // Never a ready while sitting out: mm would take it as playing again, and the host's SIT
          // OUT on a ready player would be undone by the lock-in the game has not taken back yet.
          const bool want_ready = s.lock_ready && s.lock_game == (s.game & 0xFF) &&
                                  s.lock_char != 0xFF && !(me && me->sitting_out);
          // Not ready: the coin placed on the CSS (the game's lock-in without its ready), shown to
          // the others; none while it is in the hand. Only to an mm that shows it (an older one
          // keeps no pick, so it would be sent again and again).
          const bool placed = s.lock_game == (s.game & 0xFF) && s.lock_char != 0xFF &&
                              (want_ready || v.sit_out_known);
          const std::optional<u8> want_char =
              placed ? std::optional<u8>(s.lock_char) : std::nullopt;
          const std::optional<u8> want_costume =
              placed ? std::optional<u8>(s.lock_costume) : std::nullopt;
          const bool pick_differs = me && (me->character != want_char ||
                                           (want_char && me->costume != want_costume));
          const bool pick_settled = want_ready || now - s.pick_changed >= PICK_SETTLE;
          // The picked stock count, only under the room's stock control (the server shows it
          // only then, so anything else would be sent again and again).
          const std::optional<u8> stocks =
              v.rules && v.rules->stock_control && s.lock_stocks >= 1 &&
                      s.lock_stocks <= MAX_PICKED_STOCKS ?
                  std::optional<u8>(s.lock_stocks) :
                  std::nullopt;
          if (may && me &&
              (want_ready != me->ready || (want_ready && me->stocks != stocks) ||
               (pick_differs && (want_ready || v.sit_out_known) && pick_settled)))
          {
            Request r;
            r.op = Op::Ready;
            r.flag = want_ready;
            r.character = want_char;
            r.costume = want_costume;
            r.stocks = want_ready ? stocks : std::nullopt;
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
            s.away_since &&
            now - *s.away_since >
                (s.screen == static_cast<u8>(Screen::OnlineBusy) ? AWAY_LEAVE_BUSY : AWAY_LEAVE))
        {
          Request r;
          r.op = Op::Leave;
          Send(peer, *RequestJson(r));
          ++s.sent;
          RestartAway();
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
      PollInputDelayRequest();
      ModsRoom::Files(OnlineDir());
      const DolphinBusy busy = QueryBusy();
      {
        std::lock_guard lk(s_mutex);
        DecideLaunch(busy, logged_in, info.connect_code, now);
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
  // The game window closed (emulation stops): the player leaves the room. The connection is kept
  // while Dolphin runs, so the room kept a member whose game was gone until Dolphin's main window
  // closed too (the user's, 2026-10-19): the game's last screen was the room CSS, so the away
  // leave never came, and the others waited on a player who could not ready up. A room game
  // stopping is left as well (its session leaves on the same stop, GameplayOnlineBackend).
  // Not the whole-machine backend's room game: it stops the menus' game to boot the match.
  // Outside s_mutex, which the hook takes.
  std::call_once(s_stop_hook_once, [] {
    s_stop_hook = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state != Core::State::Stopping)
        return;
      {
        std::lock_guard lk(s_mutex);
        if (s.phase == Phase::None && !s.pending && s.rejoin.empty())
          return;
        if (s.rg.active && Config::Get(Config::ONLINE_SESSION_BACKEND) == "netplay")
          return;
        Log("the game was closed: leaving the room");
      }
      Request r;
      r.op = Op::Leave;
      Submit(r);
    });
  });
  std::lock_guard lk(s_start_mutex);
  if (s_run)
    return;
  s_run = true;
  s_thread = std::thread(Thread);
}

void OnBoot()
{
  std::lock_guard lk(s_mutex);
  s.reached_menus = false;
  s.screen = static_cast<u8>(Screen::Unknown);
  s.away_since.reset();
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

void SetSetSession(bool set)
{
  std::lock_guard lk(s_mutex);
  if (s.rg.active)
    s.rg.set = set;
}

void ReportSetGame(u32 set_game, u8 winner_side, u16 stage)
{
  Request r;
  r.op = Op::Result;
  r.game = set_game;
  r.winner_side = winner_side;
  r.stage = stage;
  std::lock_guard lk(s_mutex);
  if (!s.view || !RequestJson(r))
    return;
  s.outbox.push_back(r);
  Log(fmt::format("set game {}: side {} on {:#x}", set_game, winner_side, stage));
}

RulesetListSnapshot RulesetList()
{
  std::lock_guard lk(s_mutex);
  const auto now = Clock::now();
  // Asked again after 5 s (the player may have published or used one meanwhile); one at a time.
  if (s.view && s.conn == Conn::Online && (!s.list_asked || now - *s.list_asked > 5s))
  {
    Request r;
    r.op = Op::RulesetList;
    s.outbox.push_back(r);
    s.list_asked = now;
    if (s.list_state != ListState::Ready)
      s.list_state = ListState::Loading;
  }
  RulesetListSnapshot snap;
  snap.state = s.list_state;
  snap.serial = s.list_serial;
  snap.rulesets = s.list;
  return snap;
}

void Submit(const Request& request)
{
  if (request.op == Op::Poll)
    return;
  std::lock_guard lk(s_mutex);
  const auto now = Clock::now();
  // A new request: the last error goes.
  s.error.clear();
  ++s.serial;
  switch (request.op)
  {
  case Op::Create:
  case Op::Join:
  {
    s.leave_before_pending = false;  // the player's own create / join replaces the launcher's
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
    s.leave_before_pending = false;
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

namespace
{
void SetScreenLocked(u8 screen);
}

void SetScreen(u8 screen)
{
  std::lock_guard lk(s_mutex);
  if (s.harness_screen)
    return;
  SetScreenLocked(screen);
}

namespace
{
// under s_mutex
void SetScreenLocked(u8 screen)
{
  const u8 v = screen <= static_cast<u8>(Screen::Other) ? screen : static_cast<u8>(Screen::Other);
  if (v == static_cast<u8>(Screen::Menus) || v == static_cast<u8>(Screen::OnlineCss) ||
      v == static_cast<u8>(Screen::Room))
  {
    s.reached_menus = true;
  }
  if (v != s.screen)
  {
    s.screen = v;
    RestartAway();
  }
}
}  // namespace

void SetLocalLock(bool ready, u32 game, u8 char_kind, u8 costume, u8 stocks)
{
  std::lock_guard lk(s_mutex);
  if (s.harness_lock)
    return;
  if (char_kind != s.lock_char || costume != s.lock_costume)
    s.pick_changed = Clock::now();
  s.lock_ready = ready;
  s.lock_game = game;
  s.lock_char = char_kind;
  s.lock_costume = costume;
  s.lock_stocks = stocks;
}

Snapshot GetSnapshot()
{
  std::lock_guard lk(s_mutex);
  Snapshot snap;
  snap.phase = s.phase;
  snap.view = s.view;
  snap.game = s.game;
  // The server's (who picks when the last game's loser sits out), else this machine's own.
  snap.pickers = s.view && s.view->pickers          ? *s.view->pickers :
                 s.view && !s.view->ruleset.LoserPicks() ? 0 :
                                                           s.pickers;
  snap.join_seq = s.join_seq;
  snap.direct_seq = s.direct_seq;
  snap.direct_code = s.direct_code;
  snap.direct_queue = s.direct_queue;
  snap.create_seq = s.create_seq;
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

Screen GetScreen()
{
  std::lock_guard lk(s_mutex);
  return static_cast<Screen>(s.screen);
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  const auto now = Clock::now();
  picojson::object o;
  o["running"] = picojson::value(s_run.load());
  o["watch"] = picojson::value(RoomWatch::Status());
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
  // What the game shows (picksStage): the server's when it sends them.
  o["next_pickers"] = picojson::value(static_cast<double>(
      s.view && s.view->pickers ? *s.view->pickers :
      s.view && !s.view->ruleset.LoserPicks() ? 0 :
                                                s.pickers));
  if (s.view)
  {
    picojson::object rs;
    rs["code"] = picojson::value(s.view->ruleset.code);
    rs["name"] = picojson::value(s.view->ruleset.name);
    rs["version"] = picojson::value(static_cast<double>(s.view->ruleset.version));
    rs["based_on"] = picojson::value(s.view->ruleset.based_on);
    rs["best_of"] = picojson::value(static_cast<double>(s.view->ruleset.best_of));
    o["ruleset"] = picojson::value(rs);
    if (const auto& st = s.view->set)
    {
      picojson::object so;
      so["best_of"] = picojson::value(static_cast<double>(st->best_of));
      so["game"] = picojson::value(static_cast<double>(st->game));
      so["score"] = picojson::value(picojson::array{picojson::value(static_cast<double>(st->score[0])),
                                                    picojson::value(static_cast<double>(st->score[1]))});
      so["paused"] = picojson::value(st->paused);
      so["can_resume"] = picojson::value(st->can_resume);
      so["finished"] = picojson::value(st->finished);
      so["waiting_for"] = picojson::value(st->waiting_for);
      so["winner"] = picojson::value(static_cast<double>(st->winner));
      so["side_slots"] = picojson::value(picojson::array{picojson::value(static_cast<double>(st->side_slots[0])),
                                                         picojson::value(static_cast<double>(st->side_slots[1]))});
      o["set"] = picojson::value(so);
    }
  }
  o["join_seq"] = picojson::value(static_cast<double>(s.join_seq));
  o["screen"] = picojson::value(static_cast<double>(s.screen));
  o["reached_menus"] = picojson::value(s.reached_menus);
  picojson::object lock;
  lock["ready"] = picojson::value(s.lock_ready);
  lock["game"] = picojson::value(static_cast<double>(s.lock_game));
  lock["char_kind"] = picojson::value(static_cast<double>(s.lock_char));
  lock["costume"] = picojson::value(static_cast<double>(s.lock_costume));
  lock["stocks"] = picojson::value(static_cast<double>(s.lock_stocks));
  lock["harness"] = picojson::value(s.harness_lock);
  o["screen_harness"] = picojson::value(s.harness_screen);
  o["lock"] = picojson::value(lock);
  picojson::object rg;
  rg["active"] = picojson::value(s.rg.active);
  rg["code"] = picojson::value(s.rg.code);
  rg["match_id"] = picojson::value(s.rg.match_id);
  rg["host"] = picojson::value(static_cast<double>(s.rg.host));
  rg["teams"] = picojson::value(s.rg.teams);
  rg["team_colors"] = picojson::value(s.rg.team_colors);
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
    vo["team_colors"] = picojson::value(v.team_colors);
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
        p["stocks"] = sl.player->stocks ? picojson::value(static_cast<double>(*sl.player->stocks)) :
                                          picojson::value();
        p["in_game"] = picojson::value(sl.player->in_game);
        p["sitting_out"] = picojson::value(sl.player->sitting_out);
        p["watching"] = picojson::value(sl.player->watching);
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
  o["launch_pending_kind"] =
      s.launch ? picojson::value(LaunchKindName(s.launch->kind)) : picojson::value();
  o["create_seq"] = picojson::value(static_cast<double>(s.create_seq));
  o["direct_seq"] = picojson::value(static_cast<double>(s.direct_seq));
  o["direct_code"] = picojson::value(s.direct_code);
  o["direct_queue"] = picojson::value(static_cast<double>(s.direct_queue));
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
    r.colors = flag("colors", false);
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
    // Not ready, a character is a placed coin; none, the coin is in the hand.
    std::lock_guard lk(s_mutex);
    s.harness_lock = true;
    s.lock_ready = flag("ready", true);
    s.lock_game = s.game & 0xFF;
    const u8 c = static_cast<u8>(num("character", s.lock_ready ? 0 : 0xFF));
    const u8 costume = static_cast<u8>(num("costume", 0));
    if (c != s.lock_char || costume != s.lock_costume)
      s.pick_changed = Clock::now();
    s.lock_char = c;
    s.lock_costume = costume;
    s.lock_stocks = static_cast<u8>(num("stocks", 0));
    return std::nullopt;
  }
  else if (op == "sit_out")
  {
    r.op = Op::SitOut;
    r.slot = static_cast<int>(num("slot", 0));
    r.flag = flag("sit_out", true);
    if (r.slot < 1 || r.slot > ROOM_SLOTS)
      return "slot must be 1-4";
  }
  else if (op == "watch_leave")
  {
    // As START / B while watching the room's game (Online/RoomWatch.h).
    RoomWatch::Abort();
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
    // Stands in for the game's LOCAL.screen until `release_screen`.
    std::lock_guard lk(s_mutex);
    s.harness_screen = true;
    SetScreenLocked(static_cast<u8>(num("screen", 0)));
    return std::nullopt;
  }
  else if (op == "release_screen")
  {
    std::lock_guard lk(s_mutex);
    s.harness_screen = false;
    return std::nullopt;
  }
  else
  {
    return "op must be create, join, leave, slot, teams, public, team, ready, sit_out, "
           "watch_leave, release_lock, screen or release_screen";
  }
  Submit(r);
  return std::nullopt;
}
}  // namespace Online::Rooms
