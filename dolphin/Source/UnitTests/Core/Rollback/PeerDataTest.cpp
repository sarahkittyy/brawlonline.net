// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Rollback/PeerData.h"

using namespace Gprb::PeerData;

TEST(PeerData, DeepJsonIsRefusedBeforeParsing)
{
  // picojson parses recursively without a limit: this used to overflow the network thread's
  // stack (one UDP packet from anyone who knew the port).
  const std::string deep(60000, '[');
  EXPECT_FALSE(JsonDepthOk(deep));
  EXPECT_FALSE(ParseControl(deep).has_value());
  const std::string deep_objects = std::string(20, '{') + std::string(20, '}');
  EXPECT_FALSE(ParseControl(deep_objects).has_value());

  // Brackets inside strings do not count.
  EXPECT_TRUE(JsonDepthOk(R"({"name":"[[[[[[[[[[[[[[[[[[[[\"[[[["})"));
  const auto ok = ParseControl(R"({"t":"st","lock":{"kind":7},"tasks":[["a","b"],[]]})");
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->count("lock"), 1u);
}

TEST(PeerData, ControlMessageLimits)
{
  EXPECT_FALSE(ParseControl("").has_value());
  EXPECT_FALSE(ParseControl("[1,2]").has_value());
  EXPECT_FALSE(ParseControl("{\"a\":").has_value());
  std::string big = "{\"name\":\"" + std::string(MAX_CONTROL_SIZE, 'x') + "\"}";
  EXPECT_FALSE(ParseControl(big).has_value());
}

TEST(PeerData, HexIsStrict)
{
  EXPECT_EQ(*ParseHex("00ff7A", 3), (std::vector<u8>{0x00, 0xFF, 0x7A}));
  // std::stoul threw on these (an uncaught exception on the network thread).
  EXPECT_FALSE(ParseHex("zz", 1).has_value());
  EXPECT_FALSE(ParseHex("0x", 1).has_value());
  EXPECT_FALSE(ParseHex(" 1", 1).has_value());
  EXPECT_FALSE(ParseHex("-1", 1).has_value());
  // Exact length only.
  EXPECT_FALSE(ParseHex("00", 2).has_value());
  EXPECT_FALSE(ParseHex("000", 1).has_value());
  EXPECT_TRUE(ParseHex("", 0).has_value());
}

TEST(PeerData, NumbersAreCheckedBeforeAnyCast)
{
  picojson::value v;
  auto num = [&](const std::string& text) {
    v = picojson::value();
    picojson::parse(v, text);
    return JsonUInt(&v, 0xFF);
  };
  EXPECT_EQ(num("7"), 7u);
  EXPECT_EQ(num("255"), 255u);
  EXPECT_FALSE(num("256").has_value());
  EXPECT_FALSE(num("-1").has_value());
  EXPECT_FALSE(num("1.5").has_value());
  EXPECT_FALSE(num("1e300").has_value());
  EXPECT_FALSE(num("\"7\"").has_value());
  EXPECT_FALSE(num("[7]").has_value());
  EXPECT_FALSE(num("null").has_value());
  EXPECT_FALSE(JsonUInt(nullptr, 1).has_value());
  v = picojson::value(4294967295.0);
  EXPECT_EQ(JsonUInt(&v, 0xFFFFFFFF), 0xFFFFFFFFu);
}

TEST(PeerData, NumbersThatOverflowAreRefusedBeforeParsing)
{
  // Dolphin's picojson calls std::abort() when a number parses to infinity: one control message
  // with 1e999 in it ended the process.
  EXPECT_FALSE(JsonSafeToParse(R"({"a":1e999})"));
  EXPECT_FALSE(JsonSafeToParse(R"({"a":[1,2,-1e999]})"));
  EXPECT_FALSE(JsonSafeToParse("1" + std::string(400, '0')));
  EXPECT_FALSE(ParseControl(R"({"t":"st","match":1e999})").has_value());
  EXPECT_TRUE(JsonSafeToParse(R"({"a":1e308,"b":-0.5,"c":"1e999","d":"\"1e999"})"));
  EXPECT_TRUE(ParseControl(R"({"name":"1e999 \\ 2e999"})").has_value());
}

TEST(PeerData, Whitelists)
{
  // P+'s CSS: Mario 0x00, Fox 0x07, Falco 0x15, Sonic 0x2B, Roy 0x32, Mewtwo 0x33,
  // Knuckles 0x35, solo Popo 0x11.
  for (u32 k : {0x00u, 0x07u, 0x11u, 0x15u, 0x2Bu, 0x32u, 0x33u, 0x35u})
    EXPECT_TRUE(ValidCharKind(k)) << k;
  // Giga Bowser and Wario-Man (not played online), Nana alone, alloys, bosses, "none",
  // Pokemon Trainer, garbage.
  for (u32 k : {0x2Cu, 0x2Du, 0x12u, 0x2Eu, 0x31u, 0x34u, 0x36u, 0x3Cu, 0x3Eu, 0x48u, 0xFFu, 0x100u})
    EXPECT_FALSE(ValidCharKind(k)) << k;

  EXPECT_TRUE(ValidCostume(0));
  EXPECT_TRUE(ValidCostume(MAX_COSTUME));
  EXPECT_FALSE(ValidCostume(MAX_COSTUME + 1));
  EXPECT_FALSE(ValidCostume(0xFF));

  // P+'s legal list, the rest of Brawl's versus stages, and P+'s own stages (all on P+ v3.2's
  // stage select, read from its tables).
  for (u32 k : {0x01u, 0x02u, 0x03u, 0x0Cu, 0x1Cu, 0x21u, 0x2Du, 0x2Eu, 0x33u, 0x37u, 0x40u, 0x41u,
                0x52u})
    EXPECT_TRUE(ValidStageKind(k)) << k;
  // None, Config, Result, Home-Run Contest, Target Smash, Subspace, "no stage", garbage.
  for (u32 k : {0x00u, 0x26u, 0x28u, 0x34u, 0x38u, 0x3Du, 0x80u, 0xFFFFu, 0x7FFFu})
    EXPECT_FALSE(ValidStageKind(k)) << k;
}

TEST(PeerData, InitBlockTakesOnlyTheStageVariant)
{
  // A real pair (Smashville, the host's lighting 06 against the joiner's 03).
  const auto mine = *ParseHex("002a81088000000000000000000000ff780000210300000000001c2000000000", 0x20);
  const auto host = *ParseHex("002a81088000000000000000000000ff780000210600000000001c2000000000", 0x20);
  bool rejected = true;
  const auto merged = MergeInitBlock(mine, host, &rejected);
  EXPECT_FALSE(rejected);
  EXPECT_EQ(merged, host);

  // The host trying another stage kind (+0x12), rules or time limit: ours is kept.
  for (size_t off : {size_t{0x00}, size_t{0x0B}, size_t{0x12}, size_t{0x13}, size_t{0x18}, size_t{0x1F}})
  {
    auto evil = host;
    evil[off] ^= 0x55;
    const auto m = MergeInitBlock(mine, evil, &rejected);
    EXPECT_TRUE(rejected) << off;
    EXPECT_EQ(m, mine) << off;
  }
  // A variant out of range.
  auto evil = host;
  evil[INIT_BLOCK_VARIANT] = 0xFF;
  EXPECT_EQ(MergeInitBlock(mine, evil, &rejected), mine);
  EXPECT_TRUE(rejected);
  // Wrong sizes.
  EXPECT_EQ(MergeInitBlock(mine, std::vector<u8>(0x40, 0), &rejected), mine);
  EXPECT_TRUE(rejected);
  EXPECT_EQ(MergeInitBlock(mine, {}, &rejected), mine);
  EXPECT_FALSE(rejected);
}

TEST(PeerData, StartPointsMustBeAShuffle)
{
  EXPECT_TRUE(IsPermutation({0, 1, 2, 3}, {3, 1, 0, 2}));
  EXPECT_TRUE(IsPermutation({0, 1, 2, 3}, {0, 1, 2, 3}));
  EXPECT_FALSE(IsPermutation({0, 1, 2, 3}, {0, 1, 2, 2}));
  EXPECT_FALSE(IsPermutation({0, 1, 2, 3}, {0, 1, 2, 0x7FFFFFFF}));
}

namespace
{
void PutFloatBE(u8* p, float f)
{
  u32 bits;
  std::memcpy(&bits, &f, 4);
  p[0] = static_cast<u8>(bits >> 24);
  p[1] = static_cast<u8>(bits >> 16);
  p[2] = static_cast<u8>(bits >> 8);
  p[3] = static_cast<u8>(bits);
}
}  // namespace

TEST(PeerData, PadSanitising)
{
  std::array<u8, PAD_SIZE> pad;
  // A legitimate pad passes unchanged.
  pad.fill(0);
  pad[0x01] = 0x11;          // buttons
  pad[0x30] = 0x7F;          // stick x
  pad[0x38] = 0xFF;          // NO_CONTROLLER
  pad[0x39] = pad[0x3A] = pad[0x3B] = 0xCC;
  pad[0x3C] = 3;             // Nunchuk
  PutFloatBE(&pad[0x18], 0.5f);
  auto copy = pad;
  SanitizePad(copy.data());
  EXPECT_EQ(copy, pad);

  // Hostile values.
  pad[0x3C] = 0xFE;
  pad[0x38] = 0x42;
  PutFloatBE(&pad[0x18], std::numeric_limits<float>::quiet_NaN());
  PutFloatBE(&pad[0x1C], std::numeric_limits<float>::infinity());
  PutFloatBE(&pad[0x2C], 3e30f);
  SanitizePad(pad.data());
  EXPECT_EQ(pad[0x3C], 0);
  EXPECT_EQ(pad[0x38], 0);
  for (size_t off : {size_t{0x18}, size_t{0x1C}, size_t{0x2C}})
    EXPECT_EQ(pad[off] | pad[off + 1] | pad[off + 2] | pad[off + 3], 0) << off;
  EXPECT_EQ(pad[0x01], 0x11);
  EXPECT_EQ(pad[0x30], 0x7F);
}

TEST(PeerData, PortValuesAndNames)
{
  std::array<u8, 0x3C> pv;
  pv.fill(0xFF);
  SanitizePortValues(pv);
  EXPECT_EQ(pv[0], 0x01);
  EXPECT_EQ(pv[1], 0x00);
  EXPECT_EQ(pv[0x39] | pv[0x3A] | pv[0x3B], 0);
  EXPECT_EQ(pv[0x10], 0xFF);  // the layout is the game's to sanitise

  EXPECT_EQ(TruncateName("abc"), "abc");
  EXPECT_EQ(TruncateName(std::string(1000, 'x')).size(), MAX_NAME_LEN);
  // A multi-byte character across the limit is dropped whole.
  std::string s(MAX_NAME_LEN - 1, 'a');
  s += "\xC3\xA9";
  EXPECT_EQ(TruncateName(s), std::string(MAX_NAME_LEN - 1, 'a'));
}

TEST(PeerData, LeaveReasons)
{
  // A peer's "why" goes into our logs and status: printable ASCII, bounded.
  EXPECT_EQ(LeaveReason("barrier timed out"), "barrier timed out");
  EXPECT_EQ(LeaveReason(""), "");
  EXPECT_EQ(LeaveReason("a\nb\x1b[31m\xc3\xa9"), "a?b?[31m??");
  EXPECT_EQ(LeaveReason(std::string(1000, 'x')).size(), MAX_LEAVE_REASON_LEN);
}

TEST(PeerData, TeamsAndPorts)
{
  for (u32 t : {0u, 1u, 2u, 0xFFu})
    EXPECT_TRUE(ValidTeam(t)) << t;
  for (u32 t : {3u, 4u, 0x80u, 0xFEu, 0x100u})
    EXPECT_FALSE(ValidTeam(t)) << t;
  for (u64 p : {0ull, 1ull, 2ull, 3ull})
    EXPECT_TRUE(ValidPort(p));
  for (u64 p : {4ull, 0xFFull, ~0ull})
    EXPECT_FALSE(ValidPort(p));
}

TEST(PeerData, GoneMarker)
{
  // GekkoNet's input for a player who left: the marker, which reads as a neutral controller.
  std::array<u8, PAD_SIZE> gone;
  gone.fill(0xAA);
  SetGoneMarker(gone.data());
  std::array<u8, PAD_SIZE> pad = gone;
  SanitizePad(pad.data());  // every input is sanitised first: the marker survives it
  ASSERT_TRUE(TakeGoneMarker(pad.data()));
  for (u8 b : pad)
    EXPECT_EQ(b, 0);
  EXPECT_FALSE(TakeGoneMarker(pad.data()));

  // Real input: the marker bytes are cleared before it is sent, so it is never taken for gone.
  std::array<u8, PAD_SIZE> real{};
  real[0] = 0x12;
  std::memcpy(real.data() + GONE_MARK_OFFSET, GONE_MARK.data(), GONE_MARK.size());
  ClearGoneMarkerBytes(real.data());
  EXPECT_FALSE(TakeGoneMarker(real.data()));
  EXPECT_EQ(real[0], 0x12);
  // Two of the three bytes are not the marker either.
  real[GONE_MARK_OFFSET] = GONE_MARK[0];
  real[GONE_MARK_OFFSET + 1] = GONE_MARK[1];
  EXPECT_FALSE(TakeGoneMarker(real.data()));
}
TEST(PeerData, SmallControlMessageIsOneDatagram)
{
  const std::string text = R"({"t":"st","v":1})";
  const auto d = ControlDatagrams(text, 7);
  ASSERT_EQ(d.size(), 1u);
  EXPECT_EQ(d[0][0], 'C');
  EXPECT_EQ(std::string(d[0].begin() + 1, d[0].end()), text);
  const std::string fits(MAX_DATAGRAM - 1, 'x');
  EXPECT_EQ(ControlDatagrams(fits, 0).size(), 1u);
}

TEST(PeerData, LargeControlMessageIsSplitUnderTheMtuAndPutBackTogether)
{
  // The host's state at the barrier (sync block and task order, about 5.5 KB): as one datagram it went
  // as IP fragments, which some paths drop.
  std::string text;
  for (int i = 0; text.size() < 4700; ++i)
    text += "task" + std::to_string(i) + ",";
  const auto d = ControlDatagrams(text, 0x12345678);
  ASSERT_EQ(d.size(), (text.size() + FRAGMENT_PIECE - 1) / FRAGMENT_PIECE);
  for (const auto& pkt : d)
  {
    EXPECT_EQ(pkt[0], 'F');
    EXPECT_LE(pkt.size(), MAX_DATAGRAM);
    EXPECT_LE(pkt.size(), 1472u);  // unfragmented on a 1500-byte MTU
  }
  // In any order (here: last piece first), with duplicates: the message once, when its last
  // missing piece arrives.
  ControlReassembler r;
  int done = 0;
  std::string got;
  for (size_t k = d.size(); k-- > 0;)
  {
    for (int copy = 0; copy < 2; ++copy)
    {
      if (auto t = r.Add(1, d[k].data(), d[k].size()))
      {
        ++done;
        got = *t;
      }
    }
  }
  EXPECT_EQ(done, 1);
  EXPECT_EQ(got, text);
  // The completing piece's duplicate starts the message over (until a newer message replaces it).
  EXPECT_EQ(r.InProgress(), 1u);
}

TEST(PeerData, ReassemblyKeepsSourcesAndMessagesApart)
{
  const std::string a(3000, 'a'), b(3000, 'b');
  const auto da = ControlDatagrams(a, 1), db = ControlDatagrams(b, 2);
  ASSERT_EQ(da.size(), db.size());
  ControlReassembler r;
  // Two sources at once.
  for (size_t i = 0; i + 1 < da.size(); ++i)
  {
    EXPECT_FALSE(r.Add(10, da[i].data(), da[i].size()));
    EXPECT_FALSE(r.Add(11, db[i].data(), db[i].size()));
  }
  EXPECT_EQ(r.Add(10, da.back().data(), da.back().size()), a);
  EXPECT_EQ(r.Add(11, db.back().data(), db.back().size()), b);
  // A piece of a newer message from the same source starts over: the older one's pieces are
  // never mixed in.
  EXPECT_FALSE(r.Add(10, da[0].data(), da[0].size()));
  for (size_t i = 1; i + 1 < db.size(); ++i)
    EXPECT_FALSE(r.Add(10, db[i].data(), db[i].size()));
  EXPECT_FALSE(r.Add(10, db.back().data(), db.back().size()));  // db[0] missing
  EXPECT_EQ(r.Add(10, db[0].data(), db[0].size()), b);
  // Memory is bounded: at most MAX_REASSEMBLY_SOURCES messages in progress.
  for (u64 src = 100; src < 100 + 3 * MAX_REASSEMBLY_SOURCES; ++src)
    r.Add(src, da[0].data(), da[0].size());
  EXPECT_LE(r.InProgress(), MAX_REASSEMBLY_SOURCES);
}

TEST(PeerData, MalformedPiecesAreDropped)
{
  const std::string text(3000, 'x');
  auto d = ControlDatagrams(text, 5);
  ControlReassembler r;
  auto bad = [&](std::vector<u8> pkt) { return !r.Add(1, pkt.data(), pkt.size()).has_value(); };
  EXPECT_TRUE(bad({}));
  EXPECT_TRUE(bad({'F', 0, 0, 0, 0, 0, 2}));  // no piece
  auto p = d[0];
  p[0] = 'C';
  EXPECT_TRUE(bad(p));
  p = d[0];
  p[6] = 1;  // one piece is a 'C' packet's job
  EXPECT_TRUE(bad(p));
  p = d[0];
  p[6] = static_cast<u8>(MAX_FRAGMENTS + 1);
  EXPECT_TRUE(bad(p));
  p = d[0];
  p[5] = p[6];  // index out of range
  EXPECT_TRUE(bad(p));
  p = d[0];
  p.pop_back();  // a short piece that is not the last
  EXPECT_TRUE(bad(p));
  p = d.back();
  p.resize(MAX_DATAGRAM + 1, 'x');  // too large
  EXPECT_TRUE(bad(p));
  EXPECT_EQ(r.InProgress(), 0u);
  // Too large a message is not split: the others refuse it.
  EXPECT_TRUE(ControlDatagrams(std::string(MAX_CONTROL_SIZE + FRAGMENT_PIECE, 'x'), 0).empty());
  // The real pieces still work afterwards.
  std::optional<std::string> got;
  for (const auto& pkt : d)
    got = r.Add(1, pkt.data(), pkt.size());
  EXPECT_EQ(got, text);
}

TEST(PeerData, GekkoBundlesRoundTrip)
{
  std::vector<u8> bundle;
  const std::vector<u8> ack(9, 0x03), inputs(24, 0x01), health(12, 0x06);
  ASSERT_TRUE(AppendGekkoBundle(bundle, ack.data(), ack.size()));
  ASSERT_TRUE(AppendGekkoBundle(bundle, inputs.data(), inputs.size()));
  ASSERT_TRUE(AppendGekkoBundle(bundle, health.data(), health.size()));
  EXPECT_EQ(bundle[0], 'B');
  EXPECT_EQ(bundle.size(), 1 + 3 * 2 + ack.size() + inputs.size() + health.size());
  const auto msgs = SplitGekkoBundle(bundle.data(), bundle.size());
  ASSERT_TRUE(msgs.has_value());
  ASSERT_EQ(msgs->size(), 3u);
  EXPECT_EQ((*msgs)[0], ack);
  EXPECT_EQ((*msgs)[1], inputs);
  EXPECT_EQ((*msgs)[2], health);
}

TEST(PeerData, GekkoBundlesStayUnderTheMtu)
{
  std::vector<u8> bundle;
  const std::vector<u8> big(MAX_GEKKO_BUNDLE - 1 - 2, 0x01);
  ASSERT_TRUE(AppendGekkoBundle(bundle, big.data(), big.size()));
  EXPECT_EQ(bundle.size(), MAX_GEKKO_BUNDLE);
  const u8 one = 0;
  const auto before = bundle;
  EXPECT_FALSE(AppendGekkoBundle(bundle, &one, 1));
  EXPECT_EQ(bundle, before);
  std::vector<u8> fresh;
  const std::vector<u8> too_big(MAX_GEKKO_BUNDLE, 0x01);
  EXPECT_FALSE(AppendGekkoBundle(fresh, too_big.data(), too_big.size()));
  EXPECT_TRUE(fresh.empty());
  EXPECT_FALSE(AppendGekkoBundle(fresh, &one, 0));

  std::vector<u8> many;
  for (size_t i = 0; i < MAX_BUNDLED_MESSAGES; ++i)
    ASSERT_TRUE(AppendGekkoBundle(many, &one, 1));
  EXPECT_FALSE(AppendGekkoBundle(many, &one, 1));
}

TEST(PeerData, MalformedGekkoBundlesAreDropped)
{
  std::vector<u8> good;
  const std::vector<u8> msg(5, 0x07);
  ASSERT_TRUE(AppendGekkoBundle(good, msg.data(), msg.size()));
  ASSERT_TRUE(AppendGekkoBundle(good, msg.data(), msg.size()));
  ASSERT_TRUE(SplitGekkoBundle(good.data(), good.size()).has_value());
  // Cut anywhere inside: a length past the end (or half a length).
  for (size_t n = 2; n < good.size(); ++n)
  {
    if (n != 1 + 2 + msg.size())  // there the first message ends: a valid bundle of one
      EXPECT_FALSE(SplitGekkoBundle(good.data(), n).has_value()) << n;
  }
  EXPECT_FALSE(SplitGekkoBundle(good.data(), 1).has_value());  // no message
  EXPECT_FALSE(SplitGekkoBundle(nullptr, 0).has_value());
  auto zero = good;
  zero[1] = zero[2] = 0;  // a zero length
  EXPECT_FALSE(SplitGekkoBundle(zero.data(), zero.size()).has_value());
  auto huge = good;
  huge[1] = huge[2] = 0xFF;
  EXPECT_FALSE(SplitGekkoBundle(huge.data(), huge.size()).has_value());
  auto other = good;
  other[0] = 'G';
  EXPECT_FALSE(SplitGekkoBundle(other.data(), other.size()).has_value());
  // More messages than any sender bundles.
  std::vector<u8> flood{'B'};
  for (size_t i = 0; i <= MAX_BUNDLED_MESSAGES; ++i)
  {
    flood.push_back(1);
    flood.push_back(0);
    flood.push_back(0x42);
  }
  EXPECT_FALSE(SplitGekkoBundle(flood.data(), flood.size()).has_value());
}
