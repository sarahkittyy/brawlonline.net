// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/MachineSDCard.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

#include <fmt/format.h>
#include <lzo/lzo1x.h>
#include <monocypher.h>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/SFMLHelper.h"
#include "Common/StringUtil.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/NetPlayCommon.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Machine::SDCard
{
namespace
{
constexpr u8 MANIFEST_ENTRY_ZERO = 0;
constexpr u8 MANIFEST_ENTRY_DATA = 1;
constexpr size_t MANIFEST_ENTRY_MAX_BYTES = 1 + sizeof(Hash);

constexpr u32 LZO_IN_LEN = 64 * 1024;
constexpr u32 LZO_OUT_LEN = LZO_IN_LEN + (LZO_IN_LEN / 16) + 64 + 3;

// Chunks a joiner asks for at once, across all its waiting reads and its background stream (a
// player queues at most CardServer::MAX_QUEUED_PER_PLAYER per player).
constexpr size_t MAX_OUTSTANDING_REQUESTS = 128;
// A chunk the players sent wrong this many times counts as unavailable.
constexpr int MAX_BAD_DELIVERIES = 3;

constexpr u32 HASH_CACHE_MAGIC = 0x4453534D;  // "MSSD"
// 2: BLAKE2b chunk hashes (1 was XXH3-128).
constexpr u32 HASH_CACHE_VERSION = 2;

u64 NowMs()
{
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

bool IsAllZero(std::span<const u8> data)
{
  return std::ranges::all_of(data, [](u8 b) { return b == 0; });
}

// LZO-compressed data as NetPlay::CompressBufferIntoPacket (Dolphin's) writes it, read without trusting the
// sender: the declared size is capped, and so is every block.
std::optional<std::vector<u8>> ReadCompressed(sf::Packet& packet, u64 max_size)
{
  const u64 size = Common::PacketReadU64(packet);
  if (!packet || size > max_size)
    return std::nullopt;

  std::vector<u8> out;
  out.reserve(static_cast<size_t>(size));
  if (size == 0)
    return out;

  std::vector<u8> in_buffer(LZO_OUT_LEN);
  std::vector<u8> block(LZO_IN_LEN);
  while (true)
  {
    u32 cur_len = 0;
    packet >> cur_len;
    if (!packet)
      return std::nullopt;
    if (cur_len == 0)
      break;
    if (cur_len > in_buffer.size())
      return std::nullopt;
    for (u32 i = 0; i < cur_len; ++i)
      packet >> in_buffer[i];
    if (!packet)
      return std::nullopt;

    lzo_uint new_len = block.size();
    if (lzo1x_decompress_safe(in_buffer.data(), cur_len, block.data(), &new_len, nullptr) !=
            LZO_E_OK ||
        new_len > size - out.size())
    {
      return std::nullopt;
    }
    out.insert(out.end(), block.begin(), block.begin() + new_len);
  }

  if (out.size() != size)
    return std::nullopt;
  return out;
}

u16 LE16(const u8* p)
{
  return static_cast<u16>(p[0] | (p[1] << 8));
}

u32 LE32(const u8* p)
{
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
         (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

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

std::string HashCachePath(const std::string& image_path)
{
  const XXH64_hash_t key = XXH3_64bits(image_path.data(), image_path.size());
  return fmt::format("{}MachineSD/{:016x}.hashes", File::GetUserPath(D_CACHE_IDX), key);
}

std::optional<s64> ModificationTime(const std::string& path)
{
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(StringToPath(path), ec);
  if (ec)
    return std::nullopt;
  return static_cast<s64>(t.time_since_epoch().count());
}

std::optional<ImageHashes> LoadHashCache(const std::string& image_path, u64 size, s64 mtime)
{
  File::IOFile file(HashCachePath(image_path), "rb");
  if (!file)
    return std::nullopt;

  u32 magic = 0, version = 0, chunk_size = 0, count = 0;
  u64 cached_size = 0;
  s64 cached_mtime = 0;
  if (!file.ReadArray(&magic, 1) || !file.ReadArray(&version, 1) ||
      !file.ReadArray(&cached_size, 1) || !file.ReadArray(&cached_mtime, 1) ||
      !file.ReadArray(&chunk_size, 1) || !file.ReadArray(&count, 1))
  {
    return std::nullopt;
  }
  if (magic != HASH_CACHE_MAGIC || version != HASH_CACHE_VERSION || cached_size != size ||
      cached_mtime != mtime || chunk_size != CHUNK_SIZE || count != ChunkCountFor(size))
  {
    return std::nullopt;
  }

  std::vector<u8> zero(count);
  ImageHashes result;
  result.size = size;
  result.hashes.resize(count);
  if (!file.ReadBytes(zero.data(), zero.size()) ||
      !file.ReadBytes(result.hashes.data(), result.hashes.size() * sizeof(Hash)))
  {
    return std::nullopt;
  }
  result.zero.resize(count);
  for (u32 i = 0; i < count; ++i)
    result.zero[i] = zero[i] != 0;
  return result;
}

void SaveHashCache(const std::string& image_path, s64 mtime, const ImageHashes& hashes)
{
  const std::string path = HashCachePath(image_path);
  if (!File::CreateFullPath(path))
    return;
  const std::string temp = path + ".tmp";
  {
    File::IOFile file(temp, "wb");
    if (!file)
      return;
    const u32 header[2] = {HASH_CACHE_MAGIC, HASH_CACHE_VERSION};
    const u32 chunk_size = static_cast<u32>(CHUNK_SIZE);
    const u32 count = static_cast<u32>(hashes.hashes.size());
    std::vector<u8> zero(count);
    for (u32 i = 0; i < count; ++i)
      zero[i] = hashes.zero[i] ? 1 : 0;
    if (!file.WriteArray(header, 2) || !file.WriteArray(&hashes.size, 1) ||
        !file.WriteArray(&mtime, 1) || !file.WriteArray(&chunk_size, 1) ||
        !file.WriteArray(&count, 1) || !file.WriteBytes(zero.data(), zero.size()) ||
        !file.WriteBytes(hashes.hashes.data(), hashes.hashes.size() * sizeof(Hash)))
    {
      file.Close();
      File::Delete(temp);
      return;
    }
  }
  File::Rename(temp, path);
}
}  // namespace

u64 Manifest::ChunkBytes(u32 index) const
{
  const u64 start = u64{index} * CHUNK_SIZE;
  return start >= size ? 0 : std::min(CHUNK_SIZE, size - start);
}

u32 ChunkCountFor(u64 size)
{
  return static_cast<u32>((size + CHUNK_SIZE - 1) / CHUNK_SIZE);
}

Hash HashChunk(std::span<const u8> data)
{
  Hash out;
  crypto_blake2b(out.data(), out.size(), data.data(), data.size());
  return out;
}

namespace
{
// A FAT volume at the image's start or in its MBR's first FAT partition, with its first FAT.
struct OpenedFat
{
  FatVolume volume;
  std::vector<u8> fat;

  // The FAT entry of cluster number `n` (2 is the data area's first).
  u32 Entry(u64 n) const
  {
    if (volume.fat32)
      return LE32(&fat[n * 4]) & 0x0FFFFFFF;
    return LE16(&fat[n * 2]);
  }
  bool EndOfChain(u32 e) const { return volume.fat32 ? e >= 0x0FFFFFF8 : e >= 0xFFF8; }
};

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
}  // namespace

std::optional<std::vector<bool>> FindFreeChunks(File::IOFile& image, u64 image_size)
{
  const std::optional<OpenedFat> opened = OpenFat(image, image_size);
  if (!opened)
    return std::nullopt;
  const FatVolume* volume = &opened->volume;
  const u64 cluster_bytes = volume->ClusterBytes();
  auto cluster_free = [&](u64 cluster) { return opened->Entry(cluster + 2) == 0; };

  const u32 count = ChunkCountFor(image_size);
  std::vector<bool> free(count, false);
  const u64 data_end = volume->data_offset + volume->cluster_count * cluster_bytes;
  for (u32 c = 0; c < count; ++c)
  {
    const u64 start = u64{c} * CHUNK_SIZE;
    const u64 end = std::min(start + CHUNK_SIZE, image_size);
    if (start < volume->data_offset || end > data_end)
      continue;
    const u64 first = (start - volume->data_offset) / cluster_bytes;
    const u64 last = (end - 1 - volume->data_offset) / cluster_bytes;
    bool all_free = true;
    for (u64 cl = first; cl <= last && all_free; ++cl)
      all_free = cluster_free(cl);
    free[c] = all_free;
  }
  return free;
}

namespace
{
// Appends chunk `c` to `runs` (chunks come in ascending order within a file, mostly).
void AddChunk(std::vector<std::pair<u32, u32>>* runs, u32 c)
{
  if (!runs->empty())
  {
    auto& [first, count] = runs->back();
    if (c >= first && c < first + count)
      return;
    if (c == first + count)
    {
      ++count;
      return;
    }
  }
  runs->emplace_back(c, 1);
}

bool ValidName(std::string_view name)
{
  if (name.empty() || name == "." || name == "..")
    return false;
  return std::ranges::none_of(name, [](char ch) {
    const auto u = static_cast<unsigned char>(ch);
    return u < 0x20 || ch == '/' || ch == '\\';
  });
}
}  // namespace

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

namespace
{
// Each file of a FAT volume (OpenFat's), long names included: `on_file` hears its path, size and
// cluster chain, and returns false to stop. At most MAX_CARD_FILES files.
void ForEachCardFile(
    File::IOFile& image, const OpenedFat& fat,
    const std::function<bool(std::string&& path, u64 size, const std::vector<u32>& clusters)>&
        on_file)
{
  const OpenedFat* opened = &fat;
  const FatVolume& v = opened->volume;
  const u64 cluster_bytes = v.ClusterBytes();
  const u64 last_cluster = v.cluster_count + 1;
  // A chain of at most `limit` clusters: a loop or a crafted chain ends there.
  auto chain = [&](u32 first, u64 limit, std::vector<u32>* out) {
    out->clear();
    u32 c = first;
    while (c >= 2 && c <= last_cluster && out->size() < limit)
    {
      out->push_back(c);
      const u32 e = opened->Entry(c);
      if (opened->EndOfChain(e))
        break;
      c = e;
    }
  };
  auto cluster_offset = [&](u32 c) { return v.data_offset + u64{c - 2} * cluster_bytes; };

  struct Dir
  {
    u32 cluster;  // 0: FAT16's fixed root directory
    std::string path;
    int depth;
  };
  std::vector<Dir> pending{{v.fat32 ? v.root_cluster : 0, "", 0}};
  u32 file_count = 0;
  std::vector<u32> clusters;
  std::vector<u8> dir;
  u32 dirs = 0;
  // A directory holds at most 65536 entries (2 MiB).
  const u64 max_dir_clusters = (2ull << 20) / cluster_bytes + 1;
  bool stop = false;
  while (!pending.empty() && !stop && file_count < MAX_CARD_FILES && ++dirs <= 65536)
  {
    const Dir d = std::move(pending.back());
    pending.pop_back();
    dir.clear();
    if (d.cluster == 0)
    {
      if (v.fat32 || v.root_dir_bytes == 0 || v.root_dir_bytes > (2ull << 20))
        continue;
      dir.resize(static_cast<size_t>(v.root_dir_bytes));
      if (!image.Seek(static_cast<s64>(v.root_dir_offset), File::SeekOrigin::Begin) ||
          !image.ReadBytes(dir.data(), dir.size()))
      {
        continue;
      }
    }
    else
    {
      chain(d.cluster, max_dir_clusters, &clusters);
      dir.resize(clusters.size() * static_cast<size_t>(cluster_bytes));
      bool ok = true;
      for (size_t i = 0; i < clusters.size() && ok; ++i)
      {
        ok = image.Seek(static_cast<s64>(cluster_offset(clusters[i])), File::SeekOrigin::Begin) &&
             image.ReadBytes(dir.data() + i * cluster_bytes, cluster_bytes);
      }
      if (!ok)
        continue;
    }

    std::u16string long_name;
    for (size_t pos = 0; pos + 32 <= dir.size(); pos += 32)
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
        // A long name's piece: 13 UTF-16 characters; the one marked 0x40 holds the name's end.
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
      std::string name;
      if (!long_name.empty())
      {
        name = UTF16ToUTF8(long_name);
      }
      else
      {
        std::string base(reinterpret_cast<const char*>(e), 8);
        std::string ext(reinterpret_cast<const char*>(e + 8), 3);
        if (static_cast<u8>(base[0]) == 0x05)
          base[0] = static_cast<char>(0xE5);
        base.erase(base.find_last_not_of(' ') + 1);
        ext.erase(ext.find_last_not_of(' ') + 1);
        name = ext.empty() ? base : base + "." + ext;
      }
      long_name.clear();
      if (!ValidName(name))
        continue;
      const std::string path = d.path.empty() ? name : d.path + "/" + name;
      if (path.size() > MAX_CARD_PATH)
        continue;
      const u32 first = (v.fat32 ? (u32{LE16(e + 20)} << 16) : 0) | LE16(e + 26);
      if (attr & 0x10)
      {
        if (d.depth < 32 && first >= 2)
          pending.push_back({first, path, d.depth + 1});
        continue;
      }
      const u64 size = LE32(e + 28);
      chain(first, size / cluster_bytes + 1, &clusters);
      if (!on_file(std::string(path), size, clusters) || ++file_count >= MAX_CARD_FILES)
      {
        stop = true;
        break;
      }
    }
  }
}
}  // namespace

std::optional<std::vector<CardFile>> ListCardFiles(File::IOFile& image, u64 image_size)
{
  const std::optional<OpenedFat> opened = OpenFat(image, image_size);
  if (!opened)
    return std::nullopt;
  const FatVolume& v = opened->volume;
  const u64 cluster_bytes = v.ClusterBytes();
  std::vector<CardFile> files;
  ForEachCardFile(image, *opened,
                  [&](std::string&& path, u64 size, const std::vector<u32>& clusters) {
                    CardFile file;
                    file.path = std::move(path);
                    file.size = size;
                    for (const u32 c : clusters)
                    {
                      const u64 start = v.data_offset + u64{c - 2} * cluster_bytes;
                      for (u64 chunk = start / CHUNK_SIZE;
                           chunk <= (start + cluster_bytes - 1) / CHUNK_SIZE; ++chunk)
                      {
                        AddChunk(&file.runs, static_cast<u32>(chunk));
                      }
                    }
                    files.push_back(std::move(file));
                    return true;
                  });
  return files;
}

std::vector<u8> EncodeFileList(const std::vector<CardFile>& files)
{
  std::vector<u8> raw;
  auto put32 = [&](u32 x) {
    for (int i = 0; i < 4; ++i)
      raw.push_back(static_cast<u8>(x >> (8 * i)));
  };
  put32(static_cast<u32>(files.size()));
  for (const CardFile& f : files)
  {
    put32(static_cast<u32>(f.path.size()));
    raw.insert(raw.end(), f.path.begin(), f.path.end());
    put32(static_cast<u32>(f.size));
    put32(static_cast<u32>(f.size >> 32));
    put32(static_cast<u32>(f.runs.size()));
    for (const auto& [first, count] : f.runs)
    {
      put32(first);
      put32(count);
    }
  }
  return raw;
}

void WriteFileList(const std::vector<CardFile>& files, sf::Packet& packet)
{
  NetPlay::CompressBufferIntoPacket(EncodeFileList(files), packet);
}

std::optional<std::vector<CardFile>> ReadFileList(sf::Packet& packet, u32 chunk_count)
{
  const std::optional<std::vector<u8>> raw = ReadCompressed(packet, MAX_FILE_LIST_BYTES);
  if (!raw)
    return std::nullopt;
  return DecodeFileList(*raw, chunk_count);
}

std::optional<std::vector<CardFile>> DecodeFileList(std::span<const u8> bytes, u32 chunk_count)
{
  if (bytes.size() > MAX_FILE_LIST_BYTES)
    return std::nullopt;
  size_t pos = 0;
  auto get32 = [&](u32* x) {
    if (bytes.size() - pos < 4)
      return false;
    *x = LE32(bytes.data() + pos);
    pos += 4;
    return true;
  };
  u32 count;
  if (!get32(&count) || count > MAX_CARD_FILES)
    return std::nullopt;
  std::vector<CardFile> files(count);
  for (CardFile& f : files)
  {
    u32 len, lo, hi, runs;
    if (!get32(&len) || len == 0 || len > MAX_CARD_PATH || bytes.size() - pos < len)
      return std::nullopt;
    f.path.assign(reinterpret_cast<const char*>(bytes.data() + pos), len);
    pos += len;
    if (std::ranges::any_of(f.path, [](char ch) { return static_cast<unsigned char>(ch) < 0x20; }))
      return std::nullopt;
    if (!get32(&lo) || !get32(&hi) || !get32(&runs) || runs > chunk_count ||
        (bytes.size() - pos) / 8 < runs)
    {
      return std::nullopt;
    }
    f.size = (u64{hi} << 32) | lo;
    f.runs.resize(runs);
    for (auto& [first, n] : f.runs)
    {
      get32(&first);
      get32(&n);
      if (n == 0 || first >= chunk_count || n > chunk_count - first)
        return std::nullopt;
    }
  }
  if (pos != bytes.size())
    return std::nullopt;
  return files;
}

std::vector<const CardFile*> FilesTouching(const std::vector<CardFile>& files,
                                           std::span<const u32> chunks)
{
  // Each file with how many of `chunks` it is in.
  std::vector<std::pair<const CardFile*, u64>> touched;
  for (const CardFile& f : files)
  {
    u64 count = 0;
    for (const auto& [first, n] : f.runs)
    {
      const auto from = std::ranges::lower_bound(chunks, first);
      const auto to = std::ranges::lower_bound(chunks, first + n);
      count += static_cast<u64>(to - from);
    }
    if (count > 0)
      touched.emplace_back(&f, count);
  }
  // A file that only shares a chunk with another that differs (or with a directory) comes after
  // the files whose own chunks differ (bug report #265: a card made again from its folder named
  // a movie next to a directory first).
  std::ranges::stable_sort(touched, [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first->size > b.first->size;
  });
  std::vector<const CardFile*> out;
  out.reserve(touched.size());
  for (const auto& [f, count] : touched)
    out.push_back(f);
  return out;
}

std::optional<ImageHashes> CachedImageHashes(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file)
    return std::nullopt;
  const u64 size = file.GetSize();
  const std::optional<s64> mtime = ModificationTime(path);
  if (size == 0 || size > MAX_CARD_SIZE || !mtime)
    return std::nullopt;
  return LoadHashCache(path, size, *mtime);
}

std::optional<ImageHashes> HashImage(const std::string& path, const std::atomic<bool>* cancel,
                                     const HashProgress& progress)
{
  File::IOFile file(path, "rb");
  if (!file)
    return std::nullopt;
  const u64 size = file.GetSize();
  if (size == 0 || size > MAX_CARD_SIZE)
    return std::nullopt;

  const std::optional<s64> mtime = ModificationTime(path);
  if (mtime)
  {
    if (auto cached = LoadHashCache(path, size, *mtime))
      return cached;
  }

  const u32 count = ChunkCountFor(size);
  ImageHashes result;
  result.size = size;
  result.hashes.resize(count);
  result.zero.resize(count);
  std::vector<u8> zero(count);

  // BLAKE2b takes about a second a gigabyte on one core: a read's chunks are hashed on several.
  constexpr u32 CHUNKS_PER_READ = 512;
  const u32 workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  std::vector<u8> buffer(CHUNKS_PER_READ * CHUNK_SIZE);
  for (u32 c = 0; c < count; c += CHUNKS_PER_READ)
  {
    if (cancel && cancel->load())
      return std::nullopt;
    const u64 start = u64{c} * CHUNK_SIZE;
    const u64 len = std::min<u64>(buffer.size(), size - start);
    if (!file.ReadBytes(buffer.data(), len))
      return std::nullopt;
    const u32 n = static_cast<u32>((len + CHUNK_SIZE - 1) / CHUNK_SIZE);
    auto hash_range = [&](u32 from, u32 to) {
      for (u32 i = from; i < to; ++i)
      {
        const u64 off = u64{i} * CHUNK_SIZE;
        const std::span<const u8> chunk(buffer.data() + off, std::min(CHUNK_SIZE, len - off));
        result.hashes[c + i] = HashChunk(chunk);
        zero[c + i] = IsAllZero(chunk) ? 1 : 0;
      }
    };
    std::vector<std::thread> threads;
    const u32 per = (n + workers - 1) / workers;
    for (u32 from = per; from < n; from += per)
      threads.emplace_back(hash_range, from, std::min(n, from + per));
    hash_range(0, std::min(n, per));
    for (std::thread& t : threads)
      t.join();
    if (progress)
      progress(start + len, size);
  }
  for (u32 i = 0; i < count; ++i)
    result.zero[i] = zero[i] != 0;

  // Only cache what the file still is (it could have changed while it was read).
  if (mtime && ModificationTime(path) == mtime)
    SaveHashCache(path, *mtime, result);
  return result;
}

std::optional<Manifest> BuildManifest(const std::string& path, const std::atomic<bool>* cancel,
                                      const HashProgress& progress)
{
  std::optional<ImageHashes> hashes = HashImage(path, cancel, progress);
  if (!hashes)
    return std::nullopt;
  return ManifestFromHashes(path, *hashes);
}

std::optional<Manifest> ManifestFromHashes(const std::string& path, const ImageHashes& hashes)
{
  File::IOFile file(path, "rb");
  if (!file || file.GetSize() != hashes.size || hashes.zero.size() != hashes.hashes.size())
    return std::nullopt;
  const std::optional<std::vector<bool>> free = FindFreeChunks(file, hashes.size);

  Manifest manifest;
  manifest.size = hashes.size;
  manifest.chunks.resize(hashes.hashes.size());
  for (size_t i = 0; i < manifest.chunks.size(); ++i)
  {
    ChunkInfo& info = manifest.chunks[i];
    info.zero = hashes.zero[i] || (free && (*free)[i]);
    if (!info.zero)
      info.hash = hashes.hashes[i];
  }
  return manifest;
}

std::vector<u8> CanonicalManifest(const Manifest& manifest)
{
  std::vector<u8> out;
  out.reserve(8 + manifest.chunks.size() * MANIFEST_ENTRY_MAX_BYTES);
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<u8>(manifest.size >> (8 * i)));
  for (const ChunkInfo& info : manifest.chunks)
  {
    out.push_back(info.zero ? MANIFEST_ENTRY_ZERO : MANIFEST_ENTRY_DATA);
    if (!info.zero)
      out.insert(out.end(), info.hash.begin(), info.hash.end());
  }
  return out;
}

Root ManifestRoot(const Manifest& manifest)
{
  const std::vector<u8> canon = CanonicalManifest(manifest);
  Root root;
  crypto_blake2b(root.data(), root.size(), canon.data(), canon.size());
  return root;
}

std::string RootHex(const Root& root)
{
  std::string hex;
  hex.reserve(root.size() * 2);
  for (const u8 b : root)
    hex += fmt::format("{:02x}", b);
  return hex;
}

void WriteManifest(const Manifest& manifest, sf::Packet& packet)
{
  std::vector<u8> entries;
  entries.reserve(manifest.chunks.size() * MANIFEST_ENTRY_MAX_BYTES);
  for (const ChunkInfo& info : manifest.chunks)
  {
    if (info.zero)
    {
      entries.push_back(MANIFEST_ENTRY_ZERO);
    }
    else
    {
      entries.push_back(MANIFEST_ENTRY_DATA);
      entries.insert(entries.end(), info.hash.begin(), info.hash.end());
    }
  }
  packet << manifest.size;
  NetPlay::CompressBufferIntoPacket(entries, packet);
}

std::optional<Manifest> ReadManifest(sf::Packet& packet)
{
  Manifest manifest;
  manifest.size = Common::PacketReadU64(packet);
  if (!packet || manifest.size == 0 || manifest.size > MAX_CARD_SIZE)
    return std::nullopt;

  const u32 count = ChunkCountFor(manifest.size);
  const std::optional<std::vector<u8>> entries =
      ReadCompressed(packet, u64{count} * MANIFEST_ENTRY_MAX_BYTES);
  if (!entries)
    return std::nullopt;

  manifest.chunks.resize(count);
  size_t pos = 0;
  for (u32 i = 0; i < count; ++i)
  {
    if (pos >= entries->size())
      return std::nullopt;
    const u8 kind = (*entries)[pos++];
    if (kind == MANIFEST_ENTRY_ZERO)
    {
      manifest.chunks[i].zero = true;
    }
    else if (kind == MANIFEST_ENTRY_DATA)
    {
      if (entries->size() - pos < sizeof(Hash))
        return std::nullopt;
      std::copy_n(entries->begin() + pos, sizeof(Hash), manifest.chunks[i].hash.begin());
      pos += sizeof(Hash);
    }
    else
    {
      return std::nullopt;
    }
  }
  if (pos != entries->size())
    return std::nullopt;
  return manifest;
}

std::optional<Manifest> ParseCanonicalManifest(std::span<const u8> bytes)
{
  if (bytes.size() < 8)
    return std::nullopt;
  Manifest manifest;
  for (int i = 0; i < 8; ++i)
    manifest.size |= u64{bytes[i]} << (8 * i);
  if (manifest.size == 0 || manifest.size > MAX_CARD_SIZE)
    return std::nullopt;
  const u32 count = ChunkCountFor(manifest.size);
  manifest.chunks.resize(count);
  size_t pos = 8;
  for (u32 i = 0; i < count; ++i)
  {
    if (pos >= bytes.size())
      return std::nullopt;
    const u8 kind = bytes[pos++];
    if (kind == MANIFEST_ENTRY_ZERO)
    {
      manifest.chunks[i].zero = true;
    }
    else if (kind == MANIFEST_ENTRY_DATA)
    {
      if (bytes.size() - pos < sizeof(Hash))
        return std::nullopt;
      std::copy_n(bytes.begin() + pos, sizeof(Hash), manifest.chunks[i].hash.begin());
      pos += sizeof(Hash);
    }
    else
    {
      return std::nullopt;
    }
  }
  if (pos != bytes.size())
    return std::nullopt;
  return manifest;
}

std::vector<u8> EncodeCardListing(const Manifest& manifest, const std::vector<CardFile>& files)
{
  const std::vector<u8> canon = CanonicalManifest(manifest);
  const std::vector<u8> list = EncodeFileList(files);
  std::vector<u8> out;
  out.reserve(4 + canon.size() + list.size());
  const u32 canon_size = static_cast<u32>(canon.size());
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(canon_size >> (8 * i)));
  out.insert(out.end(), canon.begin(), canon.end());
  out.insert(out.end(), list.begin(), list.end());
  return out;
}

