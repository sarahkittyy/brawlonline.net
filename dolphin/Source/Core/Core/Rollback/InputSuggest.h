// Copyright 2026 brawlonline.net
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Input suggestions (Slippi's, after Arte's "subframe rollback", Slippi Ishiiruka PR #481): the
// game reads the local controller once a frame, but the controller can be looked at more often (a
// Wii U adapter reports every 1-8 ms; a controller mapped in Dolphin is sampled by a thread). When a
// look changes what the local input of the next frame would be, the session sends it at once to the
// other players, tagged with the GekkoNet frame its change will first be in. A machine that has to predict that player's input for that frame or a later one
// predicts with it instead of the player's last input (gekko_add_input_suggestion). Only
// predictions change: every prediction is checked against the real input as before, so the game
// always runs on the real inputs, and a rollback is fixed with them. A change made between two of
// the player's reads reaches the others up to a frame sooner than the read that takes it, so fewer
// of their predictions are wrong and their rollbacks are shorter.

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
// One gfPadStatus (Addr::PAD_STRIDE), the GekkoNet input of a player.
constexpr std::size_t INPUT_BYTES = 0x40;

// The datagram: 'S', version 1, the frame (s32, little endian), the sequence (u32), the input.
constexpr u8 DATAGRAM = 'S';
constexpr u8 VERSION = 1;
constexpr std::size_t DATAGRAM_SIZE = 2 + 4 + 4 + INPUT_BYTES;

std::array<u8, DATAGRAM_SIZE> Encode(s32 frame, u32 seq, const u8* input);
// A datagram from a peer (untrusted): false when it is not one.
bool Decode(const u8* data, std::size_t size, s32* frame, u32* seq, const u8** input);

// PPR_GPRB_SUGGEST=0: no suggestions are sent, and the peers' are not used (the state says so).
bool Enabled();

struct Peer
{
  sf::IpAddress ip;
  unsigned short port = 0;
};

// CPU thread, under the session's mutex. A network match whose local player plays a GameCube
// controller on SI channel `chan` (0-3) runs: suggestions for it go out on `socket` (alive until
// Disarm) to the peers SetPeers names (those that take them).
void Arm(sf::UdpSocket* socket, int chan);
void Disarm();
void SetPeers(std::vector<Peer> peers);
// GekkoNet took the local input of `frame`, read at `instant` (the instant its controller read
// stands for); `field_us` is a field at the game's speed now.
void NoteLocalInput(int frame, Clock::time_point instant, const u8* input, double field_us);

// Whether `next` (a gfPadStatus) can start an action that `prev` could not, or end one: a press
// or release of A, B, X, Y, Z, L, R or Start, an analog trigger crossing P+'s light shield, a stick
// crossing one of the lines P+ acts on (either way). Only such changes go out as suggestions
// (rules cross and follow).
bool Meaningful(const u8* prev, const u8* next);
// GekkoNet's suggestion judge (gekko_set_suggestion_judge) for gfPadStatus inputs.
unsigned int WorseCost(const u8* previous, const u8* suggested, const u8* real);

// Since Arm.
struct SenderStats
{
  u64 local_changes = 0;  // local inputs GekkoNet took that differ from the one before (also off)
  u64 reports = 0;        // controller reports or samples while there were peers to send to
  u64 sent = 0;           // suggestions sent (each to every such peer)
  u64 held_back = 0;      // changes not sent (all: analog over the limit; else not meaningful)
  u64 crossings = 0;      // of the sent, meaningful changes (rules cross and follow)
  u64 follow_ups = 0;     // of the sent, follow-ups after a crossing (rules follow)
  u64 too_late = 0;       // reports over two fields after the newest local input's read
  u64 undecoded = 0;      // reports the host decode could not make into an input
  // The mapped controller's sampler: samples, and the time they took (the devices' update and the
  // pad's read), in total and at most.
  u64 samples = 0;
  u64 sample_us = 0;
  u64 sample_us_max = 0;
};
SenderStats Stats();
}  // namespace Gprb::Suggest
