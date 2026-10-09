// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Checks for everything the other player's machine sends to the gameplay session (control
// messages, lock-ins, the host's match setup, sync block and init block, and the pad bytes
// GekkoNet hands over). The peer is a stranger from matchmaking: none of it is trusted. A value
// that can reach emulated memory, or be used as an index or size, is checked here first, so that
// the worst a malicious peer can do is end the session (as leaving would), never write arbitrary
// bytes into the game or crash Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Gprb::PeerData
{
// Limits for control messages ('C' packets): the session's own messages are well under 4 KiB.
constexpr size_t MAX_CONTROL_SIZE = 16 * 1024;
constexpr int MAX_JSON_DEPTH = 8;
constexpr size_t MAX_NAME_LEN = 64;
// gfTaskScheduler task lists (TASK_LISTS in GameplaySession.cpp: 17 + 40 + 14 lists).
constexpr size_t MAX_TASK_LISTS = 128;
constexpr size_t MAX_TASKS_PER_LIST = 4096;
constexpr size_t MAX_TASK_NAME_LEN = 64;
// Frames GekkoNet may queue on the CSS before a match polls them.
constexpr size_t MAX_QUEUED_GEKKO_PACKETS = 1024;
// A pong's round trip larger than this is not a measurement.
constexpr double MAX_RTT_MS = 5000.0;

// True when `text` is at most `max_depth` arrays/objects deep. picojson parses recursively with
// no depth limit, so a deeply nested message would overflow the network thread's stack.
bool JsonDepthOk(std::string_view text, int max_depth = MAX_JSON_DEPTH);

// True when `text` is safe to hand to picojson: at most `max_depth` deep (above), and no number
// outside a string that overflows a double. Dolphin's picojson calls std::abort() for a number it
// parses to infinity ("1e999"), so a single such message would end the process.
bool JsonSafeToParse(std::string_view text, int max_depth = MAX_JSON_DEPTH);

// Parses `text` (a control message) with the size and depth limits. nullopt if it is too large,
// too deep, not JSON, or not an object.
std::optional<picojson::object> ParseControl(std::string_view text);

// Exactly `bytes` bytes as 2 * `bytes` hex digits, else nullopt (never throws).
std::optional<std::vector<u8>> ParseHex(std::string_view hex, size_t bytes);

// A JSON number that is a whole number in [0, max], else nullopt. (A cast of a double outside
// the target type's range is undefined behaviour, so nothing is cast before this check.)
std::optional<u64> JsonUInt(const picojson::value* v, u64 max);

// gmCharacterKind values the online character select can lock in (docs/brawl-memory-map.md):
// the playable kinds 0x00-0x11 and 0x13-0x2B, and P+'s Roy 0x32, Mewtwo 0x33 and Knuckles 0x35.
// Not P+'s hold-shield forms 0x2C (Giga Bowser) and 0x2D (Wario-Man): they are not played online.
bool ValidCharKind(u32 kind);
// Colour numbers: the character select's own bound (game-code online_menu.cpp selectedCostume).
constexpr u32 MAX_COSTUME = 0x1F;
bool ValidCostume(u32 costume);
// srStageKind values a versus match can be played on (0x01-0x7F without menus, results and
// single-player modes; P+ v3.2's stage select uses 0x01-0x33, 0x37 and 0x40-0x52). The game
// plugin checks the exact list against P+'s stage select tables (game-code online_match.cpp);
// this rejects what is never a stage.
bool ValidStageKind(u32 kind);

// The host's gmGlobalModeMelee init block (0x20 bytes): both machines build it from the same
// SESSION, and only the stage variant (+0x14, Smashville's lighting from the console clock) may
// differ. Returns the block to use: `mine` with the host's variant, or `mine` unchanged when the
// host's block differs anywhere else or its variant is out of range (`*rejected` then true; the
// barrier's setup check then ends the session if the setup really differs).
constexpr size_t INIT_BLOCK_SIZE = 0x20;
constexpr size_t INIT_BLOCK_VARIANT = 0x14;
constexpr u8 MAX_STAGE_VARIANT = 0x0F;
std::vector<u8> MergeInitBlock(const std::vector<u8>& mine, const std::vector<u8>& host,
                               bool* rejected);

// The fighter start point table (4 words): the host's must be a permutation of ours.
bool IsPermutation(const std::array<u32, 4>& mine, const std::array<u32, 4>& theirs);

// One gfPadStatus (0x40 bytes, guest byte order) as GekkoNet delivers it for any player. Applied
// to every player's input on both machines (the same bytes give the same result), before the
// pad slots are written into the game:
//  - controller type (+0x3C) outside GameCube/Classic/Wii Remote/Nunchuk -> GameCube;
//  - error (+0x38) outside NONE/NO_CONTROLLER/NOT_READY/TRANSFER -> NONE;
//  - the motion floats (+0x18..+0x2C) not finite or beyond +-1e6 -> 0.
constexpr size_t PAD_SIZE = 0x40;
void SanitizePad(u8* pad);

// Port values (ppom.h PortValues, 0x3C bytes) from the peer: only the flag the game knows
// (PV_TAG) is kept, the rumble byte (local only) and the padding are cleared. The controls layout
// itself is sanitised by the game (online_match.cpp layoutByte), the same on both machines.
template <size_t N>
void SanitizePortValues(std::array<u8, N>& pv)
{
  static_assert(N == 0x3C);
  pv[0] &= 0x01;      // flags: PV_TAG
  pv[1] = 0;          // rumble
  pv[0x39] = pv[0x3A] = pv[0x3B] = 0;  // padding
}

std::string TruncateName(const std::string& name);
}  // namespace Gprb::PeerData
