// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/LinearDiskCache.h"

namespace
{
using Cache = Common::LinearDiskCache<u32, u8>;
using Entries = std::vector<std::pair<u32, std::vector<u8>>>;

// Keeps the entries of another build whose value starts with keep_byte.
class Reader : public Common::LinearDiskCacheReader<u32, u8>
{
public:
  explicit Reader(u8 keep_byte_ = 0) : keep_byte(keep_byte_) {}

  void Read(const u32& key, const u8* value, u32 value_size) override
  {
    read.emplace_back(key, std::vector<u8>(value, value + value_size));
  }
  bool ReadFromOtherBuild(const u32& key, const u8* value, u32 value_size) override
  {
    other_build.emplace_back(key, std::vector<u8>(value, value + value_size));
    if (value_size == 0 || value[0] != keep_byte)
      return false;
    Read(key, value, value_size);
    return true;
  }

  u8 keep_byte;
  Entries read;
  Entries other_build;
};

// Reads like the Vulkan backend's pipeline cache: nothing from other builds.
class PlainReader : public Common::LinearDiskCacheReader<u32, u8>
{
public:
  void Read(const u32& key, const u8* value, u32 value_size) override
  {
    read.emplace_back(key, std::vector<u8>(value, value + value_size));
  }
  Entries read;
};

// The header: u32 id, u16 key size, u16 value size, then the writing build's revision.
constexpr u64 REVISION_OFFSET = 8;
constexpr size_t REVISION_SIZE = 40;

class LinearDiskCacheTest : public testing::Test
{
protected:
  LinearDiskCacheTest()
      : m_dir(File::CreateTempDir()), m_path(m_dir + "/test.cache")
  {
  }
  ~LinearDiskCacheTest() override
  {
    if (!m_dir.empty())
      File::DeleteDirRecursively(m_dir);
  }
  void SetUp() override { ASSERT_FALSE(m_dir.empty()); }

  void Write(const Entries& entries)
  {
    Cache cache;
    PlainReader reader;
    ASSERT_EQ(cache.OpenAndRead(m_path, reader), 0u);
    for (const auto& [key, value] : entries)
      cache.Append(key, value.data(), static_cast<u32>(value.size()));
    cache.Close();
  }

  std::string ReadRevision()
  {
    File::IOFile file(m_path, "rb");
    std::string revision(REVISION_SIZE, '\0');
    EXPECT_TRUE(file.Seek(REVISION_OFFSET, File::SeekOrigin::Begin));
    EXPECT_TRUE(file.ReadBytes(revision.data(), revision.size()));
    return revision;
  }

  // What another build's file looks like: the same, with another revision in the header.
  void SetRevision(const std::string& revision)
  {
    File::IOFile file(m_path, "r+b");
    ASSERT_TRUE(file.Seek(REVISION_OFFSET, File::SeekOrigin::Begin));
    ASSERT_TRUE(file.WriteBytes(revision.data(), revision.size()));
  }

  std::string m_dir;
  std::string m_path;
};

const Entries ENTRIES = {{1, {7, 1, 1}}, {2, {9, 2}}, {3, {7, 3, 3, 3}}, {4, {}}};
}  // namespace

TEST_F(LinearDiskCacheTest, SameBuildReadsEveryEntry)
{
  Write(ENTRIES);
  Cache cache;
  Reader reader(7);
  EXPECT_EQ(cache.OpenAndRead(m_path, reader), 4u);
  EXPECT_EQ(reader.read, ENTRIES);
  EXPECT_TRUE(reader.other_build.empty());
}

TEST_F(LinearDiskCacheTest, OtherBuildKeepsWhatTheReaderKeeps)
{
  Write(ENTRIES);
  const std::string this_build = ReadRevision();
  SetRevision(std::string(REVISION_SIZE, 'x'));

  Cache cache;
  Reader reader(7);
  EXPECT_EQ(cache.OpenAndRead(m_path, reader), 2u);
  EXPECT_EQ(reader.other_build, ENTRIES);
  const Entries kept = {ENTRIES[0], ENTRIES[2]};
  EXPECT_EQ(reader.read, kept);

  // Appends after the kept entries; the file is this build's now and holds only those.
  const std::vector<u8> appended = {5, 5};
  cache.Append(5, appended.data(), static_cast<u32>(appended.size()));
  cache.Close();
  EXPECT_EQ(ReadRevision(), this_build);

  Cache again;
  Reader reader_again(7);
  EXPECT_EQ(again.OpenAndRead(m_path, reader_again), 3u);
  const Entries expected = {ENTRIES[0], ENTRIES[2], {5, appended}};
  EXPECT_EQ(reader_again.read, expected);
  EXPECT_TRUE(reader_again.other_build.empty());
}

TEST_F(LinearDiskCacheTest, OtherBuildKeepsNothingByDefault)
{
  Write(ENTRIES);
  SetRevision(std::string(REVISION_SIZE, 'x'));

  Cache cache;
  PlainReader reader;
  EXPECT_EQ(cache.OpenAndRead(m_path, reader), 0u);
  EXPECT_TRUE(reader.read.empty());
  cache.Close();

  Cache again;
  Reader reader_again(7);
  EXPECT_EQ(again.OpenAndRead(m_path, reader_again), 0u);
  EXPECT_TRUE(reader_again.read.empty());
  EXPECT_TRUE(reader_again.other_build.empty());
}

TEST_F(LinearDiskCacheTest, OtherKeyOrValueTypeIsNotAnotherBuild)
{
  Write(ENTRIES);
  SetRevision(std::string(REVISION_SIZE, 'x'));

  // Same id and revision field, another key size: thrown away without reading.
  Common::LinearDiskCache<u64, u8> cache;
  class Reader64 : public Common::LinearDiskCacheReader<u64, u8>
  {
  public:
    void Read(const u64&, const u8*, u32) override { ++count; }
    bool ReadFromOtherBuild(const u64&, const u8*, u32) override
    {
      ++count;
      return true;
    }
    int count = 0;
  } reader;
  EXPECT_EQ(cache.OpenAndRead(m_path, reader), 0u);
  EXPECT_EQ(reader.count, 0);
}

TEST_F(LinearDiskCacheTest, OtherBuildWithATornLastEntry)
{
  Write(ENTRIES);
  SetRevision(std::string(REVISION_SIZE, 'x'));
  {
    File::IOFile file(m_path, "r+b");
    ASSERT_TRUE(file.Resize(file.GetSize() - 2));
  }

  Cache cache;
  Reader reader(7);
  EXPECT_EQ(cache.OpenAndRead(m_path, reader), 2u);
  const Entries seen = {ENTRIES[0], ENTRIES[1], ENTRIES[2]};
  EXPECT_EQ(reader.other_build, seen);
}
