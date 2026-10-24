// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A whole-machine session's GameCube memory card (GCMemcardDirectory with `session`,
// docs/design/brawl-mods.md §6.5): its state is the same bytes on every player's machine, whatever
// folder each card is in (the image digest the players vouch with, bug report #128), and the
// player who booted the game still writes the saves back to the files they came from.

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Swap.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardDirectory.h"

namespace
{
constexpr std::string_view GAME = "GM4E";
constexpr u16 SAVE_BLOCKS = 2;
constexpr s32 WRITE_SIZE = 0x80;
// LoadGCI gives the first save the card's first free block, after the directory and the BAT.
constexpr u32 FIRST_SAVE_ADDRESS = Memcard::MC_FST_BLOCKS * Memcard::BLOCK_SIZE;

u32 GameId()
{
  return Common::swap32(reinterpret_cast<const u8*>(GAME.data()));
}

Memcard::HeaderData Header()
{
  Memcard::HeaderData h;
  Memcard::InitializeHeaderData(&h, {}, Memcard::MBIT_SIZE_MEMORY_CARD_251, false, 0, 0, 0);
  return h;
}

// A save of GAME in `dir`, its blocks filled with `fill`.
std::string WriteSave(const std::string& dir, u8 fill)
{
  Memcard::DEntry e;
  std::memcpy(e.m_gamecode.data(), GAME.data(), 4);
  std::memcpy(e.m_makercode.data(), "01", 2);
  e.m_filename.fill(0);
  std::memcpy(e.m_filename.data(), "Test save", 9);
  e.m_block_count = SAVE_BLOCKS;
  e.m_first_block = 0;
  const std::string path = dir + "01-GM4E-Test save.gci";
  File::IOFile f(path, "wb");
  f.WriteBytes(&e, Memcard::DENTRY_SIZE);
  const std::vector<u8> blocks(SAVE_BLOCKS * Memcard::BLOCK_SIZE, fill);
  f.WriteBytes(blocks.data(), blocks.size());
  return path;
}

std::vector<u8> SaveState(GCMemcardDirectory& card)
{
  u8* ptr = nullptr;
  PointerWrap measure(&ptr, 0, PointerWrap::Mode::Measure);
  card.DoState(measure);
  std::vector<u8> bytes(reinterpret_cast<size_t>(ptr));
  ptr = bytes.data();
  PointerWrap write(&ptr, bytes.size(), PointerWrap::Mode::Write);
  card.DoState(write);
  return bytes;
}

void LoadState(GCMemcardDirectory& card, std::vector<u8> bytes)
{
  u8* ptr = bytes.data();
  PointerWrap read(&ptr, bytes.size(), PointerWrap::Mode::Read);
  card.DoState(read);
  ASSERT_TRUE(read.IsReadMode());
}

bool Contains(const std::vector<u8>& bytes, std::string_view text)
{
  return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

class GCMemcardDirectoryTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_dir = File::CreateTempDir();
    ASSERT_FALSE(m_dir.empty());
    // Two players' folders, of different lengths (the path's length moved everything after it).
    m_host = m_dir + "/Users/host/Saves/0123456789abcdef/GC/";
    m_joiner = m_dir + "/Users/another player/Saves/0123456789abcdef/GC/";
    ASSERT_TRUE(File::CreateFullPath(m_host));
    ASSERT_TRUE(File::CreateFullPath(m_joiner));
  }

  void TearDown() override { File::DeleteDirRecursively(m_dir); }

  std::string m_dir;
  std::string m_host;
  std::string m_joiner;
};
}  // namespace

// Bug report #128: the third player of a Shrek 2 room was refused ("player 2 does not vouch for
// this machine") because each machine's state held its own folder and the host's the paths of its
// saves.
TEST_F(GCMemcardDirectoryTest, SessionStateIsTheSameOnEveryMachine)
{
  WriteSave(m_host, 0x11);
  WriteSave(m_joiner, 0x22);  // the joiner's own copy of the save, another one
  GCMemcardDirectory host(m_host, ExpansionInterface::Slot::A, Header(), GameId(), true, true);
  const std::vector<u8> image = SaveState(host);
  EXPECT_FALSE(Contains(image, "Users"));
  EXPECT_FALSE(Contains(image, ".gci"));

  GCMemcardDirectory joiner(m_joiner, ExpansionInterface::Slot::A, Header(), GameId(), true,
                            false);
  LoadState(joiner, image);
  EXPECT_EQ(SaveState(joiner), image);

  // Both play on (a write to the save) and save again, as at the next epoch.
  const std::array<u8, WRITE_SIZE> data{0x33};
  host.Write(FIRST_SAVE_ADDRESS, WRITE_SIZE, data.data());
  joiner.Write(FIRST_SAVE_ADDRESS, WRITE_SIZE, data.data());
  EXPECT_EQ(SaveState(joiner), SaveState(host));

  // The joiner plays the host's save, not its own copy.
  std::array<u8, WRITE_SIZE> read{};
  joiner.Read(FIRST_SAVE_ADDRESS + 0x100, WRITE_SIZE, read.data());
  EXPECT_EQ(read[0], 0x11);
}

// The host still writes the session's saves back to the files they were loaded from, after its
// own rollbacks (states it loaded) too; a joiner writes nothing.
TEST_F(GCMemcardDirectoryTest, HostWritesBackToItsOwnFiles)
{
  const std::string host_file = WriteSave(m_host, 0x11);
  const std::string joiner_file = WriteSave(m_joiner, 0x22);
  {
    GCMemcardDirectory host(m_host, ExpansionInterface::Slot::A, Header(), GameId(), true, true);
    GCMemcardDirectory joiner(m_joiner, ExpansionInterface::Slot::A, Header(), GameId(), true,
                              false);
    LoadState(joiner, SaveState(host));
    const std::array<u8, WRITE_SIZE> data{0x33};
    host.Write(FIRST_SAVE_ADDRESS, WRITE_SIZE, data.data());
    joiner.Write(FIRST_SAVE_ADDRESS, WRITE_SIZE, data.data());
    LoadState(host, SaveState(host));  // a rollback
  }

  EXPECT_EQ(Common::DoFileSearch(m_host, ".gci").size(), 1u);
  std::string saved;
  ASSERT_TRUE(File::ReadFileToString(host_file, saved));
  ASSERT_EQ(saved.size(), Memcard::DENTRY_SIZE + SAVE_BLOCKS * Memcard::BLOCK_SIZE);
  EXPECT_EQ(static_cast<u8>(saved[Memcard::DENTRY_SIZE]), 0x33);
  EXPECT_EQ(static_cast<u8>(saved[Memcard::DENTRY_SIZE + 0x80]), 0x11);

  EXPECT_EQ(Common::DoFileSearch(m_joiner, ".gci").size(), 1u);
  ASSERT_TRUE(File::ReadFileToString(joiner_file, saved));
  EXPECT_EQ(static_cast<u8>(saved[Memcard::DENTRY_SIZE]), 0x22);
}
