// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/ChatCrypto.h"

#include <algorithm>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include "Common/Random.h"

namespace Online::ChatCrypto
{
namespace
{
void Append(std::vector<u8>* out, std::string_view s)
{
  out->insert(out->end(), s.begin(), s.end());
}

void Append(std::vector<u8>* out, std::span<const u8> bytes)
{
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void AppendU64(std::vector<u8>* out, u64 v)
{
  for (int i = 0; i < 8; ++i)
    out->push_back(static_cast<u8>(v >> (8 * i)));
}
}  // namespace

Identity::Identity(const Key32& seed)
{
  Key32 copy = seed;  // crypto_ed25519_key_pair wipes the seed it is given
  crypto_ed25519_key_pair(m_secret.data(), m_public.data(), copy.data());
}

Identity::~Identity()
{
  Wipe(m_secret.data(), m_secret.size());
}

Sig64 Identity::Sign(std::span<const u8> message) const
{
  Sig64 sig{};
  crypto_ed25519_sign(sig.data(), m_secret.data(), message.data(), message.size());
  return sig;
}

bool Verify(const Key32& public_key, const Sig64& signature, std::span<const u8> message)
{
  return crypto_ed25519_check(signature.data(), public_key.data(), message.data(),
                              message.size()) == 0;
}

GroupKey GroupKeyFromSecret(const Key32& secret)
{
  GroupKey k;
  k.secret = secret;
  crypto_x25519_public_key(k.public_key.data(), k.secret.data());
  return k;
}

Key32 RandomKey32()
{
  Key32 k{};
  Common::Random::Generate(k.data(), k.size());
  return k;
}

GroupKey NewGroupKey()
{
  Key32 secret = RandomKey32();
  GroupKey k = GroupKeyFromSecret(secret);
  Wipe(secret.data(), secret.size());
  return k;
}

Nonce24 RandomNonce()
{
  Nonce24 n{};
  Common::Random::Generate(n.data(), n.size());
  return n;
}

std::vector<u8> KxSigMessage(std::string_view group, const Key32& kx)
{
  std::vector<u8> m;
  Append(&m, std::string_view("brawlonline-chat-kx-v1"));
  m.push_back(0);
  Append(&m, group);
  m.push_back(0);
  Append(&m, kx);
  return m;
}

std::optional<Key32> PairKey(const GroupKey& own, const Key32& theirs, std::string_view group)
{
  Key32 shared{};
  crypto_x25519(shared.data(), own.secret.data(), theirs.data());
  if (std::ranges::all_of(shared, [](u8 b) { return b == 0; }))
    return std::nullopt;
  const bool own_first = std::ranges::lexicographical_compare(own.public_key, theirs);
  const Key32& lo = own_first ? own.public_key : theirs;
  const Key32& hi = own_first ? theirs : own.public_key;

  std::vector<u8> m;
  Append(&m, std::string_view("brawlonline-chat-key-v1"));
  m.push_back(0);
  Append(&m, group);
  m.push_back(0);
  Append(&m, shared);
  Append(&m, lo);
  Append(&m, hi);
  Key32 key{};
  crypto_blake2b(key.data(), key.size(), m.data(), m.size());
  Wipe(shared.data(), shared.size());
  Wipe(m.data(), m.size());
  return key;
}

std::vector<u8> MessageSigMessage(std::string_view group, std::string_view from, u64 seq,
                                  std::string_view text)
{
  std::vector<u8> m;
  Append(&m, std::string_view("brawlonline-chat-msg-v1"));
  m.push_back(0);
  Append(&m, group);
  m.push_back(0);
  Append(&m, from);
  m.push_back(0);
  AppendU64(&m, seq);
  Append(&m, text);
  return m;
}

std::vector<u8> AssociatedData(std::string_view group, std::string_view from, std::string_view to)
{
  std::vector<u8> m;
  Append(&m, std::string_view("brawlonline-chat-box-v1"));
  m.push_back(0);
  Append(&m, group);
  m.push_back(0);
  Append(&m, from);
  m.push_back(0);
  Append(&m, to);
  return m;
}

std::vector<u8> EncodePlaintext(u64 seq, const Sig64& signature, std::string_view text)
{
  std::vector<u8> p;
  p.reserve(PLAINTEXT_HEADER + text.size());
  p.push_back(PLAINTEXT_VERSION);
  AppendU64(&p, seq);
  Append(&p, signature);
  Append(&p, text);
  return p;
}

std::optional<Plaintext> DecodePlaintext(std::span<const u8> plaintext)
{
  if (plaintext.size() <= PLAINTEXT_HEADER ||
      plaintext.size() > PLAINTEXT_HEADER + MAX_TEXT_BYTES || plaintext[0] != PLAINTEXT_VERSION)
  {
    return std::nullopt;
  }
  Plaintext p;
  for (int i = 0; i < 8; ++i)
    p.seq |= static_cast<u64>(plaintext[1 + i]) << (8 * i);
  std::copy_n(plaintext.begin() + 9, 64, p.signature.begin());
  p.text.assign(reinterpret_cast<const char*>(plaintext.data() + PLAINTEXT_HEADER),
                plaintext.size() - PLAINTEXT_HEADER);
  return p;
}

std::vector<u8> Seal(const Key32& key, const Nonce24& nonce, std::span<const u8> associated_data,
                     std::span<const u8> plaintext)
{
  std::vector<u8> box(BOX_OVERHEAD + plaintext.size());
  std::copy(nonce.begin(), nonce.end(), box.begin());
  crypto_aead_lock(box.data() + BOX_OVERHEAD, box.data() + 24, key.data(), nonce.data(),
                   associated_data.data(), associated_data.size(), plaintext.data(),
                   plaintext.size());
  return box;
}

std::optional<std::vector<u8>> Open(const Key32& key, std::span<const u8> associated_data,
                                    std::span<const u8> box)
{
  if (box.size() <= BOX_OVERHEAD || box.size() > MAX_BOX_BYTES)
    return std::nullopt;
  std::vector<u8> plain(box.size() - BOX_OVERHEAD);
  if (crypto_aead_unlock(plain.data(), box.data() + 24, key.data(), box.data(),
                         associated_data.data(), associated_data.size(),
                         box.data() + BOX_OVERHEAD, plain.size()) != 0)
  {
    return std::nullopt;
  }
  return plain;
}

std::string ToHex(std::span<const u8> bytes)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string s;
  s.reserve(bytes.size() * 2);
  for (const u8 b : bytes)
  {
    s.push_back(DIGITS[b >> 4]);
    s.push_back(DIGITS[b & 0xF]);
  }
  return s;
}

std::optional<std::vector<u8>> FromHex(std::string_view hex, size_t max_bytes)
{
  if (hex.size() % 2 != 0 || hex.size() / 2 > max_bytes)
    return std::nullopt;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  std::vector<u8> out(hex.size() / 2);
  for (size_t i = 0; i < out.size(); ++i)
  {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return std::nullopt;
    out[i] = static_cast<u8>((hi << 4) | lo);
  }
  return out;
}

void Wipe(void* data, size_t size)
{
  crypto_wipe(data, size);
}
}  // namespace Online::ChatCrypto
