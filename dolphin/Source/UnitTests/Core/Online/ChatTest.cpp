// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Online::Chat against hostile server messages and hostile members: everything mm could pass on
// from another player (docs/chat-protocol.md), from well-formed to garbage.

#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/Online/Chat.h"
#include "Core/Online/ChatCrypto.h"

namespace CC = Online::ChatCrypto;
namespace Chat = Online::Chat;

namespace
{
constexpr char GROUP[] = "room-KFQB-12";
constexpr char ME[] = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr char BOB[] = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
constexpr char CAROL[] = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

CC::Key32 Fill(u8 v)
{
  CC::Key32 k;
  k.fill(v);
  return k;
}

// Another player, as their own Dolphin would act.
struct Peer
{
  Peer(const char* uid_, u8 seed) : uid(uid_), id(Fill(seed)), kx(CC::GroupKeyFromSecret(Fill(seed + 100))) {}
  std::string uid;
  CC::Identity id;
  CC::GroupKey kx;
  u64 seq = 0;

  picojson::object Member(const std::string& group, const std::string& name) const
  {
    picojson::object m;
    m["uid"] = picojson::value(uid);
    m["displayName"] = picojson::value(name);
    m["connectCode"] = picojson::value(name.substr(0, 3) + "#1");
    m["idKey"] = picojson::value(CC::ToHex(id.PublicKey()));
    m["kx"] = picojson::value(CC::ToHex(kx.public_key));
    m["kxSig"] = picojson::value(CC::ToHex(id.Sign(CC::KxSigMessage(group, kx.public_key))));
    return m;
  }

  std::string Box(const std::string& group, const CC::Key32& to_kx, const std::string& to,
                  const std::string& text, std::optional<u64> force_seq = {})
  {
    const u64 n = force_seq ? *force_seq : ++seq;
    const CC::Sig64 sig = id.Sign(CC::MessageSigMessage(group, uid, n, text));
    const auto key = *CC::PairKey(kx, to_kx, group);
    return CC::ToHex(CC::Seal(key, CC::RandomNonce(), CC::AssociatedData(group, uid, to),
                              CC::EncodePlaintext(n, sig, text)));
  }
};

std::string Json(const picojson::object& o)
{
  return picojson::value(o).serialize();
}

picojson::object Status()
{
  return Chat::Status();
}

double Counter(const char* name)
{
  return Status()["counters"].get<picojson::object>()[name].get<double>();
}

std::vector<std::string> Texts()
{
  std::vector<std::string> out;
  for (const auto& l : Status()["lines"].get<picojson::array>())
    out.push_back(l.get<picojson::object>().at("text").get<std::string>());
  return out;
}

class ChatTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_dir = File::CreateTempDir();
    ASSERT_FALSE(m_dir.empty());
    File::SetUserPath(D_USER_IDX, m_dir + DIR_SEP);
    Chat::ResetForTesting(ME);
    m_my_id = Chat::IdentityPublicKeyHex();
    ASSERT_EQ(m_my_id.size(), 64u);
  }
  void TearDown() override
  {
    Chat::ResetForTesting(ME);
    File::DeleteDirRecursively(m_dir);
  }

  picojson::object MyMember(std::optional<CC::Key32> kx = {}, std::optional<CC::Sig64> sig = {})
  {
    picojson::object m;
    m["uid"] = picojson::value(ME);
    m["displayName"] = picojson::value("me");
    m["connectCode"] = picojson::value("ME#1");
    m["idKey"] = picojson::value(m_my_id);
    m["kx"] = kx ? picojson::value(CC::ToHex(*kx)) : picojson::value();
    m["kxSig"] = sig ? picojson::value(CC::ToHex(*sig)) : picojson::value();
    return m;
  }

  std::string Group(const picojson::array& members, const std::string& group = GROUP,
                    const std::string& kind = "room")
  {
    picojson::object o;
    o["type"] = picojson::value("chat-group");
    o["group"] = picojson::value(group);
    o["kind"] = picojson::value(kind);
    o["you"] = picojson::value(ME);
    o["members"] = picojson::value(members);
    return Json(o);
  }

  std::string Msg(const std::string& from, const std::string& box, const std::string& group = GROUP)
  {
    picojson::object o;
    o["type"] = picojson::value("chat-msg");
    o["group"] = picojson::value(group);
    o["from"] = picojson::value(from);
    o["box"] = picojson::value(box);
    return Json(o);
  }

  // Joins GROUP with `peers`, answers with this player's own group key as mm would echo it, and
  // returns that key.
  CC::Key32 Join(std::vector<Peer*> peers)
  {
    picojson::array members{picojson::value(MyMember())};
    for (Peer* p : peers)
      members.emplace_back(p->Member(GROUP, p->uid == BOB ? "bob" : "carol"));
    EXPECT_TRUE(Chat::OnServerMessage(Group(members)));
    std::optional<CC::Key32> kx;
    std::optional<CC::Sig64> sig;
    for (const std::string& json : Chat::TakeOutbox())
    {
      picojson::value v;
      EXPECT_TRUE(picojson::parse(v, json).empty());
      auto& o = v.get<picojson::object>();
      if (o["type"].get<std::string>() != "chat-key")
        continue;
      EXPECT_EQ(o["group"].get<std::string>(), GROUP);
      kx = CC::FromHexFixed<32>(o["kx"].get<std::string>());
      sig = CC::FromHexFixed<64>(o["sig"].get<std::string>());
    }
    EXPECT_TRUE(kx && sig);
    const auto id = CC::FromHexFixed<32>(m_my_id);
    EXPECT_TRUE(CC::Verify(*id, *sig, CC::KxSigMessage(GROUP, *kx)));
    members[0] = picojson::value(MyMember(kx, sig));
    EXPECT_TRUE(Chat::OnServerMessage(Group(members)));
    return *kx;
  }

  std::string m_dir;
  std::string m_my_id;
};
}  // namespace

