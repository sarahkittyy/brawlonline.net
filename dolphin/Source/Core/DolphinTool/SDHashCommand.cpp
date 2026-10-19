// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// `sdhash`: an SD card image's root as Other Games rooms compare cards (docs/design/brawl-mods.md
// §6.5): BLAKE2b-256 of its manifest, the chunks no file uses read as zero, so two cards that
// play the same have the same root. The launcher shows it on a game and in rooms. The hashes go
// to the User folder's cache (`-u`, the netplay Dolphin's), so a session started on the card
// later reads them from there instead of the whole card again. With `--listing`, it also writes
// the card's listing (its manifest and files), which a host's launcher puts on the server.
//
// `sdcompare`: what a joiner would download of a room's card, from the host's listing and the
// joiner's own card, so its launcher can ask its player before Dolphin starts.

#include "DolphinTool/SDHashCommand.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>

#include <OptionParser.h>
#include <fmt/ostream.h>
#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Rollback/MachineSDCard.h"
#include "UICommon/UICommon.h"

namespace DolphinTool
{
namespace
{
// Prints "progress <bytes done> <bytes>" on stderr at each percent, when asked to.
Machine::SDCard::HashProgress ProgressPrinter(bool enabled)
{
  return [enabled, last_percent = -1](u64 done, u64 total) mutable {
    const int percent = total ? static_cast<int>(done * 100 / total) : 100;
    if (enabled && percent != last_percent)
    {
      last_percent = percent;
      fmt::print(std::cerr, "progress {} {}\n", done, total);
      std::cerr.flush();
    }
  };
}

void AddProgressOption(optparse::OptionParser& parser)
{
  parser.add_option("-p", "--progress")
      .action("store_true")
      .help("Print \"progress <bytes done> <bytes>\" lines on stderr while the card is read");
}
}  // namespace

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
  parser.add_option("-l", "--listing")
      .type("string")
      .action("store")
      .help("Also write the card's listing (its manifest and files) to this file")
      .metavar("FILE");
  AddProgressOption(parser);

  const optparse::Values& options = parser.parse_args(args);
  UICommon::SetUserDirectory(options["user"]);
  UICommon::Init();

  if (!options.is_set("input"))
  {
    fmt::print(std::cerr, "Error: No input set\n");
    return EXIT_FAILURE;
  }
  const std::string input = options["input"];
  const auto manifest = Machine::SDCard::BuildManifest(input, nullptr,
                                                       ProgressPrinter(options.is_set("progress")));
  if (!manifest)
  {
    fmt::print(std::cerr, "Error: could not read the SD card image\n");
    return EXIT_FAILURE;
  }
  if (options.is_set("listing"))
  {
    // Not a FAT volume this can read: the listing has no files (the sizes still tell).
    std::vector<Machine::SDCard::CardFile> files;
    if (File::IOFile image(input, "rb"); image)
    {
      if (auto listed = Machine::SDCard::ListCardFiles(image, image.GetSize()))
        files = std::move(*listed);
    }
    const std::vector<u8> listing = Machine::SDCard::EncodeCardListing(*manifest, files);
    const std::string path = options["listing"];
    if (!File::WriteStringToFile(path + ".tmp",
                                 std::string(listing.begin(), listing.end())) ||
        !File::Rename(path + ".tmp", path))
    {
      fmt::print(std::cerr, "Error: could not write {}\n", path);
      return EXIT_FAILURE;
    }
  }
  fmt::print(std::cout, "{}\n", Machine::SDCard::RootHex(Machine::SDCard::ManifestRoot(*manifest)));
  return EXIT_SUCCESS;
}

int SDCompareCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: sdcompare [options]...\n\n"
               "Prints, as JSON, what a joiner with the card -i would download of the card in the\n"
               "listing -l: {\"bytes\", \"files\", \"names\"} (the largest files that differ).");
  parser.add_option("-u", "--user")
      .action("store")
      .help("User folder path, whose cache keeps the card's hashes");
  parser.add_option("-i", "--input")
      .type("string")
      .action("store")
      .help("Path to this player's SD card image (missing: everything differs)")
      .metavar("FILE");
  parser.add_option("-l", "--listing")
      .type("string")
      .action("store")
      .help("The host's card listing (sdhash --listing)")
      .metavar("FILE");
  parser.add_option("-r", "--root")
      .type("string")
      .action("store")
      .help("The room's card root: a listing of any other card is refused");
  parser.add_option("-n", "--names")
      .type("int")
      .action("store")
      .set_default(50)
      .help("How many of the differing files to name, largest first (default 50)");
  AddProgressOption(parser);

  const optparse::Values& options = parser.parse_args(args);
  UICommon::SetUserDirectory(options["user"]);
  UICommon::Init();

  if (!options.is_set("listing") || !options.is_set("root"))
  {
    fmt::print(std::cerr, "Error: -l and -r are needed\n");
    return EXIT_FAILURE;
  }
  // The listing came from the server (a stranger's bytes): checked whole, and against the root.
  std::string bytes;
  if (!File::ReadFileToString(options["listing"], bytes))
  {
    fmt::print(std::cerr, "Error: could not read {}\n", options["listing"]);
    return EXIT_FAILURE;
  }
  const std::optional<Machine::SDCard::CardListing> listing = Machine::SDCard::DecodeCardListing(
      std::span(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
  if (!listing)
  {
    fmt::print(std::cerr, "Error: the listing is not valid\n");
    return EXIT_FAILURE;
  }
  if (Machine::SDCard::RootHex(Machine::SDCard::ManifestRoot(listing->manifest)) !=
      options["root"])
  {
    fmt::print(std::cerr, "Error: the listing is of another card\n");
    return EXIT_FAILURE;
  }

  // This player's card (the cache, or a full read); none or unreadable: everything differs, as
  // in the session.
  Machine::SDCard::ImageHashes own;
  if (options.is_set("input") && File::Exists(options["input"]))
  {
    if (auto hashes = Machine::SDCard::HashImage(options["input"], nullptr,
                                                 ProgressPrinter(options.is_set("progress"))))
    {
      own = std::move(*hashes);
    }
  }
  const Machine::SDCard::CardDifference diff =
      Machine::SDCard::CompareWithManifest(listing->manifest, own);
  const std::vector<const Machine::SDCard::CardFile*> touched =
      Machine::SDCard::FilesTouching(listing->files, diff.chunks);

  picojson::object out;
  out["bytes"] = picojson::value(static_cast<double>(diff.bytes));
  out["files"] = picojson::value(static_cast<double>(touched.size()));
  // The list was empty (not a FAT volume): the files are not known.
  out["filesKnown"] = picojson::value(!listing->files.empty());
  picojson::array names;
  const int wanted = options.get("names");
  const size_t max_names = static_cast<size_t>(std::max(0, wanted));
  for (size_t i = 0; i < touched.size() && i < max_names; ++i)
    names.emplace_back(touched[i]->path);
  out["names"] = picojson::value(names);
  fmt::print(std::cout, "{}\n", picojson::value(out).serialize());
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
