// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Jit64Common/BackPatchTable.h"

// After the emitter's headers: x64Emitter.h declares XEmitter::TEST.
#include <gtest/gtest.h>

namespace
{
// Addresses a few bytes apart, as a block's MOVs are.
const u8* Key(size_t i)
{
  return reinterpret_cast<const u8*>(uintptr_t{0x10000000} + i * 7);
}
}  // namespace

TEST(BackPatchTable, EmptyFindsNothing)
{
  BackPatchTable table;
  EXPECT_EQ(table.Size(), 0u);
  EXPECT_EQ(table.Find(Key(0)), nullptr);
}

TEST(BackPatchTable, AddedEntriesAreFound)
{
  BackPatchTable table;
  TrampolineInfo& a = table[Key(1)];
  a.pc = 0x80001000;
  TrampolineInfo& b = table[Key(2)];
  b.pc = 0x80002000;

  EXPECT_EQ(table.Size(), 2u);
  ASSERT_NE(table.Find(Key(1)), nullptr);
  EXPECT_EQ(table.Find(Key(1))->pc, 0x80001000u);
  EXPECT_EQ(table.Find(Key(2)), &b);
  EXPECT_EQ(table.Find(Key(3)), nullptr);
}

// Code freed and compiled into again puts a MOV at an address the table already has.
TEST(BackPatchTable, SameAddressAgainIsTheSameEntry)
{
  BackPatchTable table;
  TrampolineInfo& first = table[Key(5)];
  first.pc = 0x80001000;
  TrampolineInfo& again = table[Key(5)];
  EXPECT_EQ(&first, &again);
  EXPECT_EQ(again.pc, 0x80001000u);
  EXPECT_EQ(table.Size(), 1u);
}

// Past several doublings of the slots and many chunks: every entry is still found, where it was.
TEST(BackPatchTable, GrowthKeepsEntriesAndTheirAddresses)
{
  constexpr size_t COUNT = 300000;
  BackPatchTable table;
  std::vector<TrampolineInfo*> where;
  where.reserve(COUNT);
  for (size_t i = 0; i < COUNT; ++i)
  {
    TrampolineInfo& info = table[Key(i)];
    info.pc = static_cast<u32>(i);
    where.push_back(&info);
  }
  EXPECT_EQ(table.Size(), COUNT);
  for (size_t i = 0; i < COUNT; ++i)
  {
    TrampolineInfo* const found = table.Find(Key(i));
    ASSERT_EQ(found, where[i]) << i;
    ASSERT_EQ(found->pc, static_cast<u32>(i));
  }
  EXPECT_EQ(table.Find(Key(COUNT)), nullptr);
}

// Keys that are equal in their low 32 bits are different entries.
TEST(BackPatchTable, KeysThatShareTheirLowHalf)
{
  BackPatchTable table;
  const u8* const low = reinterpret_cast<const u8*>(uintptr_t{0x0000000012345678});
  const u8* const high = reinterpret_cast<const u8*>(uintptr_t{0x0000000112345678});
  table[low].pc = 1;
  table[high].pc = 2;
  EXPECT_EQ(table.Size(), 2u);
  EXPECT_EQ(table.Find(low)->pc, 1u);
  EXPECT_EQ(table.Find(high)->pc, 2u);
}

TEST(BackPatchTable, ClearEmptiesAndTheTableWorksAgain)
{
  BackPatchTable table;
  for (size_t i = 0; i < 10000; ++i)
    table[Key(i)].pc = static_cast<u32>(i);
  table.Clear();
  EXPECT_EQ(table.Size(), 0u);
  EXPECT_EQ(table.Find(Key(1)), nullptr);

  // A new entry starts out default, not as what was at that address before.
  TrampolineInfo& info = table[Key(1)];
  EXPECT_EQ(info.pc, 0u);
  EXPECT_EQ(table.Size(), 1u);
}
