// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The SD card in a netplay session (Core/NetPlaySDCard): the session's writes never reach an
// image unless the host keeps them, they roll back with the machine, a guest plays on exactly
// the host's card (its own chunks where they match, the host's fetched otherwise), and nothing
// the other side sends is trusted (manifests, requests and chunks are checked; deleted data on
// the host's card is never sent).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/SDCardUtil.h"
#include "Common/SFMLHelper.h"
#include "Common/StringUtil.h"
#include "Core/NetPlayCommon.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlaySDCard.h"

using namespace NetPlay::SDCard;

namespace
{
constexpr u64 MiB = 1024 * 1024;

class NetPlaySDCardTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_dir = File::CreateTempDir();
    ASSERT_FALSE(m_dir.empty());
    m_old_user = File::GetUserPath(D_USER_IDX);
    File::SetUserPath(D_USER_IDX, m_dir + "/User/");
  }

  void TearDown() override
  {
    File::SetUserPath(D_USER_IDX, m_old_user);
    File::DeleteDirRecursively(m_dir);
  }

  std::string Path(const std::string& name) const { return m_dir + "/" + name; }

  static void WriteFile(const std::string& path, const std::vector<u8>& data)
  {
    File::IOFile f(path, "wb");
    ASSERT_TRUE(f.WriteBytes(data.data(), data.size()));
  }

  static std::vector<u8> ReadFile(const std::string& path)
  {
    File::IOFile f(path, "rb");
    std::vector<u8> data(f.GetSize());
    EXPECT_TRUE(f.ReadBytes(data.data(), data.size()));
    return data;
  }

  // A card of `chunks` chunks whose every chunk is different (and none all zero).
  static std::vector<u8> PatternCard(u32 chunks, u8 seed)
  {
    std::vector<u8> data(chunks * CHUNK_SIZE);
    for (size_t i = 0; i < data.size(); ++i)
      data[i] = static_cast<u8>((i / 7) * 31 + (i / CHUNK_SIZE) * 17 + seed + 1);
    for (u32 c = 0; c < chunks; ++c)
      data[c * CHUNK_SIZE] = static_cast<u8>(c + seed + 1) | 1;
    return data;
  }

  // A guest card whose fake host answers from `host_data` at once.
  struct FakeHost final : SessionCard::Remote
  {
    std::vector<u8> data;
    std::shared_ptr<const Manifest> manifest;
    std::weak_ptr<SessionCard> card;
    std::mutex mutex;
    std::vector<u32> requested;
    std::set<u32> unavailable;
    std::set<u32> corrupt;
    bool silent = false;
    int keep_alives = 0;

    void Request(std::span<const u32> chunks) override
    {
      {
        std::lock_guard lock(mutex);
        requested.insert(requested.end(), chunks.begin(), chunks.end());
      }
      if (silent)
        return;
      auto c = card.lock();
      if (!c)
        return;
      for (const u32 index : chunks)
      {
        ChunkData chunk;
        chunk.index = index;
        chunk.ok = !unavailable.contains(index);
        if (chunk.ok)
        {
          const u64 start = u64{index} * CHUNK_SIZE;
          chunk.data.assign(data.begin() + start,
                            data.begin() + start + manifest->ChunkBytes(index));
          if (corrupt.contains(index))
            chunk.data[5] ^= 0xFF;
        }
        c->Deliver(chunk);
      }
    }

    void KeepAlive() override { ++keep_alives; }

    std::vector<u32> Requested()
    {
      std::lock_guard lock(mutex);
      return requested;
    }
  };

  struct GuestSetup
  {
    std::shared_ptr<SessionCard> card;
    std::shared_ptr<FakeHost> host;
    std::shared_ptr<const Manifest> manifest;
  };

  GuestSetup MakeGuest(const std::vector<u8>& host_data, const std::vector<u8>* guest_data)
  {
    WriteFile(Path("host.raw"), host_data);
    auto manifest = BuildManifest(Path("host.raw"));
    EXPECT_TRUE(manifest.has_value());
    GuestSetup g;
    g.manifest = std::make_shared<const Manifest>(std::move(*manifest));
    std::optional<ImageHashes> local;
    if (guest_data)
    {
      WriteFile(Path("guest.raw"), *guest_data);
      local = HashImage(Path("guest.raw"));
      EXPECT_TRUE(local.has_value());
    }
    g.host = std::make_shared<FakeHost>();
    g.host->data = host_data;
    g.host->manifest = g.manifest;
    g.card = SessionCard::CreateGuestCard(Path("guest.raw"), g.manifest, local, Path("cache"),
                                          g.host);
    g.host->card = g.card;
    return g;
  }

  std::string m_dir;
  std::string m_old_user;
};

