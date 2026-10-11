// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/SetFormat.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <fmt/format.h>

namespace Online::GameSetup
{
namespace
{
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
}  // namespace

namespace
{
std::optional<double> Num(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<double>())
    return std::nullopt;
  const double v = it->second.get<double>();
  if (!std::isfinite(v) || v != static_cast<double>(static_cast<s64>(v)))
    return std::nullopt;
  return v;
}

bool Ints(const picojson::object& o, const char* key, size_t max_len, double lo, double hi,
          std::vector<double>* out)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<picojson::array>())
    return false;
  const auto& a = it->second.get<picojson::array>();
  if (a.size() > max_len)
    return false;
  for (const auto& v : a)
  {
    if (!v.is<double>())
      return false;
    const double d = v.get<double>();
    if (!std::isfinite(d) || d != static_cast<double>(static_cast<s64>(d)) || d < lo || d > hi)
      return false;
    out->push_back(d);
  }
  return true;
}
}  // namespace

ParsedSet ParseSetTicket(const picojson::object& resp)
{
  ParsedSet out;
  const auto it = resp.find("set");
  if (it == resp.end() || it->second.is<picojson::null>())
    return out;
  auto fail = [&out](std::string detail) {
    out.set.reset();
    out.error = "Invalid rules from the server.";
    out.detail = std::move(detail);
    return out;
  };
  if (!it->second.is<picojson::object>())
    return fail("set is not an object");
  const auto& o = it->second.get<picojson::object>();
  SetTicket t;
  const auto best_of = Num(o, "bestOf");
  const auto bans = Num(o, "bans");
  const auto bits = Num(o, "firstStrikerBits");
  const auto game = Num(o, "game");
  const auto last_winner = Num(o, "lastWinner");
  const auto every = o.find("strikeEveryGame");
  const auto dsr = o.find("dsr");
  if (!best_of || !bans || !bits || !game || !last_winner || every == o.end() ||
      !every->second.is<bool>() || dsr == o.end() || !dsr->second.is<bool>())
  {
    return fail("bestOf, bans, firstStrikerBits, game, lastWinner, strikeEveryGame or dsr");
  }
  if (*bits < 0 || *bits > 0xFFFFFFFF || *game < 1 || *game > 99 ||
      !(*last_winner <= 1 || *last_winner == 0xFE || *last_winner == 0xFF))
  {
    return fail("firstStrikerBits, game or lastWinner out of range");
  }
  std::vector<double> starters, counterpicks, strikes, sides, wins, win_stages;
  if (!Ints(o, "starters", 32, 1, 0xFF, &starters) || !Ints(o, "counterpicks", 64, 1, 0xFF, &counterpicks) ||
      !Ints(o, "strikes", 8, 1, 8, &strikes) || !Ints(o, "sides", 2, 0, 3, &sides) ||
      !Ints(o, "wins", 2, 0, 4, &wins) || !Ints(o, "lastWinStage", 2, 1, 0xFFFF, &win_stages) ||
      sides.size() != 2 || sides[0] == sides[1] || wins.size() != 2 || win_stages.size() != 2)
  {
    return fail("a stage list, the strikes, sides, wins or lastWinStage");
  }
  Format& f = t.format;
  f.best_of = static_cast<int>(*best_of);
  f.bans = static_cast<int>(*bans);
  f.dsr = dsr->second.get<bool>();
  f.strike_every_game = every->second.get<bool>();
  f.first_striker_bits = static_cast<u32>(*bits);
  for (double d : starters)
    f.starters.push_back(static_cast<u16>(d));
  for (double d : counterpicks)
    f.counterpicks.push_back(static_cast<u16>(d));
  f.strikes.clear();
  for (double d : strikes)
    f.strikes.push_back(static_cast<u8>(d));
  if (f.best_of == 0 && !f.strike_every_game)
    return fail("no set and no strikes");
  if (const auto why = CheckFormat(f))
    return fail(*why);
  t.first_game = static_cast<u32>(*game);
  t.side_ports = {static_cast<u8>(sides[0]), static_cast<u8>(sides[1])};
  t.state.wins = {static_cast<int>(wins[0]), static_cast<int>(wins[1])};
  t.state.last_winner = static_cast<u8>(*last_winner);
  for (size_t i = 0; i < 2; ++i)
  {
    const double k = win_stages[i];
    if (k != 0xFFFF && k > 0xFF)
      return fail("lastWinStage");
    t.state.last_win_stage[i] = static_cast<u16>(k);
  }
  if (f.best_of > 0 && SetOver(f, t.state))
    return fail("a set that is already won");
  out.set = std::move(t);
  return out;
}

