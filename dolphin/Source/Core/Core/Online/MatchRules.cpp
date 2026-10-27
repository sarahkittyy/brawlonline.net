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
                                          "autoLCancel",   "inputAssist", "stockControl",
                                          "hazardStages",  "special",    "staminaType"};
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
  const auto frequency =
      GetByte(o, "itemFrequency", 0, MAX_ITEM_FREQUENCY, r.item_frequency, &out.detail);
  const auto stamina_type = GetByte(o, "staminaType", 1, STAMINA_TYPES, r.stamina_type, &out.detail);
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
  const auto stock_control = GetFlag(o, "stockControl", r.stock_control, &out.detail);
  if (!stocks || !time || !frequency || !team_attack || !hazards || !alc || !assist ||
      !stock_control || !stamina_type)
  {
    return fail(INVALID_TEXT);
  }
  r.stamina_type = *stamina_type;
  r.stocks = *stocks;
  r.time_minutes = *time;
  r.item_frequency = *frequency;
  r.team_attack = *team_attack;
  r.hazards = *hazards;
  r.auto_l_cancel = *alc;
  r.input_assist = *assist;
  r.stock_control = *stock_control;

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
  if (const auto hs = o.find("hazardStages"); hs != o.end())
  {
    const bool ok = hs->second.is<picojson::array>() &&
                    hs->second.get<picojson::array>().size() <= MAX_HAZARD_STAGES;
    if (!ok)
    {
      out.detail = fmt::format("hazardStages is not a list of up to {} stages", MAX_HAZARD_STAGES);
      return fail(INVALID_TEXT);
    }
    for (const auto& v : hs->second.get<picojson::array>())
    {
      picojson::object probe{{"hazardStages", v}};
      const auto id = GetByte(probe, "hazardStages", 1, 0xFF, 0, &out.detail);
      if (!id)
        return fail(INVALID_TEXT);
      if (std::find(r.hazard_stages.begin(), r.hazard_stages.end(), *id) != r.hazard_stages.end())
      {
        out.detail = fmt::format("hazardStages lists stage {} twice", *id);
        return fail(INVALID_TEXT);
      }
      r.hazard_stages.push_back(*id);
    }
  }
  if (const auto sp = o.find("special"); sp != o.end())
  {
    if (!sp->second.is<picojson::object>())
    {
      out.detail = "special is not an object";
      return fail(INVALID_TEXT);
    }
    for (const auto& [key, value] : sp->second.get<picojson::object>())
    {
      const auto row = std::find_if(std::begin(SPECIAL_ROWS), std::end(SPECIAL_ROWS),
                                    [&](const SpecialRow& s) { return key == s.key; });
      if (row == std::end(SPECIAL_ROWS))
      {
        out.detail = fmt::format("unknown special rule '{}'", key.substr(0, 40));
        return fail(UNKNOWN_TEXT);
      }
      const std::string v = value.is<std::string>() ? value.get<std::string>() : "";
      const size_t i = static_cast<size_t>(row - std::begin(SPECIAL_ROWS));
      if (v == "off")
        r.special[i] = 0;
      else if (v == row->values[0])
        r.special[i] = 1;
      else if (v == row->values[1])
        r.special[i] = 2;
      else
      {
        // A value this build does not know is a newer server's: refused, as an unknown key.
        out.detail = fmt::format("special {} is '{}'", key, v.substr(0, 40));
        return fail(value.is<std::string>() ? UNKNOWN_TEXT : INVALID_TEXT);
      }
    }
  }
  if (r.stamina_type != 1 && r.special[0] != 2)
  {
    out.detail = "staminaType without stamina";
    return fail(INVALID_TEXT);
  }
  out.rules = r;
  return out;
}

std::array<u8, SPECIAL_RULES_SIZE> SpecialBytes(const std::optional<MatchRules>& rules)
{
  std::array<u8, SPECIAL_RULES_SIZE> b{};
  if (!rules)
    return b;
  for (size_t i = 0; i < rules->special.size(); ++i)
    b[i] = rules->special[i];
  b[8] = rules->stamina_type;
  return b;
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
  b[RU_STOCK_CONTROL] = r.stock_control ? 1 : 0;
  for (size_t i = 0; i < r.hazard_stages.size() && i < MAX_HAZARD_STAGES; ++i)
    b[RU_HAZARD_STAGES + i] = r.hazard_stages[i];
  return b;
}

