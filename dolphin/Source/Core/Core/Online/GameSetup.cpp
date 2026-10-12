// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/GameSetup.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Logging/Log.h"
#include "Core/Rollback/GameplaySession.h"

namespace Online::GameSetup
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr u16 NO_STAGE = 0xFFFF;
// A player's actions for one game (at most a striking pattern's: 8 steps of up to 8, or the bans
// and the pick); bound what a peer sends.
constexpr size_t MAX_ACTIONS = 64;
// Slippi's GameSetup.h: a step's own time is over GRACE_SECONDS after its timer shows 0, and the
// opponent's WAIT_TIMEOUT_SECONDS after that. Its character steps have 45 s.
constexpr int GRACE_SECONDS = 3;
constexpr int WAIT_TIMEOUT_SECONDS = 15;
constexpr int CHAR_SECONDS = 45;
// Timer ids of the character steps (after the stage steps' indices).
constexpr int TIMER_WINNER_CHAR = 100;
constexpr int TIMER_LOSER_CHAR = 101;

// Lock order: never call into Gprb::Session while holding s_mutex (the session calls the stage
// decider with its own lock held).
std::mutex s_mutex;
bool s_active = false;
int s_local = 0;       // this player's side
int s_local_port = 0;  // this player's in-game port
Format s_format;
SetState s_set;
// A room's set: the set's game of the session's game 1, and each side's in-game port.
u32 s_first_game = 1;
std::array<u8, 2> s_side_ports{0, 1};  // each side's first port
std::array<std::vector<u8>, 2> s_members{std::vector<u8>{0}, std::vector<u8>{1}};
// The session host's in-game port: in a set of two-player sides its machine decides which presses
// count (SetFormat.h TakePresses).
int s_host_port = 0;
// The session's game 1's actions so far, per side (the ticket's: a session started again).
std::array<std::vector<u8>, 2> s_seed;
// The last TakeStepReport.
std::optional<StepReport> s_reported;
u32 s_act_game = 0;
std::vector<u8> s_actions;  // 1v1: this player's actions for s_act_game, in order
// Sides of two: this player's presses for s_act_game; on the host, its lists for s_host_game; on
// the others, how many of their presses the host has answered, and when it last dropped one.
std::vector<Press> s_presses;
SharedSteps s_host;
u32 s_host_game = 0;
u32 s_seen_taken = 0;
std::optional<Clock::time_point> s_overruled;
std::string s_last_extra;  // the extra last published
u32 s_timer_game = 0;
int s_timer_step = -1;
Clock::time_point s_timer_start;
u64 s_auto_actions = 0;
bool s_opponent_stalled = false;
View s_last_view;  // the last Current() (harness)
std::mt19937 s_rng{std::random_device{}()};

// gmCharacterKind -> the name the game shows (P+ v3.2's roster).
const char* CharName(u8 kind)
{
  switch (kind)
  {
  case 0x00: return "Mario";
  case 0x01: return "Donkey Kong";
  case 0x02: return "Link";
  case 0x03: return "Samus";
  case 0x04: return "Zero Suit Samus";
  case 0x05: return "Yoshi";
  case 0x06: return "Kirby";
  case 0x07: return "Fox";
  case 0x08: return "Pikachu";
  case 0x09: return "Luigi";
  case 0x0A: return "Captain Falcon";
  case 0x0B: return "Ness";
  case 0x0C: return "Bowser";
  case 0x0D: return "Peach";
  case 0x0E: return "Zelda";
  case 0x0F: return "Sheik";
  case 0x10: case 0x11: return "Ice Climbers";
  case 0x13: return "Marth";
  case 0x14: return "Mr. Game & Watch";
  case 0x15: return "Falco";
  case 0x16: return "Ganondorf";
  case 0x17: return "Wario";
  case 0x18: return "Meta Knight";
  case 0x19: return "Pit";
  case 0x1A: return "Olimar";
  case 0x1B: return "Lucas";
  case 0x1C: return "Diddy Kong";
  case 0x1D: case 0x1F: case 0x21: return "Pokemon Trainer";
  case 0x1E: return "Charizard";
  case 0x20: return "Squirtle";
  case 0x22: return "Ivysaur";
  case 0x23: return "King Dedede";
  case 0x24: return "Lucario";
  case 0x25: return "Ike";
  case 0x26: return "R.O.B.";
  case 0x27: return "Jigglypuff";
  case 0x28: return "Toon Link";
  case 0x29: return "Wolf";
  case 0x2A: return "Snake";
  case 0x2B: return "Sonic";
  case 0x32: return "Roy";
  case 0x33: return "Mewtwo";
  case 0x35: return "Knuckles";
  default: return "a character";
  }
}

using Extras = std::array<picojson::object, MAX_PORTS>;

