// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/ChatText.h"

#include <algorithm>
#include <span>

namespace Online::ChatText
{
namespace
{
struct Range
{
  char32_t first, last;
};

// docs/chat-protocol.md §5, step 2 (besides the controls, planes 15-16 and the noncharacters,
// which IsRemoved checks by rule).
constexpr Range REMOVED[] = {
    {0x00AD, 0x00AD},    // soft hyphen
    {0x034F, 0x034F},    // combining grapheme joiner
    {0x061C, 0x061C},    // Arabic letter mark
    {0x115F, 0x1160},    // Hangul fillers
    {0x17B4, 0x17B5},    // Khmer inherent vowels
    {0x180B, 0x180F},    // Mongolian variation selectors and vowel separator
    {0x200B, 0x200F},    // zero-width space/joiners, LRM, RLM
    {0x2028, 0x202E},    // line/paragraph separators, bidi embeddings and overrides
    {0x2060, 0x206F},    // word joiner, invisible operators, bidi isolates, deprecated formats
    {0x3164, 0x3164},    // Hangul filler
    {0xFE00, 0xFE0F},    // variation selectors
    {0xFEFF, 0xFEFF},    // byte order mark
    {0xFFA0, 0xFFA0},    // half-width Hangul filler
    {0xFFF0, 0xFFFB},    // specials, interlinear annotation
    {0xE000, 0xF8FF},    // private use
    {0xFDD0, 0xFDEF},    // noncharacters
    {0x1D173, 0x1D17A},  // musical format controls
    {0xE0000, 0xE0FFF},  // tags, variation selectors supplement
    {0xF0000, 0x10FFFF}, // planes 15-16 (private use)
};

// Step 3.
constexpr Range WHITESPACE[] = {
    {0x0020, 0x0020},
    {0x00A0, 0x00A0},
    {0x1680, 0x1680},
    {0x2000, 0x200A},
    {0x202F, 0x202F},
    {0x205F, 0x205F},
    {0x3000, 0x3000},
};

// Step 4.
constexpr Range COMBINING[] = {
    {0x0300, 0x036F},
    {0x0483, 0x0489},
    {0x0591, 0x05BD},
    {0x0610, 0x061A},
    {0x064B, 0x065F},
    {0x0E31, 0x0E3A},
    {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF},
    {0x1DC0, 0x1DFF},
    {0x20D0, 0x20FF},
    {0x302A, 0x302F},
    {0x3099, 0x309A},
    {0xFE20, 0xFE2F},
};

bool InRanges(std::span<const Range> ranges, char32_t cp)
{
  return std::ranges::any_of(ranges, [cp](const Range& r) { return cp >= r.first && cp <= r.last; });
}

bool IsRemoved(char32_t cp)
{
  if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F))
    return true;
  if ((cp & 0xFFFE) == 0xFFFE)  // U+xFFFE and U+xFFFF of every plane
    return true;
  return InRanges(REMOVED, cp);
}

void AppendUtf8(std::string* out, char32_t cp)
{
  if (cp < 0x80)
  {
    out->push_back(static_cast<char>(cp));
  }
  else if (cp < 0x800)
  {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000)
  {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else
  {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

size_t Utf8Length(char32_t cp)
{
  return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}
}  // namespace

std::optional<char32_t> DecodeUtf8(std::string_view text, size_t* pos)
{
  const size_t i = *pos;
  if (i >= text.size())
    return std::nullopt;
  const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
  const unsigned char b0 = byte(i);
  if (b0 < 0x80)
  {
    *pos = i + 1;
    return b0;
  }
  size_t len;
  char32_t cp;
  char32_t min;
  if ((b0 & 0xE0) == 0xC0)
  {
    len = 2;
    cp = b0 & 0x1F;
    min = 0x80;
  }
  else if ((b0 & 0xF0) == 0xE0)
  {
    len = 3;
    cp = b0 & 0x0F;
    min = 0x800;
  }
  else if ((b0 & 0xF8) == 0xF0)
  {
    len = 4;
    cp = b0 & 0x07;
    min = 0x10000;
  }
  else
  {
    return std::nullopt;  // a continuation byte, or 0xF8-0xFF
  }
  if (text.size() - i < len)
    return std::nullopt;
  for (size_t k = 1; k < len; ++k)
  {
    const unsigned char b = byte(i + k);
    if ((b & 0xC0) != 0x80)
      return std::nullopt;
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    return std::nullopt;
  *pos = i + len;
  return cp;
}

size_t CountCodePoints(std::string_view utf8)
{
  return static_cast<size_t>(std::ranges::count_if(
      utf8, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::string_view Truncate(std::string_view utf8, size_t max_code_points, size_t max_bytes)
{
  size_t end = 0;
  size_t count = 0;
  while (end < utf8.size() && count < max_code_points)
  {
    size_t next = end + 1;
    while (next < utf8.size() && (static_cast<unsigned char>(utf8[next]) & 0xC0) == 0x80)
      ++next;
    if (next > max_bytes)
      break;
    end = next;
    ++count;
  }
  return utf8.substr(0, end);
}

std::optional<std::string> Clean(std::string_view text, size_t max_code_points, size_t max_bytes)
{
  std::string out;
  out.reserve(std::min(text.size(), max_bytes));
  size_t count = 0;
  size_t combining_run = 0;
  bool pending_space = false;
  bool full = false;

  size_t pos = 0;
  while (pos < text.size())
  {
    const std::optional<char32_t> decoded = DecodeUtf8(text, &pos);
    if (!decoded)
      return std::nullopt;  // the whole message, so a sender can't hide bytes behind bad UTF-8
    if (full)
      continue;  // keep checking the rest is valid UTF-8
    const char32_t cp = *decoded;
    if (IsRemoved(cp))
      continue;
    if (InRanges(WHITESPACE, cp))
    {
      pending_space = !out.empty();
      combining_run = 0;
      continue;
    }
    if (InRanges(COMBINING, cp))
    {
      if (combining_run >= MAX_COMBINING_RUN)
        continue;
      ++combining_run;
    }
    else
    {
      combining_run = 0;
    }
    const size_t space = pending_space ? 1 : 0;
    if (count + space + 1 > max_code_points || out.size() + space + Utf8Length(cp) > max_bytes)
    {
      full = true;
      continue;
    }
    if (pending_space)
    {
      out.push_back(' ');
      ++count;
      pending_space = false;
    }
    AppendUtf8(&out, cp);
    ++count;
  }
  if (out.empty())
    return std::nullopt;
  return out;
}
}  // namespace Online::ChatText
