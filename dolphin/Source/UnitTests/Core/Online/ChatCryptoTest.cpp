// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Online/ChatCrypto.h"

using namespace Online::ChatCrypto;

namespace
{
constexpr char GROUP[] = "room-KFQB-12";
constexpr char UID_A[] = "11111111-1111-4111-8111-111111111111";
constexpr char UID_B[] = "22222222-2222-4222-8222-222222222222";

Key32 Fill(u8 v)
{
  Key32 k;
  k.fill(v);
  return k;
}

std::vector<u8> Bytes(std::string_view s)
{
  return {s.begin(), s.end()};
}
}  // namespace

TEST(ChatCrypto, Hex)
{
  EXPECT_EQ(ToHex(std::vector<u8>{0x00, 0xAB, 0xFF}), "00abff");
  EXPECT_EQ(FromHex("00abFF", 3), (std::vector<u8>{0x00, 0xAB, 0xFF}));
  EXPECT_EQ(FromHex("0", 3), std::nullopt);     // odd
  EXPECT_EQ(FromHex("zz", 3), std::nullopt);    // not hex
  EXPECT_EQ(FromHex("00112233", 3), std::nullopt);  // too long
  EXPECT_EQ(FromHexFixed<2>("0011"), (std::array<u8, 2>{0x00, 0x11}));
  EXPECT_EQ(FromHexFixed<2>("001122"), std::nullopt);
}

TEST(ChatCrypto, SignAndVerify)
{
  const Identity a(Fill(1));
  const auto m = Bytes("hello");
  const Sig64 sig = a.Sign(m);
  EXPECT_TRUE(Verify(a.PublicKey(), sig, m));
  EXPECT_FALSE(Verify(a.PublicKey(), sig, Bytes("hellp")));
  Sig64 bad = sig;
  bad[10] ^= 1;
  EXPECT_FALSE(Verify(a.PublicKey(), bad, m));
  const Identity b(Fill(2));
  EXPECT_FALSE(Verify(b.PublicKey(), sig, m));
  // The same seed gives the same key.
  EXPECT_EQ(Identity(Fill(1)).PublicKey(), a.PublicKey());
}

TEST(ChatCrypto, PairKeysAgree)
{
  const GroupKey a = GroupKeyFromSecret(Fill(3));
  const GroupKey b = GroupKeyFromSecret(Fill(4));
  const auto ab = PairKey(a, b.public_key, GROUP);
  const auto ba = PairKey(b, a.public_key, GROUP);
  ASSERT_TRUE(ab && ba);
  EXPECT_EQ(*ab, *ba);
  // Bound to the group.
  EXPECT_NE(*PairKey(a, b.public_key, "room-KFQB-13"), *ab);
  // A low-order point gives an all-zero secret: refused.
  EXPECT_EQ(PairKey(a, Key32{}, GROUP), std::nullopt);
  // Fresh keys differ.
  EXPECT_NE(NewGroupKey().public_key, NewGroupKey().public_key);
}

TEST(ChatCrypto, BoxRoundTripAndTampering)
{
  const Identity id_a(Fill(1));
  const GroupKey a = GroupKeyFromSecret(Fill(3));
  const GroupKey b = GroupKeyFromSecret(Fill(4));
  const Key32 key = *PairKey(a, b.public_key, GROUP);

  const std::string text = "hello ｗｏｒｌｄ ねこ";
  const Sig64 sig = id_a.Sign(MessageSigMessage(GROUP, UID_A, 1, text));
  const std::vector<u8> plain = EncodePlaintext(1, sig, text);
  const std::vector<u8> ad = AssociatedData(GROUP, UID_A, UID_B);
  const std::vector<u8> box = Seal(key, RandomNonce(), ad, plain);
  ASSERT_EQ(box.size(), BOX_OVERHEAD + plain.size());

  const auto opened = Open(key, ad, box);
  ASSERT_TRUE(opened);
  EXPECT_EQ(*opened, plain);
  const auto p = DecodePlaintext(*opened);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->seq, 1u);
  EXPECT_EQ(p->text, text);
  EXPECT_TRUE(Verify(id_a.PublicKey(), p->signature, MessageSigMessage(GROUP, UID_A, 1, p->text)));

  // Any flipped bit, the wrong direction or recipient, a cut box: refused.
  for (size_t i = 0; i < box.size(); ++i)
  {
    std::vector<u8> t = box;
    t[i] ^= 0x01;
    EXPECT_FALSE(Open(key, ad, t)) << i;
  }
  EXPECT_FALSE(Open(key, AssociatedData(GROUP, UID_B, UID_A), box));  // reflected back
  EXPECT_FALSE(Open(key, AssociatedData("room-KFQB-13", UID_A, UID_B), box));
  EXPECT_FALSE(Open(key, ad, std::span(box).first(BOX_OVERHEAD)));
  EXPECT_FALSE(Open(key, ad, std::span(box).first(10)));
  EXPECT_FALSE(Open(key, ad, std::vector<u8>(MAX_BOX_BYTES + 1)));
  EXPECT_FALSE(Open(Fill(9), ad, box));
}