// Every port's extra (Gprb::Session::GetPlayerExtra: what that player's machine sends; {} for
// this player). Called without s_mutex.
Extras PlayerExtras()
{
  Extras out;
  for (int p = 0; p < MAX_PORTS; ++p)
    out[p] = Gprb::Session::GetPlayerExtra(p);
  return out;
}

// The extra's `gs` for the session's `game`, or nullptr.
const picojson::object* GsOf(const picojson::object& extra, u32 game)
{
  const auto gs = extra.find("gs");
  if (gs == extra.end() || !gs->second.is<picojson::object>())
    return nullptr;
  const auto& o = gs->second.get<picojson::object>();
  const auto g = o.find("g");
  if (g == o.end() || !g->second.is<double>() || g->second.get<double>() != static_cast<double>(game))
    return nullptr;
  return &o;
}

// A list of stage kinds (1-255), at most MAX_ACTIONS; anything else ends it.
std::vector<u8> KindsFrom(const picojson::value* v)
{
  std::vector<u8> out;
  if (!v || !v->is<picojson::array>())
    return out;
  for (const auto& x : v->get<picojson::array>())
  {
    if (out.size() >= MAX_ACTIONS || !x.is<double>())
      break;
    const double d = x.get<double>();
    if (!(d >= 1 && d <= 0xFF && d == static_cast<double>(static_cast<int>(d))))
      break;
    out.push_back(static_cast<u8>(d));
  }
  return out;
}

const picojson::value* Field(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}

// 1v1: the player's own actions (`gs.a`).
std::vector<u8> ActionsFrom(const picojson::object& extra, u32 game)
{
  const picojson::object* gs = GsOf(extra, game);
  return gs ? KindsFrom(Field(*gs, "a")) : std::vector<u8>{};
}

// Sides of two: the player's presses (`gs.p`: step, stage, step, stage...).
std::vector<Press> PressesFrom(const picojson::object& extra, u32 game)
{
  std::vector<Press> out;
  const picojson::object* gs = GsOf(extra, game);
  const picojson::value* p = gs ? Field(*gs, "p") : nullptr;
  if (!p || !p->is<picojson::array>())
    return out;
  const auto& arr = p->get<picojson::array>();
  for (size_t i = 0; i + 1 < arr.size() && out.size() < MAX_ACTIONS; i += 2)
  {
    if (!arr[i].is<double>() || !arr[i + 1].is<double>())
      break;
    const double step = arr[i].get<double>(), kind = arr[i + 1].get<double>();
    if (step < 0 || step > 63 || kind < 1 || kind > 0xFF)
      break;
    out.push_back({static_cast<u8>(step), static_cast<u8>(kind)});
  }
  return out;
}

// Sides of two: the host's lists (`gs.h`) and how many presses of each port it took (`gs.n`).
std::optional<SharedSteps> HostStepsFrom(const picojson::object& extra, u32 game)
{
  const picojson::object* gs = GsOf(extra, game);
  const picojson::value* h = gs ? Field(*gs, "h") : nullptr;
  const picojson::value* n = gs ? Field(*gs, "n") : nullptr;
  if (!h || !h->is<picojson::array>() || h->get<picojson::array>().size() != 2 || !n ||
      !n->is<picojson::array>() || n->get<picojson::array>().size() != MAX_PORTS)
  {
    return std::nullopt;
  }
  SharedSteps out;
  for (int side = 0; side < 2; ++side)
    out.actions[side] = KindsFrom(&h->get<picojson::array>()[side]);
  for (int p = 0; p < MAX_PORTS; ++p)
  {
    const picojson::value& x = n->get<picojson::array>()[p];
    out.taken[p] = x.is<double>() && x.get<double>() >= 0 && x.get<double>() <= MAX_ACTIONS ?
                       static_cast<u32>(x.get<double>()) :
                       0;
  }
  return out;
}

// Under s_mutex: each side has two players (2v2): either teammate strikes, bans and picks, and the
// session host's machine decides which presses count (SetFormat.h).
bool Shared()
{
  return s_members[0].size() > 1 || s_members[1].size() > 1;
}

bool IsHost()
{
  return s_local_port == s_host_port;
}

// Under s_mutex: the session's `game` is the one this player acts in (its own actions start over;
// 1v1: from the ticket's for the session's first game). True when they changed.
bool EnterGame(u32 game)
{
  if (s_act_game == game)
    return false;
  s_act_game = game;
  s_actions = game == 1 && !Shared() ? s_seed[s_local] : std::vector<u8>{};
  s_presses.clear();
  s_seen_taken = 0;
  s_overruled.reset();
  return true;
}

