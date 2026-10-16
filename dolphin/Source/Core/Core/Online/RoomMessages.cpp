// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/RoomMessages.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Online::Rooms
{
namespace
{
// Our messages are a few hundred bytes; mm's room-state with four long names stays under 2 KiB.
constexpr size_t MAX_PACKET = 16 * 1024;

bool InAlphabet(char c)
{
  return c != '\0' && std::strchr(ROOM_CODE_ALPHABET, c) != nullptr;
}

const picojson::value* Find(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}

std::optional<std::string> Str(const picojson::object& o, const char* key, size_t max_bytes)
{
  const picojson::value* v = Find(o, key);
  if (!v || !v->is<std::string>())
    return std::nullopt;
  return CleanText(v->get<std::string>(), max_bytes);
}

std::optional<bool> Bool(const picojson::object& o, const char* key)
{
  const picojson::value* v = Find(o, key);
  if (!v || !v->is<bool>())
    return std::nullopt;
  return v->get<bool>();
}

std::optional<s64> Int(const picojson::value& v, s64 lo, s64 hi)
{
  if (!v.is<double>())
    return std::nullopt;
  const double d = v.get<double>();
  if (!std::isfinite(d) || std::floor(d) != d || d < static_cast<double>(lo) ||
      d > static_cast<double>(hi))
  {
    return std::nullopt;
  }
  return static_cast<s64>(d);
}

std::optional<s64> Int(const picojson::object& o, const char* key, s64 lo, s64 hi)
{
  const picojson::value* v = Find(o, key);
  return v ? Int(*v, lo, hi) : std::nullopt;
}

// A number 0-255, null or missing (= nullopt); `bad` when it is anything else.
std::optional<u8> OptByte(const picojson::object& o, const char* key, bool* bad)
{
  const picojson::value* v = Find(o, key);
  if (!v || v->is<picojson::null>())
    return std::nullopt;
  const auto n = Int(*v, 0, 255);
  if (!n)
  {
    *bad = true;
    return std::nullopt;
  }
  return static_cast<u8>(*n);
}

std::optional<Player> ParsePlayer(const picojson::object& o, std::string* why)
{
  Player p;
  const auto name = Str(o, "displayName", MAX_NAME_BYTES);
  const auto code = Str(o, "connectCode", MAX_CODE_BYTES);
  const auto ready = Bool(o, "ready");
  const auto team = Int(o, "team", 0, NUM_TEAMS - 1);
  const auto in_game = Bool(o, "inGame");
  if (!name || !code || !ready || !team || !in_game)
  {
    *why = "a player without displayName, connectCode, ready, team 0-2 or inGame";
    return std::nullopt;
  }
  bool bad = false;
  const auto character = OptByte(o, "character", &bad);
  const auto costume = OptByte(o, "costume", &bad);
  const auto stocks = OptByte(o, "stocks", &bad);
  if (bad || (stocks && (*stocks < 1 || *stocks > MAX_PICKED_STOCKS)))
  {
    *why = "a player's character, costume or stocks is out of range";
    return std::nullopt;
  }
  p.name = *name;
  p.code = *code;
  p.ready = *ready;
  p.team = static_cast<u8>(*team);
  p.in_game = *in_game;
  // Shown only while ready (docs/design/rooms.md #15).
  if (p.ready)
  {
    p.character = character;
    p.costume = costume;
    p.stocks = stocks;
  }
  return p;
}

// Appends one code point as UTF-8.
void PutUtf8(std::string& out, u32 cp)
{
  if (cp < 0x80)
  {
    out.push_back(static_cast<char>(cp));
  }
  else if (cp < 0x800)
  {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000)
  {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else
  {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}
}  // namespace

namespace
{
// `len` letters of the 20, as NormalizeRoomCode reads a room code.
std::optional<std::string> NormalizeLetters(const std::u16string& typed, size_t len)
{
  std::string out;
  for (char16_t c : typed)
  {
    if (c == 0)
      break;
    if (c >= 0xFF01 && c <= 0xFF5E)  // full-width forms (Brawl's keypad)
      c = static_cast<char16_t>(c - 0xFF01 + 0x21);
    if (c >= 'a' && c <= 'z')
      c = static_cast<char16_t>(c - 'a' + 'A');
    if (c >= 0x80 || !InAlphabet(static_cast<char>(c)) || out.size() >= len)
      return std::nullopt;
    out.push_back(static_cast<char>(c));
  }
  if (out.size() != len)
    return std::nullopt;
  return out;
}
}  // namespace

std::optional<std::string> NormalizeRulesetCode(const std::u16string& typed)
{
  return NormalizeLetters(typed, RULESET_CODE_LEN);
}

std::optional<std::string> NormalizeRulesetCode(const std::string& ascii)
{
  if (ascii.size() > 64)
    return std::nullopt;
  return NormalizeLetters(std::u16string(ascii.begin(), ascii.end()), RULESET_CODE_LEN);
}

std::optional<std::string> NormalizeRoomCode(const std::u16string& typed)
{
  return NormalizeLetters(typed, ROOM_CODE_LEN);
}

std::optional<std::string> NormalizeRoomCode(const std::string& ascii)
{
  if (ascii.size() > MAX_CODE_BYTES)
    return std::nullopt;
  std::u16string u;
  for (const char c : ascii)
  {
    if (static_cast<u8>(c) >= 0x80)
      return std::nullopt;
    u.push_back(static_cast<char16_t>(c));
  }
  return NormalizeRoomCode(u);
}

std::optional<std::string> NormalizeConnectCode(const std::string& ascii)
{
  if (ascii.size() > MAX_CODE_BYTES)
    return std::nullopt;
  const size_t hash = ascii.find('#');
  if (hash == std::string::npos || hash < 2 || hash > 4)
    return std::nullopt;
  std::string out;
  for (size_t i = 0; i < hash; ++i)
  {
    char c = ascii[i];
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
    if (c < 'A' || c > 'Z')
      return std::nullopt;
    out.push_back(c);
  }
  const std::string digits = ascii.substr(hash + 1);
  if (digits.empty() || digits.size() > 4)
    return std::nullopt;
  int number = 0;
  for (const char c : digits)
  {
    if (c < '0' || c > '9')
      return std::nullopt;
    number = number * 10 + (c - '0');
  }
  if (number < 1)
    return std::nullopt;
  out += '#';
  out += std::to_string(number);
  if (out.size() > MAX_CONNECT_CODE_LEN)
    return std::nullopt;
  return out;
}

namespace
{
// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
s64 DaysFromCivil(s64 y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const s64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<s64>(doe) - 719468;
}
}  // namespace

std::optional<s64> ParseIsoUtc(const std::string& t)
{
  if (t.size() > 40)
    return std::nullopt;
  size_t i = 0;
  // `n` digits at i, then the separator `sep` (0: none).
  const auto field = [&](size_t n, char sep) -> std::optional<int> {
    int v = 0;
    for (size_t k = 0; k < n; ++k, ++i)
    {
      if (i >= t.size() || t[i] < '0' || t[i] > '9')
        return std::nullopt;
      v = v * 10 + (t[i] - '0');
    }
    if (sep != 0)
    {
      if (i >= t.size() || t[i] != sep)
        return std::nullopt;
      ++i;
    }
    return v;
  };
  const auto y = field(4, '-');
  const auto mo = y ? field(2, '-') : std::nullopt;
  const auto d = mo ? field(2, 'T') : std::nullopt;
  const auto h = d ? field(2, ':') : std::nullopt;
  const auto mi = h ? field(2, ':') : std::nullopt;
  const auto sec = mi ? field(2, 0) : std::nullopt;
  if (!sec || *y < 2000 || *mo < 1 || *mo > 12 || *d < 1 || *d > 31 || *h > 23 || *mi > 59 ||
      *sec > 60)
  {
    return std::nullopt;
  }
  // Milliseconds: any number of fraction digits, the first three counted.
  s64 ms = 0;
  if (i < t.size() && t[i] == '.')
  {
    ++i;
    int digits = 0;
    for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i, ++digits)
    {
      if (digits < 3)
        ms = ms * 10 + (t[i] - '0');
    }
    if (digits == 0)
      return std::nullopt;
    for (; digits < 3; ++digits)
      ms *= 10;
  }
  if (i >= t.size() || t[i] != 'Z')
    return std::nullopt;
  const s64 days = DaysFromCivil(*y, static_cast<unsigned>(*mo), static_cast<unsigned>(*d));
  return ((days * 24 + *h) * 60 + *mi) * 60000 + *sec * 1000 + ms;
}

std::string CleanText(const std::string& s, size_t max_bytes)
{
  std::string out;
  for (size_t i = 0; i < s.size();)
  {
    const u8 c = static_cast<u8>(s[i]);
    u32 cp = 0xFFFD;
    size_t n = 1;
    if (c < 0x80)
    {
      cp = c;
    }
    else if ((c & 0xE0) == 0xC0 && i + 1 < s.size() && (s[i + 1] & 0xC0) == 0x80)
    {
      cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
      n = 2;
      if (cp < 0x80)
        cp = 0xFFFD;
    }
    else if ((c & 0xF0) == 0xE0 && i + 2 < s.size() && (s[i + 1] & 0xC0) == 0x80 &&
             (s[i + 2] & 0xC0) == 0x80)
    {
      cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
      n = 3;
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    }
    else if ((c & 0xF8) == 0xF0 && i + 3 < s.size() && (s[i + 1] & 0xC0) == 0x80 &&
             (s[i + 2] & 0xC0) == 0x80 && (s[i + 3] & 0xC0) == 0x80)
    {
      cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) |
           (s[i + 3] & 0x3F);
      n = 4;
      if (cp < 0x10000 || cp > 0x10FFFF)
        cp = 0xFFFD;
    }
    i += n;
    // Control characters (C0, DEL, C1) and the line/paragraph separators are dropped.
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp == 0x2028 || cp == 0x2029)
      continue;
    std::string ch;
    PutUtf8(ch, cp);
    if (out.size() + ch.size() > max_bytes)
      break;
    out += ch;
  }
  return out;
}

