// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Online::Spectate::Uploader (docs/spectate-protocol.md §1, docs/design/spectating.md §4): what
// it sends and, above all, when it stops sending so that the game never pays for spectators.

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <mbedtls/sha256.h>
#include <picojson.h>

#include "Core/Online/Spectate.h"
#include "Core/Rollback/ReplayFile.h"

namespace Sp = Online::Spectate;
namespace Replay = Gprb::Replay;
using namespace std::chrono_literals;

namespace
{
constexpr std::array<u8, 16> UID{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr char UID_HEX[] = "0102030405060708090a0b0c0d0e0f10";
constexpr char MATCH[] = "mode.direct-2026-10-13T18:00:00.000Z-1";

picojson::object Parse(const std::string& s)
{
  picojson::value v;
  EXPECT_TRUE(picojson::parse(v, s).empty()) << s;
  return v.get<picojson::object>();
}

std::vector<u8> Frames(u32 first, u16 count)
{
  std::vector<u8> raw;
  for (int i = 0; i < 4; ++i)
    raw.push_back(static_cast<u8>(first >> (8 * i)));
  raw.push_back(static_cast<u8>(count));
  raw.push_back(static_cast<u8>(count >> 8));
  raw.push_back(0b11);
  raw.push_back(0);
  raw.insert(raw.end(), 2 * count * 0x40, static_cast<u8>(first));
  raw.insert(raw.end(), count * 4, 0x22);
  raw.push_back(0);
  raw.push_back(0);
  return raw;
}

struct Rig
{
  Sp::Uploader up;
  Sp::Clock::time_point now = Sp::Clock::time_point{} + 1000s;
  Sp::Health health;
  std::vector<std::vector<u8>> chunks;  // as added, for checking what arrives

  explicit Rig(Sp::Options o = {}) : up(o) { up.BeginGame({MATCH, UID, 1}); }

  void Add(u32 type, const std::vector<u8>& raw, u32 flags = 0)
  {
    auto chunk = *Replay::EncodeChunk(type, flags, raw);
    chunks.push_back(chunk);
    up.AddChunk(UID, type, std::move(chunk), raw);
  }

  void AddGame(int frame_chunks)
  {
    Add(Replay::CHUNK_INFO, std::vector<u8>(300, '{'));
    Add(Replay::CHUNK_SETP, std::vector<u8>(700, 's'));
    for (int i = 0; i < frame_chunks; ++i)
      Add(Replay::CHUNK_FRMS, Frames(120 + 60 * i, 60), Replay::FLAG_DEFLATE);
  }

  Sp::Outgoing Tick(Sp::Clock::duration dt = 10ms, size_t unacked = 0, bool online = true)
  {
    now += dt;
    return up.Service(now, online, unacked, health);
  }

  // Ticks through the game's first 10 s of running (its normal) with the health as it is;
  // `late_per_tick` late frames every 100 ms.
  void Learn(u64 late_per_tick = 0)
  {
    for (int i = 0; i < 105; ++i)
    {
      health.late_frames += late_per_tick;
      Tick(100ms);
    }
  }

  void Role(const std::string& role, u32 slot, std::optional<u32> from = std::nullopt,
            bool headers = true, std::optional<double> start_rate = std::nullopt)
  {
    picojson::object o{{"type", picojson::value("spectate-role")},
                       {"gameUid", picojson::value(UID_HEX)},
                       {"role", picojson::value(role)},
                       {"slot", picojson::value(static_cast<double>(slot))},
                       {"headers", picojson::value(headers)},
                       {"rate", picojson::value(2048.0)}};
    o["from"] = from ? picojson::value(static_cast<double>(*from)) : picojson::value();
    if (start_rate)
      o["startRate"] = picojson::value(*start_rate);
    EXPECT_TRUE(up.OnMessage(o));
  }

  // Puts the data packets back together: (slot, seq) → the chunk's bytes.
  static std::vector<std::vector<u8>> Join(const std::vector<std::vector<u8>>& packets, u32 slot)
  {
    std::vector<std::vector<u8>> out;
    for (const auto& p : packets)
    {
      EXPECT_GT(p.size(), 16u);
      auto u32_at = [&](size_t at) {
        return u32(p[at]) | u32(p[at + 1]) << 8 | u32(p[at + 2]) << 16 | u32(p[at + 3]) << 24;
      };
      EXPECT_EQ(u32_at(0), slot);
      const u32 seq = u32_at(4), offset = u32_at(8), total = u32_at(12);
      if (seq == out.size())
        out.emplace_back();
      EXPECT_EQ(seq + 1, out.size());
      EXPECT_EQ(offset, out.back().size());
      out.back().insert(out.back().end(), p.begin() + 16, p.end());
      EXPECT_LE(out.back().size(), total);
    }
    return out;
  }
};

std::vector<std::string> Types(const Sp::Outgoing& o)
{
  std::vector<std::string> t;
  for (const auto& j : o.json)
    t.push_back(Parse(j)["type"].get<std::string>());
  return t;
}
}  // namespace

TEST(Spectate, OffersOnceAndAgainAfterAReconnect)
{
  Rig t;
  EXPECT_TRUE(t.Tick(10ms, 0, false).json.empty()) << "nothing while offline";
  auto out = t.Tick();
  ASSERT_EQ(out.json.size(), 1u);
  auto offer = Parse(out.json[0]);
  EXPECT_EQ(offer["type"].get<std::string>(), "spectate-offer");
  EXPECT_EQ(offer["matchId"].get<std::string>(), MATCH);
  EXPECT_EQ(offer["gameUid"].get<std::string>(), UID_HEX);
  EXPECT_EQ(offer["game"].get<double>(), 1.0);
  EXPECT_TRUE(t.Tick().json.empty());
  t.up.ConnectionLost();
  EXPECT_EQ(Types(t.Tick()), std::vector<std::string>{"spectate-offer"});
}

TEST(Spectate, ThePrimarySendsEveryChunkPacedInSmallPackets)
{
  Rig t;
  t.AddGame(10);
  t.Tick();
  t.Role("primary", 7);
  std::vector<std::vector<u8>> packets;
  size_t first_second = 0;
  for (int i = 0; i < 1000; ++i)
  {
    auto out = t.Tick();
    for (const auto& p : out.data)
    {
      EXPECT_LE(p.size(), 512u);
      if (i < 100)
        first_second += p.size();
    }
    packets.insert(packets.end(), out.data.begin(), out.data.end());
  }
  // About 2 KB/s: a 1 s burst plus 1 s of rate in the first second, never more.
  EXPECT_LE(first_second, 2 * 2048u + 512);
  EXPECT_EQ(Rig::Join(packets, 7), t.chunks);
}

namespace
{
size_t Bytes(const Sp::Outgoing& o)
{
  size_t n = 0;
  for (const auto& p : o.data)
    n += p.size();
  return n;
}
}  // namespace

TEST(Spectate, TheHeadersGoAtTheCountdownAndTheFirstTenSecondsTrickle)
{
  Rig t;
  // Stored raw, so there is always more to send than the pacer lets through.
  t.Add(Replay::CHUNK_INFO, std::vector<u8>(300, '{'));
  t.Add(Replay::CHUNK_SETP, std::vector<u8>(700, 's'));
  for (int i = 0; i < 8; ++i)
    t.Add(Replay::CHUNK_FRMS, Frames(120 + 60 * i, 60));
  // The role comes in the countdown (the session isn't running yet): the role's rate, its 1 s
  // burst first, so the headers are there before the match runs.
  t.Tick();
  t.Role("primary", 7);
  std::vector<std::vector<u8>> packets;
  size_t countdown = 0;
  for (int i = 0; i < 10; ++i)
  {
    auto out = t.Tick(100ms);
    countdown += Bytes(out);
    packets.insert(packets.end(), out.data.begin(), out.data.end());
  }
  EXPECT_GE(countdown, 2048u) << "the burst and the role's rate in the countdown";
  EXPECT_GE(Rig::Join(packets, 7).size(), 2u) << "INFO and SETP are through";
  // The match runs: its first 10 s go at the start rate (1,536 B/s), never more.
  t.health.running = true;
  size_t start = 0;
  for (int i = 0; i < 95; ++i)
  {
    auto out = t.Tick(100ms);
    start += Bytes(out);
    packets.insert(packets.end(), out.data.begin(), out.data.end());
  }
  EXPECT_GT(start, 1536u * 8) << "something goes out during the start";
  EXPECT_LE(start, 1536u * 10 + 2048u) << "but no more than the trickle";
  // After them the role's rate.
  for (int i = 0; i < 10; ++i)
    t.Tick(100ms);
  size_t after = 0;
  for (int i = 0; i < 20; ++i)
    after += Bytes(t.Tick(100ms));
  EXPECT_GT(after, 1536u * 2 + 256u) << "faster than the trickle";
  EXPECT_LE(after, 2048u * 2 + 512u);
}

TEST(Spectate, AStartRateOfZeroSendsNothingDuringTheGamesFirstTenSeconds)
{
  Rig t;
  t.AddGame(5);
  t.health.running = true;
  t.Tick();
  t.Role("primary", 7, std::nullopt, true, 0.0);
  for (int i = 0; i < 95; ++i)
    EXPECT_TRUE(t.Tick(100ms).data.empty()) << i;
  bool sent = false;
  for (int i = 0; i < 10 && !sent; ++i)
    sent = !t.Tick(100ms).data.empty();
  EXPECT_TRUE(sent) << "sending starts once the first 10 s are over";
}

TEST(Spectate, AQueueOnTheUplinkHoldsTheTrickleToo)
{
  Rig t;
  t.AddGame(30);
  t.health.mm_rtt_ms = 30;  // the menus
  t.Tick();
  t.health.running = true;
  t.health.mm_rtt_ms = 120;  // the match fills the uplink from its first frame
  t.Role("primary", 7);
  for (int i = 0; i < 50; ++i)
    EXPECT_TRUE(t.Tick(100ms).data.empty()) << i;
  t.health.mm_rtt_ms = 35;
  bool sent = false;
  for (int i = 0; i < 5 && !sent; ++i)
    sent = !t.Tick(100ms).data.empty();
  EXPECT_TRUE(sent);
}

TEST(Spectate, AQueueOnTheUplinkHoldsTheUpload)
{
  Rig t;
  t.AddGame(30);
  t.health.mm_rtt_ms = 40;
  t.Tick();
  t.Role("primary", 7);
  EXPECT_FALSE(t.Tick().data.empty());
  // 25 ms over the lowest round trip: still sending; 26 ms: a queue, nothing until it drains.
  t.health.mm_rtt_ms = 65;
  EXPECT_FALSE(t.Tick(500ms).data.empty());
  t.health.mm_rtt_ms = 66;
  for (int i = 0; i < 50; ++i)
  {
    auto out = t.Tick(100ms);
    EXPECT_TRUE(out.data.empty() && out.json.empty()) << i;
  }
  t.health.mm_rtt_ms = 45;
  auto out = t.Tick(100ms);
  EXPECT_FALSE(out.data.empty());
  // Held time does not turn into a burst: at most the pacer's 1 s of tokens.
  size_t bytes = 0;
  for (const auto& p : out.data)
    bytes += p.size();
  EXPECT_LE(bytes, 2048u + 512);
}

TEST(Spectate, AGameFullFromItsStartIsMeasuredAgainstTheMenus)
{
  Rig t;
  t.AddGame(30);
  // In the menus the round trip to mm is 30 ms.
  t.health.mm_rtt_ms = 30;
  t.Tick();
  t.Role("primary", 7);
  // The match fills the uplink from its first frame: 200 ms. A lowest taken within the game would
  // be 200 and nothing would hold; against the menus' 30 it does.
  t.health.running = true;
  t.health.mm_rtt_ms = 200;
  t.Learn();
  for (int i = 0; i < 30; ++i)
    EXPECT_TRUE(t.Tick(100ms).data.empty()) << i;
  t.health.mm_rtt_ms = 40;
  bool sent = false;
  for (int i = 0; i < 5 && !sent; ++i)
    sent = !t.Tick(100ms).data.empty();
  EXPECT_TRUE(sent);
}

TEST(Spectate, NothingIsSentWhileTheUploadIsUnacknowledged)
{
  Rig t;
  t.AddGame(2);
  t.Tick();
  t.Role("primary", 7);
  EXPECT_TRUE(t.Tick(10ms, 3000).data.empty());
  EXPECT_FALSE(t.Tick(10ms, 0).data.empty());
  // Far more in flight: the upload pauses.
  auto out = t.Tick(10ms, 9000);
  EXPECT_EQ(Types(out), std::vector<std::string>{"spectate-pause"});
  EXPECT_EQ(Parse(out.json[0])["reason"].get<std::string>(), "queue");
}

TEST(Spectate, NothingPausesWhileTheGamesNormalIsLearned)
{
  Rig t;
  t.AddGame(2);
  t.health.running = true;
  t.Tick();
  t.Role("primary", 7);
  // The start of a match (its hitch at GO) is part of the game's normal, not a reason to pause.
  t.health.late_frames = 30;
  t.health.rollback_avg = 5;
  for (int i = 0; i < 90; ++i)
    EXPECT_TRUE(t.Tick(100ms).json.empty()) << i;
}

TEST(Spectate, LateFramesOverTheGamesNormalPauseAndTenCleanSecondsResume)
{
  Rig t;
  t.AddGame(2);
  t.health.running = true;
  t.Tick();
  t.Role("primary", 7);
  t.Learn();  // no late frames
  // A hitch of a few frames is let through; more than 4 over the normal in 2 s are not.
  t.health.late_frames += 4;
  EXPECT_TRUE(t.Tick().json.empty());
  t.health.late_frames += 1;
  auto out = t.Tick();
  ASSERT_EQ(Types(out), std::vector<std::string>{"spectate-pause"});
  EXPECT_EQ(Parse(out.json[0])["reason"].get<std::string>(), "frame");
  EXPECT_EQ(Parse(out.json[0])["slot"].get<double>(), 7.0);
  t.Add(Replay::CHUNK_FRMS, Frames(240, 60), Replay::FLAG_DEFLATE);
  // The signal looks back 2 s, so it clears about 2 s after the pause; 10 clean seconds later
  // the upload resumes.
  for (int i = 0; i < 110; ++i)
  {
    out = t.Tick(100ms);
    EXPECT_TRUE(out.data.empty() && out.json.empty()) << i;
  }
  out = t.Tick(1500ms);
  EXPECT_EQ(Types(out), std::vector<std::string>{"spectate-resume"});
  EXPECT_FALSE(out.data.empty());
}

TEST(Spectate, APcThatIsAlwaysALittleLateIsNotPausedForIt)
{
  Rig t;
  t.AddGame(2);
  t.health.running = true;
  t.Tick();
  t.Role("primary", 7);
  t.Learn(1);  // 10 late frames a second from the start
  for (int i = 0; i < 300; ++i)
  {
    t.health.late_frames += 1;
    EXPECT_TRUE(t.Tick(100ms).json.empty()) << i;
  }
  // Three times as many: the game got worse; the upload pauses within the window.
  bool paused = false;
  for (int i = 0; i < 20 && !paused; ++i)
  {
    t.health.late_frames += 3;
    paused = Types(t.Tick(100ms)) == std::vector<std::string>{"spectate-pause"};
  }
  EXPECT_TRUE(paused);
}

TEST(Spectate, TheThirdPauseStops)
{
  Rig t;
  t.AddGame(20);
  t.health.running = true;
  t.Tick();
  t.Role("primary", 7);
  t.Learn();  // no late frames
  for (int pause = 1; pause <= 3; ++pause)
  {
    t.health.late_frames += 5;
    auto out = t.Tick();
    ASSERT_EQ(out.json.size(), 1u) << pause;
    auto m = Parse(out.json[0]);
    EXPECT_EQ(m["type"].get<std::string>(), pause < 3 ? "spectate-pause" : "spectate-stop");
    EXPECT_EQ(m["reason"].get<std::string>(), "frame");
    t.Tick(11s);
    t.Tick(11s);
  }
  for (int i = 0; i < 50; ++i)
  {
    auto out = t.Tick(1s);
    EXPECT_TRUE(out.data.empty() && out.json.empty());
  }
}

TEST(Spectate, RollbacksAndPingAboveTheGamesNormalPause)
{
  Rig t;
  t.AddGame(40);
  t.health.running = true;
  t.health.rollback_avg = 1.5;
  t.health.ping_ms = 60;
  t.Tick();
  t.Role("primary", 7);
  // During the first 10 s the normal is learned; deeper rollbacks then don't pause yet.
  for (int i = 0; i < 50; ++i)
    t.Tick(100ms);
  t.health.rollback_avg = 3.0;
  EXPECT_TRUE(t.Tick(100ms).json.empty());
  t.health.rollback_avg = 1.5;
  for (int i = 0; i < 60; ++i)
    t.Tick(100ms);
  t.health.rollback_avg = 2.6;
  auto out = t.Tick(100ms);
  ASSERT_EQ(Types(out), std::vector<std::string>{"spectate-pause"});
  EXPECT_EQ(Parse(out.json[0])["reason"].get<std::string>(), "rollback");

  Rig u;
  u.AddGame(40);
  u.health.running = true;
  u.health.ping_ms = 60;
  u.Tick();
  u.Role("primary", 7);
  for (int i = 0; i < 110; ++i)
    u.Tick(100ms);
  u.health.ping_ms = 79;
  EXPECT_TRUE(u.Tick(100ms).json.empty());
  u.health.ping_ms = 81;
  out = u.Tick(100ms);
  ASSERT_EQ(Types(out), std::vector<std::string>{"spectate-pause"});
  EXPECT_EQ(Parse(out.json[0])["reason"].get<std::string>(), "rtt");
}

TEST(Spectate, ATakeoverSkipsTheHeadersAndStartsAtItsFrame)
{
  Rig t;
  t.AddGame(5);
  t.Tick();
  t.Role("primary", 9, 240, false);
  std::vector<std::vector<u8>> packets;
  for (int i = 0; i < 500; ++i)
  {
    auto out = t.Tick();
    packets.insert(packets.end(), out.data.begin(), out.data.end());
  }
  const auto got = Rig::Join(packets, 9);
  // INFO, SETP, frames 120 and 180 skipped: frames 240, 300, 360.
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], t.chunks[4]);
  EXPECT_EQ(got[2], t.chunks[6]);
}