// Under s_mutex, host of a set of two-player sides: takes every player's new presses for `game`.
void HostTake(u32 game, const Extras& extras)
{
  if (s_host_game != game)
  {
    s_host_game = game;
    s_host = {};
    if (game == 1)
      s_host.actions = s_seed;
  }
  std::array<std::vector<Press>, MAX_PORTS> presses;
  std::array<int, MAX_PORTS> side_of{-1, -1, -1, -1};
  for (int side = 0; side < 2; ++side)
  {
    for (const u8 p : s_members[side])
    {
      if (p < MAX_PORTS)
        side_of[p] = side;
    }
  }
  for (int p = 0; p < MAX_PORTS; ++p)
  {
    presses[p] = p == s_local_port ? (s_act_game == game ? s_presses : std::vector<Press>{}) :
                                     PressesFrom(extras[p], game);
  }
  for (const auto& [port, press] :
       TakePresses(s_format, s_set, s_first_game - 1 + game, side_of, presses, s_host))
  {
    INFO_LOG_FMT(NETPLAY, "GameSetup: game {}: P{}'s {:#x} for step {} dropped", game, port + 1,
                 press.stage, press.step);
    if (port == s_local_port)
      s_overruled = Clock::now();
  }
}

// Under s_mutex: side `side`'s actions in the session's `game` (1v1): this player's own for its
// side, else what the other player's machine sends; the session's first game has at least the
// ticket's.
std::vector<u8> SideActions(int side, u32 game, const Extras& extras)
{
  std::vector<u8> a = side == s_local ? (s_act_game == game ? s_actions : std::vector<u8>{}) :
                                        ActionsFrom(extras[s_side_ports[side]], game);
  if (game == 1 && s_seed[side].size() > a.size())
    a = s_seed[side];
  return a;
}

// Under s_mutex: the lists every machine plays for the session's `game`: 1v1 each player's own;
// sides of two the host's (the host takes the new presses first).
std::array<std::vector<u8>, 2> SideLists(u32 game, const Extras& extras)
{
  if (!Shared())
    return {SideActions(0, game, extras), SideActions(1, game, extras)};
  std::array<std::vector<u8>, 2> lists;
  if (IsHost())
  {
    HostTake(game, extras);
    lists = s_host.actions;
  }
  else if (const auto h = HostStepsFrom(extras[s_host_port], game))
  {
    lists = h->actions;
  }
  for (int side = 0; side < 2 && game == 1; ++side)
  {
    if (s_seed[side].size() > lists[side].size())
      lists[side] = s_seed[side];
  }
  return lists;
}

// Under s_mutex: what this player shows: the lists played, and (sides of two, not the host) its
// own presses the host has not answered yet on top. A press the host answered without playing
// its stage was overruled (the partner was first): noted for the status line.
std::array<std::vector<u8>, 2> ShownLists(u32 game, const Extras& extras)
{
  std::array<std::vector<u8>, 2> lists = SideLists(game, extras);
  if (!Shared() || IsHost() || s_act_game != game)
    return lists;
  const auto h = HostStepsFrom(extras[s_host_port], game);
  const u32 taken = std::min<u32>(h ? h->taken[s_local_port] : 0, static_cast<u32>(s_presses.size()));
  for (u32 i = s_seen_taken; i < taken; ++i)
  {
    const auto& mine = lists[s_local];
    if (std::find(mine.begin(), mine.end(), s_presses[i].stage) == mine.end())
    {
      s_overruled = Clock::now();
      INFO_LOG_FMT(NETPLAY, "GameSetup: game {}: our {:#x} for step {} was overruled", game,
                   s_presses[i].stage, s_presses[i].step);
    }
  }
  s_seen_taken = std::max(s_seen_taken, taken);
  const std::vector<Press> pending(s_presses.begin() + taken, s_presses.end());
  return WithPending(s_format, s_set, s_first_game - 1 + game, s_local, lists, pending);
}

// Under s_mutex. The steps of the session's `game` as played (SetFormat.cpp): the set's game
// number decides striking and the first striker.
Steps Evaluate(u32 game, const Extras& extras)
{
  const auto lists = SideLists(game, extras);
  return EvaluateSteps(s_format, s_set, s_first_game - 1 + game, {&lists[0], &lists[1]});
}

// Under s_mutex. The steps as this player shows them (its pending presses included).
Steps EvaluateShown(u32 game, const Extras& extras)
{
  const auto lists = ShownLists(game, extras);
  return EvaluateSteps(s_format, s_set, s_first_game - 1 + game, {&lists[0], &lists[1]});
}

picojson::value KindsJson(const std::vector<u8>& kinds)
{
  picojson::array a;
  for (const u8 k : kinds)
    a.emplace_back(static_cast<double>(k));
  return picojson::value(a);
}

