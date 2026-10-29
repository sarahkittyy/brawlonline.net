// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/PeerData.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

namespace Gprb::PeerData
{
bool JsonDepthOk(std::string_view text, int max_depth)
{
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char c : text)
  {
    if (in_string)
    {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        in_string = false;
      continue;
    }
    switch (c)
    {
    case '"':
      in_string = true;
      break;
    case '[':
    case '{':
      if (++depth > max_depth)
        return false;
      break;
    case ']':
    case '}':
      --depth;
      break;
    default:
      break;
    }
  }
  return true;
}

bool JsonSafeToParse(std::string_view text, int max_depth)
{
  if (!JsonDepthOk(text, max_depth))
    return false;
  bool in_string = false;
  bool escaped = false;
  for (size_t i = 0; i < text.size(); ++i)
  {
    const char c = text[i];
    if (in_string)
    {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        in_string = false;
      continue;
    }
    if (c == '"')
    {
      in_string = true;
      continue;
    }
    if (c != '-' && (c < '0' || c > '9'))
      continue;
    // A number token as picojson reads it (_parse_number): digits, signs, exponent, point.
    size_t end = i;
    while (end < text.size() && ((text[end] >= '0' && text[end] <= '9') || text[end] == '+' ||
                                 text[end] == '-' || text[end] == 'e' || text[end] == 'E' ||
                                 text[end] == '.'))
    {
      ++end;
    }
    const std::string token(text.substr(i, end - i));
    char* parsed_end = nullptr;
    const double d = std::strtod(token.c_str(), &parsed_end);
    if (!std::isfinite(d))
      return false;
    i = end - 1;
  }
  return true;
}

std::optional<picojson::object> ParseControl(std::string_view text)
{
  if (text.empty() || text.size() > MAX_CONTROL_SIZE || !JsonSafeToParse(text))
    return std::nullopt;
  picojson::value v;
  std::string err;
  picojson::parse(v, text.begin(), text.end(), &err);
  if (!err.empty() || !v.is<picojson::object>())
    return std::nullopt;
  return v.get<picojson::object>();
}

namespace
{
int HexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
}  // namespace

std::optional<std::vector<u8>> ParseHex(std::string_view hex, size_t bytes)
{
  if (hex.size() != 2 * bytes)
    return std::nullopt;
  std::vector<u8> out(bytes);
  for (size_t i = 0; i < bytes; ++i)
  {
    const int hi = HexDigit(hex[2 * i]);
    const int lo = HexDigit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return std::nullopt;
    out[i] = static_cast<u8>((hi << 4) | lo);
  }
  return out;
}

std::optional<u64> JsonUInt(const picojson::value* v, u64 max)
{
  if (!v || !v->is<double>())
    return std::nullopt;
  const double d = v->get<double>();
  // 2^53: every whole number up to here is exact; max is never above 2^32 in practice.
  if (!std::isfinite(d) || d < 0 || d > 9007199254740992.0 || std::floor(d) != d)
    return std::nullopt;
  const u64 n = static_cast<u64>(d);
  if (n > max)
    return std::nullopt;
  return n;
}

bool ValidCharKind(u32 kind)
{
  return kind <= 0x11 || (kind >= 0x13 && kind <= 0x2B) || kind == 0x32 || kind == 0x33 ||
         kind == 0x35;
}

bool ValidCostume(u32 costume)
{
  const u32 alt = costume & (COSTUME_ALT_Z | COSTUME_ALT_R);
  return (costume & ~alt) <= MAX_COSTUME && alt != (COSTUME_ALT_Z | COSTUME_ALT_R);
}

bool ValidTeam(u32 team)
{
  return team <= 2 || team == 0xFF;
}

bool ValidPort(u64 port)
{
  return port < static_cast<u64>(MAX_PORTS);
}

void SetGoneMarker(u8* pad)
{
  std::memset(pad, 0, PAD_SIZE);
  std::memcpy(pad + GONE_MARK_OFFSET, GONE_MARK.data(), GONE_MARK.size());
}

bool TakeGoneMarker(u8* pad)
{
  if (std::memcmp(pad + GONE_MARK_OFFSET, GONE_MARK.data(), GONE_MARK.size()) != 0)
    return false;
  // A neutral, connected GameCube controller, whatever else the bytes said.
  std::memset(pad, 0, PAD_SIZE);
  return true;
}

