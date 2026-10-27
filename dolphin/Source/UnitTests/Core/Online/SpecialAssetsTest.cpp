// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Special Versus box's bundle (Core/Online/SpecialAssets.h): LZ11, and the box's entries
// picked out of an archive laid out as P+'s main-menu archive is.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Online/SpecialAssets.h"

using namespace Online::SpecialAssets;

namespace
{
void Put32(std::vector<u8>& b, size_t at, u32 v)
{
  for (int i = 0; i < 4; ++i)
    b[at + i] = static_cast<u8>(v >> (24 - 8 * i));
}
u32 Get32(const std::vector<u8>& b, size_t at)
{
  return u32(b[at]) << 24 | u32(b[at + 1]) << 16 | u32(b[at + 2]) << 8 | b[at + 3];
}

// An ARC of (type, index, data) entries, as Brawl's .pac files are.
std::vector<u8> Arc(const std::string& name, const std::vector<std::tuple<u16, u16, std::vector<u8>>>& entries)
{
  std::vector<u8> a(0x40, 0);
  std::copy_n("ARC\0", 4, a.begin());
  a[5] = 1;
  a[6] = static_cast<u8>(entries.size() >> 8);
  a[7] = static_cast<u8>(entries.size());
  std::copy(name.begin(), name.end(), a.begin() + 0x10);
  for (const auto& [type, index, data] : entries)
  {
    const size_t at = a.size();
    a.resize(at + 0x20, 0);
    a[at] = static_cast<u8>(type >> 8);
    a[at + 1] = static_cast<u8>(type);
    a[at + 2] = static_cast<u8>(index >> 8);
    a[at + 3] = static_cast<u8>(index);
    Put32(a, at + 4, static_cast<u32>(data.size()));
    a.insert(a.end(), data.begin(), data.end());
    a.resize((a.size() + 0x1F) & ~size_t{0x1F}, 0);
  }
  return a;
}

std::vector<u8> Bres(const std::string& names)
{
  std::vector<u8> b = {'b', 'r', 'e', 's', 0xFE, 0xFF, 0, 0};
  b.insert(b.end(), {'M', 'D', 'L', '0'});
  b.insert(b.end(), names.begin(), names.end());
  b.push_back(0);
  return b;
}

std::vector<u8> MenuArchive(bool with_frame)
{
  std::vector<std::tuple<u16, u16, std::vector<u8>>> menu;
  menu.emplace_back(2, 7, Bres("MenMainIcon0121_TopN"));
  menu.emplace_back(3, 14, Bres(with_frame ? std::string("MenMainSpecial04Frame\0x", 23) : "other"));
  for (int r = 1; r <= 10; ++r)
  {
    char name[32];
    snprintf(name, sizeof(name), "MenMainSpecial%04d_TopN", r);
    menu.emplace_back(2, static_cast<u16>(119 + r), Bres(name));
  }
  std::vector<u8> msbin(0x100, 0);
  Put32(msbin, 0, 0xF0);
  menu.emplace_back(1, 1, msbin);
  return Arc("arc", {{1, 0, Bres("x")}, {1, 13, Arc("MenuMain_en", menu)}});
}
}  // namespace

TEST(SpecialAssets, Lz11)
{
  // "abcabcabcd": four literals, then a back-reference of length 6 at distance 3 (LZ11's short
  // form: (len - 1) << 4 | (disp - 1) >> 8, (disp - 1) & 0xFF), then a literal.
  const std::vector<u8> in = {0x11, 10, 0, 0, 0x10, 'a', 'b', 'c', 0x50, 0x02, 'd'};
  const auto out = DecompressLz11(in, 100);
  ASSERT_TRUE(out);
  EXPECT_EQ(std::string(out->begin(), out->end()), "abcabcabcd");
  // A size beyond the limit, a back-reference before the start and a short stream are refused.
  EXPECT_FALSE(DecompressLz11(in, 9));
  EXPECT_FALSE(DecompressLz11(std::vector<u8>{0x11, 4, 0, 0, 0x80, 0x00, 0x05}, 100));
  EXPECT_FALSE(DecompressLz11(std::vector<u8>{0x11, 10, 0, 0, 0x00, 'a'}, 100));
  EXPECT_FALSE(DecompressLz11(std::vector<u8>{0x12, 1, 0, 0, 0x00, 'a'}, 100));
}

TEST(SpecialAssets, BundleHasTheBoxsEntriesInRoleOrder)
{
  std::string why;
  const auto b = BuildBundle(MenuArchive(true), &why);
  ASSERT_TRUE(b) << why;
  EXPECT_EQ(Get32(*b, 0), MAGIC);
  EXPECT_EQ(Get32(*b, 4), b->size());
  EXPECT_EQ(Get32(*b, 8), ROLE_COUNT);
  for (u32 r = 0; r < ROLE_COUNT; ++r)
  {
    const size_t e = HEADER + r * ENTRY;
    EXPECT_EQ((*b)[e + 1], r);
    const u32 off = Get32(*b, e + 4), size = Get32(*b, e + 8);
    EXPECT_EQ(off % 0x20, 0u);
    ASSERT_LE(off + size, b->size());
    if (r >= ROLE_MODEL_FIRST && r <= ROLE_MODEL_LAST)
    {
      char name[32];
      snprintf(name, sizeof(name), "MenMainSpecial%04u_TopN", r);
      const std::string body(b->begin() + off, b->begin() + off + size);
      EXPECT_NE(body.find(name), std::string::npos) << r;
    }
  }
  // Another archive, or one without the box's frame texture, makes no bundle.
  EXPECT_FALSE(BuildBundle(MenuArchive(false), &why));
  EXPECT_NE(why.find("part 0"), std::string::npos) << why;
  EXPECT_FALSE(BuildBundle(std::vector<u8>(0x100, 0), &why));
}
