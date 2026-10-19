// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Watching the room's game (Core/Online/RoomWatch): the watch channel's packets put together into a
// live replay file, and hostile packets.

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Online/RoomWatch.h"
#include "Core/Rollback/ReplayFile.h"

using namespace Online::RoomWatch;
namespace Replay = Gprb::Replay;

namespace
{
const char* INFO = R"({"v":1,"mode":"room","matchId":"mode.room-KFQB-1","players":[]})";
const char* SETUP = R"({"v":1,"sim":1})";
const char* ENDG = R"({"endMethod":"game","lastFrame":59})";
const char* UID = "abababababababababababababababab";

std::vector<u8> Text(u8 kind, const std::string& json)
{
  std::vector<u8> v{kind};
  v.insert(v.end(), json.begin(), json.end());
  return v;
}

std::vector<u8> Begin(const char* uid = UID)
{
  return Text(KIND_BEGIN,
              std::string(R"({"matchId":"mode.room-KFQB-1","number":2,"uid":")") + uid + "\"}");
}

std::vector<u8> End(const char* bad = nullptr)
{
  std::string j = std::string(R"({"matchId":"mode.room-KFQB-1","uid":")") + UID + "\"";
  if (bad)
    j += std::string(R"(,"bad":")") + bad + "\"";
  return Text(KIND_END, j + "}");
}

std::vector<u8> Chunk(u32 index, u32 type, u32 flags, const std::vector<u8>& raw)
{
  const auto c = Replay::EncodeChunk(type, flags, raw);
  EXPECT_TRUE(c);
  std::vector<u8> v{KIND_CHUNK};
  for (int i = 0; i < 4; ++i)
    v.push_back(static_cast<u8>(index >> (8 * i)));
  v.insert(v.end(), c->begin(), c->end());
  return v;
}

std::vector<u8> Bytes(const char* s)
{
  return {s, s + std::strlen(s)};
}

Replay::FrameChunk Frames(u32 first, u32 count)
{
  Replay::FrameChunk c;
  c.first_frame = first;
  c.ports = 0x3;
  for (u32 i = 0; i < count; ++i)
  {
    Replay::Slots s{};
    s[0] = static_cast<u8>(i);
    c.pads.push_back(s);
    c.checksums.push_back(0x2000 + first + i);
  }
  return c;
}
}  // namespace

TEST(RoomWatch, ChunksMakeALiveReplay)
{
  Assembler a;
  ASSERT_TRUE(a.OnPacket(Begin()));
  ASSERT_TRUE(a.OnPacket(Chunk(0, Replay::CHUNK_INFO, 0, Bytes(INFO))));
  ASSERT_TRUE(a.OnPacket(Chunk(1, Replay::CHUNK_SETP, Replay::FLAG_CRITICAL, Bytes(SETUP))));
  ASSERT_TRUE(a.OnPacket(Chunk(2, Replay::CHUNK_FRMS, Replay::FLAG_DEFLATE | Replay::FLAG_CRITICAL,
                               Replay::EncodeFrames(Frames(0, 60)))));
  ASSERT_EQ(a.Games().size(), 1u);
  const Game& g = a.Games().front();
  EXPECT_EQ(g.number, 2u);
  EXPECT_EQ(g.chunks, 3u);
  EXPECT_FALSE(g.Done());
  std::string error;
  auto f = Replay::Parse(g.bytes, Replay::ReadOptions{}, &error);
  ASSERT_TRUE(f) << error;
  EXPECT_EQ(f->info, INFO);
  EXPECT_EQ(f->setup, SETUP);
  EXPECT_EQ(f->pads.size(), 60u);
  EXPECT_TRUE(f->endg.empty());
  EXPECT_EQ(f->uid[0], 0xAB);
  // Its end.
  ASSERT_TRUE(a.OnPacket(Chunk(3, Replay::CHUNK_ENDG, 0, Bytes(ENDG))));
  ASSERT_TRUE(a.OnPacket(End()));
  EXPECT_TRUE(g.ended);
  f = Replay::Parse(g.bytes, Replay::ReadOptions{}, &error);
  ASSERT_TRUE(f) << error;
  EXPECT_EQ(f->endg, ENDG);
  // Nothing more after the end; the next game is a game of its own.
  EXPECT_FALSE(a.OnPacket(Chunk(4, Replay::CHUNK_EVNT, 0, Bytes("{}"))));
  ASSERT_TRUE(a.OnPacket(Begin("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd")));
  EXPECT_EQ(a.Games().size(), 2u);
}

TEST(RoomWatch, AMissingChunkOrACutEndsTheGame)
{
  Assembler a;
  ASSERT_TRUE(a.OnPacket(Begin()));
  ASSERT_TRUE(a.OnPacket(Chunk(0, Replay::CHUNK_INFO, 0, Bytes(INFO))));
  EXPECT_FALSE(a.OnPacket(Chunk(2, Replay::CHUNK_SETP, 0, Bytes(SETUP))));
  EXPECT_FALSE(a.Games().front().bad.empty());
  EXPECT_TRUE(a.Games().front().Done());

  Assembler b;
  ASSERT_TRUE(b.OnPacket(Begin()));
  ASSERT_TRUE(b.OnPacket(End("The rest of this game wasn't streamed.")));
  EXPECT_EQ(b.Games().front().bad, "The rest of this game wasn't streamed.");
  EXPECT_FALSE(b.Games().front().ended);
  // A new game while one is open: the open one was cut.
  Assembler c;
  ASSERT_TRUE(c.OnPacket(Begin()));
  ASSERT_TRUE(c.OnPacket(Begin("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd")));
  EXPECT_FALSE(c.Games().front().bad.empty());
}

TEST(RoomWatch, HostilePacketsAreDropped)
{
  Assembler a;
  EXPECT_FALSE(a.OnPacket({}));
  EXPECT_FALSE(a.OnPacket(std::vector<u8>{9, 1, 2}));
  EXPECT_FALSE(a.OnPacket(Chunk(0, Replay::CHUNK_INFO, 0, Bytes(INFO))));  // no game begun
  EXPECT_FALSE(a.OnPacket(Text(KIND_BEGIN, "not json")));
  EXPECT_FALSE(a.OnPacket(Text(KIND_BEGIN, R"({"matchId":"m","uid":"xyz"})")));
  EXPECT_FALSE(a.OnPacket(Text(KIND_BEGIN, R"({"matchId":"m","uid":"ABABABABABABABABABABABABABABABAB"})")));
  EXPECT_FALSE(a.OnPacket(Text(KIND_BEGIN, std::string(2000, '{'))));
  EXPECT_TRUE(a.Games().empty());
  ASSERT_TRUE(a.OnPacket(Begin()));
  EXPECT_FALSE(a.OnPacket(std::vector<u8>{KIND_CHUNK, 0, 0, 0, 0, 1, 2, 3}));  // shorter than a header
  EXPECT_TRUE(a.Games().front().Done());
  // Another game's end changes nothing.
  Assembler b;
  ASSERT_TRUE(b.OnPacket(Begin()));
  EXPECT_FALSE(b.OnPacket(Text(KIND_END, R"({"uid":"cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"})")));
  EXPECT_FALSE(b.Games().front().Done());
  // At most MAX_GAMES games are kept.
  Assembler c;
  for (size_t i = 0; i < MAX_GAMES + 2; ++i)
  {
    ASSERT_TRUE(c.OnPacket(Begin()));
    ASSERT_TRUE(c.OnPacket(End()));
  }
  EXPECT_EQ(c.Games().size(), MAX_GAMES);
}
