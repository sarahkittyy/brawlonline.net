// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/Ranked.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include "Common/HookableEvent.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Core.h"
#include "Core/Online/GameSetup.h"
#include "Core/Online/OnlineClient.h"
#include "Core/Online/OnlineSession.h"
#include "Core/Online/Rooms.h"
#include "Core/Online/User.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/PeerData.h"

namespace Online::Ranked
{
namespace
{
using Clock = std::chrono::steady_clock;

// Matchmaking::Mode::Ranked (Slippi's mode numbers).
constexpr int MODE_RANKED = 0;
constexpr int MODE_DIRECT = 2;
// A room's game is a Teams (mode 3) match (docs/rooms-protocol.md §3).
constexpr int MODE_TEAMS = 3;
constexpr int MAX_PLAYERS = Gprb::Session::MAX_LOBBY_PLAYERS;
// Best of three.
constexpr int WINS_NEEDED = 2;
constexpr u8 DRAW = 0xFE;
// Reports: Slippi's game-reporter tries 5 times, backing off.
constexpr int REPORT_ATTEMPTS = 5;
// The result: polled until the server has decided the set. It waits up to its grace period
// (2 minutes) for a missing report, so poll a little longer than that.
constexpr auto RESULT_POLL_INTERVAL = std::chrono::seconds(2);
constexpr int RESULT_POLLS = 150;
constexpr auto HTTP_TIMEOUT = std::chrono::seconds(5);
// At exit, how long the reports still to send (a leave above all) may hold the process up.
constexpr auto EXIT_REPORT_WAIT = std::chrono::seconds(2);

enum class Outcome
{
  Done,
  Retry,
  Fail,
};

struct Job
{
  std::string what;
  std::function<Outcome()> run;
  int max_attempts = REPORT_ATTEMPTS;
  // Delay before attempt n+1 (n >= 1).
  std::function<Clock::duration(int)> backoff;
  int attempts = 0;
  Clock::time_point not_before{};
};

std::mutex s_mutex;  // guards everything below
std::condition_variable s_cv;
std::deque<Job> s_jobs;
std::thread s_worker;
bool s_stop = false;
bool s_job_running = false;   // the worker is running a job (outside s_mutex)
std::condition_variable s_idle_cv;  // a job finished
Common::EventHook s_stop_hook;

// The current ranked set.
bool s_active = false;     // a ranked set is tracked
// The games of this match are reported: a 1v1 of Ranked, Unranked or Direct, or a room's game.
bool s_reporting = false;
bool s_room = false;  // a room's game: 2-4 players, each on their slot
std::string s_match_id;
// By in-game port: a 1v1's host is 0, the joiner 1; a room's players are on their slots. Empty:
// nobody on that port.
std::array<std::string, MAX_PLAYERS> s_uids;
int s_local_port = 0;
std::array<int, 2> s_wins{};
bool s_set_over = false;
bool s_peer_gone = false;
bool s_left_reported = false;

// Counters and the last result (harness, GET_RANK).
u64 s_reports_sent = 0;
u64 s_reports_failed = 0;
bool s_result_pending = false;
std::optional<float> s_rating;  // newer than the user's, once a result arrived
std::optional<u32> s_sets_played;
std::optional<float> s_change;
std::string s_last_status;

Clock::duration ReportBackoff(int attempt)
{
  return std::chrono::seconds(1 << std::min(attempt - 1, 4));  // 1, 2, 4, 8, 16 s
}

std::string Url(const std::string& path)
{
  std::string base = Config::GetAccountsUrl();
  while (!base.empty() && base.back() == '/')
    base.pop_back();
  return base + path;
}

// Parses a JSON object response; nullopt if it is not one (or too deep to parse safely).
std::optional<picojson::object> ParseObject(const Common::HttpRequest::Response& response)
{
  if (!response)
    return std::nullopt;
  const std::string body(response->begin(), response->end());
  picojson::value v;
  if (!Gprb::PeerData::JsonSafeToParse(body, 16) || !picojson::parse(v, body).empty() ||
      !v.is<picojson::object>())
  {
    return std::nullopt;
  }
  return v.get<picojson::object>();
}

// 2xx: done. 429, 5xx and no answer: try again. Other 4xx: the request itself is wrong.
Outcome Classify(s32 code)
{
  if (code >= 200 && code < 300)
    return Outcome::Done;
  if (code == 0 || code == 429 || code >= 500)
    return Outcome::Retry;
  return Outcome::Fail;
}

std::optional<double> Number(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<double>() || !std::isfinite(it->second.get<double>()))
    return std::nullopt;
  return std::clamp(it->second.get<double>(), -1e6, 1e6);
}

// Adds the play-key credentials (read when the request runs, so a re-login is honoured).
bool AddCredentials(picojson::object* body)
{
  const UserInfo info = Client::GetUser().GetUserInfo();
  if (info.uid.empty() || info.play_key.empty())
    return false;
  (*body)["uid"] = picojson::value(info.uid);
  (*body)["playKey"] = picojson::value(info.play_key);
  return true;
}

Outcome Post(const std::string& what, const std::string& path, picojson::object body)
{
  if (!AddCredentials(&body))
  {
    WARN_LOG_FMT(NETPLAY, "Ranked: {}: not logged in", what);
    return Outcome::Fail;
  }
  Common::HttpRequest request(HTTP_TIMEOUT);
  const auto response =
      request.Post(Url(path), picojson::value(body).serialize(),
                   {{"Content-Type", "application/json"}}, Common::HttpRequest::AllowedReturnCodes::All);
  const s32 code = response ? request.GetLastResponseCode() : 0;
  const Outcome outcome = Classify(code);
  if (outcome == Outcome::Done)
  {
    if (const auto o = ParseObject(response))
    {
      const auto it = o->find("status");
      std::lock_guard lk(s_mutex);
      if (it != o->end() && it->second.is<std::string>())
        s_last_status = it->second.get<std::string>();
    }
    NOTICE_LOG_FMT(NETPLAY, "Ranked: {} reported", what);
  }
  else
  {
    WARN_LOG_FMT(NETPLAY, "Ranked: {}: HTTP {}{}", what, code,
                 outcome == Outcome::Retry ? " (will retry)" : "");
  }
  return outcome;
}

// The set's result for this player: Retry while the server has not decided it.
Outcome FetchResult(const std::string& match_id)
{
  const std::string uid = Client::GetUser().GetUserInfo().uid;
  if (uid.empty())
    return Outcome::Fail;
  Common::HttpRequest request(HTTP_TIMEOUT);
  const std::string url = Url(fmt::format("/v1/ranked/result?matchId={}&uid={}",
                                          request.EscapeComponent(match_id),
                                          request.EscapeComponent(uid)));
  const auto response = request.Get(url, {}, Common::HttpRequest::AllowedReturnCodes::All);
  const s32 code = response ? request.GetLastResponseCode() : 0;
  if (Classify(code) != Outcome::Done)
    return Classify(code);
  const auto o = ParseObject(response);
  if (!o)
    return Outcome::Retry;
  const auto status_it = o->find("status");
  const std::string status =
      status_it != o->end() && status_it->second.is<std::string>() ? status_it->second.get<std::string>() : "";
  if (status == "ASSIGNED")
    return Outcome::Retry;

  std::optional<float> after, change;
  std::optional<u32> sets;
  if (const auto r = o->find("rating"); r != o->end() && r->second.is<picojson::object>())
  {
    const auto& ro = r->second.get<picojson::object>();
    if (const auto v = Number(ro, "after"))
      after = static_cast<float>(*v);
    if (const auto v = Number(ro, "change"))
      change = static_cast<float>(*v);
    if (const auto v = Number(ro, "setsPlayed"); v && *v >= 0)
      sets = static_cast<u32>(*v);
  }
  {
    std::lock_guard lk(s_mutex);
    s_last_status = status;
    s_result_pending = false;
    s_change = change.value_or(0.0f);
    if (after)
      s_rating = after;
    if (sets)
      s_sets_played = sets;
  }
  if (after && sets)
    Client::GetUser().SetRankedRating(*after, static_cast<int>(*sets));
  NOTICE_LOG_FMT(NETPLAY, "Ranked: set {} {}: rating {} ({:+.1f})", match_id, status,
                 after ? fmt::format("{:.1f}", *after) : std::string("unchanged"),
                 change.value_or(0.0f));
  return Outcome::Done;
}

void WorkerLoop()
{
  Common::SetCurrentThreadName("Online ranked reports");
  std::unique_lock lk(s_mutex);
  while (!s_stop)
  {
    if (s_jobs.empty())
    {
      s_cv.wait(lk);
      continue;
    }
    // Strictly in order: a set's result is fetched after its reports.
    Job& front = s_jobs.front();
    if (Clock::now() < front.not_before)
    {
      s_cv.wait_until(lk, front.not_before);
      continue;
    }
    Job job = std::move(front);
    s_jobs.pop_front();
    s_job_running = true;
    lk.unlock();
    const Outcome outcome = job.run();
    lk.lock();
    s_job_running = false;
    s_idle_cv.notify_all();
    ++job.attempts;
    if (outcome == Outcome::Retry && job.attempts < job.max_attempts && !s_stop)
    {
      job.not_before = Clock::now() + job.backoff(job.attempts);
      s_jobs.push_front(std::move(job));
    }
    else if (outcome != Outcome::Done)
    {
      ++s_reports_failed;
      if (job.what.starts_with("result"))
        s_result_pending = false;
      WARN_LOG_FMT(NETPLAY, "Ranked: gave up on {} after {} attempt(s)", job.what, job.attempts);
    }
  }
}

// Under s_mutex.
void Enqueue(Job job)
{
  if (!job.backoff)
    job.backoff = ReportBackoff;
  s_jobs.push_back(std::move(job));
  if (!s_worker.joinable())
  {
    s_stop = false;
    s_worker = std::thread(WorkerLoop);
  }
  s_cv.notify_all();
}

// Under s_mutex.
void EnqueueLeave(const char* kind)
{
  picojson::object body;
  body["matchId"] = picojson::value(s_match_id);
  body["kind"] = picojson::value(kind);
  ++s_reports_sent;
  Enqueue({fmt::format("leave '{}' of {}", kind, s_match_id),
           [body] { return Post("leave", "/v1/ranked/report-leave", body); }});
}

// Under s_mutex.
void EnqueueResult()
{
  s_result_pending = true;
  const std::string id = s_match_id;
  Job job{fmt::format("result of {}", id), [id] { return FetchResult(id); }, RESULT_POLLS,
          [](int) -> Clock::duration { return RESULT_POLL_INTERVAL; }};
  Enqueue(std::move(job));
}

// Under s_mutex. This player leaves a set that is not over: Slippi counts that as abandoning it.
// Reported once, whichever way the player leaves (the menus, the game window closing, the app
// quitting). Returns whether it was reported now.
bool LeaveUnfinishedSet(const char* how)
{
  if (!s_active || s_set_over || s_peer_gone || s_left_reported)
    return false;
  s_left_reported = true;
  NOTICE_LOG_FMT(NETPLAY, "Ranked: leaving set {} before it is over ({})", s_match_id, how);
  EnqueueLeave("left");
  return true;
}
}  // namespace

