// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/GameSetup.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Rollback/GameplaySession.h"

namespace Online::GameSetup
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr u16 NO_STAGE = 0xFFFF;
constexpr u8 NO_PORT = 0xFF;
// A player's actions for one game: a best of three needs at most three; bound what a peer sends.
constexpr size_t MAX_ACTIONS = 8;
// Best of three.
constexpr int WINS_NEEDED = 2;
// Slippi's GameSetup.h: a step's own time is over GRACE_SECONDS after its timer shows 0, and the
// opponent's WAIT_TIMEOUT_SECONDS after that. Its character steps have 45 s.
constexpr int GRACE_SECONDS = 3;
constexpr int WAIT_TIMEOUT_SECONDS = 15;
constexpr int CHAR_SECONDS = 45;
// Timer ids of the character steps (after the stage steps' indices).
constexpr int TIMER_WINNER_CHAR = 100;
constexpr int TIMER_LOSER_CHAR = 101;

struct Step
{
  int player;
  StepType type;
  int count;
  int seconds;
};

// Lock order: never call into Gprb::Session while holding s_mutex (the session calls the stage
// decider with its own lock held).
std::mutex s_mutex;
bool s_active = false;
int s_local = 0;
std::vector<u16> s_starters;
std::vector<u16> s_counterpicks;
std::array<int, 2> s_wins{};
u8 s_last_winner = NO_PORT;
std::array<u16, 2> s_last_win_stage{NO_STAGE, NO_STAGE};
u32 s_act_game = 0;
std::vector<u8> s_actions;  // this player's actions for s_act_game, in order
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

std::vector<u8> ActionsFrom(const picojson::object& extra, u32 game)
{
  std::vector<u8> out;
  const auto gs = extra.find("gs");
  if (gs == extra.end() || !gs->second.is<picojson::object>())
    return out;
  const auto& o = gs->second.get<picojson::object>();
  const auto g = o.find("g");
  const auto a = o.find("a");
  if (g == o.end() || !g->second.is<double>() || g->second.get<double>() != static_cast<double>(game))
    return out;
  if (a == o.end() || !a->second.is<picojson::array>())
    return out;
  for (const auto& v : a->second.get<picojson::array>())
  {
    if (out.size() >= MAX_ACTIONS || !v.is<double>())
      break;
    const double d = v.get<double>();
    if (d >= 1 && d <= 0xFF && d == static_cast<double>(static_cast<int>(d)))
      out.push_back(static_cast<u8>(d));
  }
  return out;
}

struct Eval
{
  bool striking = true;
  std::vector<Step> steps;
  int step_idx = -1;  // the step waiting for selections; -1: the stage is decided
  int left = 0;
  std::vector<u8> remaining;
  u16 stage = NO_STAGE;
};

// Under s_mutex. The steps of `game` with both players' actions applied.
Eval Evaluate(u32 game, const std::vector<u8>& mine, const std::vector<u8>& theirs)
{
  Eval e;
  e.striking = game <= 1 || s_last_winner > 1;
  const std::vector<u16>& pool = e.striking ? s_starters : s_counterpicks;
  int loser = 0;
  if (!e.striking)
  {
    const int winner = s_last_winner;
    loser = 1 - winner;
    e.steps = {{winner, StepType::Strike, 2, 30}, {loser, StepType::Pick, 1, 30}};
  }
  else
  {
    e.steps = {{0, StepType::Strike, 1, 30}, {1, StepType::Strike, 2, 30}, {0, StepType::Strike, 1, 10}};
  }
  for (const u16 k : pool)
  {
    if (k == 0 || k > 0xFF)
      continue;
    // Dave's stupid rule: the loser cannot pick the stage they last won on.
    if (!e.striking && s_wins[loser] > 0 && k == s_last_win_stage[loser])
      continue;
    e.remaining.push_back(static_cast<u8>(k));
  }
  std::array<const std::vector<u8>*, 2> lists{};
  lists[s_local] = &mine;
  lists[1 - s_local] = &theirs;
  std::array<size_t, 2> idx{};
  for (size_t i = 0; i < e.steps.size(); ++i)
  {
    const Step& st = e.steps[i];
    int need = st.count;
    while (need > 0 && idx[st.player] < lists[st.player]->size())
    {
      const u8 k = (*lists[st.player])[idx[st.player]++];
      const auto it = std::find(e.remaining.begin(), e.remaining.end(), k);
      if (it == e.remaining.end())
        continue;  // not selectable (a peer's bad action): ignored
      if (st.type == StepType::Pick)
        e.stage = k;
      else
        e.remaining.erase(it);
      --need;
    }
    if (need > 0)
    {
      e.step_idx = static_cast<int>(i);
      e.left = need;
      return e;
    }
  }
  if (e.striking && !e.remaining.empty())
    e.stage = e.remaining.front();
  return e;
}

