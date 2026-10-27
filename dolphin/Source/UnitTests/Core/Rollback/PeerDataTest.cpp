// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
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
  // P+'s alts: one of AltZ 0x40 / AltR 0x80 over a colour number, never both.
  EXPECT_TRUE(ValidCostume(COSTUME_ALT_Z));
  EXPECT_TRUE(ValidCostume(COSTUME_ALT_R | MAX_COSTUME));
  EXPECT_FALSE(ValidCostume(COSTUME_ALT_Z | COSTUME_ALT_R));
  EXPECT_FALSE(ValidCostume(COSTUME_ALT_Z | (MAX_COSTUME + 1)));
  EXPECT_FALSE(ValidCostume(0x20 | COSTUME_ALT_R));

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

namespace
{
// Costume lists as P+ v3.2 has them (0x80585B00 + slot * 0x10, +8; read live 2026-10-15): per
// costume its colour and file number. The team colours: red 0, blue 1, green 3.
std::vector<CostumeEntry> List(std::string_view hex)
{
  std::vector<CostumeEntry> out;
  const auto bytes = *ParseHex(hex, hex.size() / 2);
  for (size_t i = 0; i + 1 < bytes.size(); i += 2)
    out.push_back({bytes[i], bytes[i + 1]});
  return out;
}
const auto KIRBY = List("0600020401030001030209050406070706140015031601170818061e001f032001210722");
const auto GANONDORF = List("040008040103030200010205071400150316011704180b19051e081f0120032100220423");
const auto FALCON = List("05000804000103020103060502060414011e001f03200721082201280029032a072b052c");
const auto BOWSER = List("030008040001010309050a06000703080109050a09140015031601170718");
const auto KNUCKLES = List("00000301010209030409010a030b000c00140315011607170818001e031f012002210622");
constexpr std::array<u8, 3> TEAM_COLOURS{0x00, 0x01, 0x03};

LegacyCostumePlayer Player(const std::vector<CostumeEntry>& list, u8 costume, u8 team, u8 file)
{
  return {true, costume, team, file, {list}};
}
}  // namespace

TEST(PeerData, LegacyTeamCostume)
{
  // Kirby's own costume 0 is pink: red gets the first red one (3), blue the first blue (2).
  EXPECT_EQ(LegacyTeamCostume(KIRBY, 0x00, 0), 3u);
  EXPECT_EQ(LegacyTeamCostume(KIRBY, 0x01, 0), 2u);
  // A costume that has the team's colour already stays (the player's own shade of it): Kirby 9
  // is a second red.
  EXPECT_EQ(LegacyTeamCostume(KIRBY, 0x00, 9), 9u);
  // No costume of that colour: the player's own.
  EXPECT_EQ(LegacyTeamCostume(KIRBY, 0x0B, 4), 4u);
  EXPECT_EQ(LegacyTeamCostume({}, 0x00, 4), 4u);
}

