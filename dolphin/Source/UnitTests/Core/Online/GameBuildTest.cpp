// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The key of a game build (Core/Online/GameBuild.h): the same for tools/gamecode/game_key.py,
// which makes the keys in server/config/games.json.

#include <array>
#include <string>

#include <gtest/gtest.h>

#include "Core/Online/GameBuild.h"

using namespace Online::GameBuild;

namespace
{
std::string Repeat(std::string_view pair)
{
  std::string out;
  for (int i = 0; i < 32; ++i)
    out += pair;
  return out;
}
}  // namespace

TEST(GameBuild, Sha256)
{
  EXPECT_EQ(Sha256Hex(reinterpret_cast<const unsigned char*>("abc"), 3),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Sha256Hex(nullptr, 0),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(GameBuild, TheKeyOfABuild)
{
  // game_key.py make_key("Cobalt", "ab" * 32, ["01" * 32, "", "02" * 32, "03" * 32]).
  const std::array<std::string, 4> codesets = {Repeat("01"), "", Repeat("02"), Repeat("03")};
  EXPECT_EQ(MakeKey("Cobalt", Repeat("ab"), codesets), "Cobalt:0adfa62002f051c4dae5ebbb4c08298e");
  // Every part counts: the folder (the key's own first part), the boot file, each codeset and
  // which one it is.
  EXPECT_EQ(MakeKey("P+Ex", Repeat("ab"), codesets), "P+Ex:0adfa62002f051c4dae5ebbb4c08298e");
  EXPECT_NE(MakeKey("Cobalt", Repeat("ac"), codesets), MakeKey("Cobalt", Repeat("ab"), codesets));
  auto other = codesets;
  std::swap(other[0], other[2]);
  EXPECT_NE(MakeKey("Cobalt", Repeat("ab"), other), MakeKey("Cobalt", Repeat("ab"), codesets));
  other = codesets;
  other[1] = Repeat("04");
  EXPECT_NE(MakeKey("Cobalt", Repeat("ab"), other), MakeKey("Cobalt", Repeat("ab"), codesets));
  // A game without a folder on the card has no key.
  EXPECT_EQ(MakeKey("", Repeat("ab"), codesets), "");
  // A boot file that could not be read is "-", as a missing codeset.
  EXPECT_EQ(MakeKey("Cobalt", "", {}), MakeKey("Cobalt", "", {"", "", "", ""}));
  EXPECT_EQ(MakeKey("Cobalt", "", {}).size(), std::string("Cobalt:").size() + 32);
}
