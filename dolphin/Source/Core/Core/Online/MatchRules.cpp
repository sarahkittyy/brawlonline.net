// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/MatchRules.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <fmt/format.h>

namespace Online
{
namespace
{
constexpr const char* INVALID_TEXT = "Invalid rules from the server.";
constexpr const char* UNKNOWN_TEXT = "Update to play with these rules.";

std::optional<u8> GetByte(const picojson::object& o, const char* key, u8 lo, u8 hi, u8 fallback,
                          std::string* detail)
{
  const auto it = o.find(key);
  if (it == o.end())
    return fallback;
  if (!it->second.is<double>())
  {
    *detail = fmt::format("{} is not a number", key);
    return std::nullopt;
  }
  const double v = it->second.get<double>();
  if (!std::isfinite(v) || v != std::floor(v) || v < lo || v > hi)
  {
    *detail = fmt::format("{} {} is not {}-{}", key, v, lo, hi);
    return std::nullopt;
  }
  return static_cast<u8>(v);
}

std::optional<bool> GetFlag(const picojson::object& o, const char* key, bool fallback,
                            std::string* detail)
{
  const auto it = o.find(key);
  if (it == o.end())
    return fallback;
  if (!it->second.is<bool>())
  {
    *detail = fmt::format("{} is not true or false", key);
    return std::nullopt;
  }
  return it->second.get<bool>();
}
}  // namespace

ParsedRules ParseRules(const picojson::object& resp)
{
  ParsedRules out;
  const auto it = resp.find("rules");
  if (it == resp.end() || it->second.is<picojson::null>())
    return out;
  auto fail = [&out](const char* text) {
    out.rules.reset();
    out.error = text;
    return out;
  };
  if (!it->second.is<picojson::object>())
  {
    out.detail = "rules is not an object";
    return fail(INVALID_TEXT);
  }
  const auto& o = it->second.get<picojson::object>();
  static constexpr const char* KNOWN[] = {"style",         "stocks",     "timeMinutes",
                                          "damageRatio",   "teamAttack", "pause",
                                          "itemFrequency", "itemSwitch", "hazards",
                                          "autoLCancel",   "inputAssist"};
  for (const auto& [key, value] : o)
  {
    if (std::find(std::begin(KNOWN), std::end(KNOWN), key) == std::end(KNOWN))
    {
      out.detail = fmt::format("unknown rule '{}'", key.substr(0, 40));
      return fail(UNKNOWN_TEXT);
    }
  }

  MatchRules r;
  if (const auto s = o.find("style"); s != o.end())
  {
    const std::string name = s->second.is<std::string>() ? s->second.get<std::string>() : "";
    if (name == "time")
      r.style = MatchRules::STYLE_TIME;
    else if (name == "stock")
      r.style = MatchRules::STYLE_STOCK;
    else if (name == "coin")
      r.style = MatchRules::STYLE_COIN;
    else
    {
      out.detail = "style is not time, stock or coin";
      return fail(INVALID_TEXT);
    }
  }
  const auto stocks = GetByte(o, "stocks", 1, 99, r.stocks, &out.detail);
  const u8 min_time = r.style == MatchRules::STYLE_STOCK ? 0 : 1;
  const auto time = GetByte(o, "timeMinutes", min_time, 99, r.time_minutes, &out.detail);
  const auto frequency = GetByte(o, "itemFrequency", 0, 3, r.item_frequency, &out.detail);
  const auto team_attack = GetFlag(o, "teamAttack", r.team_attack, &out.detail);
  if (const auto p = o.find("pause"); p != o.end())
  {
    const std::string name = p->second.is<std::string>() ? p->second.get<std::string>() : "";
    if (name != "normal" && name != "hold")
    {
      out.detail = "pause is not normal or hold";
      return fail(INVALID_TEXT);
    }
    r.pause_normal = name == "normal";
  }
  const auto hazards = GetFlag(o, "hazards", r.hazards, &out.detail);
  const auto alc = GetFlag(o, "autoLCancel", r.auto_l_cancel, &out.detail);
  const auto assist = GetFlag(o, "inputAssist", r.input_assist, &out.detail);
  if (!stocks || !time || !frequency || !team_attack || !hazards || !alc || !assist)
    return fail(INVALID_TEXT);
  r.stocks = *stocks;
  r.time_minutes = *time;
  r.item_frequency = *frequency;
  r.team_attack = *team_attack;
  r.hazards = *hazards;
  r.auto_l_cancel = *alc;
  r.input_assist = *assist;

  if (const auto d = o.find("damageRatio"); d != o.end())
  {
    const double v = d->second.is<double>() ? d->second.get<double>() : NAN;
    const double tenths = v * 10.0;
    if (!std::isfinite(v) || std::abs(tenths - std::round(tenths)) > 1e-6 || tenths < 4.5 ||
        tenths > 20.5)
    {
      out.detail = "damageRatio is not 0.5-2.0 in steps of 0.1";
      return fail(INVALID_TEXT);
    }
    r.damage_ratio = static_cast<u8>(std::lround(tenths));
  }
  if (const auto sw = o.find("itemSwitch"); sw != o.end())
  {
    const bool ok = sw->second.is<picojson::array>() &&
                    sw->second.get<picojson::array>().size() == r.item_switch.size();
    if (!ok)
    {
      out.detail = "itemSwitch is not 8 bytes";
      return fail(INVALID_TEXT);
    }
    const auto& a = sw->second.get<picojson::array>();
    for (size_t i = 0; i < a.size(); ++i)
    {
      picojson::object probe{{"b", a[i]}};
      const auto b = GetByte(probe, "b", 0, 0xFF, 0, &out.detail);
      if (!b)
      {
        out.detail = "itemSwitch is not 8 bytes";
        return fail(INVALID_TEXT);
      }
      r.item_switch[i] = *b;
    }
  }
  out.rules = r;
  return out;
}

std::array<u8, RULES_SIZE> SessionBytes(const std::optional<MatchRules>& rules)
{
  std::array<u8, RULES_SIZE> b{};
  if (!rules)
    return b;
  const MatchRules& r = *rules;
  b[RU_VALID] = 1;
  b[RU_STYLE] = r.style;
  b[RU_STOCKS] = r.stocks;
  b[RU_TIME] = r.time_minutes;
  b[RU_DAMAGE] = r.damage_ratio;
  b[RU_TEAM_ATTACK] = r.team_attack ? 1 : 0;
  b[RU_PAUSE] = r.pause_normal ? 1 : 0;
  b[RU_ITEM_FREQUENCY] = r.item_frequency;
  for (size_t i = 0; i < r.item_switch.size(); ++i)
    b[RU_ITEM_SWITCH + i] = r.item_switch[i];
  b[RU_HAZARDS] = r.hazards ? 1 : 0;
  b[RU_AUTO_L_CANCEL] = r.auto_l_cancel ? 1 : 0;
  b[RU_INPUT_ASSIST] = r.input_assist ? 1 : 0;
  return b;
}

picojson::object ToJson(const MatchRules& r)
{
  static constexpr const char* STYLES[] = {"time", "stock", "coin"};
  picojson::array sw;
  for (u8 b : r.item_switch)
    sw.emplace_back(static_cast<double>(b));
  return picojson::object{
      {"style", picojson::value(STYLES[r.style < 3 ? r.style : 1])},
      {"stocks", picojson::value(static_cast<double>(r.stocks))},
      {"timeMinutes", picojson::value(static_cast<double>(r.time_minutes))},
      {"damageRatio", picojson::value(r.damage_ratio / 10.0)},
      {"teamAttack", picojson::value(r.team_attack)},
      {"pause", picojson::value(r.pause_normal ? "normal" : "hold")},
      {"itemFrequency", picojson::value(static_cast<double>(r.item_frequency))},
      {"itemSwitch", picojson::value(sw)},
      {"hazards", picojson::value(r.hazards)},
      {"autoLCancel", picojson::value(r.auto_l_cancel)},
      {"inputAssist", picojson::value(r.input_assist)},
  };
}
}  // namespace Online