Format RankedFormat(std::vector<u16> starters, std::vector<u16> counterpicks)
{
  Format f;
  f.starters = std::move(starters);
  f.counterpicks = std::move(counterpicks);
  return f;
}

std::optional<std::string> CheckFormat(const Format& f)
{
  if (f.best_of != 0 && f.best_of != 1 && f.best_of != 3 && f.best_of != 5 && f.best_of != 7)
    return fmt::format("best of {}", f.best_of);
  if (f.bans < 0 || f.bans > 3)
    return fmt::format("{} bans", f.bans);
  if (f.strikes.empty() || f.strikes.size() > 8 ||
      std::any_of(f.strikes.begin(), f.strikes.end(), [](u8 c) { return c < 1 || c > 8; }))
  {
    return "a striking pattern of 1-8 steps of 1-8 stages";
  }
  const size_t struck = std::accumulate(f.strikes.begin(), f.strikes.end(), size_t{0});
  if (Usable(f.starters) != struck + 1)
    return fmt::format("{} starters for a pattern striking {}", Usable(f.starters), struck);
  // The loser needs a stage after the bans and Dave's rule (when there are games after the first).
  const bool counterpicking = f.best_of > 1;
  if (counterpicking && Usable(f.counterpicks) < static_cast<size_t>(f.bans) + 1 + (f.dsr ? 1 : 0))
    return fmt::format("{} counterpick stages for {} bans", Usable(f.counterpicks), f.bans);
  return std::nullopt;
}

int FirstStriker(const Format& f, u32 game)
{
  return static_cast<int>((f.first_striker_bits >> ((game == 0 ? 0 : game - 1) % 32)) & 1);
}

bool SetOver(const Format& f, const SetState& set)
{
  if (f.best_of <= 0)
    return false;
  const int need = f.best_of / 2 + 1;
  return set.wins[0] >= need || set.wins[1] >= need;
}

Steps EvaluateSteps(const Format& f, const SetState& set, u32 game,
                    const std::array<const std::vector<u8>*, 2>& actions)
{
  Steps e;
  e.striking = game <= 1 || set.last_winner > 1 || f.strike_every_game;
  const std::vector<u16>& pool = e.striking ? f.starters : f.counterpicks;
  int loser = 0;
  if (!e.striking)
  {
    const int winner = set.last_winner;
    loser = 1 - winner;
    if (f.bans > 0)
      e.steps.push_back({winner, StepType::Strike, f.bans, 30});
    e.steps.push_back({loser, StepType::Pick, 1, 30});
  }
  else
  {
    // Players take turns from the first striker; Slippi gives the last single strike 10 s.
    const int first = FirstStriker(f, game);
    for (size_t i = 0; i < f.strikes.size(); ++i)
    {
      const int player = i % 2 == 0 ? first : 1 - first;
      const bool last = i + 1 == f.strikes.size();
      e.steps.push_back({player, StepType::Strike, f.strikes[i], last && f.strikes[i] == 1 ? 10 : 30});
    }
  }
  for (const u16 k : pool)
  {
    if (k == 0 || k > 0xFF)
      continue;
    if (std::find(e.remaining.begin(), e.remaining.end(), static_cast<u8>(k)) != e.remaining.end())
      continue;
    // Dave's stupid rule: the loser cannot pick the stage they last won on.
    if (!e.striking && f.dsr && set.wins[loser] > 0 && k == set.last_win_stage[loser])
      continue;
    e.remaining.push_back(static_cast<u8>(k));
  }
  std::array<size_t, 2> idx{};
  static const std::vector<u8> none;
  for (size_t i = 0; i < e.steps.size(); ++i)
  {
    const StepInfo& st = e.steps[i];
    const std::vector<u8>& list = actions[st.player] ? *actions[st.player] : none;
    int need = st.count;
    while (need > 0 && idx[st.player] < list.size())
    {
      const u8 k = list[idx[st.player]++];
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
}  // namespace Online::GameSetup
