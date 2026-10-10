// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <latch>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"

class IniFileTest : public testing::Test
{
protected:
  IniFileTest() : m_directory(File::CreateTempDir()), m_path(m_directory + "/Dolphin.ini") {}

  ~IniFileTest() override
  {
    if (!m_directory.empty())
      File::DeleteDirRecursively(m_directory);
  }

  void SetUp() override
  {
    if (m_directory.empty())
      FAIL();
  }

  // Enough keys that a save takes many writes, with each file's keys and values different, so an
  // interleaved save can't match either expected file.
  static Common::IniFile MakeIni(char tag)
  {
    Common::IniFile ini;
    for (int section = 0; section < 8; ++section)
    {
      auto* s = ini.GetOrCreateSection(fmt::format("Section{}", section));
      for (int key = 0; key < 64; ++key)
        s->Set(fmt::format("{}Key{}", tag, key), fmt::format("{}Value{}", tag, key * section));
    }
    return ini;
  }

  static std::string SaveAndRead(Common::IniFile& ini, const std::string& path)
  {
    std::string text;
    EXPECT_TRUE(ini.Save(path));
    EXPECT_TRUE(File::ReadFileToString(path, text));
    return text;
  }

  std::string m_directory;
  std::string m_path;
};

TEST_F(IniFileTest, ConcurrentSavesWriteOneWholeFile)
{
  Common::IniFile a = MakeIni('A');
  Common::IniFile b = MakeIni('B');
  const std::string expected_a = SaveAndRead(a, m_directory + "/a.ini");
  const std::string expected_b = SaveAndRead(b, m_directory + "/b.ini");
  ASSERT_NE(expected_a, expected_b);
  File::Delete(m_directory + "/a.ini");
  File::Delete(m_directory + "/b.ini");

  constexpr int ROUNDS = 20;
  constexpr int SAVES_PER_ROUND = 20;
  for (int round = 0; round < ROUNDS; ++round)
  {
    std::atomic<int> failed_saves = 0;
    std::latch start(2);
    const auto save_repeatedly = [&](Common::IniFile& ini) {
      start.arrive_and_wait();
      for (int i = 0; i < SAVES_PER_ROUND; ++i)
      {
        if (!ini.Save(m_path))
          ++failed_saves;
      }
    };
    std::thread thread_a(save_repeatedly, std::ref(a));
    std::thread thread_b(save_repeatedly, std::ref(b));
    thread_a.join();
    thread_b.join();

    EXPECT_EQ(failed_saves, 0) << "round " << round;

    std::string text;
    ASSERT_TRUE(File::ReadFileToString(m_path, text));
    ASSERT_TRUE(text == expected_a || text == expected_b) << "round " << round << ":\n" << text;
  }

  // No temp files left behind.
  const File::FSTEntry entries = File::ScanDirectoryTree(m_directory, false);
  ASSERT_EQ(entries.children.size(), 1u);
  EXPECT_EQ(entries.children[0].virtualName, "Dolphin.ini");
}