std::optional<CardListing> DecodeCardListing(std::span<const u8> bytes)
{
  if (bytes.size() < 4)
    return std::nullopt;
  const u32 canon_size = LE32(bytes.data());
  if (canon_size > bytes.size() - 4)
    return std::nullopt;
  std::optional<Manifest> manifest = ParseCanonicalManifest(bytes.subspan(4, canon_size));
  if (!manifest)
    return std::nullopt;
  CardListing listing;
  // No list at all: the files are not known (the sizes still are).
  if (const std::span<const u8> list = bytes.subspan(4 + canon_size); !list.empty())
  {
    std::optional<std::vector<CardFile>> files = DecodeFileList(list, manifest->ChunkCount());
    if (!files)
      return std::nullopt;
    listing.files = std::move(*files);
  }
  listing.manifest = std::move(*manifest);
  return listing;
}

CardDifference CompareWithManifest(const Manifest& manifest, const ImageHashes& own,
                                   const MovedChunks* moved)
{
  // SessionCard::CreateGuestCard's rule: a chunk is this card's when it is as long as the
  // session's there and its hash matches, or this card has it at another place (`moved`); any
  // other chunk that holds data is fetched.
  CardDifference diff;
  for (u32 i = 0; i < manifest.ChunkCount(); ++i)
  {
    const ChunkInfo& info = manifest.chunks[i];
    if (info.zero)
      continue;
    const u64 start = u64{i} * CHUNK_SIZE;
    const bool same_length =
        start < own.size && std::min(CHUNK_SIZE, own.size - start) == manifest.ChunkBytes(i);
    if (same_length && i < own.hashes.size() && own.hashes[i] == info.hash)
      continue;
    if (moved && moved->contains(i))
      continue;
    diff.chunks.push_back(i);
    diff.bytes += manifest.ChunkBytes(i);
  }
  return diff;
}

