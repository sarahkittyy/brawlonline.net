// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Which build of the game this Dolphin runs in the P+ online flow (docs/design/pplus-mods.md §2).
// P+ and its mods (Cobalt, P+Ex) boot from the same launcher files and differ in their folder on
// the SD card, so a build is named by what decides its code: that folder, the file Dolphin booted
// and the codesets in the folder (NETPLAY.GCT, NETBOOST.GCT, RSBE01.GCT, BOOST.GCT). The key goes
// to mm in `hello` and in every ticket (`game`): mm pairs and admits to a room only players of one
// build, and names the mods it lets play (server/config/games.json).
//
// The folder is the one the running game loads its files from (the File Patch Code's path), so the
// key is known once the game's plugin is up, a few seconds after boot. The online connection waits
// for it (Rooms.cpp), so that nothing a game asks of mm comes before mm knows its build.

#pragma once

#include <array>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Online::GameBuild
{
// The codesets of a build's folder, in the key's order.
inline constexpr std::array<std::string_view, 4> CODESETS = {"NETPLAY.GCT", "NETBOOST.GCT",
                                                             "RSBE01.GCT", "BOOST.GCT"};
// How long the online connection waits for the game's plugin to come up before it says `hello`
// without a build (a game without the plugin asks mm for nothing).
inline constexpr int HELLO_WAIT_SECONDS = 45;
// The same while a create or join the launcher handed over is waiting: the request is for the
// game's own menus, so it waits for the build itself, and mm never takes a mod's request as P+'s
// because its card was slow to boot. After this the request goes as before (and times out).
inline constexpr int REQUEST_WAIT_SECONDS = 180;

// A boot: forgets the last game's key and notes the file being booted.
void OnBoot(std::string boot_file);
// The game is up (GameBridge found the plugin's block): `game_path` is the File Patch Code's path
// ("/Project+/", "/Cobalt/./"), `card_image` the SD card image. Reads the codesets and the boot
// file once per boot; later calls do nothing.
void Resolve(std::string_view game_path, const std::string& card_image);
// The key ("Cobalt:0123..."), or empty: not known yet, or the game has no folder on the card.
std::string Key();
// True once the key is known or need not be waited for any longer (no plugin `wait_seconds`
// after the boot, or a session that has no plugin: `no_plugin`).
bool ReadyForHello(bool no_plugin, int wait_seconds = HELLO_WAIT_SECONDS);

// Where the plugin's own things go on this build's screens, for a build whose menu art is not
// P+'s (the plugin's layout.h): numbers of the build's entry of mm's games table (`layout`), which
// mm sends in the hello-resp that answers this build's `hello`. GameBridge writes them into the
// plugin's LAYOUT (ppom.h). `set`: which groups the entry has (the plugin keeps P+'s for the
// others); the values in LAYOUT's order: the stage select's turn line (5), the cards' name
// plates (3), the character select's bar (14), its word buttons (4 x 7).
struct Layout
{
  static constexpr u32 SSS_LINE = 1, CARD_PLATES = 2, CSS_BAR = 4, CSS_BUTTONS = 8;
  static constexpr size_t SSS_LINE_AT = 0, CARD_PLATES_AT = 5, CSS_BAR_AT = 8, CSS_BUTTONS_AT = 22;
  static constexpr size_t VALUES = 50;
  u32 set = 0;
  std::array<float, VALUES> values{};
  bool operator==(const Layout&) const = default;
};
// The layout mm gave for this build (an empty one, `set` 0, from an mm that says none).
void SetLayout(const Layout& layout);
// The layout and a serial that changes whenever it does (never 0).
Layout GetLayout(u32* serial);

// The key of a build (pure): "<folder>:<32 hex digits>", the digits from the SHA-256 of
// "boot=<sha256>\n" and "<NAME>=<sha256>\n" per codeset in CODESETS' order ("-" for a file that
// is not there), all lowercase hex. Empty for an empty folder.
std::string MakeKey(std::string_view folder, std::string_view boot_sha256,
                    const std::array<std::string, 4>& codeset_sha256);
// SHA-256 as lowercase hex.
std::string Sha256Hex(const unsigned char* data, size_t size);
}  // namespace Online::GameBuild
