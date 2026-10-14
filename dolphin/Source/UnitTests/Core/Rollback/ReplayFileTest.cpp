// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Replay files (Core/Rollback/ReplayFile): what the recorder writes reads back the same; a file
// cut at any byte reads as its longest valid prefix; damaged and hostile files are refused or
// cut short cleanly, never trusted (a replay is a file anyone can share).

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Core/Rollback/ReplayFile.h"

using namespace Gprb::Replay;

namespace
{
const char* INFO = R"({"v":1,"mode":"direct","matchId":"m","players":[]})";
const char* SETUP = R"({"v":1,"sim":1})";
const char* ENDG = R"({"endMethod":"game","lastFrame":119})";

FrameChunk Frames(u32 first, u32 count)
{
  FrameChunk c;
  c.first_frame = first;
  c.ports = 0x5;  // P1 and P3
  for (u32 i = 0; i < count; ++i)
  {
    Slots s{};
    for (int p = 0; p < PORTS; ++p)
    {
      if (!((c.ports >> p) & 1))
      {
        SetNoController(&s, p);
        continue;
      }
      s[p * PAD_SIZE + 0] = static_cast<u8>((first + i) & 0xFF);
      s[p * PAD_SIZE + 0x10] = static_cast<u8>(p * 7 + i / 3);
    }
    c.pads.push_back(s);
    c.checksums.push_back(0x1000 + first + i);
    if ((first + i) % PARTS_EVERY == 0)
      c.parts.push_back({first + i, {1, 2, 3}, 4});
  }
  return c;
}

std::string TempPath(const char* name)
{
  return ::File::GetTempFilenameForAtomicWrite(::File::CreateTempDir() + "/" + name);
}

std::vector<u8> WriteReplay(const std::string& path, bool finish)
{
  Writer w;
  std::string error;
  std::array<u8, 16> uid{};
  uid.fill(0xAB);
  EXPECT_TRUE(w.Open(path, uid, &error)) << error;
  EXPECT_TRUE(w.Append(CHUNK_INFO, 0, std::span(reinterpret_cast<const u8*>(INFO), std::strlen(INFO))));
  EXPECT_TRUE(w.Append(CHUNK_SETP, FLAG_CRITICAL,
                       std::span(reinterpret_cast<const u8*>(SETUP), std::strlen(SETUP))));
  for (u32 first : {0u, 60u})
  {
    const auto raw = EncodeFrames(Frames(first, 60));
    EXPECT_TRUE(w.Append(CHUNK_FRMS, FLAG_DEFLATE | FLAG_CRITICAL, raw));
    FighterChunk f;
    f.first_frame = first;
    f.count = 60;
    f.ports = 0x5;
    f.block = 16;
    f.data.assign(2, std::vector<u8>(60 * 16, 0x11));
    EXPECT_TRUE(w.Append(CHUNK_FTRM, FLAG_DEFLATE, EncodeFighters(f)));
  }
  if (finish)
    EXPECT_TRUE(w.Finish(std::span(reinterpret_cast<const u8*>(ENDG), std::strlen(ENDG)), 119));
  else
    w.Close();
  std::string bytes;
  EXPECT_TRUE(::File::ReadFileToString(path, bytes));
  return {bytes.begin(), bytes.end()};
}
}  // namespace

TEST(ReplayFile, RoundTrip)
{
  const auto bytes = WriteReplay(TempPath("a.brep"), true);
  std::string error;
  const auto f = Parse(bytes, ReadOptions{.frames = true, .fighters = true}, &error);
  ASSERT_TRUE(f) << error;
  EXPECT_TRUE(f->trailer);
  EXPECT_FALSE(f->truncated);
  EXPECT_EQ(f->info, INFO);
  EXPECT_EQ(f->setup, SETUP);
  EXPECT_EQ(f->endg, ENDG);
  EXPECT_EQ(f->uid[0], 0xAB);
  ASSERT_EQ(f->pads.size(), 120u);
  EXPECT_EQ(f->ports, 0x5);
  const auto want = Frames(60, 60);
  EXPECT_EQ(f->pads[60], want.pads[0]);
  EXPECT_EQ(f->pads[119], want.pads[59]);
  EXPECT_EQ(f->checksums[100], 0x1000u + 100);
  EXPECT_EQ(f->parts.size(), 2u);
  EXPECT_EQ(f->fighter_frames, 120u);
  ASSERT_EQ(f->fighters.size(), 2u);
  EXPECT_EQ(f->fighters[1][120 * 16 - 1], 0x11);
  // A port nobody plays reads "no controller".
  EXPECT_EQ(f->pads[5][1 * PAD_SIZE + 0x38], 0xFF);
}

TEST(ReplayFile, EveryCutReadsAsItsValidPrefix)
{
  const auto bytes = WriteReplay(TempPath("b.brep"), true);
  size_t last_frames = 0;
  for (size_t n = 0; n <= bytes.size(); ++n)
  {
    std::string error;
    const auto f = Parse(std::span(bytes.data(), n), ReadOptions{}, &error);
    if (!f)
      continue;  // before the metadata is complete: refused, never a crash
    EXPECT_TRUE(f->pads.size() % 60 == 0);
    EXPECT_GE(f->pads.size(), last_frames);
    last_frames = f->pads.size();
    EXPECT_EQ(f->trailer, n == bytes.size());
  }
  EXPECT_EQ(last_frames, 120u);
}

