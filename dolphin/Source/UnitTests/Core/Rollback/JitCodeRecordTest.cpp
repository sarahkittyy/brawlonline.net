// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PowerPC::JitCodeRecord (Core/PowerPC/PPCCache.h): the instructions a whole-machine session's JIT
// compiled each block from. A rollback's load undoes the record to the snapshot's mark and drops
// the blocks of every line whose record changed, so the record after an undo must be the record
// at the mark, and the lines it reports must cover every block that changed.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/PPCCache.h"

namespace
{
using PowerPC::JitCodeRecord;

// A block of `count` instructions from `address` (effective 0x80000000 + physical).
JitCodeRecord::Block MakeBlock(u32 physical, u32 count, u32 first_hex)
{
  JitCodeRecord::Block block;
  for (u32 i = 0; i < count; ++i)
    block.push_back({0x80000000 | (physical + i * 4), physical + i * 4, first_hex + i, true});
  return block;
}

u64 Entry(u32 physical)
{
  return JitCodeRecord::Entry(0x80000000 | physical, 0);
}

bool Has(const std::vector<u32>& lines, u32 line)
{
  return std::ranges::find(lines, line) != lines.end();
}
}  // namespace

TEST(JitCodeRecord, UndoForgetsWhatWasRecordedAfterTheMark)
{
  JitCodeRecord record;
  record.Record(Entry(0x1000), MakeBlock(0x1000, 4, 100));
  const u64 mark = record.Mark();
  record.Record(Entry(0x2000), MakeBlock(0x2000, 12, 200));  // two lines

  std::vector<u32> lines;
  record.Undo(mark, &lines);
  EXPECT_EQ(record.Find(Entry(0x2000)), nullptr);
  ASSERT_NE(record.Find(Entry(0x1000)), nullptr);
  EXPECT_TRUE(Has(lines, 0x2000));
  EXPECT_TRUE(Has(lines, 0x2020));
  EXPECT_FALSE(Has(lines, 0x1000));
  EXPECT_EQ(record.Mark(), mark + 1);  // the sequence goes on: marks never repeat
}

TEST(JitCodeRecord, UndoBringsBackWhatTheGameInvalidatedAfterTheMark)
{
  JitCodeRecord record;
  const JitCodeRecord::Block block = MakeBlock(0x3000, 8, 300);
  record.Record(Entry(0x3000), block);
  const u64 mark = record.Mark();

  record.Forget(0x3010, 4);  // an icbi of the block's line
  EXPECT_EQ(record.Find(Entry(0x3000)), nullptr);
  record.Record(Entry(0x3000), MakeBlock(0x3000, 8, 900));  // compiled again from new code

  std::vector<u32> lines;
  record.Undo(mark, &lines);
  const JitCodeRecord::Block* restored = record.Find(Entry(0x3000));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(*restored, block);
  EXPECT_TRUE(Has(lines, 0x3000));

  // The restored block is indexed again: the game can invalidate it once more.
  record.Forget(0x3000, 32);
  EXPECT_EQ(record.Find(Entry(0x3000)), nullptr);
}

TEST(JitCodeRecord, ForgetDropsOnlyTheBlocksThatReadTheRange)
{
  JitCodeRecord record;
  record.Record(Entry(0x4000), MakeBlock(0x4000, 4, 1));
  record.Record(Entry(0x4040), MakeBlock(0x4040, 4, 2));
  record.Record(Entry(0x4100), MakeBlock(0x40F8, 4, 3));  // reads the lines 0x40E0 and 0x4100

  record.Forget(0x4100, 4);
  EXPECT_NE(record.Find(Entry(0x4000)), nullptr);
  EXPECT_NE(record.Find(Entry(0x4040)), nullptr);
  EXPECT_EQ(record.Find(Entry(0x4100)), nullptr);

  // A range wider than the index: every block is looked at.
  record.Forget(0, 0xFFFFFFFF);
  EXPECT_EQ(record.Find(Entry(0x4000)), nullptr);
  EXPECT_EQ(record.Find(Entry(0x4040)), nullptr);
}

TEST(JitCodeRecord, ReplacedRecordComesBackOnUndo)
{
  JitCodeRecord record;
  const JitCodeRecord::Block first = MakeBlock(0x5000, 4, 10);
  record.Record(Entry(0x5000), first);
  const u64 mark = record.Mark();
  record.Record(Entry(0x5000), MakeBlock(0x5000, 6, 20));

  std::vector<u32> lines;
  record.Undo(mark, &lines);
  ASSERT_NE(record.Find(Entry(0x5000)), nullptr);
  EXPECT_EQ(*record.Find(Entry(0x5000)), first);
  EXPECT_TRUE(Has(lines, 0x5000));
}

TEST(JitCodeRecord, ForgetAllAndItsUndo)
{
  JitCodeRecord record;
  record.Record(Entry(0x6000), MakeBlock(0x6000, 4, 1));
  record.Record(Entry(0x7000), MakeBlock(0x7000, 4, 2));
  const u64 mark = record.Mark();

  record.ForgetAll();  // the instruction cache reset
  EXPECT_EQ(record.Find(Entry(0x6000)), nullptr);
  record.Record(Entry(0x6000), MakeBlock(0x6000, 4, 50));

  std::vector<u32> lines;
  record.Undo(mark, &lines);
  ASSERT_NE(record.Find(Entry(0x6000)), nullptr);
  EXPECT_EQ((*record.Find(Entry(0x6000)))[0].hex, 1u);
  EXPECT_NE(record.Find(Entry(0x7000)), nullptr);
  EXPECT_TRUE(Has(lines, 0x6000));
  EXPECT_TRUE(Has(lines, 0x7000));

  record.Forget(0x7000, 4);
  EXPECT_EQ(record.Find(Entry(0x7000)), nullptr);
}

TEST(JitCodeRecord, TrimKeepsWhatAnUndoToALaterMarkNeeds)
{
  JitCodeRecord record;
  record.Record(Entry(0x8000), MakeBlock(0x8000, 4, 1));
  const u64 oldest = record.Mark();
  record.Record(Entry(0x8100), MakeBlock(0x8100, 4, 2));
  const u64 later = record.Mark();
  record.Record(Entry(0x8200), MakeBlock(0x8200, 4, 3));

  record.Trim(oldest);
  std::vector<u32> lines;
  record.Undo(later, &lines);
  EXPECT_NE(record.Find(Entry(0x8000)), nullptr);
  EXPECT_NE(record.Find(Entry(0x8100)), nullptr);
  EXPECT_EQ(record.Find(Entry(0x8200)), nullptr);

  // Changes before a trimmed mark are not undone any more.
  record.Trim(later);
  lines.clear();
  record.Undo(0, &lines);
  EXPECT_NE(record.Find(Entry(0x8000)), nullptr);
  EXPECT_NE(record.Find(Entry(0x8100)), nullptr);
  EXPECT_TRUE(lines.empty());
}

TEST(JitCodeRecord, EntriesDifferByFeatureFlags)
{
  JitCodeRecord record;
  record.Record(JitCodeRecord::Entry(0x80009000, 0), MakeBlock(0x9000, 4, 1));
  EXPECT_EQ(record.Find(JitCodeRecord::Entry(0x80009000, 3)), nullptr);
  EXPECT_NE(record.Find(JitCodeRecord::Entry(0x80009000, 0)), nullptr);
}