// Under s_mutex: this player's extra for `game` when it changed since the last one published.
// 1v1: `gs.a`, its actions. Sides of two: `gs.p`, its presses; the host also `gs.h` (the lists
// played) and `gs.n` (the presses it took per port).
std::optional<picojson::object> ToPublish(u32 game)
{
  picojson::object gs;
  gs["g"] = picojson::value(static_cast<double>(game));
  if (!Shared())
  {
    gs["a"] = KindsJson(s_act_game == game ? s_actions : std::vector<u8>{});
  }
  else
  {
    picojson::array p;
    for (const Press& x : s_act_game == game ? s_presses : std::vector<Press>{})
    {
      p.emplace_back(static_cast<double>(x.step));
      p.emplace_back(static_cast<double>(x.stage));
    }
    gs["p"] = picojson::value(p);
    if (IsHost() && s_host_game == game)
    {
      gs["h"] = picojson::value(picojson::array{KindsJson(s_host.actions[0]), KindsJson(s_host.actions[1])});
      picojson::array n;
      for (const u32 t : s_host.taken)
        n.emplace_back(static_cast<double>(t));
      gs["n"] = picojson::value(n);
    }
  }
  picojson::object o;
  o["gs"] = picojson::value(gs);
  std::string text = picojson::value(o).serialize();
  if (text == s_last_extra)
    return std::nullopt;
  s_last_extra = std::move(text);
  return o;
}

// Under s_mutex.
bool SetOver()
{
  return GameSetup::SetOver(s_format, s_set);
}

// Under s_mutex: whole seconds since timer `step` of `game` started on this machine (it starts
// the first time it is asked for).
int Elapsed(u32 game, int step)
{
  if (s_timer_game != game || s_timer_step != step)
  {
    s_timer_game = game;
    s_timer_step = step;
    s_timer_start = Clock::now();
  }
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - s_timer_start).count());
}

// Under s_mutex: the seconds a timer of `seconds` shows after `elapsed`; notes an opponent's step
// that ran out long ago.
u8 TimerShown(int seconds, int elapsed, bool mine)
{
  if (!mine && elapsed > seconds + GRACE_SECONDS + WAIT_TIMEOUT_SECONDS && !s_opponent_stalled)
  {
    s_opponent_stalled = true;
    WARN_LOG_FMT(NETPLAY, "GameSetup: the opponent's step ran out {} s ago", elapsed - seconds);
  }
  return static_cast<u8>(std::clamp(seconds - elapsed, 0, 99));
}

// The distinct stage kinds of a list that a stage step can use.
size_t Usable(const std::vector<u16>& list)
{
  std::vector<u16> seen;
  for (const u16 k : list)
  {
    if (k != 0 && k <= 0xFF && std::find(seen.begin(), seen.end(), k) == seen.end())
      seen.push_back(k);
  }
  return seen.size();
}

Gprb::Session::StageDecision Decide(u32 game)
{
  // Called by the session with its lock held: read the players' extras first, then take ours.
  const Extras extras = PlayerExtras();
  std::lock_guard lk(s_mutex);
  Gprb::Session::StageDecision d;
  if (!s_active)
    return d;
  d.applies = true;
  if (SetOver())
  {
    // The set is decided: no game after it (the connection closes on the CSS).
    d.wait = true;
    return d;
  }
  const Steps e = Evaluate(game, extras);
  if (e.step_idx >= 0 || e.stage == NO_STAGE)
  {
    d.wait = true;
    return d;
  }
  d.stage = e.stage;
  return d;
}
}  // namespace

const std::vector<u16>& DefaultStarters()
{
  static const std::vector<u16> starters{0x01, 0x02, 0x21, 0x2D, 0x2E};
  return starters;
}

void Begin(int local_port, std::vector<u16> starters, std::vector<u16> counterpicks)
{
  // Striking 1-2-1 leaves one of exactly five; a game 2+ needs a stage after the two bans and
  // Dave's rule. Both machines get the same lists from the server, so both fall back alike.
  if (!starters.empty() && (starters.size() != 5 || Usable(starters) != 5))
  {
    WARN_LOG_FMT(NETPLAY, "GameSetup: {} starters from the server; using P+'s five",
                 starters.size());
    starters.clear();
  }
  if (!counterpicks.empty() && Usable(counterpicks) < 4)
  {
    WARN_LOG_FMT(NETPLAY, "GameSetup: {} counterpick stages from the server; using P+'s list",
                 counterpicks.size());
    counterpicks.clear();
  }
  Begin(local_port, RankedFormat(starters.empty() ? DefaultStarters() : std::move(starters),
                                 counterpicks.empty() ? Gprb::Session::DefaultStages() :
                                                        std::move(counterpicks)));
}

void Begin(int local_port, const SetTicket& ticket, int host_port)
{
  const int side =
      std::find(ticket.members[1].begin(), ticket.members[1].end(), local_port) != ticket.members[1].end() ? 1 : 0;
  Begin(side, ticket.format);
  std::lock_guard lk(s_mutex);
  s_local_port = local_port;
  s_set = ticket.state;
  s_first_game = ticket.first_game;
  s_side_ports = ticket.side_ports;
  s_members = ticket.members;
  s_seed = ticket.actions;
  s_host_port = host_port;
  NOTICE_LOG_FMT(NETPLAY,
                 "GameSetup: from the set's game {} ({}-{}), sides on ports {} and {}, {} and {} "
                 "steps made{}",
                 s_first_game, s_set.wins[0], s_set.wins[1], fmt::join(s_members[0], "+"),
                 fmt::join(s_members[1], "+"), s_seed[0].size(), s_seed[1].size(),
                 Shared() ? fmt::format(", either teammate presses, P{} decides", host_port + 1) : "");
}

