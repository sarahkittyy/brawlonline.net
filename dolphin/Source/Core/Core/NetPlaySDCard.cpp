// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlaySDCard.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fmt/format.h>
#include <lzo/lzo1x.h>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/SFMLHelper.h"
#include "Common/StringUtil.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/NetPlayCommon.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace NetPlay::SDCard
{
namespace
{
constexpr u8 MANIFEST_ENTRY_ZERO = 0;
constexpr u8 MANIFEST_ENTRY_DATA = 1;
constexpr size_t MANIFEST_ENTRY_MAX_BYTES = 1 + sizeof(Hash);

constexpr u32 LZO_IN_LEN = 64 * 1024;
constexpr u32 LZO_OUT_LEN = LZO_IN_LEN + (LZO_IN_LEN / 16) + 64 + 3;

// Chunks a guest asks for at once, across all its waiting reads and prefetches (the host queues
// at most CardServer::MAX_QUEUED_PER_PLAYER per guest).
constexpr size_t MAX_OUTSTANDING_REQUESTS = 128;
// A chunk the host sent wrong this many times counts as unavailable.
constexpr int MAX_BAD_DELIVERIES = 3;

constexpr u32 HASH_CACHE_MAGIC = 0x4453504E;  // "NPSD"
constexpr u32 HASH_CACHE_VERSION = 1;

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

// LZO-compressed data as NetPlay::CompressBufferIntoPacket writes it, read without trusting the
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
  return fmt::format("{}NetPlaySD/{:016x}.bin", File::GetUserPath(D_CACHE_IDX), key);
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
  const XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
  Hash out;
  for (int i = 0; i < 8; ++i)
  {
    out[i] = static_cast<u8>(h.low64 >> (8 * i));
    out[8 + i] = static_cast<u8>(h.high64 >> (8 * i));
  }
  return out;
}

std::optional<std::vector<bool>> FindFreeChunks(File::IOFile& image, u64 image_size)
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
  std::vector<u8> fat(static_cast<size_t>(volume->fat_bytes));
  if (!image.Seek(static_cast<s64>(volume->fat_offset), File::SeekOrigin::Begin) ||
      !image.ReadBytes(fat.data(), fat.size()))
  {
    return std::nullopt;
  }

  const u64 cluster_bytes = volume->bytes_per_sector * volume->sectors_per_cluster;
  auto cluster_free = [&](u64 cluster) {
    const u64 entry = cluster + 2;
    if (volume->fat32)
      return (LE32(&fat[entry * 4]) & 0x0FFFFFFF) == 0;
    return LE16(&fat[entry * 2]) == 0;
  };

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

std::optional<ImageHashes> HashImage(const std::string& path, const std::atomic<bool>* cancel)
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

  constexpr u32 CHUNKS_PER_READ = 64;
  std::vector<u8> buffer(CHUNKS_PER_READ * CHUNK_SIZE);
  for (u32 c = 0; c < count; c += CHUNKS_PER_READ)
  {
    if (cancel && cancel->load())
      return std::nullopt;
    const u64 start = u64{c} * CHUNK_SIZE;
    const u64 len = std::min<u64>(buffer.size(), size - start);
    if (!file.ReadBytes(buffer.data(), len))
      return std::nullopt;
    for (u32 i = 0; i < CHUNKS_PER_READ && c + i < count; ++i)
    {
      const u64 off = u64{i} * CHUNK_SIZE;
      const std::span<const u8> chunk(buffer.data() + off, std::min(CHUNK_SIZE, len - off));
      result.hashes[c + i] = HashChunk(chunk);
      result.zero[c + i] = IsAllZero(chunk);
    }
  }

  // Only cache what the file still is (it could have changed while it was read).
  if (mtime && ModificationTime(path) == mtime)
    SaveHashCache(path, *mtime, result);
  return result;
}

