// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// P+'s Special Versus box for the room's rules menu (docs/design/special-rules.md §5). Its models
// and textures are only in P+'s main-menu archive (/Project+/pf/menu2/mu_menumain.pac, LZ11,
// 12.8 MB unpacked), which the game never loads on the character select. Dolphin reads that file
// from the player's own SD card once, unpacks it on a thread of its own and keeps only the box's
// ARC entries (its textures, models MenMainSpecial0001-0010, the descriptions' msbin): about
// 330 KB, the bundle the game asks for (GameBridge CMD_SPECIAL_ASSETS) and gets copied into a
// buffer of its own. Nothing is shipped; a card without the file gets no box.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Online::SpecialAssets
{
enum class State : u8
{
  None = 0,         // not asked for yet
  Loading = 1,      // being built
  Ready = 2,
  Unavailable = 3,  // no card, no file, or not the archive this expects (`Error`)
};

// The bundle (little work for the game, big-endian as the game reads it):
//   +0 "SPBX", +4 total size (u32), +8 entry count (u32), +0xC 0,
//   +0x10 per entry {role u16, ARC type u16, offset u32 (from the bundle's start), size u32, 0 u32},
//   then the entries' bytes, each 32-byte aligned (the brres the game binds in place).
// Roles: 0 the textures, 1-10 the models MenMainSpecial0001-0010, 11 the descriptions' msbin.
constexpr u32 MAGIC = 0x53504258;  // "SPBX"
constexpr u32 HEADER = 0x10, ENTRY = 0x10;
enum Role : u16
{
  ROLE_TEXTURES = 0,
  ROLE_MODEL_FIRST = 1,  // MenMainSpecial0001_TopN
  ROLE_MODEL_LAST = 10,  // MenMainSpecial0010_TopN
  ROLE_MESSAGES = 11,
  ROLE_COUNT = 12,
};
// The most the game's buffer may need (the bundle is about 330 KB with P+ v3.2).
constexpr u32 MAX_BUNDLE = 0x80000;
// The largest archive read from the card.
constexpr u64 MAX_ARCHIVE = 32ull << 20;

// Starts building the bundle from the SD card image at `image_path` (once; later calls do nothing
// while it is Loading or Ready, and build again a few seconds after it was Unavailable). Returns
// the state now.
State Request(const std::string& image_path);
State GetState();
// The bundle, once Ready (empty otherwise).
std::vector<u8> Bundle();
// Why it is Unavailable, for the log.
std::string Error();

// The steps, pure (unit-tested): LZ11 (Nintendo's "LZ77 type 0x11"), and the bundle from an
// unpacked archive (nullopt with `why` when it is not P+'s main-menu archive).
std::optional<std::vector<u8>> DecompressLz11(std::span<const u8> in, u64 max_out);
std::optional<std::vector<u8>> BuildBundle(std::span<const u8> archive, std::string* why);
}  // namespace Online::SpecialAssets