u32 SetGameOf(u32 session_game)
{
  std::lock_guard lk(s_mutex);
  return s_first_game - 1 + session_game;
}

std::optional<u8> SideOfPort(u8 port)
{
  std::lock_guard lk(s_mutex);
  if (port == 0xFE)
    return port;
  for (u8 side = 0; side < 2; ++side)
  {
    if (std::find(s_members[side].begin(), s_members[side].end(), port) != s_members[side].end())
      return side;
  }
  return std::nullopt;
}

std::optional<StepReport> TakeStepReport()
{
  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  const Extras extras = PlayerExtras();
  std::lock_guard lk(s_mutex);
  if (!s_active || s_format.best_of == 0 || !lobby.active || SetOver())
    return std::nullopt;
  StepReport r;
  r.set_game = s_first_game - 1 + lobby.game;
  r.actions = SideLists(lobby.game, extras);
  r.match_started = lobby.in_match;
  if (s_reported && s_reported->set_game == r.set_game && s_reported->actions == r.actions &&
      s_reported->match_started == r.match_started)
  {
    return std::nullopt;
  }
  s_reported = r;
  return r;
}

bool SetDone()
{
  std::lock_guard lk(s_mutex);
  return !s_active || SetOver();
}

void Begin(int local_port, Format format)
{
  if (const auto why = CheckFormat(format))
  {
    WARN_LOG_FMT(NETPLAY, "GameSetup: the set's format cannot be played ({}); Ranked's instead",
                 *why);
    format = RankedFormat(DefaultStarters(), Gprb::Session::DefaultStages());
  }
  const int best_of = format.best_of;
  {
    std::lock_guard lk(s_mutex);
    s_active = true;
    s_local = local_port == 1 ? 1 : 0;
    s_local_port = s_local;
    s_format = std::move(format);
    s_set = SetState{};
    s_first_game = 1;
    s_side_ports = {0, 1};
    s_members = {std::vector<u8>{0}, std::vector<u8>{1}};
    s_host_port = 0;
    s_seed = {};
    s_reported.reset();
    s_act_game = 0;
    s_actions.clear();
    s_presses.clear();
    s_host = {};
    s_host_game = 0;
    s_seen_taken = 0;
    s_overruled.reset();
    s_last_extra.clear();
    s_timer_step = -1;
    s_auto_actions = 0;
    s_opponent_stalled = false;
  }
  Gprb::Session::SetLocalExtra({});
  Gprb::Session::SetStageDecider(Decide);
  NOTICE_LOG_FMT(NETPLAY, "GameSetup: set (best of {}), player {}", best_of, local_port + 1);
}

void End()
{
  {
    std::lock_guard lk(s_mutex);
    if (!s_active)
      return;
    s_active = false;
  }
  Gprb::Session::SetStageDecider(nullptr);
  Gprb::Session::SetLocalExtra({});
}

bool IsActive()
{
  std::lock_guard lk(s_mutex);
  return s_active;
}

void OnGameResult(u8 winner, u16 stage)
{
  std::lock_guard lk(s_mutex);
  if (!s_active)
    return;
  // The winner's side (Ranked: ports 0 and 1 are the sides; teams: the winning team's lowest port).
  if (winner != 0xFE)
  {
    const auto in = [winner](const std::vector<u8>& m) {
      return std::find(m.begin(), m.end(), winner) != m.end();
    };
    winner = in(s_members[0]) ? 0 : in(s_members[1]) ? 1 : 0xFF;
  }
  s_set.last_winner = winner;
  if (winner < 2)
  {
    ++s_set.wins[winner];
    s_set.last_win_stage[winner] = stage;
  }
}

void OnGameNotCounted()
{
  std::lock_guard lk(s_mutex);
  if (!s_active)
    return;
  --s_first_game;  // u32: SetGameOf's s_first_game - 1 + session_game stays right modulo 2^32
  s_reported.reset();
}

