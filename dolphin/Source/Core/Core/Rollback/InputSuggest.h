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
//
// Two kinds of session send them: P+'s gameplay sessions (Gprb::Session: the input is the game's
// gfPadStatus, decoded on the host) and Other Games' whole-machine sessions (Machine::Session: the
// input is the GameCube pad as the SI reports it, MachineMeaningful). The sender (a controller's
// reports and samples, the rules, the timing) is the same; a Format says how the session's input is
// made from the controller and which of its changes go out.

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

#include <SFML/Network/IpAddress.hpp>

#include "Common/CommonTypes.h"

struct GCPadStatus;

namespace sf
{
class UdpSocket;
}

namespace Gprb
{
enum class HostPadSource;
}

namespace Gprb::Suggest
{
// One gfPadStatus (Addr::PAD_STRIDE), the GekkoNet input of a player in a gameplay session, and
// the largest input of any session.
constexpr std::size_t INPUT_BYTES = 0x40;

// The datagram: 'S', version 1, the frame (s32, little endian), the sequence (u32), the input (of
// the session's input size).
constexpr u8 DATAGRAM = 'S';
constexpr u8 VERSION = 1;
constexpr std::size_t HEADER_SIZE = 2 + 4 + 4;
constexpr std::size_t DATAGRAM_SIZE = HEADER_SIZE + INPUT_BYTES;

std::array<u8, DATAGRAM_SIZE> Encode(s32 frame, u32 seq, const u8* input);
// A datagram from a peer (untrusted): false when it is not one.
bool Decode(const u8* data, std::size_t size, s32* frame, u32* seq, const u8** input);
// The same for an input of `input_bytes` (at most INPUT_BYTES): Encode writes HEADER_SIZE +
// `input_bytes` bytes to `out`.
void Encode(s32 frame, u32 seq, const u8* input, std::size_t input_bytes, u8* out);
bool Decode(const u8* data, std::size_t size, std::size_t input_bytes, s32* frame, u32* seq,
            const u8** input);

// PPR_GPRB_SUGGEST=0: no suggestions are sent, and the peers' are not used (the state says so).
bool Enabled();

struct Peer
{
  sf::IpAddress ip;
  unsigned short port = 0;
};

// How a session's local input is made from its GameCube controller, and which changes of it go out.
struct Format
{
  std::size_t input_bytes;
  // Any thread: whether the game reads SI channel `chan`'s controller from `source` now (only that
  // controller's changes go out: an adapter can be plugged in while the player plays a mapped one).
  bool (*reads)(int chan, HostPadSource source);
  // Any thread: `st`, that controller, made into the local input the session would send if the game
  // read it now (`last`: the newest local input GekkoNet took). False when it cannot be now.
  bool (*make)(int chan, HostPadSource source, const GCPadStatus& st, const u8* last, u8* out);
  // A change that can start an action or end one (rules cross and follow).
  bool (*meaningful)(const u8* prev, const u8* next);
  // Whether a change is a button's (rules all: a change of only sticks and triggers is limited).
  bool (*buttons_differ)(const u8* prev, const u8* next);
  // Tests (PPR_GPRB_SUGGEST_LIE=1): `in` made a lie, A the other way and the main stick's X random.
  void (*lie)(u8* in, u32 random);
};
// P+'s gameplay sessions' (gfPadStatus, decoded on the host: HostDecodeLatest).
extern const Format GFPAD_FORMAT;

// CPU thread, under the session's mutex. A network match whose local player plays a GameCube
// controller on SI channel `chan` (0-3) runs: suggestions for it go out on `socket` (alive until
// Disarm) to the peers SetPeers names (those that take them).
void Arm(sf::UdpSocket* socket, int chan);
// The same for a session with its own transport: `send` (any thread; once per suggestion) takes a
// datagram to every peer that takes suggestions, SetHasPeers says whether there is any.
void Arm(const Format& format, void (*send)(const u8* data, std::size_t size), int chan);
void Disarm();
void SetPeers(std::vector<Peer> peers);
void SetHasPeers(bool any);
// The local player's GameCube controller is on SI channel `chan` from now on (-1: none, as for a
// Wii Remote: nothing goes out).
void SetChannel(int chan);
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

// Other Games' inputs (Machine::Input): a GameCube pad as the SI reports it, at MACHINE_PAD: the
// buttons (big endian), stick X, stick Y, C-stick X, C-stick Y, L, R, the sticks centered on 0x80
// (the session moves an adapter controller's own origin into the values).
constexpr std::size_t MACHINE_PAD = 1;
// The game is unknown: P+'s lines (Meaningful's) against the sticks' center, and every button,
// the D-pad too.
bool MachineMeaningful(const u8* prev, const u8* next);
unsigned int MachineWorseCost(const u8* previous, const u8* suggested, const u8* real);

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
  // The gaps between one sample's start and the next's while sampling: how many, their sum, the
  // sum of their squares (a change waits sum_sq / (2 sum) on average to be sampled) and the longest.
  u64 sample_gaps = 0;
  u64 sample_gap_us = 0;
  u64 sample_gap_us_sq = 0;
  u64 sample_gap_us_max = 0;
};
SenderStats Stats();
}  // namespace Gprb::Suggest
