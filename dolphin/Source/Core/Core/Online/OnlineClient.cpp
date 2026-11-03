// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/OnlineClient.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/UPnP.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/Core.h"
#include "Core/Online/OnlineSession.h"
#include "Core/Online/Ranked.h"
#include "Core/Online/Rooms.h"
#include "Core/Online/User.h"

namespace Online::Client
{
namespace
{
std::mutex s_mutex;  // guards everything below
std::unique_ptr<User> s_user;
std::unique_ptr<Matchmaking> s_matchmaking;
SearchOptions s_last_search;
bool s_searched = false;
// s_last_search's mode while s_searched, else -1 (SearchMode, without s_mutex).
std::atomic<int> s_search_mode{-1};
// Old matchmakings being torn down (their destructor waits for the matchmaking thread).
std::vector<std::thread> s_cleanup_threads;

// Serializes a hand-off against Cleanup(): a search that connects just as it is cancelled must not
// start a session after the cleanup stopped it. Each search has a generation; Cleanup() bumps it.
std::mutex s_session_gate;
u64 s_generation = 0;
// The game's search (SearchOptions::cancel_on_stop): its generation, 0 with none. Under s_mutex.
u64 s_cancel_on_stop = 0;
std::once_flag s_stop_hook_once;
Common::EventHook s_stop_hook;

std::mutex s_handoff_mutex;
std::string s_handoff;  // "", "started", "kept" (no backend / not wanted), "failed"
std::string s_handoff_error;

// ---- The pair room (OnlineClient.h) ----
using Clock = std::chrono::steady_clock;
// Under s_mutex: the party ticket's matchmaking (none: no pair room), its party, when a refused
// one may try again, when the 2v2 search was seen ending while the party ticket still connected
// (it gets PAIR_GRACE to fail or connect by itself: both sides time out together, and the
// partner's search ending would otherwise show as "bob cancelled"), and the line the 2v2 search
// ended with.
std::unique_ptr<Matchmaking> s_pair;
std::string s_pair_party;
Clock::time_point s_pair_retry_at{};
std::optional<Clock::time_point> s_pair_search_gone_at;
std::string s_pair_error;
// The partner left the pair room's session (PairRoomOver with `wait`) while the 2v2 search still
// waits: when, and the line the search ends with if it is still waiting PAIR_GRACE later. A
// partner whose search found the match ends their pair room at once, and their leave can come
// before this search's own get-ticket-resp: then it is the match's, not a lost partner.
std::optional<Clock::time_point> s_pair_over_at;
std::string s_pair_over_error;
// Under s_session_gate: the party ticket's generation (a hand-off of an older one is dropped) and
// its match once its session started.
u64 s_pair_generation = 0;
std::optional<Match> s_pair_match;
std::atomic<bool> s_pair_session{false};
// A party ticket the server refused (or that failed otherwise than by its connect): tried again
// after this long while the party still searches.
constexpr auto PAIR_RETRY = std::chrono::seconds(3);
constexpr auto PAIR_GRACE = std::chrono::milliseconds(1500);
// The 2v2 match's hand-off waits at most this long for the pair room's match to end.
constexpr auto PAIR_STOP_WAIT = std::chrono::seconds(3);

// The partner's name in a pair room's match (or "your partner").
std::string PairPartner(const std::optional<Match>& match)
{
  if (match)
  {
    for (const auto& p : match->players)
    {
      if (!p.is_local && !p.display_name.empty())
        return p.display_name;
    }
  }
  return "your partner";
}

// Under s_mutex: the pair room ends at whatever step it is (no party ticket left, its session
// stopped).
void EndPairLocked(std::string_view why)
{
  s_pair_search_gone_at.reset();
  s_pair_over_at.reset();
  if (!s_pair)
    return;
  NOTICE_LOG_FMT(NETPLAY, "Online: the pair room of party {} ends ({})", s_pair_party, why);
  std::unique_ptr<Matchmaking> old = std::move(s_pair);
  s_pair_party.clear();
  {
    std::lock_guard gate(s_session_gate);
    ++s_pair_generation;
    if (s_pair_match)
    {
      s_pair_match.reset();
      s_pair_session = false;
      Session::Stop();
    }
  }
  s_cleanup_threads.emplace_back([old = std::move(old)]() mutable { old.reset(); });
}

// Runs on the party ticket's matchmaking thread once the partners are connected: the pair room's
// session, unless the pair room ended meanwhile.
void OnPairConnected(u64 generation, const Match& found, P2PLink& link)
{
  Match match = found;
  match.pair_room = true;
  // The party's slots are the ports, the party's host (the server's isHost) decides.
  for (const auto& p : match.players)
  {
    if (p.is_local == match.is_host)
      match.room_host_port = p.port;
  }
  // Tests (PPR_TEST_PAIR_HANDOFF_DELAY_MS=<ms>): the partners are connected, and the hand-off
  // waits this long first, so the 2v2 match can be found meanwhile.
  if (const char* delay = std::getenv("PPR_TEST_PAIR_HANDOFF_DELAY_MS"); delay && *delay)
  {
    WARN_LOG_FMT(NETPLAY, "Online: PPR_TEST_PAIR_HANDOFF_DELAY_MS: {} ms", delay);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
  }
  std::lock_guard gate(s_session_gate);
  if (generation != s_pair_generation)
  {
    NOTICE_LOG_FMT(NETPLAY, "Online: the pair room ended before its hand-off; its link closes");
    return;  // ended meanwhile; the link closes with its matchmaking
  }
  if (!Session::HasBackend())
    return;
  // No Ranked or room set state of an earlier session's: the pair room plays none of their rules.
  Ranked::OnSessionStart(match);
  if (auto error = Session::Start(match, std::move(link), {}))
  {
    ERROR_LOG_FMT(NETPLAY, "Online: the pair room's session did not start: {}", *error);
    Ranked::OnSessionStartFailed();
    return;
  }
  s_pair_match = match;
  s_pair_session = true;
  NOTICE_LOG_FMT(NETPLAY, "Online: the pair room's session started ({}, with {})", match.match_id,
                 PairPartner(match));
}

void SetHandoff(std::string state, std::string error = {})
{
  std::lock_guard lk(s_handoff_mutex);
  s_handoff = std::move(state);
  s_handoff_error = std::move(error);
}

User& GetUserLocked()
{
  if (!s_user)
  {
    s_user = std::make_unique<User>(File::GetUserPath(D_ONLINE_IDX) + "user.json",
                                    Config::GetAccountsUrl());
    s_user->ListenForLogIn();
  }
  return *s_user;
}

// Runs on the matchmaking thread. Leaving `link` open makes the matchmaking keep it (until
// Cleanup()).
void OnConnected(u64 generation, const Match& found, P2PLink& link, bool hand_off,
                 const picojson::object& selections, const SearchOptions& room)
{
  // A 2v2 party's pair room ends with its search's match (GameBridge ends it too once it sees the
  // match found, a frame later at most), and its session must be over before this one starts.
  {
    std::lock_guard lk(s_mutex);
    EndPairLocked("the 2v2 match connected");
  }
  for (const auto until = Clock::now() + PAIR_STOP_WAIT; Clock::now() < until;)
  {
    const std::string phase = Session::Status().phase;
    if (phase != "running" && phase != "barrier" && phase != "countdown" &&
        phase != "start_barrier")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Match match = found;
  match.room_host_port = room.room_host_port;
  match.room_teams = room.room_teams;
  match.room_team_colors = room.room_team_colors;
  match.room_pickers = room.room_pickers;
  std::lock_guard gate(s_session_gate);
  if (generation != s_generation)
    return;  // cancelled meanwhile; the link closes with its matchmaking
  if (!hand_off || !Session::HasBackend())
  {
    INFO_LOG_FMT(NETPLAY, "Online: match {} connected; no hand-off ({})", match.match_id,
                 hand_off ? "no session backend" : "not requested");
    SetHandoff("kept");
    return;
  }
  // Ranked's and a room set's game setup before the session: once started, the session can set
  // up game 1 at any moment, and without GameSetup's stage decider it took a random stage, no
  // strikes (seen under load on 2026-10-14, about 130 ms before the setup began).
  Ranked::OnSessionStart(match);
  if (auto error = Session::Start(match, std::move(link), selections))
  {
    ERROR_LOG_FMT(NETPLAY, "Online: session start failed: {}", *error);
    Ranked::OnSessionStartFailed();
    SetHandoff("failed", *error);
    return;
  }
  // Tests (PPR_TEST_HANDOFF_DELAY_MS=<ms>): the hand-off finishes that much after the session
  // started, as on a loaded PC, so the session runs before anything after this point.
  if (const char* delay = std::getenv("PPR_TEST_HANDOFF_DELAY_MS"); delay && *delay)
  {
    WARN_LOG_FMT(NETPLAY, "Online: PPR_TEST_HANDOFF_DELAY_MS: {} ms", delay);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
  }
  SetHandoff("started");
}

picojson::value PlayerJson(const PlayerInfo& p)
{
  picojson::object o;
  o["uid"] = picojson::value(p.uid);
  o["display_name"] = picojson::value(p.display_name);
  o["connect_code"] = picojson::value(p.connect_code);
  o["port"] = picojson::value(static_cast<double>(p.port));
  o["is_local"] = picojson::value(p.is_local);
  o["is_bot"] = picojson::value(p.is_bot);
  o["ip_address"] = picojson::value(p.ip_address);
  o["ip_address_lan"] = picojson::value(p.ip_address_lan);
  return picojson::value(o);
}

picojson::value EndpointsJson(const std::vector<Endpoint>& eps)
{
  picojson::array a;
  for (const auto& e : eps)
    a.emplace_back(e.ToString());
  return picojson::value(a);
}
}  // namespace

User& GetUser()
{
  std::lock_guard lk(s_mutex);
  return GetUserLocked();
}

std::optional<std::string> FindMatch(const SearchOptions& options)
{
  if (options.cancel_on_stop)
  {
    // The game stopping (Dolphin's Stop, the game window closing) while its search still runs:
    // nothing is left to play the match it finds. Without this the search went on, matched, and
    // started a ranked set nobody played; the opponent's "opponent left" then abandoned it, the
    // player's loss (bug report #23, 2026-10-15). A match connected already is the session's and
    // Ranked's (their own stop hooks).
    // Outside s_mutex, which the hook takes.
    std::call_once(s_stop_hook_once, [] {
      s_stop_hook = Core::AddOnStateChangedCallback([](Core::State state) {
        if (state == Core::State::Stopping)
          CancelGameSearch();
      });
    });
  }
  // Outside s_mutex, as Ranked's other hooks.
  Ranked::OnSearch();
  std::lock_guard lk(s_mutex);
  User& user = GetUserLocked();
  if (s_matchmaking && s_matchmaking->IsSearching())
    return std::string("a search is already running (mm_cancel first)");
  if (s_matchmaking && s_matchmaking->GetState() == Matchmaking::State::ConnectionSuccess)
    return std::string("already connected to an opponent (mm_cancel first)");

  // A new search has no pair room yet, nor the last one's error.
  EndPairLocked("a new search");
  s_pair_error.clear();
  s_pair_retry_at = {};
  // Every search gets a fresh matchmaking (Slippi reuses its object until a cleanup replaces it).
  const bool hand_off = options.hand_off;
  u64 generation;
  {
    std::lock_guard gate(s_session_gate);
    generation = ++s_generation;
  }
  s_cancel_on_stop = options.cancel_on_stop ? generation : 0;
  const picojson::object selections = options.selections;
  if (s_matchmaking)
  {
    auto old = std::move(s_matchmaking);
    s_cleanup_threads.emplace_back([old = std::move(old)]() mutable { old.reset(); });
  }
  SearchOptions room;
  room.room_host_port = options.room_host_port;
  room.room_teams = options.room_teams;
  room.room_team_colors = options.room_team_colors;
  room.room_pickers = options.room_pickers;
  s_matchmaking = std::make_unique<Matchmaking>(
      &user, [generation, hand_off, selections, room](const Match& match, P2PLink& link) {
        OnConnected(generation, match, link, hand_off, selections, room);
      });
  SetHandoff("");
  s_last_search = options;
  s_searched = true;
  s_search_mode = static_cast<int>(options.settings.mode);
  NOTICE_LOG_FMT(NETPLAY, "Online: searching ({}) for '{}'",
                 Matchmaking::ModeName(options.settings.mode), options.settings.connect_code);
  s_matchmaking->FindMatch(options.settings);
  return std::nullopt;
}

namespace
{
// Cleanup(); `keep_pair_error`: the pair room ended the search, and its line stays the search's.
void CleanupImpl(bool keep_pair_error)
{
  std::unique_ptr<Matchmaking> old;
  {
    std::lock_guard lk(s_mutex);
    EndPairLocked("the search's cleanup");
    if (!keep_pair_error)
      s_pair_error.clear();
    old = std::move(s_matchmaking);
    s_searched = false;
    s_search_mode = -1;
    s_cancel_on_stop = 0;
  }
  const std::optional<Match> found = old ? old->GetMatch() : std::nullopt;
  {
    // First the generation, under the gate: a match connecting meanwhile is no longer handed off,
    // so Ranked sees either a set that started or one that never will.
    std::lock_guard gate(s_session_gate);
    ++s_generation;
    // Leaving a ranked set before it is over abandons it, and leaving one before its stage select
    // (also one found but not handed off yet) does not count; both are reported to the server.
    Ranked::OnCleanup(found ? &*found : nullptr);
  }
  SetHandoff("");
  // Slippi: doConnectionCleanup runs on its own thread so the game is not blocked while ENet
  // disconnects; a fresh matchmaking (IDLE) is in place at once.
  {
    std::lock_guard gate(s_session_gate);
    Session::Stop();
  }
  std::lock_guard lk(s_mutex);
  if (old)
    s_cleanup_threads.emplace_back([old = std::move(old)]() mutable { old.reset(); });
}
}  // namespace

void Cleanup()
{
  CleanupImpl(false);
}

namespace
{
// Under s_mutex: whether the pair room must end (the 2v2 search no longer waits for its match,
// the party changed, its party ticket failed), and ends it. True: its partners could not connect
// (or the partner is gone), and the 2v2 search ends too (the caller's CleanupImpl, outside
// s_mutex).
bool CheckPairLocked(bool pair_room, const std::string& party)
{
  if (!s_pair)
    return false;
  const Matchmaking* mm = s_matchmaking.get();
  const Matchmaking::State state = mm ? mm->GetState() : Matchmaking::State::Idle;
  const bool waiting = mm && s_search_mode == static_cast<int>(Matchmaking::Mode::Twos) &&
                       state == Matchmaking::State::Matchmaking;
  const Matchmaking::State pair = s_pair->GetState();
  if (waiting && s_pair_over_at && Clock::now() - *s_pair_over_at >= PAIR_GRACE)
  {
    // The partner left the pair room and no match came: the partner is gone.
    s_pair_error = s_pair_over_error;
    EndPairLocked(s_pair_error);
    return true;
  }
  if (pair == Matchmaking::State::ErrorEncountered)
  {
    // The partners could not connect: they would not in the 2v2 match either. Anything else (the
    // server refused the ticket, the partner's search ended first): only the pair room is over.
    const bool connect = s_pair->GetErrorSource() == Matchmaking::ErrorSource::Connection;
    WARN_LOG_FMT(NETPLAY, "Online: the pair room's party ticket failed: {}",
                 s_pair->GetErrorMessage());
    if (connect)
      s_pair_error = fmt::format("Couldn't connect to {}.", PairPartner(s_pair->GetMatch()));
    EndPairLocked(connect ? "the partners could not connect" : "its ticket failed");
    s_pair_retry_at = Clock::now() + PAIR_RETRY;
    return connect;
  }
  if (waiting && pair_room && party == s_pair_party)
    return false;
  // The 2v2 search found its match, failed or was cancelled, or the party changed. A search that
  // failed while the party ticket still connects, or while its session plays: its own outcome
  // first. A partner whose party ticket failed too cancelled their search a moment before ours
  // did, and a partner's hold START ends their search as the same hold ends this session a few
  // frames later (PairRoomOver): neither is "bob cancelled" here.
  if (state == Matchmaking::State::ErrorEncountered &&
      (pair == Matchmaking::State::OpponentConnecting || s_pair_session.load()) && pair_room &&
      party == s_pair_party)
  {
    if (!s_pair_search_gone_at)
      s_pair_search_gone_at = Clock::now();
    if (Clock::now() - *s_pair_search_gone_at < PAIR_GRACE)
      return false;
  }
  EndPairLocked(state == Matchmaking::State::OpponentConnecting ||
                        state == Matchmaking::State::ConnectionSuccess ?
                    "the 2v2 match is found" :
                waiting ? "the party no longer searches together" :
                          "the 2v2 search ended");
  return false;
}

// The pair room's own thread: GameBridge does not run while a match runs, and the pair room's
// match ends as soon as the 2v2 search no longer waits (it checks every PAIR_WATCH, with the
// party's room-state as GameBridge last saw it).
constexpr auto PAIR_WATCH = std::chrono::milliseconds(50);
std::thread s_pair_watcher;  // started with the first pair room (under s_mutex), until Shutdown
std::atomic<bool> s_pair_watcher_quit{false};
bool s_pair_seen_room = false;  // under s_mutex: the last TickPairRoom's
std::string s_pair_seen_party;

void PairWatcher()
{
  while (!s_pair_watcher_quit.load())
  {
    std::this_thread::sleep_for(PAIR_WATCH);
    bool end_search;
    {
      std::lock_guard lk(s_mutex);
      end_search = CheckPairLocked(s_pair_seen_room, s_pair_seen_party);
    }
    if (end_search)
      CleanupImpl(true);
  }
}
}  // namespace

void TickPairRoom(bool pair_room, const std::string& party)
{
  bool end_search = false;
  {
    std::lock_guard lk(s_mutex);
    s_pair_seen_room = pair_room;
    s_pair_seen_party = party;
    if (s_pair)
    {
      end_search = CheckPairLocked(pair_room, party);
    }
    else
    {
      const Matchmaking* mm = s_matchmaking.get();
      // The 2v2 search waits for its match (its ticket is in), both partners search: the pair
      // room's time.
      const bool wanted = mm && s_search_mode == static_cast<int>(Matchmaking::Mode::Twos) &&
                          mm->GetState() == Matchmaking::State::Matchmaking && pair_room &&
                          !party.empty();
      if (!wanted || !s_pair_error.empty() || Clock::now() < s_pair_retry_at)
        return;
      u64 generation;
      {
        std::lock_guard gate(s_session_gate);
        generation = ++s_pair_generation;
      }
      s_pair_party = party;
      s_pair_search_gone_at.reset();
      s_pair = std::make_unique<Matchmaking>(&GetUserLocked(),
                                             [generation](const Match& match, P2PLink& link) {
                                               OnPairConnected(generation, match, link);
                                             });
      Matchmaking::SearchSettings settings;
      settings.mode = Matchmaking::Mode::Teams;
      settings.connect_code = party;
      settings.pair_room = true;
      NOTICE_LOG_FMT(NETPLAY, "Online: party {} searches together: its pair room connects", party);
      s_pair->FindMatch(settings);
      if (!s_pair_watcher.joinable())
        s_pair_watcher = std::thread(PairWatcher);
      return;
    }
  }
  if (end_search)
  {
    NOTICE_LOG_FMT(NETPLAY, "Online: the 2v2 search ends with the pair room: {}", s_pair_error);
    CleanupImpl(true);
  }
}

void PairRoomOver(const std::string& error, bool wait)
{
  {
    std::lock_guard lk(s_mutex);
    if (!s_pair)
      return;
    if (wait)
    {
      if (!s_pair_over_at)
      {
        NOTICE_LOG_FMT(NETPLAY,
                       "Online: the partner left the pair room ({}); the search's match first",
                       error);
        s_pair_over_at = Clock::now();
        s_pair_over_error = error;
      }
      return;
    }
    s_pair_error = error;
    EndPairLocked(error.empty() ? "it was left" : error);
  }
  NOTICE_LOG_FMT(NETPLAY, "Online: the 2v2 search ends with its pair room{}",
                 error.empty() ? std::string() : ": " + error);
  CleanupImpl(true);
}

std::optional<Match> PairRoomMatch()
{
  std::lock_guard gate(s_session_gate);
  return s_pair_match;
}

bool PairRoomSession()
{
  return s_pair_session.load();
}

void CancelGameSearch()
{
  {
    std::lock_guard lk(s_mutex);
    // Under the gate: a match connecting meanwhile either has been handed off (no longer searching)
    // or is not handed off at all (the generation moves on in Cleanup()).
    std::lock_guard gate(s_session_gate);
    if (!s_cancel_on_stop || s_cancel_on_stop != s_generation || !s_matchmaking ||
        !s_matchmaking->IsSearching())
    {
      return;
    }
    s_cancel_on_stop = 0;
    ++s_generation;
  }
  NOTICE_LOG_FMT(NETPLAY, "Online: the game is stopping; cancelling its search");
  Cleanup();
}

void Shutdown()
{
  // The online connection first: its thread reads the user.
  Rooms::Shutdown();
  Ranked::Shutdown();
  std::unique_ptr<Matchmaking> mm;
  std::unique_ptr<User> user;
  std::vector<std::thread> threads;
  {
    std::lock_guard lk(s_mutex);
    mm = std::move(s_matchmaking);
    user = std::move(s_user);
    threads = std::move(s_cleanup_threads);
    s_cleanup_threads.clear();
  }
  mm.reset();
  s_pair_watcher_quit = true;
  if (s_pair_watcher.joinable())
    s_pair_watcher.join();
  {
    std::lock_guard lk(s_mutex);
    EndPairLocked("the process exits");
    threads.insert(threads.end(), std::make_move_iterator(s_cleanup_threads.begin()),
                   std::make_move_iterator(s_cleanup_threads.end()));
    s_cleanup_threads.clear();
  }
  Session::Stop();
  for (auto& t : threads)
  {
    if (t.joinable())
      t.join();
  }
  user.reset();
#ifdef USE_UPNP
  // The last search's port mapping (Matchmaking::StartMatchmaking); it would expire anyway.
  Common::UPnP::UnmapUdpPort();
#endif
}

picojson::object OnlineStatus()
{
  std::unique_lock lk(s_mutex);
  User& user = GetUserLocked();
  const UserInfo info = user.GetUserInfo();
  picojson::object o;
  const AppState state = user.GetAppState();
  o["logged_in"] = picojson::value(!info.uid.empty());
  o["app_state"] = picojson::value(static_cast<double>(state));
  o["app_state_name"] = picojson::value(state == AppState::LoggedOut   ? "logged_out" :
                                        state == AppState::LoggedIn    ? "logged_in" :
                                                                         "update_required");
  o["uid"] = picojson::value(info.uid);
  o["display_name"] = picojson::value(info.display_name);
  o["connect_code"] = picojson::value(info.connect_code);
  o["latest_version"] = picojson::value(info.latest_version);
  o["app_version"] = picojson::value(APP_VERSION);
  o["has_play_key"] = picojson::value(!info.play_key.empty());
  const char* fetch = "not_fetched";
  switch (user.GetFetchStatus())
  {
  case UserFetchStatus::NotFetched:
    break;
  case UserFetchStatus::Fetching:
    fetch = "fetching";
    break;
  case UserFetchStatus::Fetched:
    fetch = "fetched";
    break;
  case UserFetchStatus::Error:
    fetch = "error";
    break;
  }
  o["user_fetch"] = picojson::value(fetch);
  o["watching"] = picojson::value(user.IsWatching());
  o["user_json_path"] = picojson::value(user.GetUserJsonPath());
  o["mm_server"] = picojson::value(fmt::format("{}:{}", Config::GetMatchmakingHost(),
                                               Config::GetMatchmakingPort()));
  o["accounts_url"] = picojson::value(Config::GetAccountsUrl());
  lk.unlock();  // GetRankInfo reads the user through GetUser(), which takes s_mutex
  const Ranked::RankInfo rank = Ranked::GetRankInfo();
  picojson::object r = Ranked::Status();
  r["rank_state"] = picojson::value(static_cast<double>(rank.state));
  r["user_rating"] = picojson::value(static_cast<double>(rank.rating));
  r["sets_played"] = picojson::value(static_cast<double>(rank.sets_played));
  o["ranked"] = picojson::value(r);
  return o;
}

std::optional<Matchmaking::Mode> SearchMode()
{
  const int mode = s_search_mode.load(std::memory_order_relaxed);
  if (mode < 0)
    return std::nullopt;
  return static_cast<Matchmaking::Mode>(mode);
}

MatchState GetMatchState()
{
  MatchState ms;
  {
    std::lock_guard lk(s_mutex);
    if (const Matchmaking* mm = s_matchmaking.get())
    {
      ms.state = mm->GetState();
      ms.error = mm->GetErrorMessage();
      ms.match = mm->GetMatch();
    }
    // The pair room ended the 2v2 search (TickPairRoom, PairRoomOver): its line.
    if (!s_pair_error.empty() && ms.state == Matchmaking::State::Idle)
    {
      ms.state = Matchmaking::State::ErrorEncountered;
      ms.error = s_pair_error;
    }
  }
  {
    std::lock_guard hk(s_handoff_mutex);
    ms.handoff = s_handoff;
  }
  ms.session = Session::Status();
  return ms;
}

picojson::object MatchmakingStatus()
{
  picojson::object o;
  std::lock_guard lk(s_mutex);
  const Matchmaking* mm = s_matchmaking.get();
  const Matchmaking::State state = mm ? mm->GetState() : Matchmaking::State::Idle;
  o["state"] = picojson::value(Matchmaking::StateName(state));
  o["state_code"] = picojson::value(static_cast<double>(state));
  o["searching"] = picojson::value(mm && mm->IsSearching());
  o["error"] = picojson::value(mm ? mm->GetErrorMessage() : std::string());
  o["error_source"] =
      picojson::value(Matchmaking::ErrorSourceName(mm ? mm->GetErrorSource() :
                                                        Matchmaking::ErrorSource::None));
  if (s_searched)
  {
    o["mode"] = picojson::value(Matchmaking::ModeName(s_last_search.settings.mode));
    o["opponent_code"] = picojson::value(s_last_search.settings.connect_code);
  }
  if (mm)
  {
    o["server"] = picojson::value(mm->GetServerAddress());
    o["local_port"] = picojson::value(static_cast<double>(mm->GetLocalPort()));
    o["lan_address"] = picojson::value(mm->GetLanAddress());
    o["tickets"] = picojson::value(static_cast<double>(mm->GetTicketCount()));
    o["connect_attempts"] = picojson::value(static_cast<double>(mm->GetConnectAttempts()));
    if (const auto match = mm->GetMatch())
    {
      picojson::object m;
      m["match_id"] = picojson::value(match->match_id);
      m["is_host"] = picojson::value(match->is_host);
      m["local_player_index"] = picojson::value(static_cast<double>(match->local_player_index));
      m["local_port"] = picojson::value(static_cast<double>(match->local_port));
      picojson::array players;
      for (const auto& p : match->players)
        players.push_back(PlayerJson(p));
      m["players"] = picojson::value(players);
      picojson::array stages;
      for (const u16 s : match->stages)
        stages.emplace_back(static_cast<double>(s));
      m["stages"] = picojson::value(stages);
      m["items"] = picojson::value(static_cast<double>(match->items));
      m["rules"] = match->rules ? picojson::value(ToJson(*match->rules)) : picojson::value();
      m["remote_addresses"] = EndpointsJson(match->remotes);
      m["connected_addresses"] = EndpointsJson(match->connected);
      m["connect_ms"] = picojson::value(static_cast<double>(match->connect_ms));
      o["match"] = picojson::value(m);
    }
  }
  {
    std::lock_guard hk(s_handoff_mutex);
    o["handoff"] = picojson::value(s_handoff);
    o["handoff_error"] = picojson::value(s_handoff_error);
  }
  // A 2v2 party's pair room (OnlineClient.h).
  {
    picojson::object p;
    p["state"] = picojson::value(
        Matchmaking::StateName(s_pair ? s_pair->GetState() : Matchmaking::State::Idle));
    p["party"] = picojson::value(s_pair_party);
    p["error"] = picojson::value(s_pair_error);
    p["session"] = picojson::value(s_pair_session.load());
    if (s_pair)
      p["local_port"] = picojson::value(static_cast<double>(s_pair->GetLocalPort()));
    o["pair_room"] = picojson::value(p);
  }
  const SessionStatus session = Session::Status();
  picojson::object s;
  s["backend"] = picojson::value(session.backend);
  s["phase"] = picojson::value(session.phase);
  s["error"] = picojson::value(session.error);
  s["detail"] = picojson::value(session.detail);
  o["session"] = picojson::value(s);
  return o;
}
}  // namespace Online::Client