bool Act(u8 kind)
{
  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  const Extras extras = PlayerExtras();
  std::optional<picojson::object> publish;
  {
    std::lock_guard lk(s_mutex);
    if (!s_active || !lobby.active || lobby.in_match || lobby.setup_ready)
      return false;
    const u32 game = lobby.game;
    EnterGame(game);
    const Steps e = EvaluateShown(game, extras);
    if (e.step_idx < 0 || e.steps[e.step_idx].player != s_local ||
        std::find(e.remaining.begin(), e.remaining.end(), kind) == e.remaining.end() ||
        (Shared() ? s_presses.size() : s_actions.size()) >= MAX_ACTIONS)
    {
      return false;
    }
    if (Shared())
    {
      // Shown at once; it counts once the host's machine takes it (the host's own at once).
      s_presses.push_back({static_cast<u8>(e.step_idx), kind});
      if (IsHost())
        HostTake(game, extras);
    }
    else
    {
      s_actions.push_back(kind);
    }
    INFO_LOG_FMT(NETPLAY, "GameSetup: game {}: {} {:#x}", game,
                 e.steps[e.step_idx].type == StepType::Pick ? "picked" : "struck", kind);
    publish = ToPublish(game);
  }
  if (publish)
    Gprb::Session::SetLocalExtra(*publish);
  return true;
}

View Current()
{
  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  const Extras extras = PlayerExtras();
  std::array<std::optional<Gprb::Session::LockIn>, MAX_PORTS> locks;
  for (int p = 0; p < MAX_PORTS; ++p)
    locks[p] = Gprb::Session::GetPlayerLock(p);
  View v;
  std::optional<picojson::object> publish;
  {
    std::lock_guard lk(s_mutex);
    if (!s_active || !lobby.active || !lobby.connected || lobby.in_match || lobby.disconnected ||
        lobby.setup_ready)
    {
      s_last_view = v;
      return v;
    }
    const u32 game = lobby.game;
    EnterGame(game);
    v.active = true;
    v.game = game;
    if (SetOver())
    {
      // The set is decided: nothing more to choose (the connection closes on the CSS).
      v.type = StepType::Done;
      v.may_lock = false;
      v.text = "Set complete";
      s_last_view = v;
      return v;
    }
    Steps e = EvaluateShown(game, extras);
    if (e.step_idx >= 0)
    {
      const StepInfo& st = e.steps[e.step_idx];
      // Slippi: a step whose time runs out is completed with a random selection, by the player
      // whose step it is (1v1), or for sides of two by the host's machine, which decides them.
      const bool decides = Shared() ? IsHost() : st.player == s_local;
      std::vector<u8>& list = Shared() ? s_host.actions[st.player] : s_actions;
      if (decides && Elapsed(game, e.step_idx) >= st.seconds + GRACE_SECONDS &&
          !e.remaining.empty() && list.size() < MAX_ACTIONS)
      {
        std::uniform_int_distribution<size_t> pick(0, e.remaining.size() - 1);
        const u8 k = e.remaining[pick(s_rng)];
        list.push_back(k);
        ++s_auto_actions;
        NOTICE_LOG_FMT(NETPLAY, "GameSetup: game {}: time is up, {} {:#x} for side {}", game,
                       st.type == StepType::Pick ? "picked" : "struck", k, st.player);
        e = EvaluateShown(game, extras);
      }
    }
    v.selectable = e.remaining;
    if (e.step_idx >= 0)
    {
      const StepInfo& st = e.steps[e.step_idx];
      // Sides of two: either teammate presses for the side.
      const bool mine = st.player == s_local;
      v.type = st.type;
      v.my_turn = mine;
      v.count = static_cast<u8>(e.left);
      v.to_sss = true;
      v.may_lock = false;
      v.seconds = TimerShown(st.seconds, Elapsed(game, e.step_idx), mine);
      const char* who = s_members[1 - s_local].size() > 1 ? "Opponents are" : "Opponent is";
      std::string what;
      if (st.type == StepType::Pick)
        what = mine ? "Pick a stage" : fmt::format("{} picking", who);
      else if (!e.striking && mine)
        what = e.left == 1 ? "Ban 1 stage" : fmt::format("Ban {} stages", e.left);
      else if (!e.striking)
        what = fmt::format("{} banning", who);
      else if (mine)
        what = e.left == 1 ? "Strike 1 stage" : fmt::format("Strike {} stages", e.left);
      else
        what = fmt::format("{} striking", who);
      // A press of ours the host's machine dropped (the partner was first): said for 2 s, while
      // the stage it showed goes back (the step has usually gone on to the other team).
      if (s_overruled && Clock::now() - *s_overruled < std::chrono::seconds(2))
        what = "Partner was first";
      v.text = fmt::format("{} ({})", what, v.seconds);
    }
    else
    {
      v.stage = e.stage;
      if (e.striking)
      {
        // Game 1: the characters locked in for the search. After a draw: the last game's (Slippi
        // strikes again without a character step), so the CSS locks in again at once.
        v.type = StepType::Done;
        v.time_up = true;
      }
      else
      {
        // Slippi: the winner chooses a character first, then the loser, seeing it; 45 s each,
        // then the character on the CSS is taken. In 2v2 each side's players together.
        const u8 winner_side = s_set.last_winner <= 1 ? s_set.last_winner : 0;
        const bool winner = winner_side == s_local;
        const auto side_locked = [&](int side) {
          for (const u8 p : s_members[side])
          {
            const auto& l = locks[p];
            if (!l || !l->ready || l->game != game || l->char_kind == 0xFF)
              return false;
          }
          return true;
        };
        const bool winner_locked = side_locked(winner_side);
        const bool loser_locked = side_locked(1 - winner_side);
        const auto& peer_lock = locks[s_side_ports[winner_side]];
        const bool pair = s_members[0].size() > 1;
        v.type = StepType::Char;
        if (!winner_locked || !loser_locked)
        {
          const bool mine = winner != winner_locked;  // the winner's step, then the loser's
          const int elapsed = Elapsed(game, winner_locked ? TIMER_LOSER_CHAR : TIMER_WINNER_CHAR);
          v.my_turn = mine;
          v.seconds = TimerShown(CHAR_SECONDS, elapsed, mine);
          v.time_up = mine && elapsed >= CHAR_SECONDS + GRACE_SECONDS;
          if (mine && winner)
            v.text = fmt::format("Press START to lock in ({})", v.seconds);
          else if (mine && pair)
            v.text = fmt::format("Opponents locked in ({})", v.seconds);
          else if (mine)
            v.text = fmt::format("Opponent picked {} ({})",
                                 CharName(peer_lock ? peer_lock->char_kind : 0xFF), v.seconds);
          else
            v.text = fmt::format("{} choosing ({})", pair ? "Opponents are" : "Opponent is", v.seconds);
        }
        v.may_lock = v.my_turn;
      }
    }
  }
  {
    std::lock_guard lk(s_mutex);
    s_last_view = v;
    publish = ToPublish(v.game);
  }
  if (publish)
    Gprb::Session::SetLocalExtra(*publish);
  return v;
}

