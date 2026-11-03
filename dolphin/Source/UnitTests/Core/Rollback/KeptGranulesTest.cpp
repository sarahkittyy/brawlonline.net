// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// CompactKeptGranules (Core/Rollback/KeptGranules.h): a whole-machine save's copy jobs keep only
// the granules that changed, each from the start of its own range; the slot must end up with
// exactly those, in scan order, with their bytes.

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Rollback/KeptGranules.h"

namespace
{
constexpr size_t GRANULE = 64;
constexpr u32 CHUNKS = 5;  // SAVESTATE_NUM_WORK_CHUNKS

// The granule written n-th in the scan: its index and bytes are a function of n.
u32 IndexOf(u32 n)
{
  return n * 3 + 7;
}
u8 ByteOf(u32 n, size_t b)
{
  return static_cast<u8>(n * 31 + b);
}

// Runs the copy jobs' side by hand: `written` granules, the ones with keep[n] set kept, chunk by
// chunk as the jobs split them; then compacts and checks against the kept ones in order.
void Check(u32 written, const std::vector<bool>& keep)
{
  const u32 split = (written + CHUNKS - 1) / CHUNKS;
  std::vector<u32> indices(written, 0xDEADBEEF);
  std::vector<u8> data(static_cast<size_t>(written) * GRANULE, 0xCC);
  std::array<u32, CHUNKS> kept{};
  for (u32 c = 0; c < CHUNKS; ++c)
  {
    const u32 offset = c * split;
    if (offset >= written)
      continue;  // as the job: nothing in its range
    u32 next = offset;
    for (u32 n = offset; n < std::min(written, offset + split); ++n)
    {
      if (!keep[n])
        continue;
      indices[next] = IndexOf(n);
      for (size_t b = 0; b < GRANULE; ++b)
        data[static_cast<size_t>(next) * GRANULE + b] = ByteOf(n, b);
      ++next;
    }
    kept[c] = next - offset;
  }
  const u32 total =
      Rollback::CompactKeptGranules(indices.data(), data.data(), GRANULE, split, kept);
  u32 at = 0;
  for (u32 n = 0; n < written; ++n)
  {
    if (!keep[n])
      continue;
    ASSERT_LT(at, total);
    EXPECT_EQ(indices[at], IndexOf(n)) << "granule " << at << " of " << total;
    for (size_t b = 0; b < GRANULE; ++b)
      ASSERT_EQ(data[static_cast<size_t>(at) * GRANULE + b], ByteOf(n, b)) << "granule " << at;
    ++at;
  }
  EXPECT_EQ(total, at);
}
}  // namespace

TEST(KeptGranulesTest, FewerGranulesThanJobs)
{
  // Ranges past the end (offset >= written) keep nothing.
  for (u32 written = 1; written <= 2 * CHUNKS; ++written)
  {
    Check(written, std::vector<bool>(written, true));
    std::vector<bool> odd(written);
    for (u32 n = 0; n < written; ++n)
      odd[n] = n % 2 == 1;
    Check(written, odd);
  }
}

TEST(KeptGranulesTest, OnlyOneChunkKeepsAnything)
{
  for (u32 only = 0; only < CHUNKS; ++only)
  {
    const u32 written = 997;
    const u32 split = (written + CHUNKS - 1) / CHUNKS;
    std::vector<bool> keep(written, false);
    for (u32 n = only * split; n < std::min(written, (only + 1) * split); n += 3)
      keep[n] = true;
    Check(written, keep);
  }
}

TEST(KeptGranulesTest, RandomShares)
{
  // A whole-machine frame writes ~11,000 granules and ~40% changed; also nothing and everything
  // (a region-mode save keeps every granule: nothing moves).
  std::mt19937 rng(14);
  for (const double share : {0.0, 0.01, 0.4, 0.99, 1.0})
  {
    for (const u32 written : {7u, 64u, 11000u, 30001u})
    {
      std::vector<bool> keep(written);
      std::bernoulli_distribution d(share);
      for (u32 n = 0; n < written; ++n)
        keep[n] = d(rng);
      Check(written, keep);
    }
  }
}