void ClearGoneMarkerBytes(u8* pad)
{
  std::memset(pad + GONE_MARK_OFFSET, 0, GONE_MARK.size());
}

bool ValidStageKind(u32 kind)
{
  // P+ v3.2's stage select offers 0x01-0x33 and 0x37 (Brawl's kinds) and 0x40-0x52 (P+'s own
  // stages), read from its page and slot tables. Never a versus stage: 0x26 Config (the rules
  // menu), 0x27, 0x28 Result, and the single-player modes 0x34-0x36 (Home-Run Contest, Stage
  // Builder, rest area) and 0x38-0x3F (Target Smash, roll call, Subspace). 0x7F bounds room for
  // more of P+'s stages; the game plugin checks the exact list.
  if (kind < 0x01 || kind > 0x7F)
    return false;
  if (kind >= 0x26 && kind <= 0x28)
    return false;
  if ((kind >= 0x34 && kind <= 0x36) || (kind >= 0x38 && kind <= 0x3F))
    return false;
  return true;
}

std::vector<u8> MergeInitBlock(const std::vector<u8>& mine, const std::vector<u8>& host,
                               bool* rejected)
{
  *rejected = false;
  if (mine.size() != INIT_BLOCK_SIZE || host.size() != INIT_BLOCK_SIZE)
  {
    *rejected = !host.empty();
    return mine;
  }
  for (size_t i = 0; i < INIT_BLOCK_SIZE; ++i)
  {
    if (i != INIT_BLOCK_VARIANT && mine[i] != host[i])
    {
      *rejected = true;
      return mine;
    }
  }
  if (host[INIT_BLOCK_VARIANT] > MAX_STAGE_VARIANT)
  {
    *rejected = true;
    return mine;
  }
  std::vector<u8> out = mine;
  out[INIT_BLOCK_VARIANT] = host[INIT_BLOCK_VARIANT];
  return out;
}

size_t LegacyTeamCostume(const std::vector<CostumeEntry>& list, u8 team_colour, size_t costume)
{
  std::optional<size_t> first;
  for (size_t i = 0; i < list.size() && i < MAX_COSTUME_LIST; ++i)
  {
    if (list[i].colour == COSTUME_LIST_END)
      break;
    if (list[i].colour != team_colour)
      continue;
    if (i == costume)
      return costume;
    if (!first)
      first = i;
  }
  return first.value_or(costume);
}

std::optional<std::array<std::optional<CostumePick>, MAX_PORTS>>
LegacyTeamCostumes(const std::array<LegacyCostumePlayer, MAX_PORTS>& players,
                   const std::array<u8, 3>& team_colours)
{
  std::array<std::optional<CostumePick>, MAX_PORTS> out;
  bool any = false;
  for (int port = 0; port < MAX_PORTS; ++port)
  {
    const auto& p = players[port];
    if (!p.present)
      continue;
    if (p.costume > MAX_COSTUME || p.team >= team_colours.size())
      return std::nullopt;
    for (const auto& list : p.lists)
    {
      const size_t i = LegacyTeamCostume(list, team_colours[p.team], p.costume);
      if (i < list.size() && i < MAX_COSTUME_LIST && list[i].colour != COSTUME_LIST_END &&
          list[i].file == p.recorded_file)
      {
        out[port] = CostumePick{static_cast<u8>(i), list[i].file};
        break;
      }
    }
    if (!out[port])
      return std::nullopt;
    any = true;
  }
  if (!any)
    return std::nullopt;
  return out;
}

bool IsPermutation(const std::array<u32, 4>& mine, const std::array<u32, 4>& theirs)
{
  return std::is_permutation(mine.begin(), mine.end(), theirs.begin(), theirs.end());
}