TEST(PeerData, OldTeamBattleCostumesAreRestored)
{
  // "2026-10-12 2239 Room G1" (v0.1.65): Kirby (red) + Captain Falcon (red) vs Ganondorf + Ganondorf
  // (green). The recorded SESSION has the costumes locked in (0, 1, 0, 0), the recorded setup key
  // the files the old plugin's team costumes load (1, 2, 2, 1): both Ganondorfs in his first green.
  std::array<LegacyCostumePlayer, MAX_PORTS> g1{Player(KIRBY, 0, 0, 0x01), Player(GANONDORF, 1, 2, 0x02),
                                                Player(GANONDORF, 0, 2, 0x02), Player(FALCON, 0, 0, 0x01)};
  const auto picks = LegacyTeamCostumes(g1, TEAM_COLOURS);
  ASSERT_TRUE(picks.has_value());
  ASSERT_TRUE((*picks)[0] && (*picks)[1] && (*picks)[2] && (*picks)[3]);
  EXPECT_EQ((*picks)[0]->color_no, 3);
  EXPECT_EQ((*picks)[0]->file, 0x01);
  EXPECT_EQ((*picks)[1]->color_no, 3);
  EXPECT_EQ((*picks)[1]->file, 0x02);
  EXPECT_EQ((*picks)[2]->color_no, 3);
  EXPECT_EQ((*picks)[3]->color_no, 2);
  EXPECT_EQ((*picks)[3]->file, 0x01);

  // "2026-10-12 2248 Room G3": Bowser (blue), Knuckles (red), Ganondorf (blue), Captain Falcon (red),
  // every costume 0 in SESSION; files 3, 0, 3, 1. Knuckles' costume 0 is red already.
  std::array<LegacyCostumePlayer, MAX_PORTS> g3{Player(BOWSER, 0, 1, 0x03), Player(KNUCKLES, 0, 0, 0x00),
                                                Player(GANONDORF, 0, 1, 0x03), Player(FALCON, 0, 0, 0x01)};
  const auto p3 = LegacyTeamCostumes(g3, TEAM_COLOURS);
  ASSERT_TRUE(p3.has_value());
  EXPECT_EQ((*p3)[0]->color_no, 3);
  EXPECT_EQ((*p3)[1]->color_no, 0);
  EXPECT_EQ((*p3)[1]->file, 0x00);
  EXPECT_EQ((*p3)[2]->color_no, 2);

  // An empty port is skipped; a CSS slot list that doesn't give the file is passed over for one
  // that does.
  auto gap = g1;
  gap[2] = {};
  gap[0].lists.insert(gap[0].lists.begin(), GANONDORF);
  const auto pg = LegacyTeamCostumes(gap, TEAM_COLOURS);
  ASSERT_TRUE(pg.has_value());
  EXPECT_FALSE((*pg)[2].has_value());
  EXPECT_EQ((*pg)[0]->file, 0x01);
}

TEST(PeerData, OtherCostumesAreNotTheOldTeamCostumes)
{
  std::array<LegacyCostumePlayer, MAX_PORTS> g1{Player(KIRBY, 0, 0, 0x01), Player(GANONDORF, 1, 2, 0x02),
                                                Player(GANONDORF, 0, 2, 0x02), Player(FALCON, 0, 0, 0x01)};
  // One recorded costume the old rule does not give (a recording from another build, or a file
  // made up): nothing is restored.
  auto other = g1;
  other[3].recorded_file = 0x00;
  EXPECT_FALSE(LegacyTeamCostumes(other, TEAM_COLOURS).has_value());
  // A team battle of today's builds, Kirby in his own pink costume on red: not the old rule's.
  auto today = g1;
  today[0].recorded_file = 0x00;
  EXPECT_FALSE(LegacyTeamCostumes(today, TEAM_COLOURS).has_value());
  // A costume with P+'s alt bits, a team out of range, no costume list, or no player at all.
  auto alt = g1;
  alt[0].costume = static_cast<u8>(COSTUME_ALT_Z);
  EXPECT_FALSE(LegacyTeamCostumes(alt, TEAM_COLOURS).has_value());
  auto team = g1;
  team[1].team = 3;
  EXPECT_FALSE(LegacyTeamCostumes(team, TEAM_COLOURS).has_value());
  auto nolist = g1;
  nolist[1].lists.clear();
  EXPECT_FALSE(LegacyTeamCostumes(nolist, TEAM_COLOURS).has_value());
  EXPECT_FALSE(LegacyTeamCostumes({}, TEAM_COLOURS).has_value());
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
  pad[0x3F] = 3;             // controller type (u32): Nunchuk
  PutFloatBE(&pad[0x18], 0.5f);
  auto copy = pad;
  SanitizePad(copy.data());
  EXPECT_EQ(copy, pad);

  // Every Wii type passes; one past them is a GameCube controller.
  for (u8 type : {u8{1}, u8{2}})
  {
    copy = pad;
    copy[0x3F] = type;
    SanitizePad(copy.data());
    EXPECT_EQ(copy[0x3F], type);
  }
  copy = pad;
  copy[0x3F] = 4;
  SanitizePad(copy.data());
  EXPECT_EQ(copy[0x3F], 0);

  // Hostile values.
  pad[0x3C] = 0xFE;
  pad[0x38] = 0x42;
  PutFloatBE(&pad[0x18], std::numeric_limits<float>::quiet_NaN());
  PutFloatBE(&pad[0x1C], std::numeric_limits<float>::infinity());
  PutFloatBE(&pad[0x2C], 3e30f);
  SanitizePad(pad.data());
  EXPECT_EQ(pad[0x3C] | pad[0x3D] | pad[0x3E] | pad[0x3F], 0);
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
  EXPECT_EQ(pv[0x39] | pv[0x3A], 0);
  EXPECT_EQ(pv[0x3B], 0x01);  // the controller: only its Wii bit
  EXPECT_EQ(pv[0x10], 0xFF);  // the layout is the game's to sanitise
  // The stock count picked on the STOCKS box (stock control) is kept up to 99.
  pv[0x39] = 3;
  SanitizePortValues(pv);
  EXPECT_EQ(pv[0x39], 3);
  pv[0x39] = MAX_PICKED_STOCKS;
  SanitizePortValues(pv);
  EXPECT_EQ(pv[0x39], 99);
  pv[0x39] = 100;
  SanitizePortValues(pv);
  EXPECT_EQ(pv[0x39], 0);
  // P+'s slot colour picked on the CSS (1-13, 0 none) is kept; of the controller byte only its
  // Wii bit.
  pv[0x3A] = 1;
  pv[0x3B] = 0xFE;
  SanitizePortValues(pv);
  EXPECT_EQ(pv[0x3A], 1);
  EXPECT_EQ(pv[0x3B], 0);
  pv[0x3A] = MAX_SLOT_COLOUR;
  SanitizePortValues(pv);
  EXPECT_EQ(pv[0x3A], 13);
  pv[0x3A] = 14;
  SanitizePortValues(pv);
  EXPECT_EQ(pv[0x3A], 0);

  EXPECT_EQ(TruncateName("abc"), "abc");
  EXPECT_EQ(TruncateName(std::string(1000, 'x')).size(), MAX_NAME_LEN);
  // A multi-byte character across the limit is dropped whole.
  std::string s(MAX_NAME_LEN - 1, 'a');
  s += "\xC3\xA9";
  EXPECT_EQ(TruncateName(s), std::string(MAX_NAME_LEN - 1, 'a'));
}