TEST(ChatCrypto, Plaintext)
{
  const Sig64 sig{};
  EXPECT_FALSE(DecodePlaintext(EncodePlaintext(1, sig, "")));  // empty text
  EXPECT_TRUE(DecodePlaintext(EncodePlaintext(1, sig, std::string(MAX_TEXT_BYTES, 'a'))));
  EXPECT_FALSE(DecodePlaintext(EncodePlaintext(1, sig, std::string(MAX_TEXT_BYTES + 1, 'a'))));
  std::vector<u8> v = EncodePlaintext(0x0102030405060708ull, sig, "x");
  EXPECT_EQ(DecodePlaintext(v)->seq, 0x0102030405060708ull);
  v[0] = 2;
  EXPECT_FALSE(DecodePlaintext(v));  // unknown version
  EXPECT_FALSE(DecodePlaintext(std::vector<u8>(PLAINTEXT_HEADER)));
  EXPECT_FALSE(DecodePlaintext(std::vector<u8>{}));
}

TEST(ChatCrypto, SignedMessagesAreUnambiguous)
{
  // The zero separators keep (group, from) pairs apart.
  EXPECT_NE(MessageSigMessage("ab", "c", 1, "x"), MessageSigMessage("a", "bc", 1, "x"));
  EXPECT_NE(KxSigMessage("ab", Key32{}), KxSigMessage("a", Key32{}));
  EXPECT_NE(AssociatedData("g", "a", "b"), AssociatedData("g", "b", "a"));
}

// The same bytes as server/crates/common/src/chat.rs `test_vectors` (docs/chat-protocol.md §4),
// cross-checked there against libsodium: Monocypher and the RustCrypto crates agree.
TEST(ChatCrypto, VectorsMatchTheServer)
{
  const auto seq_bytes = [](u8 start) {
    Key32 k;
    for (size_t i = 0; i < k.size(); ++i)
      k[i] = static_cast<u8>(start + i);
    return k;
  };
  Nonce24 nonce;
  for (size_t i = 0; i < nonce.size(); ++i)
    nonce[i] = static_cast<u8>(0x80 + i);
  const std::string text = "hello \xEF\xBD\x97\xEF\xBD\x8F\xEF\xBD\x92\xEF\xBD\x8C\xEF\xBD\x84 "
                           "\xE3\x81\xAD\xE3\x81\x93";  // "hello ｗｏｒｌｄ ねこ"
  ASSERT_EQ(text.size(), 28u);

  const Identity sender(seq_bytes(0x00));
  const Identity recipient(seq_bytes(0x20));
  const GroupKey sender_kx = GroupKeyFromSecret(seq_bytes(0x40));
  const GroupKey recipient_kx = GroupKeyFromSecret(seq_bytes(0x60));

  EXPECT_EQ(ToHex(sender.PublicKey()),
            "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8");
  EXPECT_EQ(ToHex(recipient.PublicKey()),
            "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7");
  EXPECT_EQ(ToHex(sender_kx.public_key),
            "79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a");
  EXPECT_EQ(ToHex(recipient_kx.public_key),
            "675dd574ed7789310b3d2e7681f3790b466c773b1521fecf36577958371ea52f");
  EXPECT_EQ(ToHex(sender.Sign(KxSigMessage(GROUP, sender_kx.public_key))),
            "47ec39ad5b0e785b3ad40d3afb9d876439e9cbfc6ccc4c36bc8e0e2778b11581"
            "ff51f27cce788697fdf08e404a897a7a4fff7b0b2f084cbfab00288ce939250d");
  const auto pair = PairKey(sender_kx, recipient_kx.public_key, GROUP);
  ASSERT_TRUE(pair);
  EXPECT_EQ(ToHex(*pair), "b58346539534a6603561da2c02479171669efd69a12ccc8114ba6d570ec54fc5");
  const Sig64 sig = sender.Sign(MessageSigMessage(GROUP, UID_A, 1, text));
  EXPECT_EQ(ToHex(sig), "655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e"
                        "748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a");
  const std::vector<u8> plain = EncodePlaintext(1, sig, text);
  EXPECT_EQ(ToHex(plain),
            "010100000000000000655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e"
            "748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a68656c6c6f20efbd97"
            "efbd8fefbd92efbd8cefbd8420e381ade38193");
  const std::vector<u8> ad = AssociatedData(GROUP, UID_A, UID_B);
  EXPECT_EQ(ToHex(ad),
            "627261776c6f6e6c696e652d636861742d626f782d763100726f6f6d2d4b4651422d313200313131313131"
            "31312d313131312d343131312d383131312d3131313131313131313131310032323232323232322d323232"
            "322d343232322d383232322d323232323232323232323232");
  EXPECT_EQ(ToHex(Seal(*pair, nonce, ad, plain)),
            "808182838485868788898a8b8c8d8e8f90919293949596972006179349a1893a3f0f1207a3f24df6a42108"
            "13228f3094a7319f3945640c4c55a3a6b54d78bff9f56e520e248754480102d74ec7034e692b38caddc70c"
            "057d50bff23530fa4e1d4f4a563f23b3e9b4eecd2101486f3a1a5a0111b29cbdd0d3168dc69757a22428c3"
            "b191ac6cb7289fd9dd2f6722");
  // The recipient side derives the same key and opens the box.
  EXPECT_EQ(PairKey(recipient_kx, sender_kx.public_key, GROUP), pair);
  EXPECT_EQ(Open(*pair, ad, Seal(*pair, nonce, ad, plain)), plain);
}