TEST(Spectate, AFromNoChunkStartsAtStops)
{
  Rig t;
  t.AddGame(3);
  t.Tick();
  t.Role("primary", 9, 150, false);
  auto out = t.Tick();
  EXPECT_EQ(Types(out), std::vector<std::string>{"spectate-stop"});
  EXPECT_TRUE(out.data.empty());
}

TEST(Spectate, AWitnessSendsDigestsOfTheRawFramesEveryFiveSeconds)
{
  Rig t;
  t.AddGame(3);
  t.Tick();
  t.Role("witness", 4);
  auto out = t.Tick();
  ASSERT_EQ(Types(out), std::vector<std::string>{"spectate-digest"});
  EXPECT_TRUE(out.data.empty());
  auto m = Parse(out.json[0]);
  const auto& frms = m["frms"].get<picojson::array>();
  ASSERT_EQ(frms.size(), 3u);
  EXPECT_EQ(frms[0].get<picojson::array>()[0].get<double>(), 120.0);
  std::array<u8, 32> sha{};
  const auto raw = Frames(120, 60);
  mbedtls_sha256_ret(raw.data(), raw.size(), sha.data(), 0);
  std::string hex;
  for (int i = 0; i < 8; ++i)
    hex += fmt::format("{:02x}", sha[i]);
  EXPECT_EQ(frms[0].get<picojson::array>()[1].get<std::string>(), hex);
  t.Add(Replay::CHUNK_FRMS, Frames(300, 60), Replay::FLAG_DEFLATE);
  EXPECT_TRUE(t.Tick(1s).json.empty());
  out = t.Tick(5s);
  ASSERT_EQ(Types(out), std::vector<std::string>{"spectate-digest"});
  EXPECT_EQ(Parse(out.json[0])["frms"].get<picojson::array>().size(), 1u);
  // An unhealthy witness sends nothing either.
  t.health.running = true;
  t.Learn();
  t.Add(Replay::CHUNK_FRMS, Frames(360, 60), Replay::FLAG_DEFLATE);
  t.health.rollback_avg = 3;
  EXPECT_TRUE(t.Tick(6s).json.empty());
}