picojson::object ExtraJson(u32 game, const std::vector<u8>& actions)
{
  picojson::array a;
  for (const u8 k : actions)
    a.emplace_back(static_cast<double>(k));
  picojson::object gs;
  gs["g"] = picojson::value(static_cast<double>(game));
  gs["a"] = picojson::value(a);
  picojson::object o;
  o["gs"] = picojson::value(gs);
  return o;
}

// Under s_mutex.
bool SetOver()
{
  return s_wins[0] >= WINS_NEEDED || s_wins[1] >= WINS_NEEDED;
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
  // Called by the session with its lock held: read the peer first, then take ours.
  const picojson::object extra = Gprb::Session::GetPeerExtra();
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
  const std::vector<u8> mine = s_act_game == game ? s_actions : std::vector<u8>{};
  const Eval e = Evaluate(game, mine, ActionsFrom(extra, game));
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
  {
    std::lock_guard lk(s_mutex);
    s_active = true;
    s_local = local_port == 1 ? 1 : 0;
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
    s_starters = starters.empty() ? DefaultStarters() : std::move(starters);
    s_counterpicks = counterpicks.empty() ? Gprb::Session::DefaultStages() : std::move(counterpicks);
    s_wins = {};
    s_last_winner = NO_PORT;
    s_last_win_stage = {NO_STAGE, NO_STAGE};
    s_act_game = 0;
    s_actions.clear();
    s_timer_step = -1;
    s_auto_actions = 0;
    s_opponent_stalled = false;
  }
  Gprb::Session::SetLocalExtra({});
  Gprb::Session::SetStageDecider(Decide);
  NOTICE_LOG_FMT(NETPLAY, "GameSetup: ranked set, player {}", local_port + 1);
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
  s_last_winner = winner;
  if (winner < 2)
  {
    ++s_wins[winner];
    s_last_win_stage[winner] = stage;
  }
}

bool Act(u8 kind)
{
  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  const picojson::object extra = Gprb::Session::GetPeerExtra();
  picojson::object publish;
  {
    std::lock_guard lk(s_mutex);
    if (!s_active || !lobby.active || lobby.in_match || lobby.setup_ready)
      return false;
    const u32 game = lobby.game;
    if (s_act_game != game)
    {
      s_act_game = game;
      s_actions.clear();
    }
    const Eval e = Evaluate(game, s_actions, ActionsFrom(extra, game));
    if (e.step_idx < 0 || e.steps[e.step_idx].player != s_local ||
        std::find(e.remaining.begin(), e.remaining.end(), kind) == e.remaining.end() ||
        s_actions.size() >= MAX_ACTIONS)
    {
      return false;
    }
    s_actions.push_back(kind);
    INFO_LOG_FMT(NETPLAY, "GameSetup: game {}: {} {:#x}", game,
                 e.steps[e.step_idx].type == StepType::Pick ? "picked" : "struck", kind);
    publish = ExtraJson(game, s_actions);
  }
  Gprb::Session::SetLocalExtra(publish);
  return true;
}