TEST(ReplayFile, AGameInProgressHasNoResult)
{
  const auto bytes = WriteReplay(TempPath("c.brep"), false);
  std::string error;
  const auto f = Parse(bytes, ReadOptions{}, &error);
  ASSERT_TRUE(f) << error;
  EXPECT_FALSE(f->trailer);
  EXPECT_TRUE(f->endg.empty());
  EXPECT_EQ(f->pads.size(), 120u);
}

TEST(ReplayFile, DamageAnywhereIsRefusedOrCutShort)
{
  const auto good = WriteReplay(TempPath("d.brep"), true);
  for (size_t i = 0; i < good.size(); ++i)
  {
    auto bytes = good;
    bytes[i] ^= 0x5A;
    std::string error;
    const auto f = Parse(bytes, ReadOptions{.frames = true, .fighters = true}, &error);
    if (!f)
      continue;
    // Whatever survives is whole chunks: frames in sixties, from frame 0.
    EXPECT_EQ(f->pads.size() % 60, 0u);
    if (!f->pads.empty())
      EXPECT_EQ(f->first_frame, 0u);
  }
}

TEST(ReplayFile, HostileSizesAreRefused)
{
  auto bytes = WriteReplay(TempPath("e.brep"), true);
  // INFO claims 4 GiB raw.
  auto big = bytes;
  const u32 huge = 0xFFFFFFFF;
  std::memcpy(big.data() + HEADER_SIZE + 12, &huge, 4);
  std::string error;
  EXPECT_FALSE(Parse(big, ReadOptions{}, &error));
  // Another major version.
  auto major = bytes;
  major[8] = 2;
  EXPECT_FALSE(Parse(major, ReadOptions{}, &error));
  // An unknown critical chunk after the setup: refused; an unknown other one: skipped.
  for (const bool critical : {true, false})
  {
    auto extra = std::vector<u8>(bytes.begin(), bytes.end() - TRAILER_SIZE);
    std::vector<u8> chunk(CHUNK_HEADER_SIZE + 4, 0);
    const u32 type = FourCC("XXXX"), flags = critical ? FLAG_CRITICAL : 0, len = 4;
    std::memcpy(chunk.data(), &type, 4);
    std::memcpy(chunk.data() + 4, &flags, 4);
    std::memcpy(chunk.data() + 8, &len, 4);
    std::memcpy(chunk.data() + 12, &len, 4);
    const u32 crc = 0x2144DF1C;  // CRC-32 of four zero bytes
    std::memcpy(chunk.data() + 16, &crc, 4);
    extra.insert(extra.end(), chunk.begin(), chunk.end());
    const auto f = Parse(extra, ReadOptions{}, &error);
    EXPECT_EQ(f.has_value(), !critical) << error;
  }
  // A frame chunk that does not follow the one before: refused.
  {
    Writer w;
    const auto path = TempPath("f.brep");
    ASSERT_TRUE(w.Open(path, {}, &error));
    w.Append(CHUNK_INFO, 0, std::span(reinterpret_cast<const u8*>(INFO), std::strlen(INFO)));
    w.Append(CHUNK_SETP, FLAG_CRITICAL, std::span(reinterpret_cast<const u8*>(SETUP), std::strlen(SETUP)));
    w.Append(CHUNK_FRMS, FLAG_DEFLATE | FLAG_CRITICAL, EncodeFrames(Frames(0, 60)));
    w.Append(CHUNK_FRMS, FLAG_DEFLATE | FLAG_CRITICAL, EncodeFrames(Frames(120, 60)));
    w.Close();
    EXPECT_FALSE(ReadFile(path, ReadOptions{}, &error));
  }
  // Decoders refuse impossible headers before allocating anything.
  std::vector<u8> frames(8, 0);
  frames[4] = 0xFF;
  frames[5] = 0xFF;  // 65535 frames
  frames[6] = 1;
  EXPECT_FALSE(DecodeFrames(frames, &error));
  std::vector<u8> fighters(16, 0);
  fighters[4] = 1;
  fighters[8] = 1;
  fighters[12] = 0xFF;
  fighters[13] = 0xFF;
  fighters[14] = 0xFF;  // a 16 MB block
  EXPECT_FALSE(DecodeFighters(fighters, &error));
}

TEST(ReplayFile, FileNamePartsAreSafe)
{
  EXPECT_EQ(FileNamePart("Alice", 16), "Alice");
  EXPECT_EQ(FileNamePart("a/b\\c:d*e?f\"g<h>i|j", 32), "abcdefghij");
  EXPECT_EQ(FileNamePart("..hidden..", 16), "hidden");
  EXPECT_EQ(FileNamePart("  spaced  ", 16), "spaced");
  EXPECT_EQ(FileNamePart("x\x01y\x7Fz", 16), "xyz");
  EXPECT_EQ(FileNamePart("evil\xE2\x80\xAEname", 16), "evilname");  // U+202E
  EXPECT_EQ(FileNamePart("\xC3\xA9t\xC3\xA9", 16), "\xC3\xA9t\xC3\xA9");  // été kept
  EXPECT_EQ(FileNamePart("abcdefghijklmnopqrstuvwxyz", 16), "abcdefghijklmnop");
  EXPECT_EQ(FileNamePart("\xFF\xFE", 16), "");
}