TEST(Spectate, RolesForOtherGamesAndJunkAreIgnored)
{
  Rig t;
  t.AddGame(2);
  t.Tick();
  picojson::object other{{"type", picojson::value("spectate-role")},
                         {"gameUid", picojson::value(std::string(32, 'f'))},
                         {"role", picojson::value("primary")},
                         {"slot", picojson::value(3.0)}};
  EXPECT_TRUE(t.up.OnMessage(other));
  EXPECT_TRUE(t.Tick().data.empty());
  picojson::object bad{{"type", picojson::value("spectate-role")},
                       {"gameUid", picojson::value(UID_HEX)},
                       {"role", picojson::value("primary")},
                       {"slot", picojson::value(-1.0)}};
  EXPECT_TRUE(t.up.OnMessage(bad));
  EXPECT_TRUE(t.Tick().data.empty());
  EXPECT_FALSE(t.up.OnMessage({{"type", picojson::value("room-state")}}));
  t.Role("none", 0);
  EXPECT_TRUE(t.Tick().data.empty());
}

TEST(Spectate, AGameOverItsBudgetStops)
{
  Sp::Options o;
  o.max_game_bytes = 1200;  // INFO and SETP are about 1 KB: the frames go over it
  Rig t(o);
  t.AddGame(10);
  t.Tick();
  t.Role("primary", 7);
  bool stopped = false;
  for (int i = 0; i < 300 && !stopped; ++i)
  {
    for (const auto& type : Types(t.Tick()))
      stopped |= type == "spectate-stop";
  }
  EXPECT_TRUE(stopped);
}