std::optional<View> ParseView(const picojson::object& o, std::string* why)
{
  View v;
  const auto code = Str(o, "code", MAX_CODE_BYTES);
  const auto normalized = code ? NormalizeRoomCode(*code) : std::nullopt;
  if (!normalized)
  {
    *why = "no room code of 4 of the 20 letters";
    return std::nullopt;
  }
  v.code = *normalized;
  const auto is_public = Bool(o, "public");
  const auto teams = Bool(o, "teams");
  const auto mode = Str(o, "mode", 16);
  const auto status = Str(o, "status", 16);
  const auto you = Int(o, "you", 1, ROOM_SLOTS);
  const auto host = Int(o, "host", 1, ROOM_SLOTS);
  const auto text = Str(o, "statusText", MAX_TEXT_BYTES);
  if (!is_public || !teams || !mode || !status || !you || !host || !text)
  {
    *why = "public, teams, mode, status, you, host or statusText missing or out of range";
    return std::nullopt;
  }
  v.is_public = *is_public;
  v.teams = *teams;
  if (*mode == "1v1")
    v.mode = Mode::OneVsOne;
  else if (*mode == "ffa")
    v.mode = Mode::Ffa;
  else if (*mode == "teams")
    v.mode = Mode::Teams;
  else
  {
    *why = "unknown mode";
    return std::nullopt;
  }
  if (*status == "waiting")
    v.status = Status::Waiting;
  else if (*status == "starting")
    v.status = Status::Starting;
  else if (*status == "in-game")
    v.status = Status::InGame;
  else
  {
    *why = "unknown status";
    return std::nullopt;
  }
  v.you = static_cast<int>(*you);
  v.host = static_cast<int>(*host);
  v.status_text = *text;
  // The room's rules: not fatal for the room when unusable (its games refuse them at the start).
  const ParsedRules rules = ParseRules(o);
  v.rules = rules.rules;
  v.rules_error = rules.error;
  // What it plays and its set: shown only, so a part that is not there or not valid is left out.
  if (const picojson::value* rs = Find(o, "ruleset"); rs && rs->is<picojson::object>())
  {
    const auto& ro = rs->get<picojson::object>();
    if (const auto c = Str(ro, "code", 16); c && NormalizeRulesetCode(*c))
      v.ruleset.code = *NormalizeRulesetCode(*c);
    v.ruleset.name = Str(ro, "name", MAX_NAME_BYTES).value_or("");
    v.ruleset.version = static_cast<u32>(Int(ro, "version", 0, 100000).value_or(0));
    v.ruleset.based_on = Str(ro, "basedOn", MAX_NAME_BYTES + 16).value_or("");
    v.ruleset.best_of = static_cast<u8>(Int(ro, "bestOf", 0, 7).value_or(0));
    const auto choice = Str(ro, "stageChoice", 24).value_or("loser-picks");
    v.ruleset.stage_choice = choice == "strike" || choice == "random" || choice == "loser-picks-list" ?
                                 choice :
                                 std::string("loser-picks");
    if (const picojson::value* st = Find(ro, "stages"); st && st->is<picojson::object>())
    {
      const auto& so = st->get<picojson::object>();
      const auto ids = [&](const char* key, std::vector<u16>* out) {
        const picojson::value* a = Find(so, key);
        if (!a || !a->is<picojson::array>())
          return;
        for (const picojson::value& e : a->get<picojson::array>())
        {
          if (const auto id = Int(e, 1, 0xFF); id && out->size() < 0x100)
            out->push_back(static_cast<u16>(*id));
        }
      };
      ids("starters", &v.ruleset.starters);
      ids("counterpicks", &v.ruleset.counterpicks);
    }
  }
  if (const picojson::value* sv = Find(o, "set"); sv && sv->is<picojson::object>())
  {
    const auto& so = sv->get<picojson::object>();
    const auto best_of = Int(so, "bestOf", 1, 7);
    const auto game = Int(so, "game", 1, 99);
    const picojson::value* score = Find(so, "score");
    const picojson::value* sides = Find(so, "sides");
    SetView set;
    bool ok = best_of && game && score && score->is<picojson::array>() &&
              score->get<picojson::array>().size() == 2 && sides && sides->is<picojson::array>() &&
              sides->get<picojson::array>().size() == 2;
    for (size_t i = 0; ok && i < 2; ++i)
    {
      const picojson::value& a = score->get<picojson::array>()[i];
      const picojson::value& b = sides->get<picojson::array>()[i];
      ok = a.is<double>() && b.is<double>() && a.get<double>() >= 0 && a.get<double>() <= 4 &&
           b.get<double>() >= 1 && b.get<double>() <= ROOM_SLOTS;
      if (ok)
      {
        set.score[i] = static_cast<u8>(a.get<double>());
        set.sides[i] = static_cast<int>(b.get<double>());
      }
    }
    // Each side's slots (older servers: the first only).
    for (size_t i = 0; ok && i < 2; ++i)
      set.side_slots[i] = static_cast<u8>(1u << (set.sides[i] - 1));
    if (const picojson::value* m = Find(so, "members");
        ok && m && m->is<picojson::array>() && m->get<picojson::array>().size() == 2)
    {
      for (size_t i = 0; i < 2; ++i)
      {
        const picojson::value& list = m->get<picojson::array>()[i];
        if (!list.is<picojson::array>())
          continue;
        u8 bits = 0;
        for (const picojson::value& s : list.get<picojson::array>())
        {
          if (s.is<double>() && s.get<double>() >= 1 && s.get<double>() <= ROOM_SLOTS)
            bits |= static_cast<u8>(1u << (static_cast<int>(s.get<double>()) - 1));
        }
        if (bits)
          set.side_slots[i] = bits;
      }
    }
    if (ok)
    {
      set.best_of = static_cast<u8>(*best_of);
      set.winner = static_cast<u8>(Int(so, "winner", 0, 1).value_or(0xFF));
      set.game = static_cast<u32>(*game);
      set.paused = Bool(so, "paused").value_or(false);
      set.can_resume = Bool(so, "canResume").value_or(false);
      set.finished = Bool(so, "finished").value_or(false);
      set.waiting_for = Str(so, "waitingFor", MAX_NAME_BYTES).value_or("");
      v.set = set;
    }
  }

  const picojson::value* slots = Find(o, "slots");
  if (!slots || !slots->is<picojson::array>() ||
      slots->get<picojson::array>().size() != ROOM_SLOTS)
  {
    *why = "slots is not an array of 4";
    return std::nullopt;
  }
  u32 seen = 0;
  for (const picojson::value& sv : slots->get<picojson::array>())
  {
    if (!sv.is<picojson::object>())
    {
      *why = "a slot is not an object";
      return std::nullopt;
    }
    const auto& so = sv.get<picojson::object>();
    const auto n = Int(so, "slot", 1, ROOM_SLOTS);
    const auto open = Bool(so, "open");
    const auto is_host = Bool(so, "host");
    if (!n || !open || !is_host || (seen & (1u << *n)))
    {
      *why = "a slot without a unique number 1-4, open or host";
      return std::nullopt;
    }
    seen |= 1u << *n;
    Slot& slot = v.slots[*n - 1];
    slot.open = *open;
    slot.host = *is_host;
    const picojson::value* pv = Find(so, "player");
    if (pv && pv->is<picojson::object>())
    {
      slot.player = ParsePlayer(pv->get<picojson::object>(), why);
      if (!slot.player)
        return std::nullopt;
    }
    else if (pv && !pv->is<picojson::null>())
    {
      *why = "a slot's player is neither an object nor null";
      return std::nullopt;
    }
  }
  if (!v.slots[v.you - 1].player)
  {
    *why = "`you` is an empty slot";
    return std::nullopt;
  }
  if (!v.slots[v.host - 1].player)
  {
    *why = "`host` is an empty slot";
    return std::nullopt;
  }
  return v;
}