std::optional<MatchRules> FromSessionBytes(const std::array<u8, RULES_SIZE>& b,
                                           const std::array<u8, SPECIAL_RULES_SIZE>* special)
{
  if (b[RU_VALID] != 1 || b[RU_STYLE] > MatchRules::STYLE_COIN || b[RU_STOCKS] < 1 ||
      b[RU_STOCKS] > 99 || b[RU_TIME] > 99 ||
      (b[RU_STYLE] != MatchRules::STYLE_STOCK && b[RU_TIME] < 1) || b[RU_DAMAGE] < 5 ||
      b[RU_DAMAGE] > 20 || b[RU_ITEM_FREQUENCY] > MAX_ITEM_FREQUENCY)
  {
    return std::nullopt;
  }
  for (u32 f : {RU_TEAM_ATTACK, RU_PAUSE, RU_HAZARDS, RU_AUTO_L_CANCEL, RU_INPUT_ASSIST, RU_STOCK_CONTROL})
  {
    if (b[f] > 1)
      return std::nullopt;
  }
  MatchRules r;
  r.style = b[RU_STYLE];
  r.stocks = b[RU_STOCKS];
  r.time_minutes = b[RU_TIME];
  r.damage_ratio = b[RU_DAMAGE];
  r.team_attack = b[RU_TEAM_ATTACK] != 0;
  r.pause_normal = b[RU_PAUSE] != 0;
  r.item_frequency = b[RU_ITEM_FREQUENCY];
  for (size_t i = 0; i < r.item_switch.size(); ++i)
    r.item_switch[i] = b[RU_ITEM_SWITCH + i];
  r.hazards = b[RU_HAZARDS] != 0;
  r.auto_l_cancel = b[RU_AUTO_L_CANCEL] != 0;
  r.input_assist = b[RU_INPUT_ASSIST] != 0;
  r.stock_control = b[RU_STOCK_CONTROL] != 0;
  for (u32 i = 0; i < MAX_HAZARD_STAGES; ++i)
  {
    const u8 id = b[RU_HAZARD_STAGES + i];
    if (id == 0)
      break;
    if (std::find(r.hazard_stages.begin(), r.hazard_stages.end(), id) != r.hazard_stages.end())
      return std::nullopt;
    r.hazard_stages.push_back(id);
  }
  if (special)
  {
    const auto& sp = *special;
    for (size_t i = 0; i < r.special.size(); ++i)
    {
      if (sp[i] > 2)
        return std::nullopt;
      r.special[i] = sp[i];
    }
    // The stamina type means something in a stamina match only (as the server normalizes it).
    const u8 type = sp[8] == 0 ? 1 : sp[8];
    if (type > STAMINA_TYPES)
      return std::nullopt;
    r.stamina_type = r.special[0] == 2 ? type : 1;
  }
  return r;
}

picojson::object ToJson(const MatchRules& r)
{
  static constexpr const char* STYLES[] = {"time", "stock", "coin"};
  picojson::array sw;
  for (u8 b : r.item_switch)
    sw.emplace_back(static_cast<double>(b));
  picojson::object o{
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
  // Only when on, as the server writes it (an older server refuses a key it does not know).
  if (r.stock_control)
    o["stockControl"] = picojson::value(true);
  picojson::object special;
  for (size_t i = 0; i < r.special.size(); ++i)
  {
    if (r.special[i] == 1 || r.special[i] == 2)
      special[SPECIAL_ROWS[i].key] = picojson::value(SPECIAL_ROWS[i].values[r.special[i] - 1]);
  }
  if (!special.empty())
    o["special"] = picojson::value(special);
  if (r.stamina_type != 1)
    o["staminaType"] = picojson::value(static_cast<double>(r.stamina_type));
  if (!r.hazard_stages.empty())
  {
    picojson::array hs;
    for (u8 id : r.hazard_stages)
      hs.emplace_back(static_cast<double>(id));
    o["hazardStages"] = picojson::value(hs);
  }
  return o;
}
}  // namespace Online