void SanitizePad(u8* pad)
{
  // The type is a u32 (guest byte order): 0-3, else GameCube.
  if (pad[0x3C] != 0 || pad[0x3D] != 0 || pad[0x3E] != 0 || pad[0x3F] > 3)
    std::memset(pad + 0x3C, 0, 4);
  const u8 err = pad[0x38];
  if (err != 0x00 && err != 0xFF && err != 0xFE && err != 0xFD)
    pad[0x38] = 0;
  for (size_t off = 0x18; off <= 0x2C; off += 4)
  {
    u32 be;
    std::memcpy(&be, pad + off, 4);
    // Guest floats are big-endian.
    const u32 bits = (be >> 24) | ((be >> 8) & 0xFF00) | ((be << 8) & 0xFF0000) | (be << 24);
    const float f = std::bit_cast<float>(bits);
    if (!std::isfinite(f) || std::fabs(f) > 1e6f)
      std::memset(pad + off, 0, 4);
  }
}

std::vector<std::vector<u8>> ControlDatagrams(std::string_view text, u32 id)
{
  std::vector<std::vector<u8>> out;
  if (text.size() + 1 <= MAX_DATAGRAM)
  {
    std::vector<u8> pkt(text.size() + 1);
    pkt[0] = 'C';
    std::memcpy(pkt.data() + 1, text.data(), text.size());
    out.push_back(std::move(pkt));
    return out;
  }
  const size_t count = (text.size() + FRAGMENT_PIECE - 1) / FRAGMENT_PIECE;
  if (count > MAX_FRAGMENTS)
    return out;  // over MAX_CONTROL_SIZE: the others would refuse it anyway
  for (size_t i = 0; i < count; ++i)
  {
    const std::string_view piece = text.substr(i * FRAGMENT_PIECE, FRAGMENT_PIECE);
    std::vector<u8> pkt(FRAGMENT_HEADER + piece.size());
    pkt[0] = 'F';
    for (int b = 0; b < 4; ++b)
      pkt[1 + b] = static_cast<u8>(id >> (8 * b));
    pkt[5] = static_cast<u8>(i);
    pkt[6] = static_cast<u8>(count);
    std::memcpy(pkt.data() + FRAGMENT_HEADER, piece.data(), piece.size());
    out.push_back(std::move(pkt));
  }
  return out;
}

bool AppendGekkoBundle(std::vector<u8>& bundle, const u8* msg, size_t size)
{
  if (!msg || size == 0 || size > 0xFFFF)
    return false;
  const size_t start = bundle.empty() ? 1 : 0;
  if (bundle.size() + start + 2 + size > MAX_GEKKO_BUNDLE)
    return false;
  size_t count = 0;
  for (size_t pos = 1; pos + 2 <= bundle.size(); ++count)
    pos += 2 + (bundle[pos] | (bundle[pos + 1] << 8));
  if (count >= MAX_BUNDLED_MESSAGES)
    return false;
  if (start)
    bundle.push_back('B');
  bundle.push_back(static_cast<u8>(size));
  bundle.push_back(static_cast<u8>(size >> 8));
  bundle.insert(bundle.end(), msg, msg + size);
  return true;
}

std::optional<std::vector<std::vector<u8>>> SplitGekkoBundle(const u8* data, size_t size)
{
  if (!data || size < 1 || data[0] != 'B')
    return std::nullopt;
  std::vector<std::vector<u8>> out;
  size_t pos = 1;
  while (pos < size)
  {
    if (pos + 2 > size || out.size() >= MAX_BUNDLED_MESSAGES)
      return std::nullopt;
    const size_t len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (len == 0 || len > size - pos)
      return std::nullopt;
    out.emplace_back(data + pos, data + pos + len);
    pos += len;
  }
  if (out.empty())
    return std::nullopt;
  return out;
}

