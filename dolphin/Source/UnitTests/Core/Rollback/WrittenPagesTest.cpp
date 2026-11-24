// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Rollback::WrittenPages (Core/Rollback/WrittenPages.h): a load that copies back only the pages
// written since a capture must leave the buffer equal to that capture, whichever capture it is
// and whatever was written, captured and loaded before.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <random>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Rollback/WrittenPages.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VideoState.h"

namespace
{
constexpr size_t PAGES = 16;
constexpr size_t PAGE = 8;
using Buffer = std::array<u8, PAGES * PAGE>;

struct Machine
{
  Buffer buffer{};
  Rollback::WrittenPages<PAGES, PAGE> written;
  struct Capture
  {
    u64 number;
    Buffer copy;
  };
  std::vector<Capture> captures;

  void Write(size_t offset, size_t size, u8 value)
  {
    std::memset(buffer.data() + offset, value, size);
    written.Mark(offset, size);
  }
  void Take() { captures.push_back({written.Capture(), buffer}); }
  size_t Load(size_t n)
  {
    const Capture& c = captures[n];
    const size_t pages = written.RestoreTo(c.number, [&](size_t page) {
      std::memcpy(buffer.data() + page * PAGE, c.copy.data() + page * PAGE, PAGE);
    });
    EXPECT_EQ(buffer, c.copy);
    return pages;
  }
};
}  // namespace

TEST(WrittenPagesTest, NothingWrittenNothingCopied)
{
  Machine m;
  m.Write(0, PAGES * PAGE, 7);
  m.Take();
  EXPECT_EQ(m.Load(0), 0u);
  m.Take();
  EXPECT_EQ(m.Load(0), 0u);
  EXPECT_EQ(m.Load(1), 0u);
}

TEST(WrittenPagesTest, OnlyTheWrittenPages)
{
  Machine m;
  m.Take();
  m.Write(PAGE * 3 + 2, 1, 1);          // page 3
  m.Write(PAGE * 5 - 1, 2, 2);          // pages 4 and 5
  EXPECT_EQ(m.Load(0), 3u);
  // They went back to the capture's bytes, which another capture need not hold: copied again.
  EXPECT_EQ(m.Load(0), 3u);
}

TEST(WrittenPagesTest, AWriteOfTheSameBytesIsStillCopied)
{
  Machine m;
  m.Write(0, PAGE, 9);
  m.Take();
  m.Write(0, PAGE, 9);
  EXPECT_EQ(m.Load(0), 1u);
}

TEST(WrittenPagesTest, AnOlderCaptureAfterANewerOne)
{
  Machine m;
  m.Take();                  // 0: all zero
  m.Write(0, PAGE, 1);
  m.Take();                  // 1: page 0 is 1
  m.Write(PAGE, PAGE, 2);
  m.Load(1);                 // page 1 back to zero
  m.Load(0);                 // page 0 too
  m.Load(1);                 // and a capture taken before a load that went past it
}

TEST(WrittenPagesTest, EndOfTheBufferAndBeyond)
{
  Machine m;
  m.Take();
  m.written.Mark(PAGES * PAGE - 1, 100);  // runs past the end
  m.written.Mark(PAGES * PAGE + 5, 3);    // starts past it
  m.written.Mark(4, 0);
  EXPECT_EQ(m.Load(0), 1u);
}

TEST(WrittenPagesTest, AnUnknownCaptureCopiesEverything)
{
  Machine m;
  m.Take();
  Buffer other;
  other.fill(3);
  for (const u64 number : {u64{0}, u64{99}})
  {
    const size_t pages = m.written.RestoreTo(number, [&](size_t page) {
      std::memcpy(m.buffer.data() + page * PAGE, other.data() + page * PAGE, PAGE);
    });
    EXPECT_EQ(pages, PAGES);
    EXPECT_EQ(m.buffer, other);
  }
  m.Load(0);
}

TEST(WrittenPagesTest, RandomWritesCapturesAndLoads)
{
  std::mt19937 rng(20261114);
  Machine m;
  m.Take();
  for (int step = 0; step < 20000; ++step)
  {
    const u32 what = rng() % 10;
    if (what < 5)
    {
      const size_t offset = rng() % (PAGES * PAGE);
      const size_t size = 1 + rng() % std::min<size_t>(PAGES * PAGE - offset, 3 * PAGE);
      m.Write(offset, size, static_cast<u8>(rng()));
    }
    else if (what < 8)
    {
      m.Take();
      if (m.captures.size() > 24)
        m.captures.erase(m.captures.begin());
    }
    else
    {
      // Any capture still kept, older or newer than the last one loaded: a rollback's ring drops
      // the newer ones, and this must hold without that.
      m.Load(rng() % m.captures.size());
    }
  }
}

TEST(TmemSnapshotTest, IncrementalSavesAndLoadsMatchFullCopies)
{
  constexpr size_t PAGE = VideoCommon::TMEM_SNAPSHOT_PAGE_SIZE;
  constexpr size_t TOTAL = TMEM_SIZE;
  std::mt19937 rng(20261124);

  // Initialize TMEM and mark all written.
  for (size_t i = 0; i < TOTAL; i += 64)
    s_tex_mem[i] = static_cast<u8>(rng());
  VideoCommon_TmemWritten(0, TOTAL);

  constexpr size_t SLOTS = 4;
  std::array<VideoCommon::TmemSnapshot, SLOTS> snaps;
  std::array<std::vector<u8>, SLOTS> truth;
  for (size_t s = 0; s < SLOTS; ++s)
  {
    const size_t copied = VideoCommon_SaveTmemSnapshot(snaps[s]);
    EXPECT_EQ(copied, TOTAL);
    truth[s].assign(s_tex_mem.begin(), s_tex_mem.end());
  }

  // With no writes, re-saving and re-loading copy 0 bytes.
  EXPECT_EQ(VideoCommon_SaveTmemSnapshot(snaps[0]), 0u);
  EXPECT_EQ(VideoCommon_LoadTmemSnapshot(snaps[0]), 0u);

  // Random writes, saves, and loads across slots must leave s_tex_mem equal to truth[slot]
  // while only copying touched 4 KiB pages.
  for (int step = 0; step < 200; ++step)
  {
    const u32 op = rng() % 10;
    if (op < 5)
    {
      const unsigned int off = static_cast<unsigned int>(rng() % TOTAL);
      const unsigned int len =
          static_cast<unsigned int>(1 + rng() % std::min<size_t>(TOTAL - off, 3 * PAGE));
      const u8 val = static_cast<u8>(rng());
      std::memset(s_tex_mem.data() + off, val, len);
      VideoCommon_TmemWritten(off, len);
    }
    else if (op < 8)
    {
      const size_t s = rng() % SLOTS;
      const size_t copied = VideoCommon_SaveTmemSnapshot(snaps[s]);
      EXPECT_LE(copied, TOTAL);
      truth[s].assign(s_tex_mem.begin(), s_tex_mem.end());
    }
    else
    {
      const size_t s = rng() % SLOTS;
      VideoCommon_LoadTmemSnapshot(snaps[s]);
      EXPECT_EQ(std::memcmp(s_tex_mem.data(), truth[s].data(), TOTAL), 0) << "step " << step;
    }
  }
}