std::optional<Manifest> BuildManifest(const std::string& path, const std::atomic<bool>* cancel)
{
  std::optional<ImageHashes> hashes = HashImage(path, cancel);
  if (!hashes)
    return std::nullopt;

  File::IOFile file(path, "rb");
  if (!file || file.GetSize() != hashes->size)
    return std::nullopt;
  const std::optional<std::vector<bool>> free = FindFreeChunks(file, hashes->size);

  Manifest manifest;
  manifest.size = hashes->size;
  manifest.chunks.resize(hashes->hashes.size());
  for (size_t i = 0; i < manifest.chunks.size(); ++i)
  {
    ChunkInfo& info = manifest.chunks[i];
    info.zero = hashes->zero[i] || (free && (*free)[i]);
    if (!info.zero)
      info.hash = hashes->hashes[i];
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
  CompressBufferIntoPacket(entries, packet);
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
  CompressBufferIntoPacket(data, packet);
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

CardServer::CardServer(const std::string& image_path, std::shared_ptr<const Manifest> manifest,
                       SendFunction send)
    : m_image_path(image_path), m_manifest(std::move(manifest)),
      m_header([](sf::Packet& packet) {
        packet << MessageID::SyncSaveData << SyncSaveDataID::SDCardChunkData;
      }),
      m_send(std::move(send))
{
  m_thread = std::thread(&CardServer::ThreadFunc, this);
}

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

void CardServer::OnRequest(u8 pid, sf::Packet& packet)
{
  const std::optional<std::vector<u32>> chunks = ReadChunkRequest(packet);
  if (!chunks)
  {
    WARN_LOG_FMT(NETPLAY, "SD card: malformed chunk request from player {}", pid);
    return;
  }

  {
    std::lock_guard lock(m_mutex);
    size_t& queued = m_queued_per_player[pid];
    for (const u32 c : *chunks)
    {
      // Only the chunks that hold data: a chunk no file uses may hold deleted files.
      if (c >= m_manifest->ChunkCount() || m_manifest->chunks[c].zero)
      {
        WARN_LOG_FMT(NETPLAY, "SD card: player {} asked for chunk {}, which it may not have", pid,
                     c);
        continue;
      }
      if (queued >= MAX_QUEUED_PER_PLAYER)
        break;
      m_queue.emplace_back(pid, c);
      ++queued;
    }
  }
  m_cv.notify_one();
}

void CardServer::ThreadFunc()
{
  File::IOFile image;
  if (!m_read)
    image.Open(m_image_path, "rb");
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
    }

    const auto [pid, index] = item;
    const u64 len = m_manifest->ChunkBytes(index);
    const std::span<u8> data(buffer.data(), len);
    sf::Packet packet;
    m_header(packet);
    const bool read =
        m_read ? m_read(index, data) :
                 image &&
                     image.Seek(static_cast<s64>(u64{index} * CHUNK_SIZE), File::SeekOrigin::Begin) &&
                     image.ReadBytes(data.data(), data.size());
    if (read && HashChunk(data) == m_manifest->chunks[index].hash)
    {
      WriteChunkData(index, data, packet);
    }
    else
    {
      // The card changed since its manifest was built (or cannot be read), or this player has
      // not got the chunk (a guest serving what it has).
      if (!m_read || read)
        WARN_LOG_FMT(NETPLAY, "SD card: chunk {} no longer matches the manifest", index);
      WriteChunkUnavailable(index, packet);
    }
    m_send(pid, std::move(packet));
  }
}

// --------------------------------------------------------------------------------- SessionCard

SessionCard::SessionCard(Mode mode, const std::string& image_path,
                         std::shared_ptr<const Manifest> manifest, bool write_back)
    : m_mode(mode), m_image_path(image_path), m_manifest(std::move(manifest)),
      m_write_back(write_back)
{
  m_clock = [] { return Core::System::GetInstance().GetCoreTiming().GetTicks(); };
  m_history_ticks = u64{Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond()} * 10;
}

std::shared_ptr<SessionCard> SessionCard::CreateOwnCard(const std::string& image_path,
                                                        bool write_back)
{
  std::shared_ptr<SessionCard> card(
      new SessionCard(Mode::OwnCard, image_path, nullptr, write_back));
  card->m_image.Open(image_path, "rb");
  card->m_size = card->m_image ? card->m_image.GetSize() : 0;
  if (card->m_size > MAX_CARD_SIZE)
    card->m_size = 0;
  return card;
}