TEST(Spectate, ANewGameReportsTheLastOnesPausesAndRollbacks)
{
  Rig t;
  t.AddGame(2);
  t.health.running = true;
  t.health.rollback_avg = 2.0;
  t.Tick();
  t.Role("primary", 7);
  t.Learn();
  t.health.late_frames += 5;
  EXPECT_EQ(Types(t.Tick()), std::vector<std::string>{"spectate-pause"});
  t.up.BeginGame({MATCH, {9}, 2});
  t.health.running = false;
  auto offer = Parse(t.Tick().json.at(0));
  EXPECT_EQ(offer["paused"].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(offer["rollbacks"].get<double>(), 2.0);
}

// Production 2026-10-15: a game whose only uploader stopped (late frames) never got its ENDG to
// mm, and the list's score missed it ("Game 3, 1-0"). Every player's game reports each
// game's end, whatever its role and however its upload went.
TEST(Spectate, EveryPlayerReportsTheGamesEndWhateverItsRole)
{
  const auto info = [] {
    const std::string j = R"({"matchId":"m","isTeams":false,"players":[)"
                          R"({"port":1,"uid":"aaaa","teamId":null},{"port":2,"uid":"bbbb"}]})";
    return std::vector<u8>(j.begin(), j.end());
  };
  const auto endg = [](const std::string& j) { return std::vector<u8>(j.begin(), j.end()); };

  // A witness (it never uploads the ENDG itself).
  Rig t;
  t.Add(Replay::CHUNK_INFO, info());
  t.Tick();
  t.Role("witness", 8);
  t.Add(Replay::CHUNK_ENDG, endg(R"({"endMethod":"game","winnerPort":2,"players":[]})"));
  auto out = t.Tick();
  ASSERT_EQ(Types(out), std::vector<std::string>{"spectate-end"});
  auto e = Parse(out.json[0]);
  EXPECT_EQ(e["matchId"].get<std::string>(), MATCH);
  EXPECT_EQ(e["gameUid"].get<std::string>(), UID_HEX);
  EXPECT_EQ(e["endMethod"].get<std::string>(), "game");
  EXPECT_EQ(e["winnerPort"].get<double>(), 2.0);
  EXPECT_EQ(e.count("quitterPort"), 0u);
  const auto& players = e["players"].get<picojson::array>();
  ASSERT_EQ(players.size(), 2u);
  EXPECT_EQ(players[1].get<picojson::object>().at("uid").get<std::string>(), "bbbb");
  EXPECT_EQ(players[0].get<picojson::object>().count("teamId"), 0u);
  EXPECT_TRUE(t.Tick().json.empty()) << "once";

  // A primary that stopped for the players' sake; the report waits while offline, and goes out
  // after the next game began.
  Rig s;
  s.AddGame(2);
  s.Tick();
  s.Role("primary", 7);
  s.health.running = true;
  s.Learn();
  for (int pause = 1; pause <= 3; ++pause)
  {
    s.health.late_frames += 5;
    s.Tick();
    s.Tick(11s);
    s.Tick(11s);
  }
  EXPECT_TRUE(s.up.Status()["stopped"].get<bool>());
  s.Add(Replay::CHUNK_ENDG, endg(R"({"endMethod":"game","winnerPort":"draw"})"));
  EXPECT_TRUE(s.Tick(10ms, 0, false).json.empty());
  s.up.BeginGame({MATCH, {9}, 2});
  out = s.Tick();
  e = Parse(out.json.at(0));
  EXPECT_EQ(e["type"].get<std::string>(), "spectate-end");
  EXPECT_EQ(e.count("winnerPort"), 0u) << "a draw names no winner";
  EXPECT_EQ(Parse(out.json.at(1))["type"].get<std::string>(), "spectate-offer");

  // mm said not to stream it: nothing.
  Rig n;
  n.Tick();
  n.Role("none", 0);
  n.Add(Replay::CHUNK_ENDG, endg(R"({"endMethod":"game","winnerPort":1})"));
  EXPECT_TRUE(n.Tick().json.empty());
}
