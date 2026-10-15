// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// `sdhash`: an SD card image's root as Other Games rooms compare cards (docs/design/brawl-mods.md
// §6.5): BLAKE2b-256 of its manifest, the chunks no file uses read as zero, so two cards that
// play the same have the same root. The launcher shows it on a game and in rooms. The hashes go
// to the User folder's cache (`-u`, the netplay Dolphin's), so a session started on the card
// later reads them from there instead of the whole card again.

#include "DolphinTool/SDHashCommand.h"

#include <cstdlib>
#include <iostream>

#include <OptionParser.h>
#include <fmt/ostream.h>

#include "Core/Rollback/MachineSDCard.h"
#include "UICommon/UICommon.h"

namespace DolphinTool
{
int SDHashCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: sdhash [options]...");
  parser.add_option("-u", "--user")
      .action("store")
      .help("User folder path, whose cache keeps the card's hashes");
  parser.add_option("-i", "--input")
      .type("string")
      .action("store")
      .help("Path to the SD card image")
      .metavar("FILE");
  parser.add_option("-p", "--progress")
      .action("store_true")
      .help("Print \"progress <bytes done> <bytes>\" lines on stderr while the card is read");

  const optparse::Values& options = parser.parse_args(args);
  UICommon::SetUserDirectory(options["user"]);
  UICommon::Init();

  if (!options.is_set("input"))
  {
    fmt::print(std::cerr, "Error: No input set\n");
    return EXIT_FAILURE;
  }
  const bool progress = options.is_set("progress");
  int last_percent = -1;
  const auto manifest = Machine::SDCard::BuildManifest(
      options["input"], nullptr, [&](u64 done, u64 total) {
        const int percent = total ? static_cast<int>(done * 100 / total) : 100;
        if (progress && percent != last_percent)
        {
          last_percent = percent;
          fmt::print(std::cerr, "progress {} {}\n", done, total);
          std::cerr.flush();
        }
      });
  if (!manifest)
  {
    fmt::print(std::cerr, "Error: could not read the SD card image\n");
    return EXIT_FAILURE;
  }
  fmt::print(std::cout, "{}\n", Machine::SDCard::RootHex(Machine::SDCard::ManifestRoot(*manifest)));
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