std::vector<u8> ReadAll(SessionCard& card)
{
  std::vector<u8> out(card.GetSize());
  EXPECT_TRUE(card.Read(0, out.data(), out.size()));
  return out;
}

// What a CardServer sent, parsed back.
struct SentChunk
{
  u8 pid;
  ChunkData chunk;
};

std::optional<SentChunk> ParseSent(u8 pid, sf::Packet& packet)
{
  NetPlay::MessageID mid;
  NetPlay::SyncSaveDataID sub;
  packet >> mid >> sub;
  if (mid != NetPlay::MessageID::SyncSaveData || sub != NetPlay::SyncSaveDataID::SDCardChunkData)
    return std::nullopt;
  auto chunk = ReadChunkData(packet);
  if (!chunk)
    return std::nullopt;
  return SentChunk{pid, std::move(*chunk)};
}
}  // namespace

// ------------------------------------------------------------------------------------ manifest

TEST_F(NetPlaySDCardTest, ManifestRoundTrip)
{
  Manifest m;
  m.size = 5 * CHUNK_SIZE + 1000;  // the last chunk is short
  m.chunks.resize(6);
  for (u32 i = 0; i < 6; ++i)
  {
    m.chunks[i].zero = i % 2 == 1;
    if (!m.chunks[i].zero)
      m.chunks[i].hash.fill(static_cast<u8>(i + 1));
  }
  EXPECT_EQ(m.ChunkBytes(5), 1000u);
  EXPECT_EQ(m.ChunkBytes(6), 0u);

  sf::Packet p;
  WriteManifest(m, p);
  auto r = ReadManifest(p);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->size, m.size);
  ASSERT_EQ(r->chunks.size(), m.chunks.size());
  for (size_t i = 0; i < m.chunks.size(); ++i)
  {
    EXPECT_EQ(r->chunks[i].zero, m.chunks[i].zero);
    EXPECT_EQ(r->chunks[i].hash, m.chunks[i].hash);
  }
}

TEST_F(NetPlaySDCardTest, HostileManifestsAreRefused)
{
  auto encode = [](u64 size, const std::vector<u8>& entries) {
    sf::Packet p;
    p << size;
    NetPlay::CompressBufferIntoPacket(entries, p);
    return p;
  };
  std::vector<u8> two_zero = {0, 0};

  {
    sf::Packet p = encode(0, {});
    EXPECT_FALSE(ReadManifest(p)) << "an empty card";
  }
  {
    sf::Packet p = encode(MAX_CARD_SIZE + 1, two_zero);
    EXPECT_FALSE(ReadManifest(p)) << "a card over the limit";
  }
  {
    sf::Packet p = encode(2 * CHUNK_SIZE, {0});
    EXPECT_FALSE(ReadManifest(p)) << "too few entries";
  }
  {
    sf::Packet p = encode(2 * CHUNK_SIZE, {0, 0, 0});
    EXPECT_FALSE(ReadManifest(p)) << "too many entries";
  }
  {
    sf::Packet p = encode(2 * CHUNK_SIZE, {0, 7});
    EXPECT_FALSE(ReadManifest(p)) << "an unknown entry kind";
  }
  {
    sf::Packet p = encode(2 * CHUNK_SIZE, {0, 1, 2, 3});
    EXPECT_FALSE(ReadManifest(p)) << "a hash cut short";
  }
  {
    // Claims more entry bytes than a card of that size can have.
    sf::Packet p;
    p << u64{CHUNK_SIZE};
    p << u64{1'000'000};
    EXPECT_FALSE(ReadManifest(p));
  }
  {
    sf::Packet p;
    p << u64{CHUNK_SIZE};
    EXPECT_FALSE(ReadManifest(p)) << "a packet cut short";
  }
  {
    // A compressed block bigger than LZO can produce.
    sf::Packet p;
    p << u64{CHUNK_SIZE} << u64{1} << u32{0x7FFFFFFF};
    EXPECT_FALSE(ReadManifest(p));
  }
}