// A peer's special-mode picks (docs/design/special-rules.md): the HP up to 999, the All-Star roster
// up to its first entry the game could not load, every other byte zero.
TEST(PeerData, PortExtra)
{
  std::array<u8, 0x48> px;
  px.fill(0xFF);
  SanitizePortExtra(px);
  EXPECT_EQ(px, (std::array<u8, 0x48>{}));
  px[PX_HP] = 0x01;
  px[PX_HP + 1] = 0x2C;  // 300
  px[PX_ROSTER_COUNT] = 3;
  px[3] = 0x77;           // padding
  const u8 roster[] = {0x07, 0x03, 0x02, 0x07,   // Fox, P+'s colour 3 for costume 2
                       0x14, 0x3D, 0x00, 0x15,   // Falco, P+'s AltZ
                       0x00, 0x00, 0x20, 0x00};  // a costume past the last: the roster ends
  std::copy(std::begin(roster), std::end(roster), px.begin() + PX_ROSTER);
  SanitizePortExtra(px);
  EXPECT_EQ((px[PX_HP] << 8) | px[PX_HP + 1], 300);
  EXPECT_EQ(px[3], 0);
  EXPECT_EQ(px[PX_ROSTER_COUNT], 2);
  const u8 kept[] = {0x07, 0x03, 0x02, 0x07, 0x14, 0x3D, 0x00, 0x15, 0, 0, 0, 0};
  EXPECT_TRUE(std::equal(std::begin(kept), std::end(kept), px.begin() + PX_ROSTER));
  // Too many HP is none; a count past ROSTER_MAX is cut there.
  px[PX_HP] = 0x03;
  px[PX_HP + 1] = 0xE8;  // 1000
  px[PX_ROSTER_COUNT] = 200;
  for (size_t i = 0; i < ROSTER_MAX; ++i)
  {
    px[PX_ROSTER + i * PX_ENTRY] = 0x07;
    px[PX_ROSTER + i * PX_ENTRY + 1] = 0;
    px[PX_ROSTER + i * PX_ENTRY + 2] = 0;
  }
  SanitizePortExtra(px);
  EXPECT_EQ(px[PX_HP] | px[PX_HP + 1], 0);
  EXPECT_EQ(px[PX_ROSTER_COUNT], ROSTER_MAX);
  // A character the game has no fighter for ends it, and a CSS id past the CSS's.
  px[PX_ROSTER + 3 * PX_ENTRY] = 0xFE;
  SanitizePortExtra(px);
  EXPECT_EQ(px[PX_ROSTER_COUNT], 3);
  px[PX_ROSTER_COUNT] = 3;
  px[PX_ROSTER + 1 * PX_ENTRY + 3] = 0x40;
  SanitizePortExtra(px);
  EXPECT_EQ(px[PX_ROSTER_COUNT], 1);
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

// A real setup key (GameplaySession SetupKey; bug report #40: KLON#285's game against SunnyPM,
// P2 of a 1v1, no teams, no stock control).
const char* const KEY_40 =
    "002a81088020000000000000000000ff7800002103000000000070800000000014000400220004153e0300003e030000"
    "0303040a0b0c00010c020b000303040a0b0c0001050a02800303040a0b0c0001050202800303040a0b0c000105020280"
    "0001ffff0000010004000a0008010000000000002000000000000000007fffffff7fffffff0000000000000000000000"
    "000000000000000001000000000000000000000000000000020000000000000000000000000000000000000000000000"
    "0000000000000000010000000000000000000000000000000000000000000000003f0000000000000000000000000000"
    "00000000000000000000000000000000003f000000000000000000000000000000000000000000000000000000000000"
    "003f000000000000000000000000000000000000000000000000000000000000003f0000000000000000000000000000"
    "0000000000000000000000000100000002000000033ecccccd3eaaaaab41f000003fc0000040c00000404000003f6666"
    "663e19999a3f800000000000003f8000003f0000003f4ccccd3d50e56000000000000000000000000000000003000000"
    "0100000e100000000040000000400000003f8000003f800000";

static std::vector<u8> Key40()
{
  const std::string_view hex(KEY_40);
  return *ParseHex(hex, hex.size() / 2);
}

TEST(PeerData, SetupKeyDiffNamesTheFields)
{
  const auto mine = Key40();
  ASSERT_EQ(mine.size(), 457u);
  EXPECT_TRUE(SetupKeyDiff(mine, mine).empty());

  // A player's costume, a controller's C-stick and a Code Menu value (the key's +0x85 on).
  auto theirs = mine;
  theirs[0x23] = 0x17;
  theirs[0x30 + 8] = 0x05;
  theirs[0x85 + 2 * 4 + 3] = 0x01;
  EXPECT_EQ(SetupKeyDiff(mine, theirs),
            (std::vector<std::string>{
                "P1 costume 00, peer 17", "pad 1 C-stick 0c, peer 05",
                "code menu value 2 (or a restart's restore) 00000000, peer 00000001"}));

  // The stage (init block +0x13) and the set rule.
  theirs = mine;
  theirs[0x13] = 0x04;
  theirs[0x64 + 2] = 0x02;
  EXPECT_EQ(SetupKeyDiff(mine, theirs),
            (std::vector<std::string>{"init block +0x13 21, peer 04",
                                      "set rule +0x02 01, peer 02"}));
}

TEST(PeerData, SetupKeyDiffFollowsTheKeysLayout)
{
  // A team battle: every player has a fifth byte, so everything after the players moves by 4.
  auto mine = Key40();
  mine[0x0B] = 1;
  for (int p = 3; p >= 0; --p)
    mine.insert(mine.begin() + 0x20 + 4 * p + 4, static_cast<u8>(p & 1));
  auto theirs = mine;
  theirs[0x20 + 5 + 4] = 0;           // P2 team
  theirs[0x34 + 0x0C + 0x0B] = 0x00;  // pad 2 flags (tap jump off)
  EXPECT_EQ(SetupKeyDiff(mine, theirs),
            (std::vector<std::string>{"P2 team 01, peer 00", "pad 2 flags 80, peer 00"}));

  // Stock control (set rule +5 = 2): each player's handicap follows the set rule.
  mine = Key40();
  mine[0x64 + 5] = 2;
  const u8 handicaps[8] = {0, 30, 0, 20, 0, 0, 0, 0};
  mine.insert(mine.begin() + 0x74, handicaps, handicaps + 8);
  theirs = mine;
  theirs[0x74 + 3] = 0;
  theirs[0x74 + 8] = 0x00;  // hazards (on)
  EXPECT_EQ(SetupKeyDiff(mine, theirs),
            (std::vector<std::string>{"P2 stock control handicap 0014, peer 0000",
                                      "hazards 20, peer 00"}));

  // The peer's key is shorter, or ours.
  mine = Key40();
  theirs = mine;
  theirs.resize(theirs.size() - 4);
  EXPECT_EQ(SetupKeyDiff(mine, theirs),
            (std::vector<std::string>{"length 457 bytes, peer 453",
                                      "code menu value 80 (or a restart's restore) 3f800000, peer none"}));
  EXPECT_EQ(SetupKeyDiff(theirs, mine).front(), "length 453 bytes, peer 457");

  // At most max_lines, then how many more.
  theirs = mine;
  for (size_t i = 0; i < 0x10; ++i)
    theirs[0x75 + i] = 0xFF;  // the item switch (10 of its bytes change)
  const auto lines = SetupKeyDiff(mine, theirs, 3);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[0], "item switch +0x00 00, peer ff");
  EXPECT_EQ(lines[3], "and 7 more");
}