std::shared_ptr<SessionCard> SessionCard::CreateHostCard(const std::string& image_path,
                                                         std::shared_ptr<const Manifest> manifest,
                                                         bool write_back)
{
  std::shared_ptr<SessionCard> card(
      new SessionCard(Mode::HostCard, image_path, std::move(manifest), write_back));
  card->m_image.Open(image_path, "rb");
  card->m_size = card->m_manifest->size;
  return card;
}

std::shared_ptr<SessionCard>
SessionCard::CreateGuestCard(const std::string& image_path,
                             std::shared_ptr<const Manifest> manifest,
                             const std::optional<ImageHashes>& local, const std::string& cache_dir,
                             std::shared_ptr<Remote> remote)
{
  std::shared_ptr<SessionCard> card(
      new SessionCard(Mode::GuestCard, image_path, std::move(manifest), false));
  card->m_size = card->m_manifest->size;
  card->m_remote = std::move(remote);
  if (local)
    card->m_image.Open(image_path, "rb");
  const bool have_image = card->m_image && local && card->m_image.GetSize() == local->size;

  const u32 count = card->ChunkCount();
  card->m_sources.resize(count, Source::Missing);
  for (u32 i = 0; i < count; ++i)
  {
    const ChunkInfo& info = card->m_manifest->chunks[i];
    if (info.zero)
    {
      card->m_sources[i] = Source::Zero;
      continue;
    }
    // This card's chunk must be as long as the host's for the hashes to say they match.
    const u64 len = card->ChunkBytes(i);
    const u64 start = u64{i} * CHUNK_SIZE;
    if (have_image && i < local->hashes.size() && start + len <= local->size &&
        std::min(CHUNK_SIZE, local->size - start) == len && local->hashes[i] == info.hash)
    {
      card->m_sources[i] = Source::Local;
    }
    else
    {
      card->m_missing.push_back(i);
    }
  }

  static std::atomic<u32> s_counter{0};
  card->m_fetched_path =
      fmt::format("{}/fetched-{}-{}.bin", cache_dir, NowMs(), s_counter.fetch_add(1));
  if (File::CreateFullPath(card->m_fetched_path))
    card->m_fetched_file.Open(card->m_fetched_path, "w+b");
  if (!card->m_fetched_file)
    ERROR_LOG_FMT(NETPLAY, "SD card: could not create {}", card->m_fetched_path);

  INFO_LOG_FMT(NETPLAY, "SD card: the host's card is {} bytes, {} chunks to fetch ({} bytes)",
               card->m_size, card->m_missing.size(), card->MissingBytes());
  return card;
}

SessionCard::~SessionCard()
{
  Detach();
  if (m_background_thread.joinable())
    m_background_thread.join();
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

bool SessionCard::ReadBase(u32 index, u8* dst)
{
  const u64 len = ChunkBytes(index);
  const u64 start = u64{index} * CHUNK_SIZE;
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
    case Source::Fetched:
    {
      const auto it = m_fetched_offsets.find(index);
      return it != m_fetched_offsets.end() &&
             m_fetched_file.Seek(static_cast<s64>(it->second), File::SeekOrigin::Begin) &&
             m_fetched_file.ReadBytes(dst, len);
    }
    case Source::Missing:
      return false;
    }
    return false;
  }
  }
  return false;
}