TEST_F(NetPlaySDCardTest, ChunkRequestsAndDataAreChecked)
{
  {
    sf::Packet p;
    const u32 chunks[] = {3, 9, 4};
    WriteChunkRequest(chunks, p);
    auto r = ReadChunkRequest(p);
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, std::vector<u32>({3, 9, 4}));
  }
  {
    sf::Packet p;
    p << u16{0};
    EXPECT_FALSE(ReadChunkRequest(p));
  }
  {
    sf::Packet p;
    p << static_cast<u16>(MAX_REQUEST_CHUNKS + 1);
    for (u32 i = 0; i <= MAX_REQUEST_CHUNKS; ++i)
      p << i;
    EXPECT_FALSE(ReadChunkRequest(p)) << "more chunks than a request may ask for";
  }
  {
    sf::Packet p;
    p << u16{2} << u32{1};
    EXPECT_FALSE(ReadChunkRequest(p)) << "cut short";
  }
  {
    sf::Packet p;
    std::vector<u8> data(CHUNK_SIZE, 0x5A);
    WriteChunkData(12, data, p);
    auto r = ReadChunkData(p);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->index, 12u);
    EXPECT_TRUE(r->ok);
    EXPECT_EQ(r->data, data);
  }
  {
    sf::Packet p;
    WriteChunkUnavailable(7, p);
    auto r = ReadChunkData(p);
    ASSERT_TRUE(r);
    EXPECT_FALSE(r->ok);
  }
  {
    sf::Packet p;
    std::vector<u8> data(CHUNK_SIZE + 1, 1);
    WriteChunkData(1, data, p);
    EXPECT_FALSE(ReadChunkData(p)) << "bigger than a chunk";
  }
  {
    sf::Packet p;
    WriteChunkUnavailable(MAX_CHUNKS, p);
    EXPECT_FALSE(ReadChunkData(p)) << "an index past any card";
  }
}

// --------------------------------------------------------------------------------- FAT / hashes

TEST_F(NetPlaySDCardTest, FreeChunksFollowTheFat)
{
  const std::string path = Path("card.raw");
  ASSERT_TRUE(Common::SDCardCreate(64, path));
  const u64 size = 64 * MiB;

  // The layout SDCardCreate makes: 512-byte clusters, 32 reserved sectors, two FATs.
  std::array<u8, 512> bs{};
  {
    File::IOFile f(path, "rb");
    ASSERT_TRUE(f.ReadArray(&bs));
  }
  const u64 fat_sectors = bs[0x24] | (bs[0x25] << 8) | (bs[0x26] << 16) | (u64{bs[0x27]} << 24);
  const u64 fat_offset = 32 * 512;
  const u64 data_offset = (32 + 2 * fat_sectors) * 512;

  File::IOFile f(path, "r+b");
  auto free = FindFreeChunks(f, size);
  ASSERT_TRUE(free);
  ASSERT_EQ(free->size(), size / CHUNK_SIZE);

  // Everything before the data area is in use, and so is the root directory's cluster (2).
  for (u64 c = 0; c <= data_offset / CHUNK_SIZE; ++c)
    EXPECT_FALSE((*free)[c]) << c;
  // An empty card: the rest is free.
  const u64 first_free = data_offset / CHUNK_SIZE + 1;
  for (u64 c = first_free; c < free->size(); ++c)
    EXPECT_TRUE((*free)[c]) << c;

  // A cluster in use deep in the card makes its chunk in use, and only that chunk.
  const u64 cluster = (40 * MiB - data_offset) / 512 + 7;
  const u32 used = 0x0FFFFFFF;
  ASSERT_TRUE(f.Seek(static_cast<s64>(fat_offset + (cluster + 2) * 4), File::SeekOrigin::Begin));
  ASSERT_TRUE(f.WriteArray(&used, 1));
  ASSERT_TRUE(f.Flush());
  const u64 used_chunk = (data_offset + cluster * 512) / CHUNK_SIZE;
  free = FindFreeChunks(f, size);
  ASSERT_TRUE(free);
  EXPECT_FALSE((*free)[used_chunk]);
  EXPECT_TRUE((*free)[used_chunk - 1]);
  EXPECT_TRUE((*free)[used_chunk + 1]);
}