TEST_F(ChatTest, IdentityIsKept)
{
  const std::string path = File::GetUserPath(D_ONLINE_IDX) + "chat-identity.key";
  ASSERT_TRUE(File::Exists(path));
  Chat::ResetForTesting(ME);
  EXPECT_EQ(Chat::IdentityPublicKeyHex(), m_my_id);  // read back, not made again
  // A damaged file is set aside and a new key made.
  ASSERT_TRUE(File::WriteStringToFile(path, "not hex"));
  Chat::ResetForTesting(ME);
  const std::string fresh = Chat::IdentityPublicKeyHex();
  EXPECT_EQ(fresh.size(), 64u);
  EXPECT_NE(fresh, m_my_id);
  EXPECT_TRUE(File::Exists(path + ".bad"));
}

TEST_F(ChatTest, ExchangeInARoom)
{
  Peer bob(BOB, 2);
  const CC::Key32 my_kx = Join({&bob});
  EXPECT_EQ(Status()["kind"].get<std::string>(), "room");

  // Bob -> me.
  ASSERT_TRUE(Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, "gg ｗｐ ねこ"))));
  ASSERT_EQ(Texts(), std::vector<std::string>{"gg ｗｐ ねこ"});

  // Me -> Bob: what Bob's Dolphin gets decrypts and verifies.
  ASSERT_EQ(Chat::Send("  one   more?\u202E "), Chat::SendResult::Sent);
  const auto out = Chat::TakeOutbox();
  ASSERT_EQ(out.size(), 1u);
  picojson::value v;
  ASSERT_TRUE(picojson::parse(v, out[0]).empty());
  auto& o = v.get<picojson::object>();
  EXPECT_EQ(o["type"].get<std::string>(), "chat-send");
  auto& boxes = o["boxes"].get<picojson::object>();
  ASSERT_EQ(boxes.size(), 1u);
  const auto box = CC::FromHex(boxes[BOB].get<std::string>(), CC::MAX_BOX_BYTES);
  const auto key = CC::PairKey(bob.kx, my_kx, GROUP);
  const auto plain = CC::Open(*key, CC::AssociatedData(GROUP, ME, BOB), *box);
  ASSERT_TRUE(plain);
  const auto p = CC::DecodePlaintext(*plain);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->text, "one more?");  // cleaned before it was sent
  EXPECT_EQ(p->seq, 1u);
  EXPECT_TRUE(CC::Verify(*CC::FromHexFixed<32>(m_my_id), p->signature,
                         CC::MessageSigMessage(GROUP, ME, 1, p->text)));
}