bool OpponentStalled()
{
  std::lock_guard lk(s_mutex);
  return s_active && s_opponent_stalled;
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  picojson::object o;
  o["active"] = picojson::value(s_active);
  o["local_port"] = picojson::value(static_cast<double>(s_local));
  o["last_winner"] = picojson::value(static_cast<double>(s_set.last_winner));
  o["wins"] = picojson::value(picojson::array{picojson::value(static_cast<double>(s_set.wins[0])),
                                              picojson::value(static_cast<double>(s_set.wins[1]))});
  o["best_of"] = picojson::value(static_cast<double>(s_format.best_of));
  o["game"] = picojson::value(static_cast<double>(s_act_game));
  picojson::array a;
  for (const u8 k : s_actions)
    a.emplace_back(static_cast<double>(k));
  o["actions"] = picojson::value(a);
  o["auto_actions"] = picojson::value(static_cast<double>(s_auto_actions));
  o["shared"] = picojson::value(Shared());
  o["presses"] = picojson::value(static_cast<double>(s_presses.size()));
  o["host_lists"] = picojson::value(picojson::array{KindsJson(s_host.actions[0]), KindsJson(s_host.actions[1])});
  o["opponent_stalled"] = picojson::value(s_opponent_stalled);
  picojson::array st;
  for (const u16 k : s_format.starters)
    st.emplace_back(static_cast<double>(k));
  o["starters"] = picojson::value(st);
  picojson::object view;
  view["active"] = picojson::value(s_last_view.active);
  view["game"] = picojson::value(static_cast<double>(s_last_view.game));
  view["type"] = picojson::value(static_cast<double>(s_last_view.type));
  view["my_turn"] = picojson::value(s_last_view.my_turn);
  view["count"] = picojson::value(static_cast<double>(s_last_view.count));
  view["to_sss"] = picojson::value(s_last_view.to_sss);
  view["may_lock"] = picojson::value(s_last_view.may_lock);
  view["time_up"] = picojson::value(s_last_view.time_up);
  view["seconds"] = picojson::value(static_cast<double>(s_last_view.seconds));
  view["stage"] = picojson::value(static_cast<double>(s_last_view.stage));
  view["text"] = picojson::value(s_last_view.text);
  picojson::array sel;
  for (const u8 k : s_last_view.selectable)
    sel.emplace_back(static_cast<double>(k));
  view["selectable"] = picojson::value(sel);
  o["view"] = picojson::value(view);
  return o;
}

// ---- 3-4 player matches ----

const char* SetupErrorText(SetupError error)
{
  switch (error)
  {
  case SetupError::SameTeam:
    return "Pick different teams";
  case SetupError::NoTeam:
    return "Pick a team";
  case SetupError::TooFewPlayers:
    return "Waiting for players";
  case SetupError::None:
  default:
    return "";
  }
}