TEST_F(NetPlaySDCardTest, FreeChunksBehindAnMbr)
{
  ASSERT_TRUE(Common::SDCardCreate(64, Path("volume.raw")));
  const std::vector<u8> volume = ReadFile(Path("volume.raw"));
  constexpr u64 OFFSET = 1 * MiB;  // the partition starts at sector 2048

  std::vector<u8> disk(OFFSET + volume.size());
  std::copy(volume.begin(), volume.end(), disk.begin() + OFFSET);
  disk[0x1BE + 4] = 0x0C;  // FAT32 (LBA)
  const u32 lba = OFFSET / 512;
  std::memcpy(&disk[0x1BE + 8], &lba, 4);
  const u32 sectors = static_cast<u32>(volume.size() / 512);
  std::memcpy(&disk[0x1BE + 12], &sectors, 4);
  disk[510] = 0x55;
  disk[511] = 0xAA;
  WriteFile(Path("disk.raw"), disk);

  File::IOFile f(Path("disk.raw"), "rb");
  auto free = FindFreeChunks(f, disk.size());
  ASSERT_TRUE(free);
  // The MBR's chunk is in use; the end of the card is free.
  EXPECT_FALSE((*free)[0]);
  EXPECT_TRUE(free->back());
}

TEST_F(NetPlaySDCardTest, NotFatMeansEverythingInUse)
{
  WriteFile(Path("junk.raw"), PatternCard(4, 3));
  File::IOFile f(Path("junk.raw"), "rb");
  EXPECT_FALSE(FindFreeChunks(f, 4 * CHUNK_SIZE));

  // A boot sector whose numbers point outside the image.
  ASSERT_TRUE(Common::SDCardCreate(64, Path("card.raw")));
  std::vector<u8> card = ReadFile(Path("card.raw"));
  card.resize(8 * MiB);  // cut: the FATs and the data area no longer fit
  WriteFile(Path("cut.raw"), card);
  File::IOFile cut(Path("cut.raw"), "rb");
  EXPECT_FALSE(FindFreeChunks(cut, card.size()));
}

TEST_F(NetPlaySDCardTest, HashesAreCachedUntilTheImageChanges)
{
  const std::string path = Path("card.raw");
  std::vector<u8> data = PatternCard(6, 1);
  std::fill_n(data.begin() + 2 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  WriteFile(path, data);

  auto first = HashImage(path);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->hashes.size(), 6u);
  EXPECT_TRUE(first->zero[2]);
  EXPECT_FALSE(first->zero[3]);
  EXPECT_EQ(first->hashes[4],
            HashChunk(std::span<const u8>(data.data() + 4 * CHUNK_SIZE, CHUNK_SIZE)));

  // Same size and time: from the cache (identical).
  auto second = HashImage(path);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->hashes, first->hashes);

  // A change (with its own modification time) is hashed again.
  data[4 * CHUNK_SIZE + 9] ^= 0x40;
  WriteFile(path, data);
  std::filesystem::last_write_time(StringToPath(path),
                                   std::filesystem::file_time_type::clock::now() +
                                       std::chrono::hours(1));
  auto third = HashImage(path);
  ASSERT_TRUE(third);
  EXPECT_NE(third->hashes[4], first->hashes[4]);
  EXPECT_EQ(third->hashes[3], first->hashes[3]);

  // A cancelled hash gives nothing.
  std::atomic<bool> cancel{true};
  std::filesystem::last_write_time(StringToPath(path),
                                   std::filesystem::file_time_type::clock::now() +
                                       std::chrono::hours(2));
  EXPECT_FALSE(HashImage(path, &cancel));
}

TEST_F(NetPlaySDCardTest, ManifestZeroesFreeAndEmptyChunks)
{
  const std::string path = Path("card.raw");
  ASSERT_TRUE(Common::SDCardCreate(64, path));
  // Deleted data in a free cluster: not sent, read as zero.
  {
    std::vector<u8> junk(CHUNK_SIZE, 0xEE);
    File::IOFile f(path, "r+b");
    ASSERT_TRUE(f.Seek(50 * MiB, File::SeekOrigin::Begin));
    ASSERT_TRUE(f.WriteBytes(junk.data(), junk.size()));
  }
  auto m = BuildManifest(path);
  ASSERT_TRUE(m);
  EXPECT_FALSE(m->chunks[0].zero) << "the boot sector";
  EXPECT_TRUE(m->chunks[50 * MiB / CHUNK_SIZE].zero) << "a free chunk with deleted data";

  auto card = SessionCard::CreateHostCard(path, std::make_shared<const Manifest>(*m), false);
  std::vector<u8> chunk(CHUNK_SIZE, 1);
  ASSERT_TRUE(card->Read(50 * MiB, chunk.data(), chunk.size()));
  EXPECT_TRUE(std::ranges::all_of(chunk, [](u8 b) { return b == 0; }))
      << "the host plays on the same card as its guests";
}

