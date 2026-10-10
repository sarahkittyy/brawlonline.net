// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Online/ChatText.h"

using namespace Online::ChatText;

namespace
{
std::string U8(std::u32string_view cps)
{
  std::string out;
  for (const char32_t cp : cps)
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
  return out;
}

std::vector<char32_t> Decode(const std::string& s)
{
  std::vector<char32_t> out;
  size_t pos = 0;
  while (pos < s.size())
  {
    const auto cp = DecodeUtf8(s, &pos);
    EXPECT_TRUE(cp.has_value());
    if (!cp)
      break;
    out.push_back(*cp);
  }
  return out;
}

bool IsCombining(char32_t cp)
{
  return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x483 && cp <= 0x489) ||
         (cp >= 0x591 && cp <= 0x5BD) || (cp >= 0x610 && cp <= 0x61A) ||
         (cp >= 0x64B && cp <= 0x65F) || (cp >= 0xE31 && cp <= 0xE3A) ||
         (cp >= 0xE47 && cp <= 0xE4E) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
         (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
         (cp >= 0x302A && cp <= 0x302F) || cp == 0x3099 || cp == 0x309A ||
         (cp >= 0xFE20 && cp <= 0xFE2F);
}

bool IsForbidden(char32_t cp)
{
  return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == 0xAD || cp == 0x34F ||
         cp == 0x61C || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF ||
         (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xE0000 && cp <= 0xE0FFF) || cp >= 0xF0000 ||
         (cp >= 0xFDD0 && cp <= 0xFDEF) || (cp & 0xFFFE) == 0xFFFE || cp == 0x3164 ||
         (cp >= 0xFFF0 && cp <= 0xFFFB);
}

bool IsOtherSpace(char32_t cp)
{
  return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
         cp == 0x205F || cp == 0x3000;
}

// Everything Clean promises about what it returns.
void CheckInvariants(const std::string& in, size_t max_cp, size_t max_bytes)
{
  const auto out = Clean(in, max_cp, max_bytes);
  if (!out)
    return;
  ASSERT_FALSE(out->empty());
  ASSERT_LE(out->size(), max_bytes);
  const auto cps = Decode(*out);
  ASSERT_LE(cps.size(), max_cp);
  ASSERT_NE(cps.front(), U' ');
  ASSERT_NE(cps.back(), U' ');
  size_t run = 0;
  for (size_t i = 0; i < cps.size(); ++i)
  {
    ASSERT_FALSE(IsForbidden(cps[i])) << std::hex << static_cast<unsigned>(cps[i]);
    ASSERT_FALSE(IsOtherSpace(cps[i])) << std::hex << static_cast<unsigned>(cps[i]);
    if (i > 0)
      ASSERT_FALSE(cps[i] == U' ' && cps[i - 1] == U' ');
    run = IsCombining(cps[i]) ? run + 1 : 0;
    ASSERT_LE(run, MAX_COMBINING_RUN);
  }
  // Cleaning is idempotent.
  ASSERT_EQ(Clean(*out, max_cp, max_bytes), out);
}
}  // namespace

TEST(ChatText, KeepsOrdinaryText)
{
  EXPECT_EQ(Clean("gg one more?"), "gg one more?");
  EXPECT_EQ(Clean(U8(U"ねこ ｗｏｒｌｄ 漢字 café Ωμέγα привет")),
            U8(U"ねこ ｗｏｒｌｄ 漢字 café Ωμέγα привет"));
  EXPECT_EQ(Clean("<script>alert(1)</script> %s%n%x {}"),
            "<script>alert(1)</script> %s%n%x {}");  // nothing interprets it
}

TEST(ChatText, InvalidUtf8DropsTheMessage)
{
  EXPECT_EQ(Clean("ok\xFF"), std::nullopt);
  EXPECT_EQ(Clean("\xC0\xAF"), std::nullopt);              // overlong '/'
  EXPECT_EQ(Clean("\xE0\x80\xAF"), std::nullopt);          // overlong
  EXPECT_EQ(Clean("\xED\xA0\x80"), std::nullopt);          // surrogate
  EXPECT_EQ(Clean("\xF4\x90\x80\x80"), std::nullopt);      // above U+10FFFF
  EXPECT_EQ(Clean("abc\xE3\x81"), std::nullopt);           // truncated
  EXPECT_EQ(Clean("\x80"), std::nullopt);                  // lone continuation
  EXPECT_EQ(Clean(std::string("a\0b", 3)), "ab");          // NUL is a control: removed
}