TEST_F(ChatTest, RefusesForgedReplayedAndMisaddressed)
{
  Peer bob(BOB, 2);
  Peer carol(CAROL, 3);
  const CC::Key32 my_kx = Join({&bob, &carol});

  const std::string good = bob.Box(GROUP, my_kx, ME, "first");
  ASSERT_TRUE(Chat::OnServerMessage(Msg(BOB, good)));
  // The same box again: a replay.
  Chat::OnServerMessage(Msg(BOB, good));
  EXPECT_EQ(Counter("drop_replay"), 1);
  // An older sequence number.
  Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, "old", 1)));
  EXPECT_EQ(Counter("drop_replay"), 2);
  // Carol relabels Bob's box as hers: the pair key differs.
  Chat::OnServerMessage(Msg(CAROL, bob.Box(GROUP, my_kx, ME, "not carol")));
  EXPECT_EQ(Counter("drop_box"), 1);
  // A box for someone else.
  Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, CAROL, "for carol")));
  EXPECT_EQ(Counter("drop_box"), 2);
  // Bob's box, signed with Carol's identity.
  {
    const u64 n = ++bob.seq;
    const CC::Sig64 sig = carol.id.Sign(CC::MessageSigMessage(GROUP, BOB, n, "forged"));
    const auto key = *CC::PairKey(bob.kx, my_kx, GROUP);
    const std::string box = CC::ToHex(CC::Seal(key, CC::RandomNonce(),
                                               CC::AssociatedData(GROUP, BOB, ME),
                                               CC::EncodePlaintext(n, sig, "forged")));
    Chat::OnServerMessage(Msg(BOB, box));
    EXPECT_EQ(Counter("drop_signature"), 1);
  }
  // From me, from a stranger, to another group.
  Chat::OnServerMessage(Msg(ME, good));
  Chat::OnServerMessage(Msg("dddddddd-dddd-4ddd-8ddd-dddddddddddd", good));
  Chat::OnServerMessage(Msg(BOB, bob.Box("room-KFQB-13", my_kx, ME, "x"), "room-KFQB-13"));
  EXPECT_EQ(Counter("drop_member"), 3);
  // Text that is not UTF-8 or cleans to nothing.
  Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, "\xFF\xFE")));
  Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, "\u200B\u202E")));
  EXPECT_EQ(Counter("drop_text"), 2);
  // Bad hex, an oversized box.
  Chat::OnServerMessage(Msg(BOB, "zz"));
  Chat::OnServerMessage(Msg(BOB, std::string(2 * (CC::MAX_BOX_BYTES + 1), 'a')));
  EXPECT_EQ(Texts(), std::vector<std::string>{"first"});
}

TEST_F(ChatTest, RateLimitsEachSender)
{
  Peer bob(BOB, 2);
  const CC::Key32 my_kx = Join({&bob});
  for (int i = 0; i < 30; ++i)
    Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, "spam")));
  EXPECT_LE(Counter("accepted"), Chat::RECEIVE_BURST + 1);
  EXPECT_GE(Counter("drop_rate"), 30 - Chat::RECEIVE_BURST - 1);
}

