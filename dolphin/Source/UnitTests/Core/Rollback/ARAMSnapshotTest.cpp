// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A GameCube's ARAM in whole-machine rollback snapshots (DSPManager::SaveARAMSnapshot /
// LoadARAMSnapshot, bug report #135): outside the savestate, copied chunk by chunk.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <random>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/DSP.h"
#include "Core/System.h"

namespace
{
constexpr u32 CHUNK = 1u << DSP::ARAM_CHUNK_SHIFT;

class ARAMSnapshotTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_dsp.Reinit(true);  // the default System is a GameCube
    ASSERT_EQ(m_dsp.GetARAMSize(), u32{DSP::ARAM_SIZE});
  }
  void TearDown() override { m_dsp.Shutdown(); }

  std::vector<u8> Live() const
  {
    return std::vector<u8>(m_dsp.GetARAMPtr(), m_dsp.GetARAMPtr() + DSP::ARAM_SIZE);
  }

  DSP::DSPManager& m_dsp = Core::System::GetInstance().GetDSP();
};
}  // namespace

TEST_F(ARAMSnapshotTest, SavesAndLoadsCopyOnlyWhatChanged)
{
  DSP::ARAMSnapshot a, b;
  EXPECT_EQ(m_dsp.SaveARAMSnapshot(a), size_t{DSP::ARAM_SIZE});  // a new copy: everything
  EXPECT_EQ(m_dsp.SaveARAMSnapshot(a), 0u);                      // nothing written since

  m_dsp.WriteARAM(0x5A, 0x12345);
  EXPECT_EQ(m_dsp.SaveARAMSnapshot(b), size_t{DSP::ARAM_SIZE});
  EXPECT_EQ(m_dsp.SaveARAMSnapshot(a), size_t{CHUNK});  // only the chunk written
  EXPECT_EQ(m_dsp.LoadARAMSnapshot(b), 0u);              // the same as now

  m_dsp.WriteARAM(0x01, 0x12346);
  m_dsp.WriteARAM(0x02, 0x800000);
  EXPECT_EQ(m_dsp.LoadARAMSnapshot(a), size_t{2 * CHUNK});
  EXPECT_EQ(m_dsp.ReadARAM(0x12345), 0x5A);
  EXPECT_EQ(m_dsp.ReadARAM(0x12346), 0x00);
  EXPECT_EQ(m_dsp.ReadARAM(0x800000), 0x00);
  EXPECT_EQ(m_dsp.SaveARAMSnapshot(b), 0u);  // a and b, and now ARAM, hold the same
}

TEST_F(ARAMSnapshotTest, WritesOutsideWriteARAMMarkTheirChunks)
{
  DSP::ARAMSnapshot a;
  m_dsp.SaveARAMSnapshot(a);
  u8* const aram = m_dsp.GetARAMPtr();

  // Across a chunk's end, and wrapping at ARAM's end.
  std::memset(aram + 3 * CHUNK - 2, 0x77, 4);
  m_dsp.MarkARAMWritten(3 * CHUNK - 2, 4);
  aram[DSP::ARAM_SIZE - 1] = 0x66;
  aram[0] = 0x66;
  m_dsp.MarkARAMWritten(DSP::ARAM_SIZE - 1, 2);
  const std::vector<u8> written = Live();

  DSP::ARAMSnapshot b;
  m_dsp.SaveARAMSnapshot(b);
  EXPECT_EQ(m_dsp.LoadARAMSnapshot(a), size_t{4 * CHUNK});
  EXPECT_EQ(Live(), std::vector<u8>(DSP::ARAM_SIZE, 0));
  EXPECT_EQ(m_dsp.LoadARAMSnapshot(b), size_t{4 * CHUNK});
  EXPECT_EQ(Live(), written);
}

// A rollback's pattern: a ring of snapshots saved frame after frame, loads of older ones and
// re-runs that write again, against a full copy taken at each save.
TEST_F(ARAMSnapshotTest, RandomRollbacksMatchFullCopies)
{
  constexpr int SLOTS = 8;
  std::array<DSP::ARAMSnapshot, SLOTS> slots;
  std::array<std::vector<u8>, SLOTS> full;
  std::array<bool, SLOTS> saved{};
  std::mt19937 rng(135);
  u8* const aram = m_dsp.GetARAMPtr();
  int next = 0;

  for (int frame = 0; frame < 160; ++frame)
  {
    // This frame's writes: bytes through WriteARAM and blocks written directly (as the debugger
    // and a DMA do), some in a few hot areas, some anywhere.
    const int writes = static_cast<int>(rng() % 40);
    for (int i = 0; i < writes; ++i)
    {
      const u32 base = (rng() % 4 == 0) ? rng() : (rng() % 4) * 0x100000 + rng() % 0x8000;
      if (rng() % 2)
      {
        m_dsp.WriteARAM(static_cast<u8>(rng()), base);
      }
      else
      {
        const u32 at = base & DSP::ARAM_MASK;
        const u32 size = std::min<u32>(1 + rng() % (3 * CHUNK), DSP::ARAM_SIZE - at);
        std::memset(aram + at, static_cast<int>(rng() & 0xFF), size);
        m_dsp.MarkARAMWritten(at, size);
      }
    }

    m_dsp.SaveARAMSnapshot(slots[next]);
    full[next] = Live();
    saved[next] = true;

    if (rng() % 3 == 0)
    {
      // Roll back 1-7 frames.
      const int back = 1 + static_cast<int>(rng() % (SLOTS - 1));
      const int slot = (next - back + SLOTS) % SLOTS;
      if (saved[slot])
      {
        m_dsp.LoadARAMSnapshot(slots[slot]);
        ASSERT_EQ(Live(), full[slot]) << "frame " << frame << ", " << back << " back";
        next = slot;  // the re-run saves over the newer slots
      }
    }
    next = (next + 1) % SLOTS;
  }
}

// A new ARAM (the next boot) is not taken for the bytes an older snapshot kept.
TEST_F(ARAMSnapshotTest, ANewARAMIsCopiedAgain)
{
  DSP::ARAMSnapshot a;
  m_dsp.WriteARAM(0x42, 0x100);
  m_dsp.SaveARAMSnapshot(a);
  m_dsp.Shutdown();
  m_dsp.Reinit(true);
  EXPECT_EQ(m_dsp.SaveARAMSnapshot(a), size_t{DSP::ARAM_SIZE});
  EXPECT_EQ(m_dsp.ReadARAM(0x100), 0x00);
}