// ------------------------------------------------------------------------------ the session card

TEST_F(NetPlaySDCardTest, OwnCardWritesNeverReachTheImage)
{
  const std::string path = Path("card.raw");
  const std::vector<u8> original = PatternCard(4, 2);
  WriteFile(path, original);

  auto card = SessionCard::CreateOwnCard(path, false);
  card->SetClockForTesting([] { return u64{0}; }, 100);
  ASSERT_TRUE(card->IsOpen());
  EXPECT_EQ(card->GetSize(), original.size());
  EXPECT_EQ(ReadAll(*card), original);

  // A write across a chunk boundary, of part of each chunk.
  std::vector<u8> w(1000, 0xAB);
  const u64 at = CHUNK_SIZE - 300;
  ASSERT_TRUE(card->Write(at, w.data(), w.size()));
  std::vector<u8> expected = original;
  std::copy(w.begin(), w.end(), expected.begin() + at);
  EXPECT_EQ(ReadAll(*card), expected) << "the game reads its writes";
  EXPECT_EQ(ReadFile(path), original) << "the image is untouched";

  EXPECT_FALSE(card->Finish()) << "a card that does not keep its writes";
  EXPECT_EQ(ReadFile(path), original);

  // Out of range.
  u8 b = 0;
  EXPECT_FALSE(card->Read(original.size(), &b, 1));
  EXPECT_FALSE(card->Write(original.size() - 1, w.data(), 2));
}

TEST_F(NetPlaySDCardTest, KeptWritesAreWrittenBackAtTheEnd)
{
  const std::string path = Path("card.raw");
  const std::vector<u8> original = PatternCard(4, 5);
  WriteFile(path, original);

  auto card = SessionCard::CreateOwnCard(path, true);
  card->SetClockForTesting([] { return u64{0}; }, 100);
  std::vector<u8> w(70000, 0x11);
  ASSERT_TRUE(card->Write(100, w.data(), w.size()));
  EXPECT_EQ(ReadFile(path), original) << "nothing reaches the image during the game";

  EXPECT_TRUE(card->Finish());
  std::vector<u8> expected = original;
  std::copy(w.begin(), w.end(), expected.begin() + 100);
  EXPECT_EQ(ReadFile(path), expected);
}

TEST_F(NetPlaySDCardTest, WritesRollBack)
{
  const std::string path = Path("card.raw");
  const std::vector<u8> original = PatternCard(3, 9);
  WriteFile(path, original);
  auto card = SessionCard::CreateOwnCard(path, false);
  u64 now = 0;
  card->SetClockForTesting([&] { return now; }, 1000);

  std::vector<u8> a(10, 0xA1), b(10, 0xB2), c(CHUNK_SIZE, 0xC3);
  EXPECT_EQ(card->GetWriteSeq(), 0u);
  ASSERT_TRUE(card->Write(5, a.data(), a.size()));
  const std::vector<u8> after_a = ReadAll(*card);
  ASSERT_TRUE(card->Write(5, b.data(), b.size()));        // the same chunk again
  ASSERT_TRUE(card->Write(CHUNK_SIZE, c.data(), c.size()));  // a whole other chunk
  EXPECT_EQ(card->GetWriteSeq(), 3u);

  card->RewindTo(1);
  EXPECT_EQ(card->GetWriteSeq(), 1u);
  EXPECT_EQ(ReadAll(*card), after_a);

  // The timeline goes on from there.
  ASSERT_TRUE(card->Write(5, b.data(), b.size()));
  EXPECT_EQ(card->GetWriteSeq(), 2u);
  card->RewindTo(0);
  EXPECT_EQ(ReadAll(*card), original);

  // History older than its window is let go; a rewind within the window still works.
  ASSERT_TRUE(card->Write(5, a.data(), a.size()));
  now = 5000;
  ASSERT_TRUE(card->Write(5, b.data(), b.size()));
  const std::vector<u8> after_b = ReadAll(*card);
  now = 5100;
  ASSERT_TRUE(card->Write(5, c.data(), 10));
  card->RewindTo(2);
  EXPECT_EQ(ReadAll(*card), after_b);
}

