// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// BatchedChangeCRC (Core/Rollback/ChangeCRC.h, GitHub issue #14): the change CRC's batched feed.
// Peers compare the CRC, so batching must give the one the serial feed gave (two crc32 calls per
// granule, its page number and then its 64 bytes) whatever the granule count or flush boundaries.

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Core/Rollback/ChangeCRC.h"

namespace
{
using Rollback::BatchedChangeCRC;
constexpr size_t GRANULE = BatchedChangeCRC::kGranuleSize;

struct Stream
{
  std::vector<u32> pages;
  std::vector<u8> bytes;
};

Stream MakeStream(u32 granules, u32 seed)
{
  std::mt19937 rng(seed);
  Stream s;
  u32 page = rng() % 1000;
  for (u32 g = 0; g < granules; ++g)
  {
    page += 1 + rng() % 40;  // ascending, as a save's changed granules
    s.pages.push_back(page);
    for (size_t b = 0; b < GRANULE; ++b)
      s.bytes.push_back(static_cast<u8>(rng()));
  }
  return s;
}

// The serial feed as RollbackManager::UpdateShadowAfterSave did it before.
u32 Serial(const Stream& s)
{
  u32 crc = Common::StartCRC32();
  for (size_t g = 0; g < s.pages.size(); ++g)
  {
    crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(&s.pages[g]), sizeof(u32));
    crc = Common::UpdateCRC32(crc, s.bytes.data() + g * GRANULE, GRANULE);
  }
  return crc;
}

u32 Batched(const Stream& s)
{
  BatchedChangeCRC feed;
  for (size_t g = 0; g < s.pages.size(); ++g)
    feed.Feed(s.pages[g], s.bytes.data() + g * GRANULE);
  return feed.Finish();
}
}  // namespace

TEST(BatchedChangeCRC, MatchesTheSerialFeedAcrossFlushBoundaries)
{
  constexpr u32 N = BatchedChangeCRC::kGranulesPerFlush;
  for (const u32 granules : {0u, 1u, 2u, N - 1, N, N + 1, 2 * N, 3 * N + 17, 1000u, 20000u})
  {
    for (u32 seed = 1; seed <= 4; ++seed)
    {
      const Stream s = MakeStream(granules, seed);
      EXPECT_EQ(Batched(s), Serial(s)) << "granules " << granules << " seed " << seed;
    }
  }
}

TEST(BatchedChangeCRC, EmptyFeedIsTheEmptyStream)
{
  BatchedChangeCRC feed;
  EXPECT_EQ(feed.Finish(), Common::StartCRC32());
}

TEST(BatchedChangeCRC, AnyChangedByteChangesTheCRC)
{
  const Stream s = MakeStream(300, 7);
  const u32 base = Batched(s);
  for (const size_t at : {size_t{0}, size_t{63}, size_t{64} * 128, s.bytes.size() - 1})
  {
    Stream t = s;
    t.bytes[at] ^= 1;
    EXPECT_NE(Batched(t), base) << "byte " << at;
    EXPECT_EQ(Batched(t), Serial(t)) << "byte " << at;
  }
  Stream t = s;
  t.pages[200] += 1;
  EXPECT_NE(Batched(t), base);
  EXPECT_EQ(Batched(t), Serial(t));
}
