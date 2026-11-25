// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Rollback/InputSuggest.h (core/standalone only): input
// suggestions sample the local controllers between the game's reads, which only Dolphin's input
// code does today, so outside Dolphin they are off, as with PPR_GPRB_SUGGEST=0: nothing is sent,
// the session's state does not offer them (no "sg"), and the peers' are not used. A peer that sends
// them sees this machine as one that does not take them, so crossplay is unchanged.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

#include <SFML/Network/IpAddress.hpp>

#include "Common/CommonTypes.h"

namespace sf
{
class UdpSocket;
}

namespace Gprb::Suggest
{
constexpr std::size_t INPUT_BYTES = 0x40;
constexpr u8 DATAGRAM = 'S';
constexpr u8 VERSION = 1;

inline bool Enabled()
{
  return false;
}

inline bool Decode(const u8*, std::size_t, s32*, u32*, const u8**)
{
  return false;
}

struct Peer
{
  sf::IpAddress ip;
  unsigned short port = 0;
};

inline void Arm(sf::UdpSocket*, int)
{
}
inline void Disarm()
{
}
inline void SetPeers(std::vector<Peer>)
{
}
inline void SetChannel(int)
{
}
inline void NoteLocalInput(int, std::chrono::steady_clock::time_point, const u8*, double)
{
}

// Never given to GekkoNet (Enabled() is false); here so the session's call compiles.
inline unsigned int WorseCost(const unsigned char*, const unsigned char*, const unsigned char*)
{
  return 0;
}

struct SenderStats
{
  u64 local_changes = 0;
  u64 reports = 0;
  u64 sent = 0;
  u64 held_back = 0;
  u64 crossings = 0;
  u64 follow_ups = 0;
  u64 too_late = 0;
  u64 undecoded = 0;
  u64 samples = 0;
  u64 sample_us = 0;
  u64 sample_us_max = 0;
  u64 sample_gaps = 0;
  u64 sample_gap_us = 0;
  u64 sample_gap_us_sq = 0;
  u64 sample_gap_us_max = 0;
};
inline SenderStats Stats()
{
  return {};
}
}  // namespace Gprb::Suggest