TEST_F(ChatTest, MembersAndKeys)
{
  Peer bob(BOB, 2);
  Peer carol(CAROL, 3);
  const CC::Key32 my_kx = Join({&bob});
  // Carol joins with a group key signed by someone else: listed, but nobody encrypts to her.
  picojson::object c = carol.Member(GROUP, "carol");
  c["kxSig"] = picojson::value(CC::ToHex(bob.id.Sign(CC::KxSigMessage(GROUP, carol.kx.public_key))));
  const auto me = MyMember(my_kx, {});
  picojson::array members{picojson::value(me), picojson::value(bob.Member(GROUP, "bob")),
                          picojson::value(c)};
  // (my own kx without a signature is ignored for me; it only matters to the others)
  Chat::OnServerMessage(Group(members));
  EXPECT_GE(Counter("bad_keys"), 1);
  for (const auto& m : Status()["members"].get<picojson::array>())
  {
    const auto& mo = m.get<picojson::object>();
    if (mo.at("uid").get<std::string>() == CAROL)
      EXPECT_FALSE(mo.at("can_read").get<bool>());
  }
  ASSERT_EQ(Chat::Send("hi"), Chat::SendResult::Sent);
  for (const std::string& json : Chat::TakeOutbox())
  {
    picojson::value v;
    picojson::parse(v, json);
    auto& o = v.get<picojson::object>();
    if (o["type"].get<std::string>() == "chat-send")
    {
      EXPECT_TRUE(o["boxes"].get<picojson::object>().contains(BOB));
      EXPECT_FALSE(o["boxes"].get<picojson::object>().contains(CAROL));
    }
  }
  // Carol leaves: a grey line; then everyone else leaves.
  members.pop_back();
  Chat::OnServerMessage(Group(members));
  const auto texts = Texts();
  EXPECT_EQ(texts.back(), "carol left");
  // The group ends.
  Chat::OnServerMessage(R"({"type":"chat-group","group":null})");
  EXPECT_EQ(Status()["kind"].get<std::string>(), "none");
  EXPECT_TRUE(Texts().empty());
  EXPECT_EQ(Chat::Send("hi"), Chat::SendResult::NoGroup);
}

