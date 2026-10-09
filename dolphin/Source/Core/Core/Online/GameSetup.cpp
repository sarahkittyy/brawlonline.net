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
    e.steps = {{winner, StepType::Strike, 1, 30}, {loser, StepType::Pick, 1, 30}};
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

Gprb::Session::StageDecision Decide(u32 game)
{
  // Called by the session with its lock held: read the peer first, then take ours.
  const picojson::object extra = Gprb::Session::GetPeerExtra();
  std::lock_guard lk(s_mutex);
  Gprb::Session::StageDecision d;
  if (!s_active)
    return d;
  d.applies = true;
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
    s_starters = starters.empty() ? DefaultStarters() : std::move(starters);
    s_counterpicks = counterpicks.empty() ? Gprb::Session::DefaultStages() : std::move(counterpicks);
    s_wins = {};
    s_last_winner = NO_PORT;
    s_last_win_stage = {NO_STAGE, NO_STAGE};
    s_act_game = 0;
    s_actions.clear();
    s_timer_step = -1;
    s_auto_actions = 0;
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
    Eval e = Evaluate(game, s_actions, ActionsFrom(extra, game));
    v.active = true;
    v.game = game;
    if (e.step_idx >= 0)
    {
      const Step& st = e.steps[e.step_idx];
      if (s_timer_game != game || s_timer_step != e.step_idx)
      {
        s_timer_game = game;
        s_timer_step = e.step_idx;
        s_timer_start = Clock::now();
      }
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - s_timer_start).count();
      const int left = std::max<int>(0, st.seconds - static_cast<int>(elapsed));
      const bool mine = st.player == s_local;
      if (mine && left == 0 && !e.remaining.empty() && s_actions.size() < MAX_ACTIONS)
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
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - s_timer_start).count();
      v.seconds = static_cast<u8>(std::clamp<int>(st.seconds - static_cast<int>(elapsed), 0, 99));
      std::string what;
      if (st.type == StepType::Pick)
        what = mine ? "Pick a stage" : "Opponent is picking";
      else if (!e.striking)
        what = mine ? "Ban 1 stage" : "Opponent is banning";
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
        v.type = StepType::Done;
      }
      else
      {
        // Slippi: the winner chooses a character first, then the loser, seeing it.
        const bool winner = s_last_winner == s_local;
        const bool peer_locked = lobby.remote_ready;
        v.type = StepType::Char;
        v.my_turn = winner || peer_locked;
        v.may_lock = v.my_turn;
        if (!winner)
        {
          v.text = peer_locked ? fmt::format("Opponent picked {}", CharName(peer_lock.char_kind)) :
                                 "Opponent is choosing";
        }
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
}  // namespace Online::GameSetup