namespace
{
constexpr u32 MOVED_CACHE_MAGIC = 0x4D56534D;  // "MSVM"
constexpr u32 MOVED_CACHE_VERSION = 1;

// One file per image and session card (the "Joins with" menu compares a game's card with several
// rooms'): the image's hash cache name, then the root.
std::string MovedCachePath(const std::string& image_path, const Root& root)
{
  std::string path = HashCachePath(image_path);
  path.resize(path.size() - std::string_view(".hashes").size());
  return fmt::format("{}-{}.moved", path, RootHex(root).substr(0, 32));
}

std::optional<MovedChunks> LoadMovedCache(const std::string& image_path, const Root& root,
                                          u64 size, s64 mtime, const Manifest& manifest)
{
  File::IOFile file(MovedCachePath(image_path, root), "rb");
  if (!file)
    return std::nullopt;
  u32 magic = 0, version = 0, count = 0;
  u64 cached_size = 0;
  s64 cached_mtime = 0;
  Root cached_root{};
  if (!file.ReadArray(&magic, 1) || !file.ReadArray(&version, 1) ||
      !file.ReadArray(&cached_size, 1) || !file.ReadArray(&cached_mtime, 1) ||
      !file.ReadArray(&cached_root) || !file.ReadArray(&count, 1))
  {
    return std::nullopt;
  }
  if (magic != MOVED_CACHE_MAGIC || version != MOVED_CACHE_VERSION || cached_size != size ||
      cached_mtime != mtime || cached_root != root || count > manifest.ChunkCount())
  {
    return std::nullopt;
  }
  MovedChunks moved;
  for (u32 i = 0; i < count; ++i)
  {
    u32 index = 0;
    u64 offset = 0;
    if (!file.ReadArray(&index, 1) || !file.ReadArray(&offset, 1) ||
        index >= manifest.ChunkCount() || offset > size ||
        manifest.ChunkBytes(index) > size - offset)
    {
      return std::nullopt;
    }
    moved.emplace_hint(moved.end(), index, offset);
  }
  return moved;
}

void SaveMovedCache(const std::string& image_path, const Root& root, u64 size, s64 mtime,
                    const MovedChunks& moved)
{
  const std::string path = MovedCachePath(image_path, root);
  if (!File::CreateFullPath(path))
    return;
  const std::string temp = path + ".tmp";
  {
    File::IOFile file(temp, "wb");
    if (!file)
      return;
    const u32 header[2] = {MOVED_CACHE_MAGIC, MOVED_CACHE_VERSION};
    const u32 count = static_cast<u32>(moved.size());
    bool ok = file.WriteArray(header, 2) && file.WriteArray(&size, 1) &&
              file.WriteArray(&mtime, 1) && file.WriteArray(root) && file.WriteArray(&count, 1);
    for (auto it = moved.begin(); ok && it != moved.end(); ++it)
      ok = file.WriteArray(&it->first, 1) && file.WriteArray(&it->second, 1);
    if (!ok)
    {
      file.Close();
      File::Delete(temp);
      return;
    }
  }
  File::Rename(temp, path);
}

// A file of this player's card: where its bytes are in the image, as runs of consecutive
// clusters.
struct OwnFile
{
  struct Extent
  {
    u64 file_offset;
    u64 image_offset;
    u64 length;
  };
  u64 size = 0;
  std::vector<Extent> extents;

