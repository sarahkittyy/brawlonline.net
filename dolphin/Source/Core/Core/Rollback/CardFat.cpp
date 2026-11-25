// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/CardFat.h"

#include <algorithm>
#include <array>
#include <string>

#include "Common/StringUtil.h"

namespace Machine::SDCard
{
namespace
{
u16 LE16(const u8* p)
{
  return static_cast<u16>(p[0] | (p[1] << 8));
}

u32 LE32(const u8* p)
{
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
         (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

// A FAT16/FAT32 boot sector at `volume_offset`, checked against the image's size.
std::optional<FatVolume> ParseBootSector(const std::array<u8, 512>& bs, u64 volume_offset,
                                         u64 image_size)
{
  if (bs[510] != 0x55 || bs[511] != 0xAA || (bs[0] != 0xEB && bs[0] != 0xE9))
    return std::nullopt;

  const u64 bps = LE16(&bs[0x0B]);
  const u64 spc = bs[0x0D];
  const u64 reserved = LE16(&bs[0x0E]);
  const u64 num_fats = bs[0x10];
  const u64 root_entries = LE16(&bs[0x11]);
  const u64 total16 = LE16(&bs[0x13]);
  const u64 fat16_size = LE16(&bs[0x16]);
  const u64 total32 = LE32(&bs[0x20]);
  const u64 fat32_size = LE32(&bs[0x24]);

  if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || spc == 0 ||
      (spc & (spc - 1)) != 0 || reserved == 0 || num_fats == 0 || num_fats > 2)
  {
    return std::nullopt;
  }

  const u64 fat_sectors = fat16_size != 0 ? fat16_size : fat32_size;
  const u64 total_sectors = total16 != 0 ? total16 : total32;
  const u64 root_dir_sectors = (root_entries * 32 + bps - 1) / bps;
  const u64 data_start = reserved + num_fats * fat_sectors + root_dir_sectors;
  if (fat_sectors == 0 || total_sectors <= data_start)
    return std::nullopt;

  FatVolume v;
  v.bytes_per_sector = bps;
  v.sectors_per_cluster = spc;
  v.cluster_count = (total_sectors - data_start) / spc;
  // FAT12 (under 4085 clusters) packs its entries in 12 bits: not read, every chunk is in use.
  if (v.cluster_count < 4085)
    return std::nullopt;
  v.fat32 = v.cluster_count >= 65525;
  if (v.fat32 != (fat16_size == 0))
    return std::nullopt;
  v.fat_offset = volume_offset + reserved * bps;
  v.fat_bytes = fat_sectors * bps;
  v.data_offset = volume_offset + data_start * bps;
  v.root_cluster = v.fat32 ? LE32(&bs[0x2C]) : 0;
  v.root_dir_offset = volume_offset + (reserved + num_fats * fat_sectors) * bps;
  v.root_dir_bytes = root_dir_sectors * bps;

  const u64 entry_bytes = v.fat32 ? 4 : 2;
  if ((v.cluster_count + 2) * entry_bytes > v.fat_bytes)
    return std::nullopt;
  if (v.data_offset + v.cluster_count * spc * bps > image_size ||
      v.fat_offset + v.fat_bytes > image_size)
  {
    return std::nullopt;
  }
  return v;
}
}  // namespace

u32 OpenedFat::Entry(u64 n) const
{
  if (volume.fat32)
    return LE32(&fat[n * 4]) & 0x0FFFFFFF;
  return LE16(&fat[n * 2]);
}

std::optional<OpenedFat> OpenFat(File::IOFile& image, u64 image_size)
{
  std::array<u8, 512> sector{};
  if (image_size < sector.size() || !image.Seek(0, File::SeekOrigin::Begin) ||
      !image.ReadArray(&sector))
  {
    return std::nullopt;
  }

  std::optional<FatVolume> volume = ParseBootSector(sector, 0, image_size);
  if (!volume && sector[510] == 0x55 && sector[511] == 0xAA)
  {
    // An MBR: the first FAT partition.
    for (int i = 0; i < 4 && !volume; ++i)
    {
      const u8* entry = &sector[0x1BE + 16 * i];
      const u8 type = entry[4];
      if (type != 0x01 && type != 0x04 && type != 0x06 && type != 0x0B && type != 0x0C &&
          type != 0x0E)
      {
        continue;
      }
      const u64 offset = u64{LE32(&entry[8])} * 512;
      std::array<u8, 512> bs{};
      if (offset == 0 || offset + bs.size() > image_size ||
          !image.Seek(static_cast<s64>(offset), File::SeekOrigin::Begin) || !image.ReadArray(&bs))
      {
        continue;
      }
      volume = ParseBootSector(bs, offset, image_size);
    }
  }
  if (!volume)
    return std::nullopt;

  // Read the first FAT (at most 256 MiB: a 32 GiB card with 512-byte clusters).
  if (volume->fat_bytes > (256ull << 20))
    return std::nullopt;
  OpenedFat opened{*volume, std::vector<u8>(static_cast<size_t>(volume->fat_bytes))};
  if (!image.Seek(static_cast<s64>(volume->fat_offset), File::SeekOrigin::Begin) ||
      !image.ReadBytes(opened.fat.data(), opened.fat.size()))
  {
    return std::nullopt;
  }
  return opened;
}

std::optional<std::vector<u8>> ReadCardFile(File::IOFile& image, u64 image_size,
                                            std::string_view path, u64 max_size)
{
  const std::optional<OpenedFat> opened = OpenFat(image, image_size);
  if (!opened)
    return std::nullopt;
  const FatVolume& v = opened->volume;
  const u64 cluster_bytes = v.ClusterBytes();
  const u64 last_cluster = v.cluster_count + 1;
  auto cluster_offset = [&](u32 c) { return v.data_offset + u64{c - 2} * cluster_bytes; };
  // A chain's clusters, at most `limit` (a loop or a crafted chain ends there).
  auto chain = [&](u32 first, u64 limit) {
    std::vector<u32> out;
    u32 c = first;
    while (c >= 2 && c <= last_cluster && out.size() < limit)
    {
      out.push_back(c);
      const u32 e = opened->Entry(c);
      if (opened->EndOfChain(e))
        break;
      c = e;
    }
    return out;
  };
  auto read_clusters = [&](const std::vector<u32>& clusters, u64 bytes) -> std::optional<std::vector<u8>> {
    std::vector<u8> out(static_cast<size_t>(bytes));
    u64 done = 0;
    for (const u32 c : clusters)
    {
      if (done >= bytes)
        break;
      const u64 n = std::min(cluster_bytes, bytes - done);
      if (!image.Seek(static_cast<s64>(cluster_offset(c)), File::SeekOrigin::Begin) ||
          !image.ReadBytes(out.data() + done, static_cast<size_t>(n)))
      {
        return std::nullopt;
      }
      done += n;
    }
    if (done < bytes)
      return std::nullopt;
    return out;
  };
  const auto lower = [](std::string s) {
    for (char& c : s)
      c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
  };

  // The directory being searched: FAT16's fixed root (cluster 0) or a cluster chain.
  u32 dir_cluster = v.fat32 ? v.root_cluster : 0;
  std::string_view rest = path;
  while (!rest.empty())
  {
    const size_t slash = rest.find('/');
    const std::string want = lower(std::string(rest.substr(0, slash)));
    rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    const bool last = rest.empty();
    std::vector<u8> dir;
    if (dir_cluster == 0)
    {
      if (v.fat32 || v.root_dir_bytes == 0 || v.root_dir_bytes > (2ull << 20))
        return std::nullopt;
      dir.resize(static_cast<size_t>(v.root_dir_bytes));
      if (!image.Seek(static_cast<s64>(v.root_dir_offset), File::SeekOrigin::Begin) ||
          !image.ReadBytes(dir.data(), dir.size()))
      {
        return std::nullopt;
      }
    }
    else
    {
      const auto clusters = chain(dir_cluster, (2ull << 20) / cluster_bytes + 1);
      auto bytes = read_clusters(clusters, clusters.size() * cluster_bytes);
      if (!bytes)
        return std::nullopt;
      dir = std::move(*bytes);
    }
    std::optional<std::pair<u32, u64>> found;  // first cluster, size
    bool found_dir = false;
    std::u16string long_name;
    for (size_t pos = 0; pos + 32 <= dir.size() && !found; pos += 32)
    {
      const u8* e = &dir[pos];
      if (e[0] == 0x00)
        break;
      if (e[0] == 0xE5)
      {
        long_name.clear();
        continue;
      }
      const u8 attr = e[11];
      if (attr == 0x0F)
      {
        std::u16string part;
        bool ended = false;
        for (const auto& [offset, n] : {std::pair{1, 5}, std::pair{14, 6}, std::pair{28, 2}})
        {
          for (int i = 0; i < n && !ended; ++i)
          {
            const char16_t ch = static_cast<char16_t>(LE16(e + offset + 2 * i));
            if (ch == 0x0000 || ch == 0xFFFF)
              ended = true;
            else
              part.push_back(ch);
          }
        }
        long_name = (e[0] & 0x40) ? part : part + long_name;
        continue;
      }
      if (attr & 0x08)
      {
        long_name.clear();
        continue;
      }
      std::string base(reinterpret_cast<const char*>(e), 8);
      std::string ext(reinterpret_cast<const char*>(e + 8), 3);
      base.erase(base.find_last_not_of(' ') + 1);
      ext.erase(ext.find_last_not_of(' ') + 1);
      const std::string short_name = ext.empty() ? base : base + "." + ext;
      const std::string name = long_name.empty() ? short_name : UTF16ToUTF8(long_name);
      long_name.clear();
      if (lower(name) != want && lower(short_name) != want)
        continue;
      const u32 first = (v.fat32 ? (u32{LE16(e + 20)} << 16) : 0) | LE16(e + 26);
      found = std::pair{first, u64{LE32(e + 28)}};
      found_dir = (attr & 0x10) != 0;
    }
    if (!found || found_dir == last)
      return std::nullopt;
    if (!last)
    {
      dir_cluster = found->first;
      if (dir_cluster < 2)
        return std::nullopt;
      continue;
    }
    const u64 size = found->second;
    if (size > max_size)
      return std::nullopt;
    return read_clusters(chain(found->first, size / cluster_bytes + 1), size);
  }
  return std::nullopt;
}
}  // namespace Machine::SDCard