void OnSessionStart(const Match& match)
{
  // The gameplay session tells us about every game; only ranked sets are tracked.
  Gprb::Session::SetGameResultCallback(OnGameResult);
  // Closing the game window (or quitting the app) mid-set abandons it at once: without a leave
  // of its own the set waited for the opponent's "opponent left" and the server's 2-minute grace,
  // and with no report from the opponent either it went void after 30 minutes, the quitter's
  // loss with it (the user's, 2026-10-14). Core::Stop notifies Stopping before the CPU stops.
  if (!s_stop_hook)
  {
    s_stop_hook = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state != Core::State::Stopping)
        return;
      std::lock_guard lk(s_mutex);
      LeaveUnfinishedSet("the game is closing");
    });
  }
  // Ranked's stage striking (GameSetup.h). Outside s_mutex: it talks to the session, which calls
  // OnGameResult with its own lock held.
  const bool ranked = match.mode == MODE_RANKED && match.players.size() == 2;
  // Ranked: a pause-screen quit (a runback too) loses the game; elsewhere it does not count, and a
  // runback plays the same match again (docs/game-code.md §11, the quit per mode). Ranked has no
  // pause since 2026-10-14 (the plugin's noRankedPause), so there it is only a safeguard.
  Gprb::Session::SetQuitLoses(ranked);
  // Ranked and a room's set: a desync plays the game again from the state every machine agreed on
  // (the user's, 2026-10-13); elsewhere it only ends the game (docs/game-code.md, desync recovery).
  const bool room_set = match.set && match.set->format.best_of > 0 &&
                        match.players.size() == match.set->members[0].size() + match.set->members[1].size();
  Gprb::Session::SetDesyncRecovery(ranked || room_set);
  int room_local_port = -1;
  for (const auto& p : match.players)
  {
    if (p.is_local)
      room_local_port = p.port - 1;
  }
  if (match.set && match.players.size() == match.set->members[0].size() + match.set->members[1].size())
  {
    // A room's set (docs/design/room-rules.md §5; 1v1 or 2v2): in-game ports are the room's slots.
    GameSetup::Begin(room_local_port, *match.set, match.room_host_port - 1);
    std::array<std::string, GameSetup::MAX_PORTS> names;
    for (const auto& p : match.players)
    {
      if (p.port >= 1 && p.port <= GameSetup::MAX_PORTS)
        names[p.port - 1] = p.display_name;
    }
    GameSetup::SetPortNames(names);
    Rooms::SetSetSession(match.set->format.best_of > 0);
  }
  else if (ranked)
  {
    int local_port = 0;
    for (const auto& p : match.players)
    {
      if (p.is_local)
        local_port = p.is_local == match.is_host ? 0 : 1;
    }
    GameSetup::Begin(local_port, match.starters, match.stages);
    // Ranked's in-game ports: the host 0, the joiner 1.
    std::array<std::string, GameSetup::MAX_PORTS> names;
    for (const auto& p : match.players)
      names[p.is_local ? local_port : 1 - local_port] = p.display_name;
    GameSetup::SetPortNames(names);
  }
  else
  {
    GameSetup::End();
  }
  std::lock_guard lk(s_mutex);
  if (s_active && !s_set_over)
    WARN_LOG_FMT(NETPLAY, "Ranked: set {} replaced before it ended", s_match_id);
  s_active = ranked;
  // Every online 1v1 game and every room game is reported (Slippi's reportOnlineGame), for the
  // players' match history; only Ranked's are rated. A room's game has every player's place.
  s_room = match.mode == MODE_TEAMS && match.room_host_port >= 1 &&
           match.room_host_port <= MAX_PLAYERS && match.players.size() >= 2 &&
           match.players.size() <= MAX_PLAYERS;
  s_reporting = s_room || (match.players.size() == 2 && match.mode >= MODE_RANKED &&
                           match.mode <= MODE_DIRECT);
  if (!s_reporting)
    return;
  s_match_id = match.match_id;
  s_uids = {};
  for (const auto& p : match.players)
  {
    // In-game ports, as in the gameplay session: a room's slots, else the decider (host) is
    // port 0.
    const int port = s_room ? p.port - 1 : (p.is_local == match.is_host ? 0 : 1);
    if (port < 0 || port >= MAX_PLAYERS)
      continue;
    s_uids[port] = p.uid;
    if (p.is_local)
      s_local_port = port;
  }
  s_wins = {};
  s_set_over = false;
  s_peer_gone = false;
  s_left_reported = false;
  s_change.reset();
  s_last_status = "ASSIGNED";
  if (s_active)
    NOTICE_LOG_FMT(NETPLAY, "Ranked: set {} started (best of {})", s_match_id, WINS_NEEDED * 2 - 1);
  else
    NOTICE_LOG_FMT(NETPLAY, "Ranked: reporting the games of {}", s_match_id);
}