std::optional<std::string> ControlReassembler::Add(u64 source, const u8* data, size_t size)
{
  if (!data || size <= FRAGMENT_HEADER || size > MAX_DATAGRAM || data[0] != 'F')
    return std::nullopt;
  u32 id = 0;
  for (int b = 0; b < 4; ++b)
    id |= static_cast<u32>(data[1 + b]) << (8 * b);
  const u8 index = data[5], count = data[6];
  if (count < 2 || count > MAX_FRAGMENTS || index >= count)
    return std::nullopt;
  // Every piece but the last is full, so the pieces cannot add up to more than count pieces.
  const size_t piece_size = size - FRAGMENT_HEADER;
  if (index + 1 < count ? piece_size != FRAGMENT_PIECE : piece_size > FRAGMENT_PIECE)
    return std::nullopt;

  auto it = std::find_if(m_partials.begin(), m_partials.end(),
                         [&](const Partial& p) { return p.source == source; });
  if (it == m_partials.end())
  {
    if (m_partials.size() >= MAX_REASSEMBLY_SOURCES)
    {
      it = std::min_element(m_partials.begin(), m_partials.end(),
                            [](const Partial& a, const Partial& b) { return a.touched < b.touched; });
      *it = Partial{};
    }
    else
    {
      it = m_partials.emplace(m_partials.end());
    }
    it->source = source;
    it->id = id;
    it->count = count;
  }
  else if (it->id != id || it->count != count)
  {
    *it = Partial{};
    it->source = source;
    it->id = id;
    it->count = count;
  }
  it->touched = ++m_clock;
  if (it->have & (1u << index))
    return std::nullopt;  // a duplicate
  it->have |= 1u << index;
  it->pieces[index].assign(reinterpret_cast<const char*>(data) + FRAGMENT_HEADER, piece_size);
  if (it->have != (1u << count) - 1)
    return std::nullopt;
  std::string text;
  for (u8 i = 0; i < count; ++i)
    text += it->pieces[i];
  m_partials.erase(it);
  return text;
}

std::string TruncateName(const std::string& name)
{
  if (name.size() <= MAX_NAME_LEN)
    return name;
  // Do not cut a UTF-8 sequence in half.
  size_t n = MAX_NAME_LEN;
  while (n > 0 && (static_cast<u8>(name[n]) & 0xC0) == 0x80)
    --n;
  return name.substr(0, n);
}

std::string LeaveReason(std::string_view why)
{
  std::string out(why.substr(0, MAX_LEAVE_REASON_LEN));
  for (char& c : out)
  {
    if (c < 0x20 || c > 0x7E)
      c = '?';
  }
  return out;
}

std::vector<std::string> SetupKeyDiff(const std::vector<u8>& mine, const std::vector<u8>& theirs,
                                      size_t max_lines)
{
  std::vector<std::string> out;
  size_t more = 0;
  const auto add = [&](std::string line) {
    if (out.size() < max_lines)
      out.push_back(std::move(line));
    else
      ++more;
  };
  if (mine.size() != theirs.size())
    add(fmt::format("length {} bytes, peer {}", mine.size(), theirs.size()));
  const auto hex = [](const std::vector<u8>& k, size_t o, size_t n) {
    if (o >= k.size())
      return std::string("none");
    std::string s;
    for (size_t i = o; i < std::min(o + n, k.size()); ++i)
      s += fmt::format("{:02x}", k[i]);
    return s;
  };
  size_t o = 0;
  const auto field = [&](size_t n, const std::string& name) {
    const std::string a = hex(mine, o, n), b = hex(theirs, o, n);
    if (a != b)
      add(fmt::format("{} {}, peer {}", name, a, b));
    o += n;
  };
  const auto at = [&](size_t i) -> u8 { return i < mine.size() ? mine[i] : 0; };

  // The init block (gmGlobalModeMelee +0x08: stage kind, rules, time limit, stage variant).
  constexpr size_t INIT = 0x20, INIT_TEAMS = 0x0B;
  for (size_t i = 0; i < INIT; ++i)
    field(1, fmt::format("init block +{:#04x}", i));
  // Per player: character, state, stocks, costume, and in a team battle the team.
  const bool teams = at(INIT_TEAMS) != 0;
  for (int p = 1; p <= 4; ++p)
  {
    field(1, fmt::format("P{} character", p));
    field(1, fmt::format("P{} state", p));
    field(1, fmt::format("P{} stocks", p));
    field(1, fmt::format("P{} costume", p));
    if (teams)
      field(1, fmt::format("P{} team", p));
  }
  // g_PadConfig: each GameCube pad's layout (or, for a port whose player plays its Wii Remote
  // pad, their Wii layouts folded into the same 12 bytes), then player -> pad.
  static constexpr const char* PAD_BYTES[12] = {"L",       "R", "Z", "D-pad up", "D-pad side",
                                                "D-pad down", "A", "B", "C-stick",  "Y",
                                                "X",       "flags"};
  for (int pad = 0; pad < 4; ++pad)
  {
    for (const char* b : PAD_BYTES)
      field(1, fmt::format("pad {} {}", pad + 1, b));
  }
  for (int p = 1; p <= 4; ++p)
    field(1, fmt::format("P{} pad", p));
  // Brawl's set rule (16 bytes), then under stock control (+5 = 2) each player's handicap.
  const size_t rule = o;
  for (size_t i = 0; i < 0x10; ++i)
    field(1, fmt::format("set rule +{:#04x}", i));
  if (at(rule + 5) == 2)
  {
    for (int p = 1; p <= 4; ++p)
      field(2, fmt::format("P{} stock control handicap", p));
  }
  field(1, "hazards");
  for (size_t i = 0; i < 0x10; ++i)
    field(1, fmt::format("item switch +{:#04x}", i));
  // P+'s Code Menu values (4 bytes a line), then a desync restart's restored stocks, damage and
  // time when there is one.
  const size_t end = std::max(mine.size(), theirs.size());
  for (int line = 0; o < end; ++line)
    field(4, fmt::format("code menu value {} (or a restart's restore)", line));
  if (more)
    out.push_back(fmt::format("and {} more", more));
  return out;
}