TEST_F(ChatTest, RefusesBadGroups)
{
  Peer bob(BOB, 2);
  const auto ok = [&] {
    return picojson::array{picojson::value(MyMember()), picojson::value(bob.Member(GROUP, "bob"))};
  };
  // Not us.
  {
    picojson::object o;
    o["type"] = picojson::value("chat-group");
    o["group"] = picojson::value(GROUP);
    o["kind"] = picojson::value("room");
    o["you"] = picojson::value(BOB);
    o["members"] = picojson::value(ok());
    Chat::OnServerMessage(Json(o));
  }
  Chat::OnServerMessage(Group(ok(), "room KFQB"));                  // a space in the id
  Chat::OnServerMessage(Group(ok(), std::string(97, 'a')));         // too long
  Chat::OnServerMessage(Group(ok(), GROUP, "party"));               // unknown kind
  Chat::OnServerMessage(Group({picojson::value(bob.Member(GROUP, "bob"))}));  // without us
  {
    auto five = ok();
    for (int i = 0; i < 3; ++i)
      five.push_back(five[1]);
    Chat::OnServerMessage(Group(five));  // 5 members, duplicates
  }
  {
    auto bad = ok();
    bad[1].get<picojson::object>()["idKey"] = picojson::value("00");
    Chat::OnServerMessage(Group(bad));
  }
  {
    auto bad = ok();
    bad[1].get<picojson::object>()["uid"] = picojson::value("BBBBBBBB-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    Chat::OnServerMessage(Group(bad));
  }
  EXPECT_EQ(Counter("bad_groups"), 8);
  EXPECT_EQ(Status()["kind"].get<std::string>(), "none");
}

TEST_F(ChatTest, HostileJson)
{
  // Not chat: left to the rooms code.
  EXPECT_FALSE(Chat::OnServerMessage(R"({"type":"room-state"})"));
  EXPECT_FALSE(Chat::OnServerMessage("garbage \"chat-"));
  // Would crash or abort picojson: refused before it parses.
  EXPECT_FALSE(Chat::OnServerMessage(std::string(5000, '[') + "\"chat-" + std::string(5000, ']')));
  EXPECT_FALSE(Chat::OnServerMessage(R"({"type":"chat-msg","x":1e999})"));
  EXPECT_FALSE(Chat::OnServerMessage(R"({"type":"chat-msg"})" + std::string(9000, ' ')));
  // Chat messages with wrong types or no fields: handled (consumed), nothing happens.
  EXPECT_TRUE(Chat::OnServerMessage(R"({"type":"chat-msg"})"));
  EXPECT_TRUE(Chat::OnServerMessage(R"({"type":"chat-msg","group":1,"from":[],"box":{}})"));
  EXPECT_TRUE(Chat::OnServerMessage(R"({"type":"chat-group","group":"g","members":"x"})"));
  EXPECT_TRUE(Chat::OnServerMessage(R"({"type":"chat-error","error":"\u0000\u202E"})"));
  EXPECT_TRUE(Chat::OnServerMessage(R"({"type":"chat-whatever"})"));
  EXPECT_EQ(Status()["kind"].get<std::string>(), "none");
}

TEST_F(ChatTest, ReportCarriesVerifiableMessages)
{
  Peer bob(BOB, 2);
  const CC::Key32 my_kx = Join({&bob});
  EXPECT_FALSE(Chat::Report(BOB, ""));  // nothing said yet
  for (const char* t : {"one", "two", "three"})
    Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, t)));
  ASSERT_TRUE(Chat::Report(BOB, "rude\n"));
  bool found = false;
  for (const std::string& json : Chat::TakeOutbox())
  {
    picojson::value v;
    picojson::parse(v, json);
    auto& o = v.get<picojson::object>();
    if (o["type"].get<std::string>() != "chat-report")
      continue;
    found = true;
    EXPECT_EQ(o["from"].get<std::string>(), BOB);
    EXPECT_EQ(o["reason"].get<std::string>(), "rude");
    const auto& msgs = o["messages"].get<picojson::array>();
    ASSERT_EQ(msgs.size(), 3u);
    u64 seq = 0;
    for (const auto& m : msgs)
    {
      const auto& mo = m.get<picojson::object>();
      const u64 n = static_cast<u64>(mo.at("seq").get<double>());
      EXPECT_GT(n, seq);
      seq = n;
      const auto sig = CC::FromHexFixed<64>(mo.at("sig").get<std::string>());
      EXPECT_TRUE(CC::Verify(bob.id.PublicKey(), *sig,
                             CC::MessageSigMessage(GROUP, BOB, n, mo.at("text").get<std::string>())));
    }
  }
  EXPECT_TRUE(found);
  EXPECT_FALSE(Chat::Report(ME, ""));
}

TEST_F(ChatTest, HiddenPlayersAreKept)
{
  Chat::SetHidden(BOB, true);
  Chat::SetHidden("not-a-uid", true);
  EXPECT_TRUE(Chat::IsHidden(BOB));
  Chat::ResetForTesting(ME);  // read back from chat-hidden.json
  EXPECT_TRUE(Chat::IsHidden(BOB));
  EXPECT_FALSE(Chat::IsHidden("not-a-uid"));
  Chat::SetHidden(BOB, false);
  Chat::ResetForTesting(ME);
  EXPECT_FALSE(Chat::IsHidden(BOB));
  // A damaged file is ignored.
  File::WriteStringToFile(File::GetUserPath(D_ONLINE_IDX) + "chat-hidden.json",
                          std::string(3000, '[') + "1e999");
  Chat::ResetForTesting(ME);
  EXPECT_FALSE(Chat::IsHidden(BOB));
}