  // Where in the image the bytes `x` from the file's start would be (before its start or past
  // its end too) if the image goes on around the extent nearest `x` as it does there: a chunk
  // that holds the file's start or end with its neighbours is found when an image made from the
  // same folder put the same neighbours there.
  std::optional<u64> ImageOffset(s64 x) const
  {
    if (extents.empty())
      return std::nullopt;
    const u64 nearest = static_cast<u64>(std::clamp<s64>(x, 0, static_cast<s64>(size) - 1));
    auto it = std::ranges::upper_bound(extents, nearest, {}, &Extent::file_offset);
    if (it != extents.begin())
      --it;
    const s64 at = static_cast<s64>(it->image_offset) + (x - static_cast<s64>(it->file_offset));
    if (at < 0)
      return std::nullopt;
    return static_cast<u64>(at);
  }
};

std::string LowerPath(std::string_view path)
{
  std::string out(path);
  for (char& ch : out)
  {
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
  }
  return out;
}
}  // namespace

MovedChunks FindMovedChunks(const std::string& image_path, const ImageHashes& own,
                            const Manifest& manifest, const std::vector<CardFile>& files,
                            const std::atomic<bool>* cancel, const HashProgress& progress)
{
  const CardDifference diff = CompareWithManifest(manifest, own);
  if (diff.chunks.empty() || files.empty() || own.size == 0)
    return {};
  File::IOFile image(image_path, "rb");
  if (!image || image.GetSize() != own.size)
    return {};
  const std::optional<s64> mtime = ModificationTime(image_path);
  const Root root = ManifestRoot(manifest);
  if (mtime)
  {
    if (auto cached = LoadMovedCache(image_path, root, own.size, *mtime, manifest))
      return std::move(*cached);
  }

  // This card's files by path (FAT names are not case sensitive).
  std::map<std::string, OwnFile> own_files;
  {
    const std::optional<OpenedFat> opened = OpenFat(image, own.size);
    if (!opened)
      return {};
    const FatVolume& v = opened->volume;
    const u64 cluster_bytes = v.ClusterBytes();
    ForEachCardFile(image, *opened,
                    [&](std::string&& path, u64 size, const std::vector<u32>& clusters) {
                      OwnFile f;
                      f.size = size;
                      u64 file_offset = 0;
                      for (const u32 c : clusters)
                      {
                        if (file_offset >= size)
                          break;
                        const u64 at = v.data_offset + u64{c - 2} * cluster_bytes;
                        const u64 len = std::min(cluster_bytes, size - file_offset);
                        OwnFile::Extent* last = f.extents.empty() ? nullptr : &f.extents.back();
                        if (last && last->image_offset + last->length == at)
                          last->length += len;
                        else
                          f.extents.push_back({file_offset, at, len});
                        file_offset += len;
                      }
                      // A chain shorter than the file (a broken card): its bytes are not known.
                      if (size > 0 && file_offset == size)
                        own_files.emplace(LowerPath(path), std::move(f));
                      return true;
                    });
  }

  // The session's files this card has too (the same path and size) in chunks it has to fetch.
  std::vector<bool> needed(manifest.ChunkCount(), false);
  for (const u32 c : diff.chunks)
    needed[c] = true;
  struct Pair
  {
    const CardFile* theirs;
    const OwnFile* mine;
  };
  std::vector<Pair> pairs;
  for (const CardFile& f : files)
  {
    if (f.runs.empty() || f.size == 0)
      continue;
    const auto it = own_files.find(LowerPath(f.path));
    if (it == own_files.end() || it->second.size != f.size)
      continue;
    const auto& [first, count] = f.runs.front();
    bool wanted = false;
    for (u32 c = first; c < first + count && !wanted; ++c)
      wanted = needed[c];
    if (wanted)
      pairs.push_back({&f, &it->second});
  }
  // In card order: the reads go through this card about front to back.
  std::ranges::sort(pairs, {}, [](const Pair& p) { return p.theirs->runs.front().first; });

  // Where in its first chunk a file starts on the session's card: its data area's start plus
  // whole clusters, so the same few places for every file. Those seen so far (and how often), the
  // most seen first; a file with a chunk wholly its own tries every sector of the chunk until one
  // shows, a few times more often than files turned out to be here (each try costs 128 chunk
  // hashes: a card with the same names but other contents gives up soon).
  struct Search
  {
    std::vector<std::pair<u32, u64>> starts;
    u32 misses = 0, hits = 0;
  };
  // Looks for one of the session's files on this card; the chunks found go in `out`.
  auto search = [&](File::IOFile& file, std::vector<u8>& buffer, Search& state, const Pair& pair,
                    MovedChunks* out) {
    // Whether the bytes at `at` of this card are the session's chunk `c`.
    auto matches = [&](u32 c, std::optional<u64> at) {
      const u64 len = manifest.ChunkBytes(c);
      if (!at || *at > own.size || len > own.size - *at)
        return false;
      const std::span<u8> data(buffer.data(), len);
      return file.Seek(static_cast<s64>(*at), File::SeekOrigin::Begin) &&
             file.ReadBytes(data.data(), data.size()) && HashChunk(data) == manifest.chunks[c].hash;
    };
    // Its first run: from the chunk its first cluster is in. Further runs (a file in pieces on
    // the session's card) do not say where in the file they start.
    const u32 first = pair.theirs->runs.front().first;
    const u32 count = pair.theirs->runs.front().second;
    auto image_offset = [&](u32 c, u64 start) {
      return pair.mine->ImageOffset(static_cast<s64>(u64{c - first} * CHUNK_SIZE) -
                                    static_cast<s64>(start));
    };
    // The chunk that tells where the file starts: one wholly inside it if there is one (its
    // bytes are the file's alone), else its first.
    const bool inside = count >= 3;
    const u32 probe = inside ? first + 1 : first;
    std::optional<u64> start;
    for (const auto& known : state.starts)
    {
      if (matches(probe, image_offset(probe, known.second)))
      {
        start = known.second;
        break;
      }
    }
    if (!start && inside && state.misses < 16 + 2 * state.hits)
    {
      for (u64 s = 0; s < CHUNK_SIZE && !start; s += 512)
      {
        if (std::ranges::any_of(state.starts, [&](const auto& k) { return k.second == s; }))
          continue;
        if (matches(probe, image_offset(probe, s)))
          start = s;
      }
      if (!start)
        ++state.misses;
    }
    if (!start)
      return;
    ++state.hits;
    auto it = std::ranges::find(state.starts, *start, &std::pair<u32, u64>::second);
    if (it == state.starts.end())
    {
      state.starts.emplace_back(0, *start);
      it = state.starts.end() - 1;
    }
    ++it->first;
    std::ranges::stable_sort(state.starts, std::greater{}, &std::pair<u32, u64>::first);
    for (u32 c = first; c < first + count && out; ++c)
    {
      if (!needed[c] || out->contains(c))
        continue;
      if (const std::optional<u64> at = image_offset(c, *start); matches(c, at))
        out->emplace(c, *at);
    }
  };

  // The places files start, from the first files that have a chunk of their own: every thread
  // starts with them, so one whose share is all small files (a folder of thousands of portraits)
  // still finds them.
  Search learned;
  {
    std::vector<u8> buffer(CHUNK_SIZE);
    u32 tried = 0;
    for (const Pair& pair : pairs)
    {
      if (learned.hits >= 8 || tried >= 64 || (cancel && cancel->load()))
        break;
      if (pair.theirs->runs.front().second < 3)
        continue;
      ++tried;
      search(image, buffer, learned, pair, nullptr);
    }
    learned.misses = 0;
    learned.hits = std::min(learned.hits, 8u);
  }

  // Several threads, each on its own share of the files with its own handle on the image.
  const u32 workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  std::vector<MovedChunks> found(workers);
  std::mutex progress_mutex;
  u64 done = 0;
  std::atomic<bool> stopped{false};
  auto work = [&](u32 worker) {
    File::IOFile file(image_path, "rb");
    if (!file)
      return;
    std::vector<u8> buffer(CHUNK_SIZE);
    Search state = learned;
    const size_t begin = pairs.size() * worker / workers;
    const size_t end = pairs.size() * (worker + 1) / workers;
    for (size_t p = begin; p < end; ++p)
    {
      if ((cancel && cancel->load()) || stopped.load())
      {
        stopped = true;
        return;
      }
      search(file, buffer, state, pairs[p], &found[worker]);
      if (progress)
      {
        std::lock_guard lock(progress_mutex);
        progress(++done, pairs.size());
      }
    }
  };
  {
    std::vector<std::thread> threads;
    for (u32 w = 1; w < workers; ++w)
      threads.emplace_back(work, w);
    work(0);
    for (std::thread& t : threads)
      t.join();
  }
  if (stopped.load())
    return {};

  MovedChunks moved;
  for (const MovedChunks& m : found)
    moved.insert(m.begin(), m.end());
  INFO_LOG_FMT(BRAWLBACK, "SD card: {} of the {} chunks that differ are elsewhere on {}",
               moved.size(), diff.chunks.size(), image_path);
  // Only cache what the file still is (it could have changed while it was read).
  if (mtime && ModificationTime(image_path) == mtime)
    SaveMovedCache(image_path, root, own.size, *mtime, moved);
  return moved;
}