View Current()
{
  const Gprb::Session::Lobby lobby = Gprb::Session::GetLobby();
  const picojson::object extra = Gprb::Session::GetPeerExtra();
  const Gprb::Session::LockIn peer_lock = Gprb::Session::GetPeerLock();
  const Gprb::Session::LockIn local_lock = Gprb::Session::GetLocalLock();
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
    if (s_act_game != game)
    {
      s_act_game = game;
      s_actions.clear();
      publish = ExtraJson(game, s_actions);
    }
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
    Eval e = Evaluate(game, s_actions, ActionsFrom(extra, game));
    if (e.step_idx >= 0)
    {
      const Step& st = e.steps[e.step_idx];
      const bool mine = st.player == s_local;
      if (mine && Elapsed(game, e.step_idx) >= st.seconds + GRACE_SECONDS &&
          !e.remaining.empty() && s_actions.size() < MAX_ACTIONS)
      {
        // Slippi: a step whose time runs out is completed with a random selection.
        std::uniform_int_distribution<size_t> pick(0, e.remaining.size() - 1);
        const u8 k = e.remaining[pick(s_rng)];
        s_actions.push_back(k);
        ++s_auto_actions;
        publish = ExtraJson(game, s_actions);
        NOTICE_LOG_FMT(NETPLAY, "GameSetup: game {}: time is up, {} {:#x}", game,
                       st.type == StepType::Pick ? "picked" : "struck", k);
        e = Evaluate(game, s_actions, ActionsFrom(extra, game));
      }
    }
    v.selectable = e.remaining;
    if (e.step_idx >= 0)
    {
      const Step& st = e.steps[e.step_idx];
      const bool mine = st.player == s_local;
      v.type = st.type;
      v.my_turn = mine;
      v.count = static_cast<u8>(e.left);
      v.to_sss = true;
      v.may_lock = false;
      v.seconds = TimerShown(st.seconds, Elapsed(game, e.step_idx), mine);
      std::string what;
      if (st.type == StepType::Pick)
        what = mine ? "Pick a stage" : "Opponent is picking";
      else if (!e.striking && mine)
        what = e.left == 1 ? "Ban 1 stage" : fmt::format("Ban {} stages", e.left);
      else if (!e.striking)
        what = "Opponent is banning";
      else if (mine)
        what = e.left == 1 ? "Strike 1 stage" : fmt::format("Strike {} stages", e.left);
      else
        what = "Opponent is striking";
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
        // then the character on the CSS is taken.
        const bool winner = s_last_winner == s_local;
        const bool local_locked = local_lock.ready && local_lock.game == game;
        const bool winner_locked = winner ? local_locked : lobby.remote_ready;
        const bool loser_locked = winner ? lobby.remote_ready : local_locked;
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
          else if (mine)
            v.text = fmt::format("Opponent picked {} ({})", CharName(peer_lock.char_kind), v.seconds);
          else
            v.text = fmt::format("Opponent is choosing ({})", v.seconds);
        }
        v.may_lock = v.my_turn;
      }
    }
  }
  {
    std::lock_guard lk(s_mutex);
    s_last_view = v;
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
  o["last_winner"] = picojson::value(static_cast<double>(s_last_winner));
  o["wins"] = picojson::value(picojson::array{picojson::value(static_cast<double>(s_wins[0])),
                                              picojson::value(static_cast<double>(s_wins[1]))});
  o["game"] = picojson::value(static_cast<double>(s_act_game));
  picojson::array a;
  for (const u8 k : s_actions)
    a.emplace_back(static_cast<double>(k));
  o["actions"] = picojson::value(a);
  o["auto_actions"] = picojson::value(static_cast<double>(s_auto_actions));
  o["opponent_stalled"] = picojson::value(s_opponent_stalled);
  picojson::array st;
  for (const u16 k : s_starters)
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
