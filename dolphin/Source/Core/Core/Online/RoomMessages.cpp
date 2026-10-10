// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/RoomMessages.h"

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
  if (bad)
  {
    *why = "a player's character or costume is not 0-255";
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

std::optional<std::string> NormalizeRoomCode(const std::u16string& typed)
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
    if (c >= 0x80 || !InAlphabet(static_cast<char>(c)) || out.size() >= ROOM_CODE_LEN)
      return std::nullopt;
    out.push_back(static_cast<char>(c));
  }
  if (out.size() != ROOM_CODE_LEN)
    return std::nullopt;
  return out;
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
  else if (m.type == "error")
  {
    m.kind = Message::Kind::Error;
    m.error = Str(o, "error", MAX_TEXT_BYTES).value_or("");
  }
  else if (m.type == "create-ticket-resp")
  {
    m.kind = Message::Kind::CreateTicketResp;
    m.error = Str(o, "error", MAX_TEXT_BYTES).value_or("");
  }
  return m;
}

std::string HelloJson(const std::string& uid, const std::string& play_key,
                      const std::string& app_version, const std::string& platform)
{
  picojson::object user;
  user["uid"] = picojson::value(uid);
  user["playKey"] = picojson::value(play_key);
  picojson::object o;
  o["type"] = picojson::value("hello");
  o["user"] = picojson::value(user);
  o["appVersion"] = picojson::value(app_version);
  o["platform"] = picojson::value(platform);
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
    break;
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
    break;
  case Op::Back:
    o["type"] = picojson::value("room-back");
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