Message Parse(const std::string& packet)
{
  Message m;
  if (packet.size() > MAX_PACKET)
  {
    m.kind = Message::Kind::Invalid;
    m.why = "packet too large";
    return m;
  }
  picojson::value root;
  const std::string err = picojson::parse(root, packet);
  if (!err.empty() || !root.is<picojson::object>())
  {
    m.kind = Message::Kind::Invalid;
    m.why = "not a JSON object";
    return m;
  }
  const auto& o = root.get<picojson::object>();
  m.type = Str(o, "type", 32).value_or("");
  const auto invalid = [&](std::string why) {
    m.kind = Message::Kind::Invalid;
    m.why = std::move(why);
    return m;
  };
  if (m.type == "hello-resp")
  {
    m.kind = Message::Kind::HelloResp;
    m.error = Str(o, "error", MAX_TEXT_BYTES).value_or("");
    m.latest_version = Str(o, "latestVersion", 32).value_or("");
  }
  else if (m.type == "room-state")
  {
    m.view = ParseView(o, &m.why);
    if (!m.view)
      return invalid(m.why);
    m.kind = Message::Kind::RoomState;
  }
  else if (m.type == "room-error")
  {
    const auto op = Str(o, "op", 32);
    const auto error = Str(o, "error", MAX_TEXT_BYTES);
    if (!op || !error || error->empty())
      return invalid("room-error without op or error");
    m.kind = Message::Kind::RoomError;
    m.op = *op;
    m.error = *error;
  }
  else if (m.type == "room-left")
  {
    m.kind = Message::Kind::RoomLeft;
    m.code = NormalizeRoomCode(Str(o, "code", MAX_CODE_BYTES).value_or("")).value_or("");
    m.reason = Str(o, "reason", MAX_TEXT_BYTES).value_or("");
  }
  else if (m.type == "room-start")
  {
    const auto code = NormalizeRoomCode(Str(o, "code", MAX_CODE_BYTES).value_or(""));
    const auto match_id = Str(o, "matchId", 128);
    if (!code || !match_id)
      return invalid("room-start without a room code or matchId");
    m.kind = Message::Kind::RoomStart;
    m.code = *code;
    m.match_id = *match_id;
    m.timeout_secs = static_cast<int>(Int(o, "timeoutSecs", 1, 120).value_or(15));
  }
  else if (m.type == "room-unreachable")
  {
    const auto match_id = Str(o, "matchId", 128);
    const picojson::value* pairs = Find(o, "pairs");
    if (!match_id || match_id->empty() || !pairs || !pairs->is<picojson::array>())
      return invalid("room-unreachable without matchId or pairs");
    for (const picojson::value& e : pairs->get<picojson::array>())
    {
      // At most the six pairs of four players.
      if (m.pairs.size() >= 6)
        break;
      if (!e.is<picojson::array>() || e.get<picojson::array>().size() != 2)
        return invalid("room-unreachable: a pair is not two slots");
      const auto a = Int(e.get<picojson::array>()[0], 1, ROOM_SLOTS);
      const auto b = Int(e.get<picojson::array>()[1], 1, ROOM_SLOTS);
      if (!a || !b || *a == *b)
        return invalid("room-unreachable: a pair is not two slots");
      m.pairs.push_back({static_cast<u8>(std::min(*a, *b)), static_cast<u8>(std::max(*a, *b))});
    }
    m.kind = Message::Kind::RoomUnreachable;
    m.match_id = *match_id;
  }
  else if (m.type == "error")
  {
    m.kind = Message::Kind::Error;
    m.error = Str(o, "error", MAX_TEXT_BYTES).value_or("");
  }
  else if (m.type == "ruleset-list")
  {
    // Shown only: an entry without a ruleset code is left out.
    m.kind = Message::Kind::RulesetList;
    const picojson::value* list = Find(o, "rulesets");
    if (list && list->is<picojson::array>())
    {
      for (const picojson::value& e : list->get<picojson::array>())
      {
        if (m.rulesets.size() >= RULESET_LIST_MAX || !e.is<picojson::object>())
          break;
        const auto& eo = e.get<picojson::object>();
        const auto code = NormalizeRulesetCode(Str(eo, "code", 16).value_or(""));
        if (!code)
          continue;
        RulesetEntry r;
        r.code = *code;
        r.name = Str(eo, "name", MAX_NAME_BYTES).value_or("");
        r.version = static_cast<u32>(Int(eo, "version", 0, 100000).value_or(0));
        r.by = Str(eo, "by", MAX_NAME_BYTES).value_or("");
        r.best_of = static_cast<u8>(Int(eo, "bestOf", 0, 7).value_or(0));
        m.rulesets.push_back(std::move(r));
      }
    }
  }
  else if (m.type == "create-ticket-resp")
  {
    m.kind = Message::Kind::CreateTicketResp;
    m.error = Str(o, "error", MAX_TEXT_BYTES).value_or("");
  }
  return m;
}