void WriteChunkRequest(std::span<const u32> chunks, sf::Packet& packet)
{
  packet << static_cast<u16>(chunks.size());
  for (const u32 c : chunks)
    packet << c;
}

std::optional<std::vector<u32>> ReadChunkRequest(sf::Packet& packet)
{
  u16 count = 0;
  packet >> count;
  if (!packet || count == 0 || count > MAX_REQUEST_CHUNKS)
    return std::nullopt;
  std::vector<u32> chunks(count);
  for (u32& c : chunks)
    packet >> c;
  if (!packet)
    return std::nullopt;
  return chunks;
}

void WriteChunkData(u32 index, std::span<const u8> data, sf::Packet& packet)
{
  packet << index << true;
  NetPlay::CompressBufferIntoPacket(data, packet);
}

void WriteChunkUnavailable(u32 index, sf::Packet& packet)
{
  packet << index << false;
}

std::optional<ChunkData> ReadChunkData(sf::Packet& packet)
{
  ChunkData chunk;
  packet >> chunk.index >> chunk.ok;
  if (!packet || chunk.index >= MAX_CHUNKS)
    return std::nullopt;
  if (!chunk.ok)
    return chunk;
  std::optional<std::vector<u8>> data = ReadCompressed(packet, CHUNK_SIZE);
  if (!data)
    return std::nullopt;
  chunk.data = std::move(*data);
  return chunk;
}

// ---------------------------------------------------------------------------------- CardServer

CardServer::CardServer(std::shared_ptr<const Manifest> manifest, ReadFunction read,
                       HeaderFunction header, SendFunction send)
    : m_manifest(std::move(manifest)), m_read(std::move(read)), m_header(std::move(header)),
      m_send(std::move(send))
{
  m_thread = std::thread(&CardServer::ThreadFunc, this);
}

CardServer::~CardServer()
{
  {
    std::lock_guard lock(m_mutex);
    m_stop = true;
  }
  m_cv.notify_all();
  m_thread.join();
}

void CardServer::OnRequest(u8 player, sf::Packet& packet)
{
  const std::optional<std::vector<u32>> chunks = ReadChunkRequest(packet);
  if (!chunks)
  {
    WARN_LOG_FMT(BRAWLBACK, "SD card: malformed chunk request from player {}", player + 1);
    return;
  }

  {
    std::lock_guard lock(m_mutex);
    size_t& queued = m_queued_per_player[player];
    for (const u32 c : *chunks)
    {
      // Only the chunks that hold data: a chunk no file uses may hold deleted files.
      if (c >= m_manifest->ChunkCount() || m_manifest->chunks[c].zero)
      {
        WARN_LOG_FMT(BRAWLBACK, "SD card: player {} asked for chunk {}, which it may not have",
                     player + 1, c);
        continue;
      }
      if (queued >= MAX_QUEUED_PER_PLAYER)
        break;
      if (!m_queued.emplace(player, c).second)
        continue;
      m_queue.emplace_back(player, c);
      ++queued;
    }
  }
  m_cv.notify_one();
}

void CardServer::ThreadFunc()
{
  std::vector<u8> buffer(CHUNK_SIZE);

  while (true)
  {
    std::pair<u8, u32> item;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
      if (m_stop)
        return;
      item = m_queue.front();
      m_queue.pop_front();
      --m_queued_per_player[item.first];
      m_queued.erase(item);
    }

    const auto [player, index] = item;
    const u64 len = m_manifest->ChunkBytes(index);
    const std::span<u8> data(buffer.data(), len);
    sf::Packet packet;
    m_header(packet);
    const bool read = m_read(index, data);
    if (read && HashChunk(data) == m_manifest->chunks[index].hash)
    {
      WriteChunkData(index, data, packet);
    }
    else
    {
      // This player has not got the chunk, or its card changed under the session.
      if (read)
        WARN_LOG_FMT(BRAWLBACK, "SD card: chunk {} no longer matches the manifest", index);
      WriteChunkUnavailable(index, packet);
    }
    m_send(player, std::move(packet));
  }
}

// --------------------------------------------------------------------------------- SessionCard

SessionCard::SessionCard(Mode mode, const std::string& image_path,
                         std::shared_ptr<const Manifest> manifest)
    : m_mode(mode), m_image_path(image_path), m_manifest(std::move(manifest))
{
  m_clock = [] { return Core::System::GetInstance().GetCoreTiming().GetTicks(); };
  m_history_ticks = u64{Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond()} * 10;
}

std::shared_ptr<SessionCard> SessionCard::CreateOwnCard(const std::string& image_path)
{
  std::shared_ptr<SessionCard> card(new SessionCard(Mode::OwnCard, image_path, nullptr));
  card->m_image.Open(image_path, "rb", File::SharedAccess::ReadWrite);
  card->m_size = card->m_image ? card->m_image.GetSize() : 0;
  if (card->m_size > MAX_CARD_SIZE)
    card->m_size = 0;
  return card;
}

std::shared_ptr<SessionCard> SessionCard::CreateHostCard(const std::string& image_path,
                                                         std::shared_ptr<const Manifest> manifest)
{
  std::shared_ptr<SessionCard> card(
      new SessionCard(Mode::HostCard, image_path, std::move(manifest)));
  card->m_image.Open(image_path, "rb", File::SharedAccess::ReadWrite);
  card->m_size = card->m_manifest->size;
  return card;
}