TEST(PeerData, SetupKeyCodeMenuAtFollowsTheKeysLayout)
{
  auto key = Key40();
  EXPECT_EQ(SetupKeyCodeMenuAt(key), 0x85u);
  // A team battle: a fifth byte per player.
  key[0x0B] = 1;
  EXPECT_EQ(SetupKeyCodeMenuAt(key), 0x89u);
  // Stock control (set rule +5 = 2): each player's handicap after the set rule.
  key = Key40();
  key[0x64 + 5] = 2;
  EXPECT_EQ(SetupKeyCodeMenuAt(key), 0x8Du);
  // Too short for the set rule, or for the item switch.
  key = Key40();
  key.resize(0x70);
  EXPECT_EQ(SetupKeyCodeMenuAt(key), std::nullopt);
  key = Key40();
  key.resize(0x84);
  EXPECT_EQ(SetupKeyCodeMenuAt(key), std::nullopt);
  key.resize(0x85);
  EXPECT_EQ(SetupKeyCodeMenuAt(key), 0x85u);
}

TEST(PeerData, ReplayCodeMenuLinesByKind)
{
  using L = ReplayCodeMenuLine;
  EXPECT_EQ(ClassifyReplayCodeMenuLine("Autosave Replays:  ", false), L::Ours);
  EXPECT_EQ(ClassifyReplayCodeMenuLine("Code Menu Activation:  ", false), L::Ours);
  EXPECT_EQ(ClassifyReplayCodeMenuLine("Player 2:  ", true), L::Recorded);
  EXPECT_EQ(ClassifyReplayCodeMenuLine("Player 2:  ", false), L::Compared);
  // Gameplay, the debug displays and everything not known to stay out of the match: compared.
  for (const char* text : {"Hitstun Multiplier:  ", "Automatic L-Cancelling:  ", "Alternate Stages:  ",
                           "Debug Mode:  ", "Save Previous Replay:  ", "Autosave:  ", "HUD:  "})
  {
    EXPECT_EQ(ClassifyReplayCodeMenuLine(text, false), L::Compared) << text;
  }
}