TEST(ChatText, RemovesControlsBidiAndInvisibles)
{
  EXPECT_EQ(Clean("a\r\nb\tc\x1B[31md\x7F"), "abc[31md");
  EXPECT_EQ(Clean(U8(U"x\u0085y\u009Bz")), "xyz");
  // Trojan-source style reordering: every bidi control goes.
  EXPECT_EQ(Clean(U8(U"\u202Eevil\u202C \u2066a\u2069\u200F\u061C")), "evil a");
  EXPECT_EQ(Clean(U8(U"zero\u200Bwidth\u200D\u2060\uFEFF")), "zerowidth");
  EXPECT_EQ(Clean(U8(U"tag\U000E0041\U000E007F var\uFE0F\U000E0100")), "tag var");
  EXPECT_EQ(Clean(U8(U"pua\uE000\U000F0000\U0010FFFD")), "pua");
  EXPECT_EQ(Clean(U8(U"non\uFDD0\uFFFE\U0001FFFF")), "non");
  EXPECT_EQ(Clean(U8(U"fill\u3164\u115F\uFFA0er")), "filler");
  EXPECT_EQ(Clean(U8(U"line\u2028para\u2029")), "linepara");
  EXPECT_EQ(Clean(U8(U"\u200B\u202E")), std::nullopt);  // nothing left
}

TEST(ChatText, CollapsesWhitespace)
{
  EXPECT_EQ(Clean("   a    b   "), "a b");
  EXPECT_EQ(Clean(U8(U"a\u00A0\u3000\u2003b")), "a b");
  EXPECT_EQ(Clean(U8(U"\u3000\u00A0")), std::nullopt);
  EXPECT_EQ(Clean("a \x01 b"), "a b");
}

TEST(ChatText, LimitsCombiningRuns)
{
  // "Zalgo": at most two marks in a row survive.
  EXPECT_EQ(Clean(U8(U"e\u0301\u0302\u0303\u0304\u0305x")), U8(U"e\u0301\u0302x"));
  // A removed character between marks does not start a new run.
  EXPECT_EQ(Clean(U8(U"e\u0301\u200B\u0302\u0303")), U8(U"e\u0301\u0302"));
  // Japanese voiced marks are combining too, two are fine.
  EXPECT_EQ(Clean(U8(U"か\u3099")), U8(U"か\u3099"));
}

TEST(ChatText, CutsOnCodePointBoundaries)
{
  const std::string kana = U8(std::u32string(150, U'ね'));  // 3 bytes each
  const auto out = Clean(kana);
  ASSERT_TRUE(out);
  EXPECT_EQ(out->size(), MAX_MESSAGE_CODE_POINTS * 3);
  const std::string ascii(500, 'a');
  EXPECT_EQ(Clean(ascii)->size(), MAX_MESSAGE_CODE_POINTS);
  EXPECT_EQ(Clean(ascii, 1000, MAX_MESSAGE_BYTES)->size(), MAX_MESSAGE_BYTES);
  // 4-byte characters never split.
  const std::string emoji = U8(std::u32string(200, U'\U0001F600'));
  const auto e = Clean(emoji, 1000, 301);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->size(), 300u);
  // A name.
  EXPECT_EQ(Decode(*Clean(ascii, MAX_NAME_CODE_POINTS)).size(), MAX_NAME_CODE_POINTS);
  // The cut drops a trailing space instead of ending on it.
  EXPECT_EQ(Clean("ab cd", 3, 300), "ab");
}

TEST(ChatText, Truncate)
{
  EXPECT_EQ(Truncate(U8(U"aねb"), 2, 100), U8(U"aね"));
  EXPECT_EQ(Truncate(U8(U"aねb"), 10, 3), "a");
  EXPECT_EQ(Truncate("", 10, 10), "");
  EXPECT_EQ(CountCodePoints(U8(U"aね\U0001F600")), 3u);
}

TEST(ChatText, FuzzInvariants)
{
  std::mt19937 rng(20261010);
  // Pieces that exercise every rule, mixed with random bytes.
  const std::vector<std::string> pieces = {
      "a",          " ",           "\t",          "\n",        "\x7F",
      U8(U"\u0301"), U8(U"\u202E"), U8(U"\u200B"), U8(U"ね"),  U8(U"\u3000"),
      U8(U"\uFEFF"), U8(U"\U000E0041"), U8(U"\U0001F600"), U8(U"\uE000"), "\xFF",
      "\xC3",        U8(U"\u3099"), U8(U"\u00A0"), "%n",       "##"};
  for (int i = 0; i < 200000; ++i)
  {
    std::string s;
    const int n = static_cast<int>(rng() % 160);
    for (int k = 0; k < n; ++k)
    {
      if (rng() % 4 == 0)
        s.push_back(static_cast<char>(rng() & 0xFF));
      else
        s += pieces[rng() % pieces.size()];
    }
    const size_t max_cp = 1 + rng() % 120;
    const size_t max_bytes = 4 + rng() % 400;
    CheckInvariants(s, max_cp, max_bytes);
    if (HasFatalFailure())
    {
      ADD_FAILURE() << "input index " << i;
      return;
    }
  }
}
