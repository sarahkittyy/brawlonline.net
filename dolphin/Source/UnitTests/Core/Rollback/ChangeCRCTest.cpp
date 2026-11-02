// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// BatchedChangeCRC (Core/Rollback/ChangeCRC.h, GitHub issue #14): the change CRC's batched feed.
// Peers compare the CRC, so batching must give the one the serial feed gave (two crc32 calls per
// granule, its page number and then its 64 bytes) whatever the granule count or flush boundaries.
// RunCRC: the slice and full checksums' feed, runs of adjacent granules in one crc32 call each;
// it must give the CRC of one call per counted granule, whatever breaks the runs.
// FeedCountedGranules: the slice and full checksums' walk; it must count the granules, bytes and
// GPU-written skips that the per-granule loop it replaced counted, whatever the regions.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <random>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Core/Rollback/ChangeCRC.h"

namespace
{
using Rollback::BatchedChangeCRC;
using Rollback::CountedRAM;
using Rollback::MemoryRegion;
using Rollback::RunCRC;
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

namespace
{
// Two "RAM" areas (as MEM1 and MEM2) walked granule by granule as FeedCountedGranules walks them:
// each granule left out (GPU-written or excluded), masked (a copy in a reused scratch buffer with
// some bytes zeroed) or counted as it is.
enum class Fate : u8
{
  Counted,
  Skipped,
  Masked,
};

struct Walk
{
  std::vector<u8> mem1, mem2;
  std::vector<Fate> fates;  // mem1's granules, then mem2's
};

Walk MakeWalk(u32 mem1_granules, u32 mem2_granules, u32 skip_in, u32 mask_in, u32 seed)
{
  std::mt19937 rng(seed);
  Walk w;
  w.mem1.resize(mem1_granules * GRANULE);
  w.mem2.resize(mem2_granules * GRANULE);
  for (u8& b : w.mem1)
    b = static_cast<u8>(rng());
  for (u8& b : w.mem2)
    b = static_cast<u8>(rng());
  for (u32 g = 0; g < mem1_granules + mem2_granules; ++g)
  {
    Fate f = Fate::Counted;
    if (skip_in && rng() % skip_in == 0)
      f = Fate::Skipped;
    else if (mask_in && rng() % mask_in == 0)
      f = Fate::Masked;
    w.fates.push_back(f);
  }
  return w;
}

const u8* GranuleOf(const Walk& w, u32 g)
{
  const u32 mem1_granules = static_cast<u32>(w.mem1.size() / GRANULE);
  return g < mem1_granules ? w.mem1.data() + g * GRANULE :
                             w.mem2.data() + (g - mem1_granules) * GRANULE;
}

void Mask(const u8* src, u8* scratch, u32 g)
{
  std::memcpy(scratch, src, GRANULE);
  std::memset(scratch + g % 48, 0, 4 + g % 13);
}

// One crc32 call per counted granule, as SaveChecksumWithSlice and FullChecksum did before.
u32 PerGranule(const Walk& w, u32 first, u32 last, u32 seed_crc)
{
  u32 crc = seed_crc;
  u8 scratch[GRANULE];
  for (u32 g = first; g < last; ++g)
  {
    const u8* src = GranuleOf(w, g);
    if (w.fates[g] == Fate::Skipped)
      continue;
    if (w.fates[g] == Fate::Masked)
    {
      Mask(src, scratch, g);
      src = scratch;
    }
    crc = Common::UpdateCRC32(crc, src, GRANULE);
  }
  return crc;
}

u32 Runs(const Walk& w, u32 first, u32 last, u32 seed_crc)
{
  RunCRC crc(seed_crc);
  u8 scratch[GRANULE];
  for (u32 g = first; g < last; ++g)
  {
    const u8* src = GranuleOf(w, g);
    if (w.fates[g] == Fate::Skipped)
      continue;
    if (w.fates[g] == Fate::Masked)
    {
      Mask(src, scratch, g);
      crc.FeedCopy(scratch, GRANULE);
    }
    else
    {
      crc.Feed(src, GRANULE);
    }
  }
  return crc.Finish();
}
}  // namespace

TEST(RunCRC, MatchesOneCallPerGranule)
{
  constexpr u32 MEM1 = 3000, MEM2 = 5000;
  // Every granule counted (one run per RAM area), sparse and dense breaks, masked granules alone
  // and back to back (the scratch buffer is reused), and everything left out.
  const std::array<std::array<u32, 2>, 6> mixes{{{0, 0}, {500, 0}, {7, 0}, {0, 3}, {5, 2}, {1, 0}}};
  for (const auto& [skip_in, mask_in] : mixes)
  {
    for (u32 seed = 1; seed <= 3; ++seed)
    {
      const Walk w = MakeWalk(MEM1, MEM2, skip_in, mask_in, seed);
      // Slices inside MEM1, across the MEM1/MEM2 boundary, inside MEM2, and all of it, from the
      // empty stream's CRC and from another (the slice continues the change CRC).
      const std::array<std::array<u32, 2>, 5> ranges{{{0, 0},
                                                      {100, 1100},
                                                      {MEM1 - 400, MEM1 + 400},
                                                      {MEM1 + 17, MEM1 + 2017},
                                                      {0, MEM1 + MEM2}}};
      for (const auto& [first, last] : ranges)
      {
        for (const u32 seed_crc : {Common::StartCRC32(), 0x1234abcdu})
        {
          EXPECT_EQ(Runs(w, first, last, seed_crc), PerGranule(w, first, last, seed_crc))
              << "skip 1/" << skip_in << " mask 1/" << mask_in << " seed " << seed << " ["
              << first << ", " << last << ")";
        }
      }
    }
  }
}

TEST(RunCRC, AnyCountedByteChangesTheCRC)
{
  Walk w = MakeWalk(256, 256, 9, 11, 5);
  const u32 base = Runs(w, 0, 512, Common::StartCRC32());
  for (u32 g = 0; g < 512; g += 37)
  {
    if (w.fates[g] != Fate::Counted)
      continue;
    u8* at = const_cast<u8*>(GranuleOf(w, g)) + g % GRANULE;
    *at ^= 0x80;
    EXPECT_NE(Runs(w, 0, 512, Common::StartCRC32()), base) << "granule " << g;
    EXPECT_EQ(Runs(w, 0, 512, Common::StartCRC32()), PerGranule(w, 0, 512, Common::StartCRC32()));
    *at ^= 0x80;
  }
}

namespace
{
// Small guest RAM: MEM1 and MEM2 areas, a GPU-written bitmap by page, exclusions and masks.
struct Guest
{
  std::vector<u8> mem1, mem2, gpu;
  std::vector<MemoryRegion> excluded, masks;