std::string HelloJson(const std::string& uid, const std::string& play_key,
                      const std::string& app_version, const std::string& platform,
                      const std::string& chat_key)
{
  picojson::object user;
  user["uid"] = picojson::value(uid);
  user["playKey"] = picojson::value(play_key);
  picojson::object o;
  o["type"] = picojson::value("hello");
  o["user"] = picojson::value(user);
  o["appVersion"] = picojson::value(app_version);
  o["platform"] = picojson::value(platform);
  if (!chat_key.empty())
    o["chatKey"] = picojson::value(chat_key);
  return picojson::value(o).serialize();
}

std::optional<std::string> RequestJson(const Request& r)
{
  picojson::object o;
  switch (r.op)
  {
  case Op::Create:
    o["type"] = picojson::value("room-create");
    o["public"] = picojson::value(r.flag);
    if (!r.ruleset.empty())
    {
      const auto code = NormalizeRulesetCode(r.ruleset);
      if (!code)
        return std::nullopt;
      o["ruleset"] = picojson::value(*code);
    }
    else if (r.remembered)
    {
      o["remembered"] = picojson::value(true);
    }
    break;
  case Op::RulesetList:
    o["type"] = picojson::value("room-rulesets");
    break;
  case Op::Ruleset:
  {
    o["type"] = picojson::value("room-ruleset");
    if (r.ruleset.empty())
    {
      o["code"] = picojson::value();
      break;
    }
    const auto code = NormalizeRulesetCode(r.ruleset);
    if (!code)
      return std::nullopt;
    o["code"] = picojson::value(*code);
    break;
  }
  case Op::Join:
  {
    const auto code = NormalizeRoomCode(r.code);
    if (!code)
      return std::nullopt;
    o["type"] = picojson::value("room-join");
    o["code"] = picojson::value(*code);
    break;
  }
  case Op::Leave:
    o["type"] = picojson::value("room-leave");
    break;
  case Op::Slot:
    if (r.slot < 1 || r.slot > ROOM_SLOTS)
      return std::nullopt;
    o["type"] = picojson::value("room-slot");
    o["slot"] = picojson::value(static_cast<double>(r.slot));
    o["open"] = picojson::value(r.flag);
    break;
  case Op::Teams:
    o["type"] = picojson::value("room-teams");
    o["on"] = picojson::value(r.flag);
    break;
  case Op::Public:
    o["type"] = picojson::value("room-public");
    o["public"] = picojson::value(r.flag);
    break;
  case Op::Team:
    if (r.team >= NUM_TEAMS)
      return std::nullopt;
    o["type"] = picojson::value("room-team");
    o["team"] = picojson::value(static_cast<double>(r.team));
    break;
  case Op::Ready:
    o["type"] = picojson::value("room-ready");
    o["ready"] = picojson::value(r.flag);
    if (r.flag && r.character)
      o["character"] = picojson::value(static_cast<double>(*r.character));
    if (r.flag && r.costume)
      o["costume"] = picojson::value(static_cast<double>(*r.costume));
    if (r.flag && r.stocks)
    {
      if (*r.stocks < 1 || *r.stocks > MAX_PICKED_STOCKS)
        return std::nullopt;
      o["stocks"] = picojson::value(static_cast<double>(*r.stocks));
    }
    break;
  case Op::Back:
    o["type"] = picojson::value("room-back");
    break;
  case Op::Unreachable:
  {
    if (r.match_id.empty() || r.match_id.size() > 128 || r.ports.empty() ||
        r.ports.size() > ROOM_SLOTS - 1)
    {
      return std::nullopt;
    }
    picojson::array slots;
    for (const int p : r.ports)
    {
      if (p < 1 || p > ROOM_SLOTS)
        return std::nullopt;
      slots.emplace_back(static_cast<double>(p));
    }
    o["type"] = picojson::value("room-unreachable");
    o["matchId"] = picojson::value(r.match_id);
    o["slots"] = picojson::value(slots);
    break;
  }
  case Op::Result:
    if (r.game < 1 || r.game > 99 ||
        (r.winner_side > 1 && r.winner_side != SIDE_DRAW && r.winner_side != SIDE_VOID) || r.stage > 0xFF)
      return std::nullopt;
    o["type"] = picojson::value("room-result");
    o["game"] = picojson::value(static_cast<double>(r.game));
    o["winnerSide"] = picojson::value(static_cast<double>(r.winner_side));
    o["stage"] = picojson::value(static_cast<double>(r.stage));
    break;
  case Op::Step:
  {
    if (r.game < 1 || r.game > 99 || r.actions[0].size() > 64 || r.actions[1].size() > 64)
      return std::nullopt;
    picojson::array lists;
    for (const auto& side : r.actions)
    {
      picojson::array a;
      for (const u8 k : side)
      {
        if (k == 0)
          return std::nullopt;
        a.emplace_back(static_cast<double>(k));
      }
      lists.emplace_back(a);
    }
    o["type"] = picojson::value("room-step");
    o["game"] = picojson::value(static_cast<double>(r.game));
    o["actions"] = picojson::value(lists);
    o["matchStarted"] = picojson::value(r.flag);
    break;
  }
  case Op::BestOf:
    if (r.best_of != 0 && r.best_of != 3 && r.best_of != 5 && r.best_of != 7)
      return std::nullopt;
    o["type"] = picojson::value("room-best-of");
    o["bestOf"] = picojson::value(static_cast<double>(r.best_of));
    break;
  case Op::Stages:
  {
    if (r.starters.empty() && r.counterpicks.empty())
      return std::nullopt;
    const auto ids = [](const std::vector<u16>& v) -> std::optional<picojson::array> {
      picojson::array a;
      for (const u16 k : v)
      {
        if (k == 0 || k > 0xFF)
          return std::nullopt;
        a.emplace_back(static_cast<double>(k));
      }
      return a;
    };
    const auto starters = ids(r.starters), counterpicks = ids(r.counterpicks);
    if (!starters || !counterpicks)
      return std::nullopt;
    picojson::object so;
    so["starters"] = picojson::value(*starters);
    so["counterpicks"] = picojson::value(*counterpicks);
    o["type"] = picojson::value("room-stages");
    o["stages"] = picojson::value(so);
    if (r.stage_choice == "loser-picks" || r.stage_choice == "loser-picks-list")
      o["stageChoice"] = picojson::value(r.stage_choice);
    else if (!r.stage_choice.empty())
      return std::nullopt;
    break;
  }
  case Op::SetAction:
    o["type"] = picojson::value("room-set");
    o["action"] = picojson::value(r.flag ? "resume" : "restart");
    break;
  case Op::Rules:
    if (!r.rules)
      return std::nullopt;
    o["type"] = picojson::value("room-rules");
    o["rules"] = picojson::value(ToJson(*r.rules));
    break;
  case Op::Poll:
  default:
    return std::nullopt;
  }
  return picojson::value(o).serialize();
}