std::shared_ptr<SessionCard> SessionCard::CreateGuestCard(const std::string& image_path,
                                                          std::shared_ptr<const Manifest> manifest,
                                                          const std::string& cache_dir,
                                                          std::shared_ptr<Remote> remote,
                                                          const ImageHashes* own,
                                                          const MovedChunks* moved)
{
  std::shared_ptr<SessionCard> card(
      new SessionCard(Mode::GuestCard, image_path, std::move(manifest)));
  card->m_size = card->m_manifest->size;
  card->m_remote = std::move(remote);
  card->m_image.Open(image_path, "rb", File::SharedAccess::ReadWrite);
  const u64 own_size = card->m_image ? card->m_image.GetSize() : 0;

  // This card's hashes: given (the join's check), or from an earlier check in the cache. With
  // them, every chunk is known now to match or not; without, a chunk is checked when first read.
  std::optional<ImageHashes> cached;
  if (own && own->size == own_size)
  {
    cached = *own;
  }
  else if (own_size != 0)
  {
    if (const std::optional<s64> mtime = ModificationTime(image_path))
      cached = LoadHashCache(image_path, own_size, *mtime);
  }

  const u32 count = card->ChunkCount();
  card->m_sources.resize(count, Source::Missing);
  u32 differ = 0;
  for (u32 i = 0; i < count; ++i)
  {
    if (card->m_manifest->chunks[i].zero)
    {
      card->m_sources[i] = Source::Zero;
      continue;
    }
    // This card's chunk must be as long as the session's for the hashes to say they match.
    const u64 start = u64{i} * CHUNK_SIZE;
    if (start < own_size && std::min(CHUNK_SIZE, own_size - start) == card->ChunkBytes(i))
    {
      if (!cached)
        card->m_sources[i] = Source::Unchecked;
      else if (cached->hashes[i] == card->m_manifest->chunks[i].hash)
        card->m_sources[i] = Source::Local;
      else
        ++differ;
    }
  }
  // The chunks this card has at another place (FindMovedChunks, from these hashes).
  u32 found = 0;
  if (moved && own && own->size == own_size)
  {
    for (const auto& [index, offset] : *moved)
    {
      if (index < count && card->m_sources[index] == Source::Missing && offset <= own_size &&
          card->ChunkBytes(index) <= own_size - offset)
      {
        card->m_sources[index] = Source::Moved;
        card->m_moved.emplace(index, offset);
        ++found;
      }
    }
  }
  if (cached)
  {
    INFO_LOG_FMT(BRAWLBACK, "SD card: {} chunks of this player's card differ, {} of them elsewhere "
                            "on it",
                 differ, found);
  }

  static std::atomic<u32> s_counter{0};
  card->m_fetched_path =
      fmt::format("{}/fetched-{}-{}.bin", cache_dir, NowMs(), s_counter.fetch_add(1));
  if (File::CreateFullPath(card->m_fetched_path))
    card->m_fetched_file.Open(card->m_fetched_path, "w+b");
  if (!card->m_fetched_file)
    ERROR_LOG_FMT(BRAWLBACK, "SD card: could not create {}", card->m_fetched_path);

  INFO_LOG_FMT(BRAWLBACK,
               "SD card: the session's card is {} bytes; {} bytes to fetch, {} bytes of this "
               "player's card to check as the game reads them",
               card->m_size, card->MissingBytes(), card->UncheckedBytes());
  return card;
}

SessionCard::~SessionCard()
{
  Detach();
  if (m_sync_thread.joinable())
    m_sync_thread.join();
  m_fetched_file.Close();
  if (!m_fetched_path.empty())
    File::Delete(m_fetched_path);
}

void SessionCard::SetClockForTesting(std::function<u64()> clock, u64 history_ticks)
{
  m_clock = std::move(clock);
  m_history_ticks = history_ticks;
}

u32 SessionCard::ChunkCount() const
{
  return ChunkCountFor(m_size);
}

u64 SessionCard::ChunkBytes(u32 index) const
{
  const u64 start = u64{index} * CHUNK_SIZE;
  return start >= m_size ? 0 : std::min(CHUNK_SIZE, m_size - start);
}

bool SessionCard::IsOpen() const
{
  return m_size != 0;
}

u64 SessionCard::GetSize() const
{
  return m_size;
}

u64 SessionCard::GetWriteSeq() const
{
  return m_write_seq;
}

void SessionCard::CheckLocal(std::span<const u32> chunks)
{
  if (m_mode != Mode::GuestCard)
    return;
  std::vector<u32> unchecked;
  {
    std::lock_guard lock(m_fetch_mutex);
    for (const u32 c : chunks)
    {
      if (c < m_sources.size() && m_sources[c] == Source::Unchecked)
        unchecked.push_back(c);
    }
  }
  if (unchecked.empty())
    return;

  std::vector<u8> buffer(CHUNK_SIZE);
  std::lock_guard serve_lock(m_serve_mutex);
  if (!m_serve_image)
    m_serve_image.Open(m_image_path, "rb", File::SharedAccess::ReadWrite);
  for (const u32 c : unchecked)
  {
    const std::span<u8> data(buffer.data(), ChunkBytes(c));
    const bool same = m_serve_image &&
                      m_serve_image.Seek(static_cast<s64>(u64{c} * CHUNK_SIZE),
                                         File::SeekOrigin::Begin) &&
                      m_serve_image.ReadBytes(data.data(), data.size()) &&
                      HashChunk(data) == m_manifest->chunks[c].hash;
    std::lock_guard lock(m_fetch_mutex);
    if (m_sources[c] == Source::Unchecked)
    {
      m_sources[c] = same ? Source::Local : Source::Missing;
      if (!same)
        INFO_LOG_FMT(BRAWLBACK, "SD card: chunk {} of this player's card differs: fetched", c);
    }
  }
}

bool SessionCard::ReadBase(u32 index, u64 from, u64 len, u8* dst)
{
  const u64 start = u64{index} * CHUNK_SIZE + from;
  switch (m_mode)
  {
  case Mode::OwnCard:
    return m_image.Seek(static_cast<s64>(start), File::SeekOrigin::Begin) &&
           m_image.ReadBytes(dst, len);
  case Mode::HostCard:
    if (m_manifest->chunks[index].zero)
    {
      std::memset(dst, 0, len);
      return true;
    }
    return m_image.Seek(static_cast<s64>(start), File::SeekOrigin::Begin) &&
           m_image.ReadBytes(dst, len);
  case Mode::GuestCard:
  {
    std::lock_guard lock(m_fetch_mutex);
    switch (m_sources[index])
    {
    case Source::Zero:
      std::memset(dst, 0, len);
      return true;
    case Source::Local:
      return m_image.Seek(static_cast<s64>(start), File::SeekOrigin::Begin) &&
             m_image.ReadBytes(dst, len);
    case Source::Moved:
    {
      const auto it = m_moved.find(index);
      return it != m_moved.end() &&
             m_image.Seek(static_cast<s64>(it->second + from), File::SeekOrigin::Begin) &&
             m_image.ReadBytes(dst, len);
    }
    case Source::Fetched:
    {
      const auto it = m_fetched_offsets.find(index);
      return it != m_fetched_offsets.end() &&
             m_fetched_file.Seek(static_cast<s64>(it->second + from), File::SeekOrigin::Begin) &&
             m_fetched_file.ReadBytes(dst, len);
    }
    case Source::Unchecked:
    case Source::Missing:
      return false;
    }
    return false;
  }
  }
  return false;
}

bool SessionCard::ReadChunk(u32 index, u64 from, u64 len, u8* dst)
{
  if (const auto it = m_overlay.find(index); it != m_overlay.end())
  {
    std::memcpy(dst, it->second->data() + from, len);
    return true;
  }
  if (m_mode == Mode::GuestCard)
  {
    const u32 chunk[] = {index};
    CheckLocal(chunk);
    if (!WaitForChunks(chunk, true))
      return false;
  }
  return ReadBase(index, from, len, dst);
}

bool SessionCard::Read(u64 offset, u8* dst, u64 size)
{
  if (size == 0)
    return true;
  if (offset >= m_size || size > m_size - offset)
    return false;

  const u32 first = static_cast<u32>(offset / CHUNK_SIZE);
  const u32 last = static_cast<u32>((offset + size - 1) / CHUNK_SIZE);

  if (m_read_seen.size() != ChunkCount())
    m_read_seen.assign(ChunkCount(), false);
  for (u32 c = first; c <= last; ++c)
  {
    if (!m_read_seen[c])
    {
      m_read_seen[c] = true;
      m_read_order.push_back(c);
    }
  }

  // A guest checks its own chunks and fetches everything else the read needs at once.
  if (m_mode == Mode::GuestCard)
  {
    std::vector<u32> range;
    for (u32 c = first; c <= last; ++c)
    {
      if (!m_overlay.contains(c))
        range.push_back(c);
    }
    CheckLocal(range);
    std::vector<u32> needed;
    {
      std::lock_guard lock(m_fetch_mutex);
      for (const u32 c : range)
      {
        if (m_sources[c] == Source::Missing)
          needed.push_back(c);
      }
    }
    if (!needed.empty())
    {
      ++m_waits;
      if (!m_logged_late_fetch)
      {
        m_logged_late_fetch = true;
        ERROR_LOG_FMT(BRAWLBACK,
                      "SD card: the game read {} chunks this player had not got (from chunk {}): "
                      "the card is synced before the join, so this is a bug; fetching them",
                      needed.size(), needed.front());
      }
      const u64 start = NowMs();
      RequestMissing(needed);
      // While that request is on its way: the chunks after the read that this card lacks too.
      std::vector<u32> ahead;
      for (u32 c = last + 1; c < ChunkCount() && c - last <= READ_AHEAD_CHUNKS; ++c)
      {
        if (!m_overlay.contains(c))
          ahead.push_back(c);
      }
      CheckLocal(ahead);
      RequestMissing(ahead);
      if (!WaitForChunks(needed, true))
        return false;
      INFO_LOG_FMT(BRAWLBACK, "SD card: a read waited {} ms for {} chunks from the players",
                   NowMs() - start, needed.size());
    }
  }

  // Only the bytes asked for, straight into the game's buffer: the game's FAT code reads a 512-byte
  // sector at a time, thousands a second while it opens files (directories, the FAT), and a re-run
  // frame reads them all again. Each sector cost a zeroed 64 KiB buffer and a 64 KiB read of the
  // image before (bug report #125: Cobalt's lookup of a BrawlEx fighter's HUD face, 5,334 sector
  // reads in its 71 frames).
  u64 done = 0;
  for (u32 c = first; c <= last; ++c)
  {
    const u64 chunk_start = u64{c} * CHUNK_SIZE;
    const u64 from = std::max(offset, chunk_start) - chunk_start;
    const u64 to = std::min(offset + size, chunk_start + ChunkBytes(c)) - chunk_start;
    if (!ReadChunk(c, from, to - from, dst + done))
      return false;
    done += to - from;
  }
  return true;
}