TEST_F(ChatTest, FuzzServerMessages)
{
  Peer bob(BOB, 2);
  const CC::Key32 my_kx = Join({&bob});
  std::vector<std::string> seeds = {
      Msg(BOB, bob.Box(GROUP, my_kx, ME, "seed message")),
      Group({picojson::value(MyMember()), picojson::value(bob.Member(GROUP, "bob"))}),
      R"({"type":"chat-error","op":"chat-send","error":"slow down"})",
      R"({"type":"chat-reported","from":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"})",
  };
  std::mt19937 rng(1010);
  for (int i = 0; i < 20000; ++i)
  {
    std::string s = seeds[rng() % seeds.size()];
    const int edits = 1 + static_cast<int>(rng() % 8);
    for (int k = 0; k < edits && !s.empty(); ++k)
    {
      const size_t at = rng() % s.size();
      switch (rng() % 4)
      {
      case 0:
        s[at] = static_cast<char>(rng() & 0xFF);
        break;
      case 1:
        s.erase(at, 1 + rng() % 8);
        break;
      case 2:
        s.insert(at, 1, static_cast<char>(rng() & 0xFF));
        break;
      case 3:
        s.insert(at, s.substr(rng() % s.size(), rng() % 32));
        break;
      }
    }
    Chat::OnServerMessage(s);
    if (i % 997 == 0)
      Chat::TakeOutbox();
  }
  // Still working afterwards.
  (void)Chat::GetView(0);
  SUCCEED();
}

TEST_F(ChatTest, AKeyPutBackDoesNotReplay)
{
  Peer bob(BOB, 2);
  const CC::Key32 my_kx = Join({&bob});
  const auto me = MyMember(my_kx, {});
  const std::string old_box = bob.Box(GROUP, my_kx, ME, "under the first key");
  ASSERT_TRUE(Chat::OnServerMessage(Msg(BOB, old_box)));

  // Bob reconnects with a new group key: his numbers start over under it.
  const CC::GroupKey first = bob.kx;
  bob.kx = CC::GroupKeyFromSecret(Fill(77));
  bob.seq = 0;
  Chat::OnServerMessage(Group({picojson::value(me), picojson::value(bob.Member(GROUP, "bob"))}));
  Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, "under the second key")));
  EXPECT_EQ(Counter("accepted"), 2);

  // The first key put back: what was sent under it can't be played again.
  bob.kx = first;
  Chat::OnServerMessage(Group({picojson::value(me), picojson::value(bob.Member(GROUP, "bob"))}));
  Chat::OnServerMessage(Msg(BOB, old_box));
  EXPECT_EQ(Counter("drop_replay"), 1);
  EXPECT_EQ(Counter("accepted"), 2);
}

TEST_F(ChatTest, ReportFitsInOnePacket)
{
  Peer bob(BOB, 2);
  const CC::Key32 my_kx = Join({&bob});
  // Text that JSON escapes six times over (each control is \u00XX): 8 of them can't all fit.
  const std::string text = "a" + std::string(299, '\x01');
  for (int i = 0; i < 8; ++i)
    Chat::OnServerMessage(Msg(BOB, bob.Box(GROUP, my_kx, ME, text)));
  ASSERT_EQ(Counter("accepted"), 8);
  ASSERT_TRUE(Chat::Report(BOB, ""));
  for (const std::string& json : Chat::TakeOutbox())
  {
    picojson::value v;
    picojson::parse(v, json);
    auto& o = v.get<picojson::object>();
    if (o["type"].get<std::string>() != "chat-report")
      continue;
    EXPECT_LE(json.size(), 8u * 1024);
    const auto& msgs = o["messages"].get<picojson::array>();
    EXPECT_GE(msgs.size(), 1u);
    EXPECT_LT(msgs.size(), 8u);
    // The newest ones.
    EXPECT_EQ(static_cast<u64>(msgs.back().get<picojson::object>().at("seq").get<double>()), 8u);
  }
}