const char* OpName(Op op)
{
  switch (op)
  {
  case Op::Poll:
    return "poll";
  case Op::Create:
    return "create";
  case Op::Join:
    return "join";
  case Op::Leave:
    return "leave";
  case Op::Slot:
    return "slot";
  case Op::Teams:
    return "teams";
  case Op::Public:
    return "public";
  case Op::Team:
    return "team";
  case Op::Ready:
    return "ready";
  case Op::Back:
    return "back";
  case Op::Rules:
    return "rules";
  case Op::Result:
    return "result";
  case Op::Step:
    return "step";
  case Op::BestOf:
    return "best-of";
  case Op::Stages:
    return "stages";
  case Op::Ruleset:
    return "ruleset";
  case Op::RulesetList:
    return "rulesets";
  case Op::SetAction:
    return "set";
  case Op::Unreachable:
    return "unreachable";
  }
  return "?";
}

const char* ModeName(Mode m)
{
  switch (m)
  {
  case Mode::OneVsOne:
    return "1v1";
  case Mode::Ffa:
    return "ffa";
  case Mode::Teams:
    return "teams";
  }
  return "?";
}

const char* StatusName(Status s)
{
  switch (s)
  {
  case Status::Waiting:
    return "waiting";
  case Status::Starting:
    return "starting";
  case Status::InGame:
    return "in-game";
  }
  return "?";
}
}  // namespace Online::Rooms