bool SessionCard::ReadChunk(u32 index, u8* dst)
{
  if (const auto it = m_overlay.find(index); it != m_overlay.end())
  {
    std::memcpy(dst, it->second->data(), it->second->size());
    return true;
  }
  if (m_mode == Mode::GuestCard)
  {
    const u32 chunk[] = {index};
    if (!WaitForChunks(chunk, true))
      return false;
  }
  return ReadBase(index, dst);
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

  // A guest fetches everything the read needs at once.
  if (m_mode == Mode::GuestCard)
  {
    std::vector<u32> needed;
    {
      std::lock_guard lock(m_fetch_mutex);
      for (u32 c = first; c <= last; ++c)
      {
        if (m_sources[c] == Source::Missing && !m_overlay.contains(c))
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

  std::vector<u8> chunk(CHUNK_SIZE);
  u64 done = 0;
  for (u32 c = first; c <= last; ++c)
  {
    const u64 chunk_start = u64{c} * CHUNK_SIZE;
    const u64 from = std::max(offset, chunk_start) - chunk_start;
    const u64 to = std::min(offset + size, chunk_start + ChunkBytes(c)) - chunk_start;
    if (!ReadChunk(c, chunk.data()))
      return false;
    std::memcpy(dst + done, chunk.data() + from, to - from);
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
  // A write of part of a chunk keeps the rest of it: a guest fetches those chunks first, so a
  // write that fails changes nothing.
  if (m_mode == Mode::GuestCard)
  {
    std::vector<u32> needed;
    {
      std::lock_guard lock(m_fetch_mutex);
      for (const u32 c : {first, last})
      {
        const u64 chunk_start = u64{c} * CHUNK_SIZE;
        const bool partial = offset > chunk_start || offset + size < chunk_start + ChunkBytes(c);
        if (partial && m_sources[c] == Source::Missing && !m_overlay.contains(c) &&
            std::ranges::find(needed, c) == needed.end())
        {
          needed.push_back(c);
        }
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
    if ((from != 0 || to != len) && !ReadChunk(c, buffer->data()))
    {
      ERROR_LOG_FMT(NETPLAY, "SD card: could not read chunk {} to write part of it", c);
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
    WARN_LOG_FMT(NETPLAY, "SD card: cannot go forward to write {} from {}", write_seq,
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
    WARN_LOG_FMT(NETPLAY, "SD card: rewound past the kept history (to write {})", write_seq);
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
      WARN_LOG_FMT(NETPLAY, "SD card: the host could not send chunk {}", chunk.index);
      m_unavailable.insert(chunk.index);
    }
    else if (!valid)
    {
      // A wrong chunk is dropped and asked for again, a few times.
      WARN_LOG_FMT(NETPLAY, "SD card: chunk {} from the host does not match its hash",
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
  for (const u32 c : m_missing)
  {
    if (m_sources[c] == Source::Missing)
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
        ERROR_LOG_FMT(NETPLAY, "SD card: the host did not send its chunks in time");
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
      OSD::AddMessage("Loading the host's SD card...", 3000);
    }
  }
}

bool SessionCard::Prefetch(u64 budget, const std::atomic<bool>* cancel,
                           const std::function<void(u64 done, u64 total)>& progress)
{
  if (m_mode != Mode::GuestCard)
    return true;

  std::vector<u32> chunks;
  u64 total = 0;
  {
    std::lock_guard lock(m_fetch_mutex);
    for (const u32 c : m_missing)
    {
      if (total >= budget)
        break;
      if (m_sources[c] == Source::Missing)
      {
        chunks.push_back(c);
        total += ChunkBytes(c);
      }
    }
  }

  u64 done = 0;
  for (size_t i = 0; i < chunks.size(); i += MAX_OUTSTANDING_REQUESTS)
  {
    if (cancel && cancel->load())
      return false;
    const size_t n = std::min(MAX_OUTSTANDING_REQUESTS, chunks.size() - i);
    const std::span<const u32> window(chunks.data() + i, n);
    if (!WaitForChunks(window, false))
      return false;
    for (const u32 c : window)
      done += ChunkBytes(c);
    if (progress)
      progress(done, total);
  }
  return true;
}

void SessionCard::StartBackgroundFetch(std::span<const u32> first)
{
  if (m_mode != Mode::GuestCard || m_background_thread.joinable())
    return;
  {
    std::lock_guard lock(m_fetch_mutex);
    m_background_order.clear();
    m_background_order.reserve(m_missing.size());
    std::vector<bool> queued(ChunkCount(), false);
    for (const u32 c : first)
    {
      if (c < ChunkCount() && m_sources[c] == Source::Missing && !queued[c])
      {
        queued[c] = true;
        m_background_order.push_back(c);
      }
    }
    for (const u32 c : m_missing)
    {
      if (!queued[c])
        m_background_order.push_back(c);
    }
  }
  m_background_thread = std::thread(&SessionCard::BackgroundThreadFunc, this);
}

bool SessionCard::ReadForPeer(u32 index, std::span<u8> data)
{
  if (m_mode == Mode::OwnCard || index >= ChunkCount() || data.size() != ChunkBytes(index))
    return false;
  const u64 start = u64{index} * CHUNK_SIZE;
  if (m_mode == Mode::GuestCard)
  {
    std::lock_guard lock(m_fetch_mutex);
    switch (m_sources[index])
    {
    case Source::Local:
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
    m_serve_image.Open(m_image_path, "rb");
  return m_serve_image &&
         m_serve_image.Seek(static_cast<s64>(start), File::SeekOrigin::Begin) &&
         m_serve_image.ReadBytes(data.data(), data.size());
}

std::vector<u32> SessionCard::ReadChunks(size_t max) const
{
  const size_t n = std::min(max, m_read_order.size());
  return {m_read_order.begin(), m_read_order.begin() + n};
}

void SessionCard::BackgroundThreadFunc()
{
  constexpr u32 BATCH = 8;
  const u64 start = NowMs();
  u64 streamed = 0;
  size_t cursor = 0;

  while (!m_stop_background.load())
  {
    std::vector<u32> batch;
    {
      std::lock_guard lock(m_fetch_mutex);
      if (m_detached || m_fetched_bytes >= BACKGROUND_CAP_BYTES)
        break;
      while (cursor < m_background_order.size() && batch.size() < BATCH)
      {
        const u32 c = m_background_order[cursor++];
        if (m_sources[c] == Source::Missing && !m_unavailable.contains(c) &&
            !m_requested_at.contains(c))
        {
          batch.push_back(c);
        }
      }
    }
    if (batch.empty())
      break;

    // A failed chunk only fails the reads that need it; the stream goes on.
    WaitForChunks(batch, false);
    for (const u32 c : batch)
      streamed += ChunkBytes(c);

    // Keep to the rate.
    const u64 due_ms = streamed * 1000 / BACKGROUND_BYTES_PER_SECOND;
    while (!m_stop_background.load() && NowMs() - start < due_ms)
    {
      std::unique_lock lock(m_fetch_mutex);
      m_fetch_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
  }
  INFO_LOG_FMT(NETPLAY, "SD card: background fetch done ({} bytes)", streamed);
}

void SessionCard::Detach()
{
  {
    std::lock_guard lock(m_fetch_mutex);
    m_detached = true;
    m_remote.reset();
  }
  m_stop_background = true;
  m_fetch_cv.notify_all();
}

bool SessionCard::Finish()
{
  Detach();
  if (m_background_thread.joinable())
    m_background_thread.join();

  if (!m_write_back || m_overlay.empty() || m_mode == Mode::GuestCard)
    return false;

  m_image.Close();
  File::IOFile image(m_image_path, "r+b");
  if (!image)
  {
    ERROR_LOG_FMT(NETPLAY, "SD card: could not open {} to keep the session's writes",
                  m_image_path);
    return false;
  }
  for (const auto& [index, data] : m_overlay)
  {
    if (!image.Seek(static_cast<s64>(u64{index} * CHUNK_SIZE), File::SeekOrigin::Begin) ||
        !image.WriteBytes(data->data(), data->size()))
    {
      ERROR_LOG_FMT(NETPLAY, "SD card: could not write chunk {} back to {}", index,
                    m_image_path);
      return true;
    }
  }
  INFO_LOG_FMT(NETPLAY, "SD card: kept {} written chunks in {}", m_overlay.size(), m_image_path);
  m_overlay.clear();
  return true;
}
}  // namespace NetPlay::SDCard
