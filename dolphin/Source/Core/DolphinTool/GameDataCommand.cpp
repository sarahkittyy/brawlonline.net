// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// `gamedata`: what a disc gives the game, whatever file holds it (docs/design/disc-images.md). A
// Wii disc's game is its game partition: where it starts on the disc (the emulated loading times
// depend on it) and its TMD, which holds the hash of the partition's H3 table, which holds the
// hashes of every block. Once every block the game can read checks out against those hashes, the
// game reads exactly what that TMD says, so an ISO, an RVZ, a WBFS, a scrubbed image and an image
// converted back to an ISO of one disc give the same "game_data"; a disc with any of the game's
// data changed or moved does not. The junk filling, the update partition and the image's format
// are not part of it. A GameCube disc has no hashes: its "game_data" is null.
//
// Prints JSON: game_id, revision, platform ("wii" or "gamecube"), and for a Wii disc
// data_offset, tmd_sha1, h3_ok, used_block_errors, unused_block_errors and game_data (the SHA-1
// of "game-data-v1\n", the game ID, the offset and the TMD; null unless everything checked out).

#include "DolphinTool/GameDataCommand.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>

#include <OptionParser.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Swap.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeVerifier.h"
#include "UICommon/UICommon.h"

namespace DolphinTool
{
namespace
{
std::string Hex(const Common::SHA1::Digest& digest)
{
  return fmt::format("{:02x}", fmt::join(digest, ""));
}
}  // namespace

int GameDataCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: gamedata [options]...");

  parser.add_option("-u", "--user")
      .type("string")
      .action("store")
      .help("User folder path, required for temporary processing files. "
            "Will be automatically created if this option is not set.")
      .set_default("");

  parser.add_option("-i", "--input")
      .type("string")
      .action("store")
      .help("Path to disc image FILE.")
      .metavar("FILE");

  parser.add_option("-p", "--progress")
      .action("store_true")
      .help("Print \"progress <bytes done> <bytes>\" lines on stderr while the disc is read");

  const optparse::Values& options = parser.parse_args(args);

  UICommon::SetUserDirectory(options["user"]);
  UICommon::Init();

  if (!options.is_set("input"))
  {
    fmt::print(std::cerr, "Error: No input set\n");
    return EXIT_FAILURE;
  }
  const bool progress = options.is_set_by_user("progress");

  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(options["input"]);
  if (!volume || !DiscIO::IsDisc(volume->GetVolumeType()))
  {
    fmt::print(std::cerr, "Error: Unable to open disc image\n");
    return EXIT_FAILURE;
  }

  picojson::object json;
  const std::string game_id = volume->GetGameID();
  json["game_id"] = picojson::value(game_id);
  if (const std::optional<u16> revision = volume->GetRevision())
    json["revision"] = picojson::value(static_cast<double>(*revision));
  const bool wii = volume->GetVolumeType() == DiscIO::Platform::WiiDisc;
  json["platform"] = picojson::value(wii ? "wii" : "gamecube");
  json["game_data"] = picojson::value();

  if (wii)
  {
    const DiscIO::Partition partition = volume->GetGamePartition();
    const std::vector<u8>& tmd = volume->GetTMD(partition).GetBytes();
    const bool hashes = volume->HasWiiHashes();
    const bool h3_ok = hashes && volume->CheckH3TableIntegrity(partition);
    json["data_offset"] = picojson::value(static_cast<double>(partition.offset));
    json["tmd_sha1"] = picojson::value(Hex(Common::SHA1::CalculateDigest(tmd)));
    json["h3_ok"] = picojson::value(h3_ok);

    DiscIO::VolumeVerifier verifier(*volume, false, {});
    verifier.Start();
    int last_percent = -1;
    while (verifier.GetBytesProcessed() != verifier.GetTotalBytes())
    {
      verifier.Process();
      const u64 done = verifier.GetBytesProcessed(), total = verifier.GetTotalBytes();
      const int percent = total ? static_cast<int>(done * 100 / total) : 100;
      if (progress && percent != last_percent)
      {
        last_percent = percent;
        fmt::print(std::cerr, "progress {} {}\n", done, total);
        std::cerr.flush();
      }
    }
    verifier.Finish();

    const std::optional<DiscIO::VolumeVerifier::BlockErrors> errors =
        verifier.GetBlockErrors(partition);
    json["used_block_errors"] =
        errors ? picojson::value(static_cast<double>(errors->used)) : picojson::value();
    json["unused_block_errors"] =
        errors ? picojson::value(static_cast<double>(errors->unused)) : picojson::value();

    if (h3_ok && errors && errors->used == 0 && !tmd.empty())
    {
      const auto context = Common::SHA1::CreateContext();
      const std::string_view tag = "game-data-v1\n";
      context->Update(reinterpret_cast<const u8*>(tag.data()), tag.size());
      context->Update(reinterpret_cast<const u8*>(game_id.data()), game_id.size());
      const u64 offset_be = Common::swap64(partition.offset);
      context->Update(reinterpret_cast<const u8*>(&offset_be), sizeof(offset_be));
      context->Update(tmd.data(), tmd.size());
      json["game_data"] = picojson::value(Hex(context->Finish()));
    }
  }

  std::cout << picojson::value(json) << '\n';
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