TeamSetup DecideTeams(bool teams_on, const std::array<Seat, MAX_PORTS>& seats)
{
  TeamSetup t;
  int present = 0;
  for (const Seat& s : seats)
    present += s.present ? 1 : 0;
  if (present < 2)
  {
    t.error = SetupError::TooFewPlayers;
    return t;
  }
  // Two players: a 1v1 whatever the switch says.
  if (!teams_on || present == 2)
    return t;
  u8 first = NO_TEAM;
  bool two_colours = false;
  for (int i = 0; i < MAX_PORTS; ++i)
  {
    if (!seats[i].present)
      continue;
    const u8 team = seats[i].team;
    if (team >= NUM_TEAMS)
    {
      t.error = SetupError::NoTeam;
      return t;
    }
    if (first == NO_TEAM)
      first = team;
    else if (team != first)
      two_colours = true;
    t.team[i] = team;
  }
  if (!two_colours)
  {
    t.error = SetupError::SameTeam;
    t.team.fill(NO_TEAM);
    return t;
  }
  t.teams = true;
  return t;
}

Outcome DecideOutcome(bool teams, const std::array<PortEnd, MAX_PORTS>& ports)
{
  // The sides: one per player, or one per team colour.
  struct Side
  {
    u8 ports = 0;  // bits
    bool in = false;
    s64 stocks = 0;
    double damage = 0;
    u8 last_out = 0;  // the latest elimination of its players
  };
  std::array<Side, MAX_PORTS> sides{};
  std::array<int, MAX_PORTS> side_of{-1, -1, -1, -1};
  int n = 0;
  for (int i = 0; i < MAX_PORTS; ++i)
  {
    const PortEnd& p = ports[i];
    if (!p.present)
      continue;
    int s = -1;
    if (teams && p.team < NUM_TEAMS)
    {
      for (int j = 0; j < i; ++j)
      {
        if (ports[j].present && side_of[j] >= 0 && ports[j].team == p.team)
          s = side_of[j];
      }
    }
    if (s < 0)
      s = n++;
    side_of[i] = s;
    Side& side = sides[s];
    side.ports |= static_cast<u8>(1u << i);
    if (p.out == 0)
    {
      side.in = true;
      side.stocks += std::max<s32>(p.stocks, 0);
      // A damage the game cannot show (NaN, huge) ranks last.
      side.damage += (p.damage >= 0 && p.damage < 100000.0f) ? p.damage : 100000.0;
    }
    side.last_out = std::max(side.last_out, p.out);
  }
  Outcome o;
  if (n == 0)
    return o;
  // -1: a ranks before b, 1: after, 0: tied.
  const auto compare = [](const Side& a, const Side& b) {
    if (a.in != b.in)
      return a.in ? -1 : 1;
    if (a.in)
    {
      if (a.stocks != b.stocks)
        return a.stocks > b.stocks ? -1 : 1;
      if (a.damage != b.damage)
        return a.damage < b.damage ? -1 : 1;
      return 0;
    }
    if (a.last_out != b.last_out)
      return a.last_out > b.last_out ? -1 : 1;
    return 0;
  };
  std::array<u8, MAX_PORTS> side_place{};
  u8 worst = 0;
  for (int s = 0; s < n; ++s)
  {
    int better = 0;
    for (int t = 0; t < n; ++t)
      better += compare(sides[t], sides[s]) < 0 ? 1 : 0;
    side_place[s] = static_cast<u8>(better + 1);
    worst = std::max(worst, side_place[s]);
  }
  int first_sides = 0;
  int first_side = -1;
  for (int s = 0; s < n; ++s)
  {
    if (side_place[s] == 1)
    {
      ++first_sides;
      first_side = s;
    }
  }
  const auto lowest_port = [](u8 bits) {
    for (int i = 0; i < MAX_PORTS; ++i)
    {
      if (bits & (1u << i))
        return i;
    }
    return 0;
  };
  o.winner = first_sides == 1 ? static_cast<u8>(lowest_port(sides[first_side].ports)) : 0xFE;
  for (int i = 0; i < MAX_PORTS; ++i)
    o.place[i] = side_of[i] >= 0 ? side_place[side_of[i]] : 0;
  if (worst == 1)
  {
    // Everyone tied: each side's lowest port picks (a 1v1 draw: both, as Direct).
    for (int s = 0; s < n; ++s)
      o.pickers |= static_cast<u8>(1u << lowest_port(sides[s].ports));
  }
  else
  {
    u8 losers = 0;
    for (int s = 0; s < n; ++s)
    {
      if (side_place[s] == worst)
        losers |= sides[s].ports;
    }
    o.pickers = static_cast<u8>(1u << lowest_port(losers));
  }
  return o;
}

int StagePickPort(u8 pickers, const std::array<u16, MAX_PORTS>& picks)
{
  constexpr u16 NONE = 0xFFFF;
  for (int i = 0; i < MAX_PORTS; ++i)
  {
    if ((pickers & (1u << i)) && picks[i] != NONE)
      return i;
  }
  return -1;
}
}  // namespace Online::GameSetup
