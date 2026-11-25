// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A FAT16/FAT32 SD card image's files, read from the image file (MachineSDCard.h's card checks,
// Online/SpecialAssets.h's Special Versus box). Needs only Common, so the Brawl Online core's
// standalone build (core/standalone) reuses it as is.

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace Machine::SDCard
{
struct FatVolume
{
  u64 bytes_per_sector = 0;
  u64 sectors_per_cluster = 0;
  u64 fat_offset = 0;    // bytes, in the image
  u64 fat_bytes = 0;     // one FAT
  u64 data_offset = 0;   // bytes, in the image: cluster 2
  u64 cluster_count = 0;
  bool fat32 = false;
  u32 root_cluster = 0;      // FAT32: the root directory's first cluster
  u64 root_dir_offset = 0;   // FAT16: the root directory's place and length, in the image
  u64 root_dir_bytes = 0;

  u64 ClusterBytes() const { return bytes_per_sector * sectors_per_cluster; }
};

// A FAT volume at the image's start or in its MBR's first FAT partition, with its first FAT.
struct OpenedFat
{
  FatVolume volume;
  std::vector<u8> fat;

  // The FAT entry of cluster number `n` (2 is the data area's first).
  u32 Entry(u64 n) const;
  bool EndOfChain(u32 e) const { return volume.fat32 ? e >= 0x0FFFFFF8 : e >= 0xFFF8; }
};

std::optional<OpenedFat> OpenFat(File::IOFile& image, u64 image_size);

// One file's bytes from such an image, by its path from the root ("Project+/pf/menu2/x.pac", any
// case, long or short names), or nullopt when there is no such file, it is larger than
// `max_size`, or its cluster chain is broken (docs/design/special-rules.md: the Special Versus
// box's models come from P+'s own menu archive on the player's card).
std::optional<std::vector<u8>> ReadCardFile(File::IOFile& image, u64 image_size,
                                            std::string_view path, u64 max_size);
}  // namespace Machine::SDCard