void OnSessionStartFailed()
{
  GameSetup::End();
  std::lock_guard lk(s_mutex);
  if (s_active)
    NOTICE_LOG_FMT(NETPLAY, "Ranked: set {} not started (the session did not start)", s_match_id);
  s_active = false;
  s_reporting = false;
}

void OnGameResult(const Gprb::Session::GameResult& r)
{
  std::lock_guard lk(s_mutex);
  if (!s_reporting || (s_active && s_set_over))
    return;
  if (r.end == Gprb::Session::GameEnd::Desync)
  {
    // A desync in Ranked or a room's set (SetDesyncRecovery): no result; the session plays the
    // same set game again, on its stage with its characters, from the agreed state.
    if (GameSetup::IsActive())
      GameSetup::OnGameRestarting(r.game, r.stage);
    NOTICE_LOG_FMT(NETPLAY, "Ranked: game {} desynced: it starts again", r.game);
    return;
  }
  if (r.end != Gprb::Session::GameEnd::GameSet && !s_active)
  {
    // A quit outside Ranked did not count (no history either). A room's set plays that game
    // again: a quit from its first step (the server's agreement on SIDE_VOID clears its steps),
    // a runback on the same stage (the session set it up again; nothing to report).
    if (s_room && GameSetup::IsActive())
    {
      const u32 set_game = GameSetup::SetGameOf(r.game);
      GameSetup::OnGameNotCounted();
      if (r.end == Gprb::Session::GameEnd::Quit)
        Rooms::ReportSetGame(set_game, Rooms::SIDE_VOID, r.stage == Gprb::Session::NO_STAGE ? 0 : r.stage);
      NOTICE_LOG_FMT(NETPLAY, "Ranked: room set game {} {} by P{}: played again", set_game,
                     r.end == Gprb::Session::GameEnd::Runback ? "run back" : "quit", r.quitter + 1);
    }
    return;
  }
  picojson::object body;
  body["matchId"] = picojson::value(s_match_id);
  // Ranked: the set's game, which a desynced game played again keeps (the server counts a set's
  // games in order and stops at a game nobody reported).
  const u32 game_index = s_active && GameSetup::IsActive() ? GameSetup::SetGameOf(r.game) : r.game;
  body["gameIndex"] = picojson::value(static_cast<double>(game_index));
  const size_t ports = s_room ? MAX_PLAYERS : 2;
  body["winner"] = r.winner < ports && !s_uids[r.winner].empty() ? picojson::value(s_uids[r.winner]) :
                                                                   picojson::value();
  if (r.stage != Gprb::Session::NO_STAGE)
    body["stageId"] = picojson::value(static_cast<double>(r.stage));
  body["durationFrames"] = picojson::value(static_cast<double>(r.frames));
  picojson::array players;
  for (size_t i = 0; i < ports; ++i)
  {
    if (s_uids[i].empty())
      continue;
    picojson::object p;
    p["uid"] = picojson::value(s_uids[i]);
    if (s_room)
    {
      // The history shows a room game's places (and teams), from what most players report.
      p["port"] = picojson::value(static_cast<double>(i + 1));
      if (!r.present[i])
      {
        // Left during the game: not placed.
        p["left"] = picojson::value(true);
        players.emplace_back(p);
        continue;
      }
      // 3-4 players: the game's places; a 1v1 has none (the winner first, both after a draw).
      const u8 place = r.place[i] ? r.place[i] : (r.winner == i || r.winner == DRAW ? 1 : 2);
      p["place"] = picojson::value(static_cast<double>(place));
      if (r.team[i] != Gprb::Session::NO_TEAM)
        p["team"] = picojson::value(static_cast<double>(r.team[i]));
    }
    p["characterId"] = picojson::value(static_cast<double>(r.char_kind[i]));
    p["stocksRemaining"] = picojson::value(static_cast<double>(r.stocks[i]));
    p["damage"] = picojson::value(std::isfinite(r.damage[i]) ? static_cast<double>(r.damage[i]) : 0.0);
    players.emplace_back(p);
  }
  body["players"] = picojson::value(players);
  ++s_reports_sent;
  Enqueue({fmt::format("game {} of {}", r.game, s_match_id),
           [body] { return Post("game", "/v1/ranked/report-game", body); }});
  if (s_room && GameSetup::IsActive())
  {
    // A room's set: the server keeps the score (room-result); GameSetup the next game's steps.
    const u32 set_game = GameSetup::SetGameOf(r.game);
    const auto side = GameSetup::SideOfPort(r.winner);
    GameSetup::OnGameResult(r.winner, r.stage);
    if (side)
      Rooms::ReportSetGame(set_game, *side, r.stage == Gprb::Session::NO_STAGE ? 0 : r.stage);
    return;
  }
  if (!s_active)
    return;

  if (r.winner < 2)
    ++s_wins[r.winner];
  GameSetup::OnGameResult(r.winner, r.stage);
  NOTICE_LOG_FMT(NETPLAY, "Ranked: game {} {} ({}-{})", r.game,
                 r.winner == DRAW ? "drawn" :
                 r.winner == s_local_port ? "won" : "lost",
                 s_wins[s_local_port], s_wins[1 - s_local_port]);
  if (std::ranges::any_of(s_wins, [](int w) { return w >= WINS_NEEDED; }))
  {
    s_set_over = true;
    NOTICE_LOG_FMT(NETPLAY, "Ranked: set {} over, {}", s_match_id,
                   s_wins[s_local_port] >= WINS_NEEDED ? "won" : "lost");
    EnqueueResult();
  }
}