// P+ v3.2's 81 value lines in key order (KEY_40's): Autosave Replays (4), Code Menu Activation (8),
// the slot colours Player 1-4 (52-55).
static std::vector<ReplayCodeMenuLine> PPlusLines()
{
  std::vector<ReplayCodeMenuLine> lines(81, ReplayCodeMenuLine::Compared);
  lines[4] = lines[8] = ReplayCodeMenuLine::Ours;
  for (size_t i = 52; i <= 55; ++i)
    lines[i] = ReplayCodeMenuLine::Recorded;
  return lines;
}

constexpr size_t CODE_MENU_AT = 0x85;

static void SetLine(std::vector<u8>& key, size_t line, u32 value)
{
  for (int b = 0; b < 4; ++b)
    key[CODE_MENU_AT + 4 * line + b] = static_cast<u8>(value >> (24 - 8 * b));
}

TEST(PeerData, ReplaySetupKeyKeepsOurLinesTheMatchDoesNotRead)
{
  // The 2026-10-15 Ranked replay (launcher v0.1.79): its recorder's plugin had Autosave Replays
  // ON (line 4 = 1), this one writes OFF. The rest of the setup is the same.
  const auto recorded = Key40();
  ASSERT_EQ(recorded.size(), CODE_MENU_AT + 81 * 4);
  auto here = recorded;
  SetLine(here, 4, 0);
  ASSERT_NE(here, recorded);
  EXPECT_EQ(ReplaySetupKey(here, recorded, PPlusLines()), here);

  // Also the activation and the slot colours (written in from the recording before the match;
  // compared or not, a colour never refuses a replay).
  SetLine(here, 8, 0);
  SetLine(here, 53, 7);
  EXPECT_EQ(ReplaySetupKey(here, recorded, PPlusLines()), here);

  // A gameplay line still refuses it: Hitstun Multiplier (56) and P2's Automatic L-Cancelling
  // (30). The diff names it.
  for (const size_t line : {56u, 30u})
  {
    auto other = here;
    SetLine(other, line, 1);
    const auto key = ReplaySetupKey(here, other, PPlusLines());
    EXPECT_NE(key, here) << line;
    EXPECT_EQ(SetupKeyDiff(here, key),
              (std::vector<std::string>{fmt::format(
                  "code menu value {} (or a restart's restore) {:08x}, peer 00000001", line,
                  line == 56 ? 0x3ecccccd : 0)}))
        << line;
  }

  // A restart's restore after the lines does not move them.
  auto here_restore = here, recorded_restore = recorded;
  for (const int b : {3, 0, 0, 0, 0, 2, 0, 0, 0, 0})
  {
    here_restore.push_back(static_cast<u8>(b));
    recorded_restore.push_back(static_cast<u8>(b));
  }
  EXPECT_EQ(ReplaySetupKey(here_restore, recorded_restore, PPlusLines()), here_restore);
}