TEST_F(NetPlaySDCardTest, GuestPlaysOnTheHostsCard)
{
  const std::vector<u8> host = PatternCard(16, 1);
  std::vector<u8> guest = host;
  // Chunks 3 and 9 differ; the guest's card is shorter (chunks 14-15 are missing there).
  guest[3 * CHUNK_SIZE + 77] ^= 0x01;
  guest[9 * CHUNK_SIZE + 1] ^= 0x80;
  guest.resize(14 * CHUNK_SIZE);

  GuestSetup g = MakeGuest(host, &guest);
  EXPECT_EQ(g.card->GetMode(), SessionCard::Mode::GuestCard);
  EXPECT_EQ(g.card->GetSize(), host.size());
  EXPECT_EQ(g.card->MissingBytes(), 4 * CHUNK_SIZE);

  EXPECT_EQ(ReadAll(*g.card), host);
  std::vector<u32> requested = g.host->Requested();
  std::ranges::sort(requested);
  EXPECT_EQ(requested, std::vector<u32>({3, 9, 14, 15})) << "only what differs is fetched";
  EXPECT_EQ(g.card->MissingBytes(), 0u);

  // Reading again fetches nothing more; the guest's own image is never written.
  EXPECT_EQ(ReadAll(*g.card), host);
  EXPECT_EQ(g.host->Requested().size(), 4u);
  EXPECT_EQ(ReadFile(Path("guest.raw")), guest);
  EXPECT_FALSE(g.card->Finish()) << "a guest never keeps the session's writes";
}

TEST_F(NetPlaySDCardTest, GuestWithoutACardFetchesEverythingButZeroes)
{
  std::vector<u8> host = PatternCard(8, 4);
  std::fill_n(host.begin() + 5 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  GuestSetup g = MakeGuest(host, nullptr);
  EXPECT_EQ(g.card->MissingBytes(), 7 * CHUNK_SIZE);
  EXPECT_EQ(ReadAll(*g.card), host);
  for (const u32 c : g.host->Requested())
    EXPECT_NE(c, 5u) << "an all-zero chunk is never fetched";
}

TEST_F(NetPlaySDCardTest, PartialWriteFetchesTheRestOfTheChunk)
{
  const std::vector<u8> host = PatternCard(4, 6);
  GuestSetup g = MakeGuest(host, nullptr);
  g.card->SetClockForTesting([] { return u64{0}; }, 100);

  std::vector<u8> w(16, 0x77);
  ASSERT_TRUE(g.card->Write(2 * CHUNK_SIZE + 100, w.data(), w.size()));
  EXPECT_EQ(g.host->Requested(), std::vector<u32>({2}));

  // A whole-chunk write needs nothing from the host.
  std::vector<u8> whole(CHUNK_SIZE, 0x31);
  ASSERT_TRUE(g.card->Write(CHUNK_SIZE, whole.data(), whole.size()));
  EXPECT_EQ(g.host->Requested(), std::vector<u32>({2}));

  std::vector<u8> expected = host;
  std::copy(w.begin(), w.end(), expected.begin() + 2 * CHUNK_SIZE + 100);
  std::copy(whole.begin(), whole.end(), expected.begin() + CHUNK_SIZE);
  EXPECT_EQ(ReadAll(*g.card), expected);
}

TEST_F(NetPlaySDCardTest, BadChunksFromTheHostAreRejected)
{
  const std::vector<u8> host = PatternCard(6, 8);
  GuestSetup g = MakeGuest(host, nullptr);

  // A chunk whose bytes do not match the manifest is never used.
  g.host->corrupt.insert(2);
  std::vector<u8> buf(CHUNK_SIZE);
  EXPECT_FALSE(g.card->Read(2 * CHUNK_SIZE, buf.data(), buf.size()));

  // One the host says it cannot send fails the reads that need it, not the others.
  g.host->unavailable.insert(4);
  EXPECT_FALSE(g.card->Read(4 * CHUNK_SIZE, buf.data(), buf.size()));
  EXPECT_TRUE(g.card->Read(0, buf.data(), buf.size()));
  EXPECT_TRUE(std::equal(buf.begin(), buf.end(), host.begin()));

  // A chunk the guest did not need, of the wrong length, or past the card: ignored.
  ChunkData stray;
  stray.index = 1;
  stray.ok = true;
  stray.data.assign(10, 0);
  g.card->Deliver(stray);
  stray.index = 1000;
  g.card->Deliver(stray);
  EXPECT_TRUE(g.card->Read(CHUNK_SIZE, buf.data(), buf.size()));
  EXPECT_TRUE(std::equal(buf.begin(), buf.end(), host.begin() + CHUNK_SIZE));
}

TEST_F(NetPlaySDCardTest, DetachEndsAWaitingRead)
{
  const std::vector<u8> host = PatternCard(4, 2);
  GuestSetup g = MakeGuest(host, nullptr);
  g.host->silent = true;

  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    g.card->Detach();
  });
  const auto start = std::chrono::steady_clock::now();
  std::vector<u8> buf(16);
  EXPECT_FALSE(g.card->Read(0, buf.data(), buf.size()));
  stopper.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
  EXPECT_GT(g.host->keep_alives, 0) << "the game's connection is kept alive while it waits";
}