void OnPeerGone()
{
  std::lock_guard lk(s_mutex);
  if (!s_active || s_set_over || s_peer_gone || s_left_reported)
    return;
  s_peer_gone = true;
  NOTICE_LOG_FMT(NETPLAY, "Ranked: the opponent left set {}", s_match_id);
  EnqueueLeave("opponent_left");
  EnqueueResult();
}

void OnCleanup()
{
  GameSetup::End();
  std::lock_guard lk(s_mutex);
  s_reporting = false;
  if (!s_active)
    return;
  if (LeaveUnfinishedSet("left the connection"))
    EnqueueResult();
  s_active = false;
}

bool IsSetOver()
{
  std::lock_guard lk(s_mutex);
  return s_active && s_set_over;
}

RankInfo GetRankInfo()
{
  RankInfo info;
  User& user = Client::GetUser();
  const UserInfo ui = user.GetUserInfo();
  std::lock_guard lk(s_mutex);
  if (ui.uid.empty())
    return info;
  info.rating = s_rating.value_or(ui.ranked_rating);
  info.sets_played = s_sets_played.value_or(static_cast<u32>(std::max(ui.ranked_update_count, 0)));
  info.change = s_change;
  if (s_result_pending || user.GetFetchStatus() == UserFetchStatus::Fetching)
    info.state = RankInfo::State::Fetching;
  else if (s_rating || user.GetFetchStatus() == UserFetchStatus::Fetched)
    info.state = RankInfo::State::Ready;
  return info;
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  picojson::object o;
  o["active"] = picojson::value(s_active);
  o["reporting"] = picojson::value(s_reporting);
  o["room"] = picojson::value(s_room);
  o["match_id"] = picojson::value(s_match_id);
  o["wins"] = picojson::value(picojson::array{picojson::value(static_cast<double>(s_wins[0])),
                                              picojson::value(static_cast<double>(s_wins[1]))});
  o["local_port"] = picojson::value(static_cast<double>(s_local_port));
  o["set_over"] = picojson::value(s_set_over);
  o["peer_gone"] = picojson::value(s_peer_gone);
  o["left_reported"] = picojson::value(s_left_reported);
  o["reports_sent"] = picojson::value(static_cast<double>(s_reports_sent));
  o["reports_failed"] = picojson::value(static_cast<double>(s_reports_failed));
  o["pending_jobs"] = picojson::value(static_cast<double>(s_jobs.size()));
  o["result_pending"] = picojson::value(s_result_pending);
  o["last_status"] = picojson::value(s_last_status);
  o["rating"] = s_rating ? picojson::value(static_cast<double>(*s_rating)) : picojson::value();
  o["change"] = s_change ? picojson::value(static_cast<double>(*s_change)) : picojson::value();
  o["setup"] = picojson::value(GameSetup::Status());
  return o;
}

void Shutdown()
{
  Gprb::Session::SetGameResultCallback(nullptr);
  std::thread worker;
  {
    std::unique_lock lk(s_mutex);
    // The app quits mid-set (Cmd-Q, the main window closed without a stop first): leave it too.
    LeaveUnfinishedSet("the app is quitting");
    // The reports still to send get a short wait (a set's result is for the CSS: not fetched).
    std::erase_if(s_jobs, [](const Job& job) { return job.what.starts_with("result"); });
    if ((!s_jobs.empty() || s_job_running) && s_worker.joinable())
    {
      const bool sent = s_idle_cv.wait_for(lk, EXIT_REPORT_WAIT,
                                           [] { return s_jobs.empty() && !s_job_running; });
      NOTICE_LOG_FMT(NETPLAY, "Ranked: reports at exit {}", sent ? "sent" : "still waiting, dropped");
    }
    s_stop = true;
    if (!s_jobs.empty())
      WARN_LOG_FMT(NETPLAY, "Ranked: {} report(s) not sent at exit", s_jobs.size());
    s_jobs.clear();
    worker = std::move(s_worker);
    s_cv.notify_all();
  }
  if (worker.joinable())
    worker.join();
}
}  // namespace Online::Ranked