bool SessionCard::Write(u64 offset, const u8* src, u64 size)
{
  if (size == 0)
    return true;
  if (offset >= m_size || size > m_size - offset)
    return false;

  const u32 first = static_cast<u32>(offset / CHUNK_SIZE);
  const u32 last = static_cast<u32>((offset + size - 1) / CHUNK_SIZE);
  // A write of part of a chunk keeps the rest of it: a guest gets those chunks first, so a write
  // that fails changes nothing.
  if (m_mode == Mode::GuestCard)
  {
    std::vector<u32> partial;
    for (const u32 c : {first, last})
    {
      const u64 chunk_start = u64{c} * CHUNK_SIZE;
      const bool part = offset > chunk_start || offset + size < chunk_start + ChunkBytes(c);
      if (part && !m_overlay.contains(c) && std::ranges::find(partial, c) == partial.end())
        partial.push_back(c);
    }
    CheckLocal(partial);
    std::vector<u32> needed;
    {
      std::lock_guard lock(m_fetch_mutex);
      for (const u32 c : partial)
      {
        if (m_sources[c] == Source::Missing)
          needed.push_back(c);
      }
    }
    if (!needed.empty())
    {
      ++m_waits;
      if (!WaitForChunks(needed, true))
        return false;
    }
  }

  const u64 now = m_clock();
  const u64 seq = m_write_seq + 1;

  u64 done = 0;
  for (u32 c = first; c <= last; ++c)
  {
    const u64 chunk_start = u64{c} * CHUNK_SIZE;
    const u64 len = ChunkBytes(c);
    const u64 from = std::max(offset, chunk_start) - chunk_start;
    const u64 to = std::min(offset + size, chunk_start + len) - chunk_start;

    auto buffer = std::make_shared<std::vector<u8>>(len);
    if ((from != 0 || to != len) && !ReadChunk(c, 0, len, buffer->data()))
    {
      ERROR_LOG_FMT(BRAWLBACK, "SD card: could not read chunk {} to write part of it", c);
      return false;
    }
    std::memcpy(buffer->data() + from, src + done, to - from);
    done += to - from;

    auto& slot = m_overlay[c];
    m_history.push_back(UndoEntry{seq, now, c, slot});
    slot = std::move(buffer);
  }

  m_write_seq = seq;
  PruneHistory(now);
  return true;
}

void SessionCard::PruneHistory(u64 now)
{
  while (!m_history.empty() && now - m_history.front().time > m_history_ticks)
  {
    m_pruned_seq = std::max(m_pruned_seq, m_history.front().seq);
    m_history.pop_front();
  }
}

void SessionCard::RewindTo(u64 write_seq)
{
  if (write_seq > m_write_seq)
  {
    // A state from after this point: only a rollback load happens in a session, and it never
    // goes forward.
    WARN_LOG_FMT(BRAWLBACK, "SD card: cannot go forward to write {} from {}", write_seq,
                 m_write_seq);
    return;
  }
  while (!m_history.empty() && m_history.back().seq > write_seq)
  {
    const UndoEntry& entry = m_history.back();
    if (entry.previous)
      m_overlay[entry.chunk] = entry.previous;
    else
      m_overlay.erase(entry.chunk);
    m_history.pop_back();
  }
  if (write_seq < m_pruned_seq)
  {
    // Rollback never goes back this far (the history covers seconds of emulated time).
    WARN_LOG_FMT(BRAWLBACK, "SD card: rewound past the kept history (to write {})", write_seq);
  }
  m_write_seq = write_seq;
}

void SessionCard::Deliver(const ChunkData& chunk)
{
  if (m_mode != Mode::GuestCard || chunk.index >= ChunkCount())
    return;

  const bool valid = chunk.ok && chunk.data.size() == ChunkBytes(chunk.index) &&
                     HashChunk(chunk.data) == m_manifest->chunks[chunk.index].hash;
  {
    std::lock_guard lock(m_fetch_mutex);
    if (m_detached || m_sources[chunk.index] != Source::Missing)
      return;

    if (!chunk.ok)
    {
      WARN_LOG_FMT(BRAWLBACK, "SD card: no player could send chunk {}", chunk.index);
      m_unavailable.insert(chunk.index);
    }
    else if (!valid)
    {
      // A wrong chunk is dropped and asked for again, a few times.
      WARN_LOG_FMT(BRAWLBACK, "SD card: chunk {} from a player does not match its hash",
                   chunk.index);
      m_requested_at.erase(chunk.index);
      if (++m_bad_deliveries[chunk.index] >= MAX_BAD_DELIVERIES)
        m_unavailable.insert(chunk.index);
    }
    else
    {
      const u64 offset = m_fetched_file ? m_fetched_file.GetSize() : 0;
      if (m_fetched_file && m_fetched_file.Seek(static_cast<s64>(offset), File::SeekOrigin::Begin) &&
          m_fetched_file.WriteBytes(chunk.data.data(), chunk.data.size()))
      {
        m_fetched_offsets[chunk.index] = offset;
        m_sources[chunk.index] = Source::Fetched;
        m_fetched_bytes += chunk.data.size();
      }
      else
      {
        m_unavailable.insert(chunk.index);
      }
      m_requested_at.erase(chunk.index);
    }
  }
  m_fetch_cv.notify_all();
}

SessionCard::Stats SessionCard::GetStats() const
{
  Stats stats;
  stats.size = m_size;
  stats.missing_bytes = MissingBytes();
  {
    std::lock_guard lock(m_fetch_mutex);
    stats.fetched_bytes = m_fetched_bytes;
  }
  stats.waits = m_waits.load();
  stats.writes = m_write_seq;
  return stats;
}

u64 SessionCard::MissingBytes() const
{
  if (m_mode != Mode::GuestCard)
    return 0;
  std::lock_guard lock(m_fetch_mutex);
  u64 bytes = 0;
  for (u32 c = 0; c < m_sources.size(); ++c)
  {
    if (m_sources[c] == Source::Missing)
      bytes += ChunkBytes(c);
  }
  return bytes;
}

u64 SessionCard::UncheckedBytes() const
{
  if (m_mode != Mode::GuestCard)
    return 0;
  std::lock_guard lock(m_fetch_mutex);
  u64 bytes = 0;
  for (u32 c = 0; c < m_sources.size(); ++c)
  {
    if (m_sources[c] == Source::Unchecked)
      bytes += ChunkBytes(c);
  }
  return bytes;
}

bool SessionCard::WaitForChunks(std::span<const u32> chunks, bool keep_alive)
{
  const u64 start = NowMs();
  bool told_player = false;

  while (true)
  {
    std::vector<u32> to_request;
    std::shared_ptr<Remote> remote;
    {
      std::unique_lock lock(m_fetch_mutex);
      bool all_here = true;
      const u64 now = NowMs();
      for (const u32 c : chunks)
      {
        if (m_sources[c] != Source::Missing)
          continue;
        if (m_unavailable.contains(c))
          return false;
        all_here = false;
        const auto it = m_requested_at.find(c);
        const bool due = it == m_requested_at.end() || now - it->second >= REQUEST_RETRY_MS;
        if (due && (it != m_requested_at.end() ||
                    m_requested_at.size() < MAX_OUTSTANDING_REQUESTS))
        {
          m_requested_at[c] = now;
          to_request.push_back(c);
        }
      }
      // What arrived before a detach still reads.
      if (all_here)
        return true;
      if (m_detached)
        return false;
      if (now - start >= FETCH_TIMEOUT_MS)
      {
        ERROR_LOG_FMT(BRAWLBACK, "SD card: the players did not send its chunks in time");
        return false;
      }
      remote = m_remote;
      if (to_request.empty())
        m_fetch_cv.wait_for(lock, std::chrono::milliseconds(keep_alive ? 20 : 100));
    }

    if (remote)
    {
      for (size_t i = 0; i < to_request.size(); i += MAX_REQUEST_CHUNKS)
      {
        const size_t n = std::min<size_t>(MAX_REQUEST_CHUNKS, to_request.size() - i);
        remote->Request(std::span<const u32>(to_request.data() + i, n));
      }
      if (keep_alive)
        remote->KeepAlive();
    }

    if (keep_alive && !told_player && NowMs() - start >= 500)
    {
      told_player = true;
      OSD::AddMessage("Loading the session's SD card...", 3000);
    }
  }
}

void SessionCard::RequestMissing(std::span<const u32> chunks)
{
  std::vector<u32> to_request;
  std::shared_ptr<Remote> remote;
  {
    std::lock_guard lock(m_fetch_mutex);
    if (m_detached)
      return;
    const u64 now = NowMs();
    for (const u32 c : chunks)
    {
      if (c < m_sources.size() && m_sources[c] == Source::Missing && !m_unavailable.contains(c) &&
          !m_requested_at.contains(c) && m_requested_at.size() < MAX_OUTSTANDING_REQUESTS)
      {
        m_requested_at[c] = now;
        to_request.push_back(c);
      }
    }
    remote = m_remote;
  }
  if (!remote)
    return;
  for (size_t i = 0; i < to_request.size(); i += MAX_REQUEST_CHUNKS)
  {
    const size_t n = std::min<size_t>(MAX_REQUEST_CHUNKS, to_request.size() - i);
    remote->Request(std::span<const u32>(to_request.data() + i, n));
  }
}

