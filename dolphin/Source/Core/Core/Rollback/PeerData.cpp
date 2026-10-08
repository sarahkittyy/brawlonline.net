// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/PeerData.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

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
  return kind <= 0x11 || (kind >= 0x13 && kind <= 0x2D) || kind == 0x32 || kind == 0x33 ||
         kind == 0x35;
}

bool ValidCostume(u32 costume)
{
  return costume <= MAX_COSTUME;
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

bool IsPermutation(const std::array<u32, 4>& mine, const std::array<u32, 4>& theirs)
{
  return std::is_permutation(mine.begin(), mine.end(), theirs.begin(), theirs.end());
}

void SanitizePad(u8* pad)
{
  if (pad[0x3C] > 3)
    pad[0x3C] = 0;
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
}  // namespace Gprb::PeerData
