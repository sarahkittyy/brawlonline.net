// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Chat crypto (docs/chat-protocol.md §4) on Monocypher: Ed25519 identities, X25519 group keys,
// BLAKE2b pair keys, XChaCha20-Poly1305 boxes. The server's and mmclient's Rust
// (common::chat) does the same; UnitTests/Core/Online/ChatCryptoTest.cpp checks both produce the
// same bytes. Everything that parses received bytes checks every length first.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Online::ChatCrypto
{
using Key32 = std::array<u8, 32>;
using Sig64 = std::array<u8, 64>;
using Nonce24 = std::array<u8, 24>;

constexpr size_t MAX_TEXT_BYTES = 300;
constexpr size_t MAX_BOX_BYTES = 512;
constexpr u8 PLAINTEXT_VERSION = 1;
constexpr size_t PLAINTEXT_HEADER = 1 + 8 + 64;  // version, seq, signature
constexpr size_t BOX_OVERHEAD = 24 + 16;         // nonce, mac

// An Ed25519 key pair from its 32-byte seed.
class Identity
{
public:
  explicit Identity(const Key32& seed);
  ~Identity();
  Identity(const Identity&) = delete;
  Identity& operator=(const Identity&) = delete;

  const Key32& PublicKey() const { return m_public; }
  Sig64 Sign(std::span<const u8> message) const;

private:
  std::array<u8, 64> m_secret{};  // Monocypher's: seed followed by the public key
  Key32 m_public{};
};

bool Verify(const Key32& public_key, const Sig64& signature, std::span<const u8> message);

// An X25519 key pair for one group.
struct GroupKey
{
  Key32 secret{};
  Key32 public_key{};
};
GroupKey GroupKeyFromSecret(const Key32& secret);
GroupKey NewGroupKey();  // from Common::Random (the OS's CSPRNG)
Key32 RandomKey32();
Nonce24 RandomNonce();

// What the group key signature signs: "brawlonline-chat-kx-v1" 0 group 0 kx.
std::vector<u8> KxSigMessage(std::string_view group, const Key32& kx);
// The pair key of two members' group keys; nullopt for an all-zero shared secret (a low-order
// public key).
std::optional<Key32> PairKey(const GroupKey& own, const Key32& theirs, std::string_view group);
// What a message signature signs: "brawlonline-chat-msg-v1" 0 group 0 from 0 u64le(seq) text.
std::vector<u8> MessageSigMessage(std::string_view group, std::string_view from, u64 seq,
                                  std::string_view text);
// The associated data of a box: "brawlonline-chat-box-v1" 0 group 0 from 0 to.
std::vector<u8> AssociatedData(std::string_view group, std::string_view from, std::string_view to);

std::vector<u8> EncodePlaintext(u64 seq, const Sig64& signature, std::string_view text);
struct Plaintext
{
  u64 seq = 0;
  Sig64 signature{};
  std::string text;  // raw bytes, not yet cleaned
};
// nullopt unless: the version byte is 1, the text is 1..MAX_TEXT_BYTES bytes.
std::optional<Plaintext> DecodePlaintext(std::span<const u8> plaintext);

// nonce ‖ mac ‖ ciphertext.
std::vector<u8> Seal(const Key32& key, const Nonce24& nonce, std::span<const u8> associated_data,
                     std::span<const u8> plaintext);
// The plaintext, or nullopt if the box is too short or too long or does not authenticate.
std::optional<std::vector<u8>> Open(const Key32& key, std::span<const u8> associated_data,
                                    std::span<const u8> box);

std::string ToHex(std::span<const u8> bytes);
// Lowercase or uppercase hex of at most `max_bytes` bytes; nullopt otherwise (odd length, a
// non-hex digit, too long).
std::optional<std::vector<u8>> FromHex(std::string_view hex, size_t max_bytes);
// Exactly N bytes of hex.
template <size_t N>
std::optional<std::array<u8, N>> FromHexFixed(std::string_view hex)
{
  if (hex.size() != 2 * N)
    return std::nullopt;
  const auto v = FromHex(hex, N);
  if (!v)
    return std::nullopt;
  std::array<u8, N> out{};
  std::copy(v->begin(), v->end(), out.begin());
  return out;
}

// Overwrites secrets (crypto_wipe: not optimised away).
void Wipe(void* data, size_t size);
}  // namespace Online::ChatCrypto