std::vector<u32> SessionCard::MissingChunks() const
{
  std::vector<u32> out;
  if (m_mode != Mode::GuestCard)
    return out;
  std::lock_guard lock(m_fetch_mutex);
  for (u32 c = 0; c < m_sources.size(); ++c)
  {
    if (m_sources[c] == Source::Missing)
      out.push_back(c);
  }
  return out;
}

void SessionCard::StartSync()
{
  if (m_mode != Mode::GuestCard || m_sync_thread.joinable())
    return;
  {
    std::lock_guard lock(m_fetch_mutex);
    m_sync_start_fetched = m_fetched_bytes;
    m_sync_total = 0;
    for (u32 c = 0; c < m_sources.size(); ++c)
    {
      if (m_sources[c] == Source::Missing)
        m_sync_total += ChunkBytes(c);
    }
    m_sync_finished = false;
    m_sync_failed = false;
  }
  m_sync_thread = std::thread(&SessionCard::SyncThreadFunc, this);
}

SessionCard::SyncProgress SessionCard::GetSyncProgress() const
{
  std::lock_guard lock(m_fetch_mutex);
  SyncProgress p;
  p.total = m_sync_total;
  p.done = std::min(m_sync_total, m_fetched_bytes - m_sync_start_fetched);
  p.finished = m_sync_finished;
  p.failed = m_sync_failed;
  return p;
}

void SessionCard::SyncThreadFunc()
{
  // The players pace what they send (MachineSession's PaceSD, so the game they play keeps its
  // link); the requests out at once follow what they send (Remote::Capacity).
  while (true)
  {
    std::vector<u32> to_request;
    std::shared_ptr<Remote> remote;
    size_t capacity;
    {
      std::lock_guard lock(m_fetch_mutex);
      remote = m_remote;
    }
    capacity = std::min(MAX_OUTSTANDING_REQUESTS, remote ? remote->Capacity() : size_t{0});
    {
      std::unique_lock lock(m_fetch_mutex);
      if (m_detached)
      {
        m_sync_failed = true;
        break;
      }
      const u64 now = NowMs();
      bool missing = false;
      bool unavailable = false;
      for (u32 c = 0; c < m_sources.size(); ++c)
      {
        if (m_sources[c] != Source::Missing)
          continue;
        missing = true;
        if (m_unavailable.contains(c))
        {
          unavailable = true;
          break;
        }
        // A retry counts against the requests out too: a slow player is not asked for more.
        const auto it = m_requested_at.find(c);
        const bool asked = it != m_requested_at.end();
        if (asked && now - it->second < SYNC_RETRY_MS)
          continue;
        const size_t out = m_requested_at.size() - (asked ? 1 : 0) + to_request.size();
        if (out < capacity)
          to_request.push_back(c);
      }
      if (unavailable)
      {
        m_sync_failed = true;
        break;
      }
      if (!missing)
      {
        m_sync_finished = true;
        break;
      }
      for (const u32 c : to_request)
        m_requested_at[c] = now;
      remote = m_remote;
      if (to_request.empty())
        m_fetch_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (remote)
    {
      for (size_t i = 0; i < to_request.size(); i += MAX_REQUEST_CHUNKS)
      {
        const size_t n = std::min<size_t>(MAX_REQUEST_CHUNKS, to_request.size() - i);
        remote->Request(std::span<const u32>(to_request.data() + i, n));
      }
    }
  }
  m_fetch_cv.notify_all();
}

bool SessionCard::WriteFetchedToImage(const ImageHashes& own, std::string* error)
{
  if (m_mode != Mode::GuestCard)
  {
    *error = "not a joiner's card";
    return false;
  }
  std::lock_guard serve_lock(m_serve_mutex);
  std::lock_guard lock(m_fetch_mutex);
  if (std::ranges::any_of(m_sources, [](Source src) {
        return src == Source::Missing || src == Source::Unchecked;
      }))
  {
    *error = "the card has not got every chunk yet";
    return false;
  }
  m_serve_image.Close();
  // The chunks this card has at another place go into the fetched file first: writing a chunk
  // into the image could overwrite where another one is read from.
  if (!m_moved.empty())
  {
    File::IOFile from(m_image_path, "rb", File::SharedAccess::ReadWrite);
    std::vector<u8> buffer(CHUNK_SIZE);
    for (const auto& [index, offset] : m_moved)
    {
      const u64 len = ChunkBytes(index);
      const u64 to = m_fetched_file ? m_fetched_file.GetSize() : 0;
      if (!from || !from.Seek(static_cast<s64>(offset), File::SeekOrigin::Begin) ||
          !from.ReadBytes(buffer.data(), len) ||
          HashChunk({buffer.data(), len}) != m_manifest->chunks[index].hash || !m_fetched_file ||
          !m_fetched_file.Seek(static_cast<s64>(to), File::SeekOrigin::Begin) ||
          !m_fetched_file.WriteBytes(buffer.data(), len))
      {
        *error = fmt::format("could not copy chunk {} of {}", index, m_image_path);
        return false;
      }
      m_fetched_offsets[index] = to;
      m_sources[index] = Source::Fetched;
    }
    m_moved.clear();
  }
  if (!File::Exists(m_image_path) && !File::CreateEmptyFile(m_image_path))
  {
    *error = fmt::format("could not create {}", m_image_path);
    return false;
  }
  File::IOFile image(m_image_path, "r+b", File::SharedAccess::ReadWrite);
  if (!image || !image.Resize(m_size))
  {
    *error = fmt::format("could not open {} to write it", m_image_path);
    return false;
  }
  ImageHashes hashes;
  hashes.size = m_size;
  hashes.hashes.resize(ChunkCount());
  hashes.zero.resize(ChunkCount());
  std::vector<u8> buffer(CHUNK_SIZE);
  for (u32 c = 0; c < ChunkCount(); ++c)
  {
    const u64 len = ChunkBytes(c);
    const u64 start = u64{c} * CHUNK_SIZE;
    if (m_sources[c] == Source::Fetched)
    {
      const auto it = m_fetched_offsets.find(c);
      if (it == m_fetched_offsets.end() ||
          !m_fetched_file.Seek(static_cast<s64>(it->second), File::SeekOrigin::Begin) ||
          !m_fetched_file.ReadBytes(buffer.data(), len) ||
          !image.Seek(static_cast<s64>(start), File::SeekOrigin::Begin) ||
          !image.WriteBytes(buffer.data(), len))
      {
        *error = fmt::format("could not write chunk {} of {}", c, m_image_path);
        return false;
      }
      hashes.hashes[c] = m_manifest->chunks[c].hash;
      hashes.zero[c] = false;
    }
    else if (start + len <= own.size && c < own.hashes.size())
    {
      // Unchanged (it matches the session's card, or no file of it uses it).
      hashes.hashes[c] = own.hashes[c];
      hashes.zero[c] = own.zero[c];
    }
    else
    {
      // Past the end of the card it was before: the new zeros Resize made.
      hashes.hashes[c] = HashChunk(std::vector<u8>(len, 0));
      hashes.zero[c] = true;
    }
  }
  image.Close();
  if (const std::optional<s64> mtime = ModificationTime(m_image_path))
    SaveHashCache(m_image_path, *mtime, hashes);
  INFO_LOG_FMT(BRAWLBACK, "SD card: wrote {} fetched chunks into {}", m_fetched_offsets.size(),
               m_image_path);
  return true;
}

bool SessionCard::ReadForPeer(u32 index, std::span<u8> data)
{
  if (m_mode == Mode::OwnCard || index >= ChunkCount() || data.size() != ChunkBytes(index))
    return false;
  // Where in this card's image (a moved chunk is at another place of it).
  u64 at = u64{index} * CHUNK_SIZE;
  if (m_mode == Mode::GuestCard)
  {
    const u32 chunk[] = {index};
    CheckLocal(chunk);
    std::lock_guard lock(m_fetch_mutex);
    switch (m_sources[index])
    {
    case Source::Local:
      break;
    case Source::Moved:
      at = m_moved.at(index);
      break;
    case Source::Fetched:
    {
      // Deliver writes this file under the same lock.
      const auto it = m_fetched_offsets.find(index);
      return it != m_fetched_offsets.end() &&
             m_fetched_file.Seek(static_cast<s64>(it->second), File::SeekOrigin::Begin) &&
             m_fetched_file.ReadBytes(data.data(), data.size());
    }
    default:
      return false;
    }
  }
  std::lock_guard lock(m_serve_mutex);
  if (!m_serve_image)
    m_serve_image.Open(m_image_path, "rb", File::SharedAccess::ReadWrite);
  return m_serve_image && m_serve_image.Seek(static_cast<s64>(at), File::SeekOrigin::Begin) &&
         m_serve_image.ReadBytes(data.data(), data.size());
}

std::vector<u32> SessionCard::ReadChunks(size_t max) const
{
  const size_t n = std::min(max, m_read_order.size());
  return {m_read_order.begin(), m_read_order.begin() + n};
}

void SessionCard::Detach()
{
  {
    std::lock_guard lock(m_fetch_mutex);
    m_detached = true;
    m_remote.reset();
  }
  m_fetch_cv.notify_all();
}
}  // namespace Machine::SDCard