TEST(PeerData, ReplaySetupKeyLeavesOtherLayoutsAlone)
{
  const auto recorded = Key40();
  auto here = recorded;
  SetLine(here, 4, 0);
  // Not as many lines in the keys as this game has: nothing is taken.
  auto lines = PPlusLines();
  lines.push_back(ReplayCodeMenuLine::Ours);
  EXPECT_EQ(ReplaySetupKey(here, recorded, lines), recorded);
  // The values start elsewhere in the recording (a team battle here, not there).
  auto teams = here;
  teams[0x0B] = 1;
  teams.insert(teams.begin() + 0x24, 0);
  EXPECT_EQ(ReplaySetupKey(teams, recorded, PPlusLines()), recorded);
  // Keys too short for any Code Menu.
  EXPECT_EQ(ReplaySetupKey({}, recorded, PPlusLines()), recorded);
  EXPECT_TRUE(ReplaySetupKey(here, {}, PPlusLines()).empty());
}

TEST(PeerData, RecordedCodeMenuValuesAreSlotColours)
{
  auto recorded = Key40();
  SetLine(recorded, 52, 7);
  SetLine(recorded, 53, 12);
  SetLine(recorded, 54, 13);          // not a colour
  SetLine(recorded, 55, 0x01000002);  // nor this
  const auto values = RecordedCodeMenuValues(recorded, PPlusLines());
  ASSERT_EQ(values.size(), 81u);
  EXPECT_EQ(values[52], 7u);
  EXPECT_EQ(values[53], 12u);
  EXPECT_EQ(values[54], std::nullopt);
  EXPECT_EQ(values[55], std::nullopt);
  // Only Recorded lines: Autosave Replays (1 in this key) is never written in.
  for (size_t i = 0; i < values.size(); ++i)
  {
    if (i < 52 || i > 55)
      EXPECT_EQ(values[i], std::nullopt) << i;
  }
  // A key without the lines.
  recorded.resize(CODE_MENU_AT + 80 * 4);
  for (const auto& v : RecordedCodeMenuValues(recorded, PPlusLines()))
    EXPECT_EQ(v, std::nullopt);
}
