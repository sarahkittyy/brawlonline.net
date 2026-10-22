// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/OnlineClient.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
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
  std::lock_guard lk(s_mutex);
  User& user = GetUserLocked();
  if (s_matchmaking && s_matchmaking->IsSearching())
    return std::string("a search is already running (mm_cancel first)");
  if (s_matchmaking && s_matchmaking->GetState() == Matchmaking::State::ConnectionSuccess)
    return std::string("already connected to an opponent (mm_cancel first)");

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

void Cleanup()
{
  std::unique_ptr<Matchmaking> old;
  {
    std::lock_guard lk(s_mutex);
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