TEST_F(NetPlaySDCardTest, PrefetchTakesTheFirstChunksUpToItsBudget)
{
  const std::vector<u8> host = PatternCard(10, 3);
  GuestSetup g = MakeGuest(host, nullptr);
  EXPECT_TRUE(g.card->Prefetch(3 * CHUNK_SIZE, nullptr));
  EXPECT_EQ(g.host->Requested(), std::vector<u32>({0, 1, 2}));
  EXPECT_EQ(g.card->MissingBytes(), 7 * CHUNK_SIZE);
}

TEST_F(NetPlaySDCardTest, BackgroundFetchStreamsTheRest)
{
  const std::vector<u8> host = PatternCard(6, 7);
  GuestSetup g = MakeGuest(host, nullptr);
  g.card->StartBackgroundFetch();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (g.card->MissingBytes() != 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(g.card->MissingBytes(), 0u);
  g.card->Detach();
  EXPECT_EQ(ReadAll(*g.card), host) << "everything fetched reads without the host";
}

// ------------------------------------------------------------------------------ the card server

TEST_F(NetPlaySDCardTest, CardServerSendsOnlyChunksWithData)
{
  const std::string path = Path("host.raw");
  std::vector<u8> host = PatternCard(8, 1);
  std::fill_n(host.begin() + 6 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  WriteFile(path, host);
  auto manifest = std::make_shared<const Manifest>(*BuildManifest(path));

  std::mutex mutex;
  std::vector<SentChunk> sent;
  {
    CardServer server(path, manifest, [&](u8 pid, sf::Packet&& p) {
      auto s = ParseSent(pid, p);
      ASSERT_TRUE(s);
      std::lock_guard lock(mutex);
      sent.push_back(std::move(*s));
    });

    sf::Packet request;
    const u32 chunks[] = {1, 6, 99, 7};  // 6 is all zero, 99 is past the card
    WriteChunkRequest(chunks, request);
    server.OnRequest(2, request);

    sf::Packet malformed;
    malformed << u16{5};
    server.OnRequest(2, malformed);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
      {
        std::lock_guard lock(mutex);
        if (sent.size() >= 2)
          break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  ASSERT_EQ(sent.size(), 2u);
  for (const SentChunk& s : sent)
  {
    EXPECT_EQ(s.pid, 2);
    EXPECT_TRUE(s.chunk.ok);
    EXPECT_TRUE(s.chunk.index == 1 || s.chunk.index == 7);
    EXPECT_TRUE(std::equal(s.chunk.data.begin(), s.chunk.data.end(),
                           host.begin() + s.chunk.index * CHUNK_SIZE));
  }
}

TEST_F(NetPlaySDCardTest, CardServerRefusesAChangedChunk)
{
  const std::string path = Path("host.raw");
  std::vector<u8> host = PatternCard(4, 1);
  WriteFile(path, host);
  auto manifest = std::make_shared<const Manifest>(*BuildManifest(path));
  host[2 * CHUNK_SIZE + 3] ^= 1;
  WriteFile(path, host);

  std::mutex mutex;
  std::vector<SentChunk> sent;
  {
    CardServer server(path, manifest, [&](u8 pid, sf::Packet&& p) {
      auto s = ParseSent(pid, p);
      ASSERT_TRUE(s);
      std::lock_guard lock(mutex);
      sent.push_back(std::move(*s));
    });
    sf::Packet request;
    const u32 chunks[] = {2};
    WriteChunkRequest(chunks, request);
    server.OnRequest(3, request);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
      {
        std::lock_guard lock(mutex);
        if (!sent.empty())
          break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_FALSE(sent[0].chunk.ok);
}

TEST_F(NetPlaySDCardTest, HostToGuestEndToEnd)
{
  // The real wire format between a CardServer and a guest card.
  const std::string host_path = Path("host.raw");
  std::vector<u8> host = PatternCard(12, 2);
  WriteFile(host_path, host);
  auto built = BuildManifest(host_path);
  ASSERT_TRUE(built);
  sf::Packet manifest_packet;
  WriteManifest(*built, manifest_packet);
  auto manifest = ReadManifest(manifest_packet);
  ASSERT_TRUE(manifest);
  auto shared = std::make_shared<const Manifest>(std::move(*manifest));

  std::weak_ptr<SessionCard> weak_card;
  CardServer server(host_path, shared, [&](u8, sf::Packet&& p) {
    auto s = ParseSent(0, p);
    ASSERT_TRUE(s);
    if (auto card = weak_card.lock())
      card->Deliver(s->chunk);
  });

  struct Wire final : SessionCard::Remote
  {
    CardServer* server;
    void Request(std::span<const u32> chunks) override
    {
      sf::Packet p;
      WriteChunkRequest(chunks, p);
      server->OnRequest(2, p);
    }
  };
  auto wire = std::make_shared<Wire>();
  wire->server = &server;

  std::vector<u8> guest = PatternCard(12, 9);  // nothing in common
  WriteFile(Path("guest.raw"), guest);
  auto card = SessionCard::CreateGuestCard(Path("guest.raw"), shared, HashImage(Path("guest.raw")),
                                           Path("cache"), wire);
  weak_card = card;
  EXPECT_EQ(ReadAll(*card), host);
  card->Detach();
}

// -------------------------------------------------------------------------------- code names

TEST(NetPlayCodeNames, AreCleanedAndBounded)
{
  EXPECT_EQ(NetPlay::SanitizeCodeName("  Unlock\tAll\x01 Characters \n"), "UnlockAll Characters");
  const std::string long_name(100, 'x');
  EXPECT_EQ(NetPlay::SanitizeCodeName(long_name).size(), NetPlay::MAX_CODE_NAME_BYTES);

  // Cut inside a UTF-8 sequence: the partial character goes.
  std::string utf8(NetPlay::MAX_CODE_NAME_BYTES - 1, 'a');
  utf8 += "\xE3\x81\x82";  // a 3-byte character straddling the limit
  const std::string cut = NetPlay::SanitizeCodeName(utf8);
  EXPECT_EQ(cut, std::string(NetPlay::MAX_CODE_NAME_BYTES - 1, 'a'));

  std::vector<std::string> names = {"Infinite Time", "", "\x02\x03", "All Stages"};
  sf::Packet p;
  NetPlay::WriteCodeNames(names, p);
  EXPECT_EQ(NetPlay::ReadCodeNames(p), std::vector<std::string>({"Infinite Time", "All Stages"}));

  sf::Packet none;
  EXPECT_TRUE(NetPlay::ReadCodeNames(none).empty()) << "an older host sends no names";

  sf::Packet too_many;
  too_many << static_cast<u16>(NetPlay::MAX_CODE_NAMES + 1);
  EXPECT_TRUE(NetPlay::ReadCodeNames(too_many).empty());

  std::vector<std::string> many;
  for (int i = 0; i < 11; ++i)
    many.push_back(fmt::format("Code {}", i));
  EXPECT_EQ(NetPlay::DescribeCodeNames(many),
            "Code 0, Code 1, Code 2, Code 3, Code 4, Code 5, Code 6, Code 7 and 3 more");
}