  CountedRAM Ram() const
  {
    return {mem1.data(), static_cast<u32>(mem1.size() / GRANULE),
            mem2.data(), static_cast<u32>(mem2.size() / GRANULE),
            gpu.data(),  excluded,
            masks};
  }
};

// A region somewhere in MEM1 or MEM2 (unaligned, from 1 byte to `max_len`), sometimes running past
// its area's end.
MemoryRegion RandomRegion(std::mt19937& rng, const Guest& g, u32 max_len)
{
  const bool mem2 = rng() % 2;
  const u32 base = mem2 ? MEM2_BASE : 0;
  const u32 size = static_cast<u32>(mem2 ? g.mem2.size() : g.mem1.size());
  const u32 len = 1 + rng() % max_len;
  const u32 start = rng() % 8 == 0 ? size - std::min(size, len / 2) : rng() % size;
  return {base + start, base + start + len, nullptr};
}

Guest MakeGuest(u32 seed, u32 gpu_in, u32 exclusions, u32 masks)
{
  std::mt19937 rng(seed);
  Guest g;
  g.mem1.resize(4096 * GRANULE);
  g.mem2.resize(6144 * GRANULE);
  for (u8& b : g.mem1)
    b = static_cast<u8>(rng());
  for (u8& b : g.mem2)
    b = static_cast<u8>(rng());
  g.gpu.assign(MEM2_FIRST_PAGE + g.mem2.size() / GRANULE, 0);
  for (u32 i = 0; gpu_in && i < 12; ++i)
  {
    // Stretches of GPU-written granules (a copy's lines) and single ones.
    const u32 page = rng() % 2 ? rng() % 4096 : MEM2_FIRST_PAGE + rng() % 6144;
    const u32 len = rng() % 2 ? 1 : 1 + rng() % gpu_in;
    for (u32 p = page; p < page + len && p < g.gpu.size(); ++p)
      g.gpu[p] = 1;
  }
  for (u32 i = 0; i < exclusions; ++i)
    g.excluded.push_back(RandomRegion(rng, g, i % 2 ? 40 : 30000));
  for (u32 i = 0; i < masks; ++i)
    g.masks.push_back(RandomRegion(rng, g, i % 3 ? 20 : 200));
  return g;
}

struct Counted
{
  u32 crc, also;
  std::vector<u32> gpu_skipped;
};

// The loop RollbackManager::SaveChecksumWithSlice had before FeedCountedGranules: every region
// looked at, and one crc32 call, per granule.
Counted PerGranuleWalk(const Guest& guest, u32 first, u32 last, u32 seed_crc)
{
  const u32 mem1_pages = static_cast<u32>(guest.mem1.size() / GRANULE);
  Counted out{seed_crc, Common::StartCRC32(), {}};
  u8 scratch[GRANULE];
  for (u32 g = first; g < last; ++g)
  {
    const bool mem2 = g >= mem1_pages;
    const u32 page = mem2 ? MEM2_FIRST_PAGE + (g - mem1_pages) : g;
    if (guest.gpu[page])
    {
      out.gpu_skipped.push_back(page << ROLLBACK_PAGE_SHIFT);
      continue;
    }
    const u32 phys = page << ROLLBACK_PAGE_SHIFT;
    bool excluded = false;
    for (const MemoryRegion& r : guest.excluded)
      excluded |= phys < r.phys_end && phys + ROLLBACK_PAGE_SIZE > r.phys_start;
    if (excluded)
      continue;
    const u8* src = Rollback::MaskedForChecksum(
        phys,
        mem2 ? guest.mem2.data() + static_cast<size_t>(g - mem1_pages) * GRANULE :
               guest.mem1.data() + static_cast<size_t>(g) * GRANULE,
        guest.masks, scratch);
    out.crc = Common::UpdateCRC32(out.crc, src, GRANULE);
    out.also = Common::UpdateCRC32(out.also, src, GRANULE);
  }
  return out;
}

Counted RunWalk(const Guest& guest, u32 first, u32 last, u32 seed_crc)
{
  RunCRC crc(seed_crc), also;
  Counted out{};
  Rollback::FeedCountedGranules(guest.Ram(), first, last, crc, &also, &out.gpu_skipped);
  out.crc = crc.Finish();
  out.also = also.Finish();
  return out;
}
}  // namespace

TEST(FeedCountedGranules, MatchesThePerGranuleLoop)
{
  constexpr u32 MEM1 = 4096, MEM2 = 6144;
  // (GPU-written stretch length, exclusions, masks): none, each alone, all together, many.
  const std::array<std::array<u32, 3>, 6> mixes{
      {{0, 0, 0}, {300, 0, 0}, {0, 3, 0}, {0, 0, 5}, {50, 3, 6}, {20, 12, 30}}};
  for (const auto& [gpu_in, exclusions, masks] : mixes)
  {
    for (u32 seed = 1; seed <= 6; ++seed)
    {
      const Guest guest = MakeGuest(seed * 101 + gpu_in, gpu_in, exclusions, masks);
      std::mt19937 rng(seed);
      std::vector<std::array<u32, 2>> ranges{
          {0, MEM1 + MEM2}, {0, 0}, {MEM1 - 300, MEM1 + 300}, {MEM1, MEM1 + MEM2}};
      for (int i = 0; i < 8; ++i)
      {
        const u32 a = rng() % (MEM1 + MEM2), b = rng() % (MEM1 + MEM2);
        ranges.push_back({std::min(a, b), std::max(a, b)});
      }
      for (const auto& [first, last] : ranges)
      {
        const Counted want = PerGranuleWalk(guest, first, last, 0x5eed5eedu);
        const Counted got = RunWalk(guest, first, last, 0x5eed5eedu);
        EXPECT_EQ(got.crc, want.crc) << "mix " << gpu_in << "/" << exclusions << "/" << masks
                                     << " seed " << seed << " [" << first << ", " << last << ")";
        EXPECT_EQ(got.also, want.also);
        EXPECT_EQ(got.gpu_skipped, want.gpu_skipped);
      }
    }
  }
}

TEST(FeedCountedGranules, LeavesOutWhatThePerGranuleLoopLeftOut)
{
  // Granule-exact cases: a region ending exactly on a granule boundary does not touch the next
  // granule, a 1-byte one touches its granule, an empty one at an unaligned address touches its
  // granule (as the old loop had it), and masked granules back to back each count their own copy.
  Guest guest = MakeGuest(9, 0, 0, 0);
  guest.excluded = {{64 * 10, 64 * 12, nullptr},
                    {64 * 20 + 63, 64 * 20 + 64, nullptr},
                    {64 * 30 + 5, 64 * 30 + 5, nullptr},
                    {MEM2_BASE + 64 * 100, MEM2_BASE + 64 * 100 + 1, nullptr}};
  guest.masks = {{64 * 40 + 60, 64 * 43 + 2, nullptr}, {64 * 41 + 8, 64 * 41 + 9, nullptr}};
  guest.gpu[11] = 1;  // inside an exclusion: still reported as GPU-written
  guest.gpu[42] = 1;  // masked and GPU-written
  for (const auto& [first, last] : std::array<std::array<u32, 2>, 4>{
           {{0, 4096 + 6144}, {11, 12}, {41, 4096 + 101}, {12, 44}}})
  {
    const Counted want = PerGranuleWalk(guest, first, last, Common::StartCRC32());
    const Counted got = RunWalk(guest, first, last, Common::StartCRC32());
    EXPECT_EQ(got.crc, want.crc) << "[" << first << ", " << last << ")";
    EXPECT_EQ(got.also, want.also);
    EXPECT_EQ(got.gpu_skipped, want.gpu_skipped);
  }
}