bool ValidRosterColour(u8 colour)
{
  return colour <= MAX_ROSTER_COLOUR;
}

std::optional<size_t> SetupKeyCodeMenuAt(const std::vector<u8>& key)
{
  constexpr size_t INIT = 0x20, INIT_TEAMS = 0x0B, PADS = 4 * 12 + 4, RULE = 0x10,
                   RULE_HANDICAP = 5, HANDICAPS = 4 * 2, ITEM_SWITCH = 0x10;
  if (key.size() < INIT)
    return std::nullopt;
  size_t o = INIT + MAX_PORTS * (key[INIT_TEAMS] != 0 ? 5 : 4) + PADS;
  if (key.size() < o + RULE)
    return std::nullopt;
  const bool stock_control = key[o + RULE_HANDICAP] == 2;
  o += RULE + (stock_control ? HANDICAPS : 0) + 1 + ITEM_SWITCH;
  if (key.size() < o)
    return std::nullopt;
  return o;
}

ReplayCodeMenuLine ClassifyReplayCodeMenuLine(std::string_view text, bool slot_colour)
{
  if (slot_colour)
    return ReplayCodeMenuLine::Recorded;
  // P+'s replay autosave writes the game's replay to the SD card after the match (playback keeps
  // its own, OFF: no write to the playback card); the activation only lets the menu open.
  for (const std::string_view ours : {"Autosave Replays:", "Code Menu Activation:"})
  {
    if (text.starts_with(ours))
      return ReplayCodeMenuLine::Ours;
  }
  return ReplayCodeMenuLine::Compared;
}

std::vector<u8> ReplaySetupKey(const std::vector<u8>& here, const std::vector<u8>& recorded,
                               const std::vector<ReplayCodeMenuLine>& lines)
{
  std::vector<u8> out = recorded;
  const auto at = SetupKeyCodeMenuAt(here);
  const size_t size = 4 * lines.size();
  if (!at || SetupKeyCodeMenuAt(recorded) != at || here.size() < *at + size ||
      recorded.size() < *at + size)
  {
    return out;
  }
  for (size_t i = 0; i < lines.size(); ++i)
  {
    if (lines[i] != ReplayCodeMenuLine::Compared)
      std::copy_n(here.begin() + *at + 4 * i, 4, out.begin() + *at + 4 * i);
  }
  return out;
}

std::vector<std::optional<u32>> RecordedCodeMenuValues(const std::vector<u8>& recorded,
                                                       const std::vector<ReplayCodeMenuLine>& lines)
{
  std::vector<std::optional<u32>> out(lines.size());
  const auto at = SetupKeyCodeMenuAt(recorded);
  if (!at || recorded.size() < *at + 4 * lines.size())
    return out;
  for (size_t i = 0; i < lines.size(); ++i)
  {
    if (lines[i] != ReplayCodeMenuLine::Recorded)
      continue;
    const u8* v = recorded.data() + *at + 4 * i;
    const u32 value = u32(v[0]) << 24 | u32(v[1]) << 16 | u32(v[2]) << 8 | v[3];
    if (value <= MAX_RECORDED_SLOT_COLOUR)
      out[i] = value;
  }
  return out;
}
}  // namespace Gprb::PeerData
