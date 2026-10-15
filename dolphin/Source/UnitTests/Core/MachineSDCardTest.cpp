// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The SD card of a whole-machine session (Core/Rollback/MachineSDCard): the session's writes never
// reach an image and they roll back with the machine, a joiner plays exactly the session's card
// (its own chunks where they match, the others fetched before its join), and nothing the other
// players send is trusted (manifests, file lists, requests and chunks are checked; deleted data on
// a card is never sent).

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
#include "Core/Rollback/MachineSDCard.h"

using namespace Machine::SDCard;

namespace
{
constexpr u64 MiB = 1024 * 1024;

class MachineSDCardTest : public ::testing::Test
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
    if (guest_data)
      WriteFile(Path("guest.raw"), *guest_data);
    g.host = std::make_shared<FakeHost>();
    g.host->data = host_data;
    g.host->manifest = g.manifest;
    g.card = SessionCard::CreateGuestCard(Path("guest.raw"), g.manifest, Path("cache"), g.host);
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

// What a CardServer sent, parsed back (its packets start with the caller's header byte).
constexpr u8 TEST_HEADER = 0x5A;

struct SentChunk
{
  u8 pid;
  ChunkData chunk;
};

std::optional<SentChunk> ParseSent(u8 pid, sf::Packet& packet)
{
  u8 header = 0;
  packet >> header;
  if (header != TEST_HEADER)
    return std::nullopt;
  auto chunk = ReadChunkData(packet);
  if (!chunk)
    return std::nullopt;
  return SentChunk{pid, std::move(*chunk)};
}
}  // namespace

// ------------------------------------------------------------------------------------ manifest

TEST_F(MachineSDCardTest, ManifestRoundTrip)
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

TEST_F(MachineSDCardTest, HostileManifestsAreRefused)
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

TEST_F(MachineSDCardTest, ChunkRequestsAndDataAreChecked)
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

TEST_F(MachineSDCardTest, FreeChunksFollowTheFat)
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

TEST_F(MachineSDCardTest, FreeChunksBehindAnMbr)
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

TEST_F(MachineSDCardTest, NotFatMeansEverythingInUse)
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

TEST_F(MachineSDCardTest, HashesAreCachedUntilTheImageChanges)
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

TEST_F(MachineSDCardTest, ManifestZeroesFreeAndEmptyChunks)
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

  auto card = SessionCard::CreateHostCard(path, std::make_shared<const Manifest>(*m));
  std::vector<u8> chunk(CHUNK_SIZE, 1);
  ASSERT_TRUE(card->Read(50 * MiB, chunk.data(), chunk.size()));
  EXPECT_TRUE(std::ranges::all_of(chunk, [](u8 b) { return b == 0; }))
      << "the starter plays the same card as its joiners";
}

// ------------------------------------------------------------------------------ the session card

TEST_F(MachineSDCardTest, OwnCardWritesNeverReachTheImage)
{
  const std::string path = Path("card.raw");
  const std::vector<u8> original = PatternCard(4, 2);
  WriteFile(path, original);

  auto card = SessionCard::CreateOwnCard(path);
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


  // Out of range.
  u8 b = 0;
  EXPECT_FALSE(card->Read(original.size(), &b, 1));
  EXPECT_FALSE(card->Write(original.size() - 1, w.data(), 2));
}

TEST_F(MachineSDCardTest, WritesRollBack)
{
  const std::string path = Path("card.raw");
  const std::vector<u8> original = PatternCard(3, 9);
  WriteFile(path, original);
  auto card = SessionCard::CreateOwnCard(path);
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

TEST_F(MachineSDCardTest, GuestPlaysOnTheHostsCard)
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
  EXPECT_EQ(g.card->MissingBytes(), 2 * CHUNK_SIZE) << "chunks 14 and 15: its card has none";
  EXPECT_EQ(g.card->UncheckedBytes(), 14 * CHUNK_SIZE);

  EXPECT_EQ(ReadAll(*g.card), host);
  std::vector<u32> requested = g.host->Requested();
  std::ranges::sort(requested);
  EXPECT_EQ(requested, std::vector<u32>({3, 9, 14, 15})) << "only what differs is fetched";
  EXPECT_EQ(g.card->MissingBytes(), 0u);
  EXPECT_EQ(g.card->UncheckedBytes(), 0u);

  // Reading again fetches nothing more; the guest's own image is never written.
  EXPECT_EQ(ReadAll(*g.card), host);
  EXPECT_EQ(g.host->Requested().size(), 4u);
  EXPECT_EQ(ReadFile(Path("guest.raw")), guest);
}

TEST_F(MachineSDCardTest, GuestWithoutACardFetchesEverythingButZeroes)
{
  std::vector<u8> host = PatternCard(8, 4);
  std::fill_n(host.begin() + 5 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  GuestSetup g = MakeGuest(host, nullptr);
  EXPECT_EQ(g.card->MissingBytes(), 7 * CHUNK_SIZE);
  EXPECT_EQ(ReadAll(*g.card), host);
  for (const u32 c : g.host->Requested())
    EXPECT_NE(c, 5u) << "an all-zero chunk is never fetched";
}

TEST_F(MachineSDCardTest, PartialWriteFetchesTheRestOfTheChunk)
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

TEST_F(MachineSDCardTest, BadChunksFromTheHostAreRejected)
{
  const std::vector<u8> host = PatternCard(6, 8);
  GuestSetup g = MakeGuest(host, nullptr);

  // A chunk whose bytes do not match the manifest is never used. One the host says it cannot
  // send (4, asked for with the chunks after 2) fails the reads that need it, not the others.
  g.host->corrupt.insert(2);
  g.host->unavailable.insert(4);
  std::vector<u8> buf(CHUNK_SIZE);
  EXPECT_FALSE(g.card->Read(2 * CHUNK_SIZE, buf.data(), buf.size()));
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

TEST_F(MachineSDCardTest, DetachEndsAWaitingRead)
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

// Waits for a sync (StartSync) to end.
SessionCard::SyncProgress WaitForSync(SessionCard& card)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline)
  {
    const auto p = card.GetSyncProgress();
    if (p.finished || p.failed)
      return p;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return card.GetSyncProgress();
}

TEST_F(MachineSDCardTest, SyncFetchesEverythingBeforeTheGame)
{
  const std::vector<u8> host = PatternCard(6, 7);
  GuestSetup g = MakeGuest(host, nullptr);
  g.card->StartSync();
  const auto p = WaitForSync(*g.card);
  EXPECT_TRUE(p.finished);
  EXPECT_FALSE(p.failed);
  EXPECT_EQ(p.total, 6 * CHUNK_SIZE);
  EXPECT_EQ(p.done, 6 * CHUNK_SIZE);
  EXPECT_EQ(g.card->MissingBytes(), 0u);
  g.card->Detach();
  EXPECT_EQ(ReadAll(*g.card), host) << "everything fetched reads without the host";
  EXPECT_EQ(g.card->GetStats().waits, 0u) << "no read waited";
}

TEST_F(MachineSDCardTest, SyncFailsWhenNoPlayerHasAChunk)
{
  const std::vector<u8> host = PatternCard(6, 7);
  GuestSetup g = MakeGuest(host, nullptr);
  g.host->unavailable.insert(4);
  g.card->StartSync();
  const auto p = WaitForSync(*g.card);
  EXPECT_TRUE(p.failed);
  EXPECT_FALSE(p.finished);
  g.card->Detach();
}

TEST_F(MachineSDCardTest, OwnHashesSayWhatDiffersAtOnce)
{
  // The join's check: the joiner's whole card hashed first, so what differs is known before
  // anything is read, and only that is fetched.
  const std::vector<u8> host = PatternCard(40, 3);
  std::vector<u8> guest = host;
  guest[2 * CHUNK_SIZE + 1] ^= 0x01;
  guest[33 * CHUNK_SIZE + 500] ^= 0x02;
  WriteFile(Path("host.raw"), host);
  WriteFile(Path("guest.raw"), guest);
  auto manifest = std::make_shared<const Manifest>(*BuildManifest(Path("host.raw")));
  const auto own = HashImage(Path("guest.raw"));
  ASSERT_TRUE(own);
  auto fake = std::make_shared<FakeHost>();
  fake->data = host;
  fake->manifest = manifest;
  auto card = SessionCard::CreateGuestCard(Path("guest.raw"), manifest, Path("cache"), fake, &*own);
  fake->card = card;
  EXPECT_EQ(card->MissingChunks(), std::vector<u32>({2, 33}));
  EXPECT_EQ(card->UncheckedBytes(), 0u) << "every chunk known";
  card->StartSync();
  EXPECT_TRUE(WaitForSync(*card).finished);
  std::vector<u32> requested = fake->Requested();
  std::ranges::sort(requested);
  EXPECT_EQ(requested, std::vector<u32>({2, 33}));
  card->Detach();
  EXPECT_EQ(ReadAll(*card), host);
}

TEST_F(MachineSDCardTest, OverwriteMakesTheCardTheSessions)
{
  // "Yes, and overwrite local card": a shorter card that differs in two chunks becomes the
  // session's card, and the hash cache says so (no full read at its next session).
  const std::vector<u8> host = PatternCard(12, 4);
  std::vector<u8> guest(host.begin(), host.begin() + 9 * CHUNK_SIZE);
  guest[1 * CHUNK_SIZE + 3] ^= 0x08;
  guest[7 * CHUNK_SIZE + 3] ^= 0x08;
  WriteFile(Path("host.raw"), host);
  WriteFile(Path("guest.raw"), guest);
  auto manifest = std::make_shared<const Manifest>(*BuildManifest(Path("host.raw")));
  const auto own = HashImage(Path("guest.raw"));
  ASSERT_TRUE(own);
  auto fake = std::make_shared<FakeHost>();
  fake->data = host;
  fake->manifest = manifest;
  auto card = SessionCard::CreateGuestCard(Path("guest.raw"), manifest, Path("cache"), fake, &*own);
  fake->card = card;
  EXPECT_EQ(card->MissingChunks(), std::vector<u32>({1, 7, 9, 10, 11}));
  card->StartSync();
  ASSERT_TRUE(WaitForSync(*card).finished);
  std::string error;
  ASSERT_TRUE(card->WriteFetchedToImage(*own, &error)) << error;
  card->Detach();
  card.reset();
  EXPECT_EQ(ReadFile(Path("guest.raw")), host);
  const auto cached = CachedImageHashes(Path("guest.raw"));
  ASSERT_TRUE(cached) << "the hashes were kept";
  EXPECT_EQ(cached->hashes, HashImage(Path("host.raw"))->hashes);
  EXPECT_EQ(ManifestRoot(*BuildManifest(Path("guest.raw"))), ManifestRoot(*manifest));
}

// ------------------------------------------------------------------------------ the card server

std::unique_ptr<CardServer> ImageServer(const std::string& path,
                                        std::shared_ptr<const Manifest> manifest,
                                        CardServer::SendFunction send)
{
  auto image = std::make_shared<File::IOFile>(path, "rb");
  return std::make_unique<CardServer>(
      std::move(manifest),
      [image](u32 index, std::span<u8> data) {
        return image->Seek(static_cast<s64>(u64{index} * CHUNK_SIZE), File::SeekOrigin::Begin) &&
               image->ReadBytes(data.data(), data.size());
      },
      [](sf::Packet& p) { p << TEST_HEADER; }, std::move(send));
}

TEST_F(MachineSDCardTest, CardServerSendsOnlyChunksWithData)
{
  const std::string path = Path("host.raw");
  std::vector<u8> host = PatternCard(8, 1);
  std::fill_n(host.begin() + 6 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  WriteFile(path, host);
  auto manifest = std::make_shared<const Manifest>(*BuildManifest(path));

  std::mutex mutex;
  std::vector<SentChunk> sent;
  {
    auto server = ImageServer(path, manifest, [&](u8 pid, sf::Packet&& p) {
      auto s = ParseSent(pid, p);
      ASSERT_TRUE(s);
      std::lock_guard lock(mutex);
      sent.push_back(std::move(*s));
    });

    sf::Packet request;
    const u32 chunks[] = {1, 6, 99, 7};  // 6 is all zero, 99 is past the card
    WriteChunkRequest(chunks, request);
    server->OnRequest(2, request);

    sf::Packet malformed;
    malformed << u16{5};
    server->OnRequest(2, malformed);

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

TEST_F(MachineSDCardTest, CardServerRefusesAChangedChunk)
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
    auto server = ImageServer(path, manifest, [&](u8 pid, sf::Packet&& p) {
      auto s = ParseSent(pid, p);
      ASSERT_TRUE(s);
      std::lock_guard lock(mutex);
      sent.push_back(std::move(*s));
    });
    sf::Packet request;
    const u32 chunks[] = {2};
    WriteChunkRequest(chunks, request);
    server->OnRequest(3, request);
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

TEST_F(MachineSDCardTest, HostToGuestEndToEnd)
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
  auto server = ImageServer(host_path, shared, [&](u8, sf::Packet&& p) {
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
  wire->server = server.get();

  std::vector<u8> guest = PatternCard(12, 9);  // nothing in common
  WriteFile(Path("guest.raw"), guest);
  auto card = SessionCard::CreateGuestCard(Path("guest.raw"), shared, Path("cache"), wire);
  weak_card = card;
  EXPECT_EQ(ReadAll(*card), host);
  card->Detach();
}

// ------------------------------------------------- whole-machine sessions (Rollback/MachineSession)

TEST_F(MachineSDCardTest, CanonicalManifestSurvivesTheWire)
{
  std::vector<u8> host = PatternCard(6, 3);
  std::fill_n(host.begin() + 2 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  WriteFile(Path("host.raw"), host);
  const auto built = BuildManifest(Path("host.raw"));
  ASSERT_TRUE(built);
  sf::Packet wire;
  WriteManifest(*built, wire);
  const auto received = ReadManifest(wire);
  ASSERT_TRUE(received);
  // What every player of a session digests: the same on the sender and the receiver.
  EXPECT_EQ(CanonicalManifest(*received), CanonicalManifest(*built));

  host[4 * CHUNK_SIZE + 9] ^= 1;
  WriteFile(Path("other.raw"), host);
  EXPECT_NE(CanonicalManifest(*BuildManifest(Path("other.raw"))), CanonicalManifest(*built))
      << "another card is another manifest";
}

TEST_F(MachineSDCardTest, PlayersServeOnlyTheChunksTheyHave)
{
  std::vector<u8> host = PatternCard(6, 4);
  std::fill_n(host.begin() + 5 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  std::vector<u8> guest = host;
  guest[1 * CHUNK_SIZE + 2] ^= 1;  // differs: chunks 1 and 3 are not this guest's to serve
  guest[3 * CHUNK_SIZE + 2] ^= 1;
  GuestSetup g = MakeGuest(host, &guest);
  std::vector<u8> chunk(CHUNK_SIZE);

  EXPECT_TRUE(g.card->ReadForPeer(0, chunk)) << "its own chunk, the same as the session's";
  EXPECT_TRUE(std::equal(chunk.begin(), chunk.end(), host.begin()));
  EXPECT_FALSE(g.card->ReadForPeer(1, chunk)) << "its own differs: never sent";
  EXPECT_FALSE(g.card->ReadForPeer(5, chunk)) << "a zero chunk holds no file";
  EXPECT_FALSE(g.card->ReadForPeer(99, chunk));

  std::vector<u8> one(CHUNK_SIZE);
  ASSERT_TRUE(g.card->Read(3 * CHUNK_SIZE, one.data(), one.size()));
  EXPECT_TRUE(g.card->ReadForPeer(3, chunk)) << "fetched, so it has it now";
  EXPECT_TRUE(std::equal(chunk.begin(), chunk.end(), host.begin() + 3 * CHUNK_SIZE));

  // The session's writes are this player's game only: what it serves is the card before them.
  const std::vector<u8> scribble(16, 0xAB);
  ASSERT_TRUE(g.card->Write(0, scribble.data(), scribble.size()));
  ASSERT_TRUE(g.card->ReadForPeer(0, chunk));
  EXPECT_TRUE(std::equal(chunk.begin(), chunk.end(), host.begin()));

  WriteFile(Path("own.raw"), host);
  auto own = SessionCard::CreateOwnCard(Path("own.raw"));
  EXPECT_FALSE(own->ReadForPeer(0, chunk)) << "a player's own card is nobody else's";

  auto starter = SessionCard::CreateHostCard(Path("host.raw"), g.manifest);
  EXPECT_TRUE(starter->ReadForPeer(1, chunk));
  EXPECT_TRUE(std::equal(chunk.begin(), chunk.end(), host.begin() + CHUNK_SIZE));
  g.card->Detach();
}

TEST_F(MachineSDCardTest, ReadChunksListsFirstReadsInOrder)
{
  const std::vector<u8> data = PatternCard(6, 5);
  WriteFile(Path("card.raw"), data);
  auto card = SessionCard::CreateOwnCard(Path("card.raw"));
  std::vector<u8> buffer(2 * CHUNK_SIZE);
  ASSERT_TRUE(card->Read(3 * CHUNK_SIZE + 10, buffer.data(), 20));
  ASSERT_TRUE(card->Read(1 * CHUNK_SIZE, buffer.data(), 8));
  ASSERT_TRUE(card->Read(3 * CHUNK_SIZE, buffer.data(), 8));
  ASSERT_TRUE(card->Read(CHUNK_SIZE / 2, buffer.data(), CHUNK_SIZE));  // chunks 0 and 1
  EXPECT_EQ(card->ReadChunks(100), (std::vector<u32>{3, 1, 0}));
  EXPECT_EQ(card->ReadChunks(2), (std::vector<u32>{3, 1}));
}

TEST_F(MachineSDCardTest, AGuestFetchesFromAnotherGuest)
{
  // A room: the session's card comes from its starter, the second joiner fetches from the first
  // (the starter may have left), through servers that read from the players' cards.
  std::vector<u8> host = PatternCard(9, 7);
  std::fill_n(host.begin() + 4 * CHUNK_SIZE, CHUNK_SIZE, u8{0});
  GuestSetup first = MakeGuest(host, nullptr);
  first.card->StartSync();
  ASSERT_TRUE(WaitForSync(*first.card).finished);
  ASSERT_EQ(first.card->MissingBytes(), 0u);

  constexpr u8 HEADER = 0x5A;
  std::weak_ptr<SessionCard> second_weak;
  std::weak_ptr<SessionCard> first_weak = first.card;
  CardServer server(
      first.manifest,
      [first_weak](u32 index, std::span<u8> data) {
        auto c = first_weak.lock();
        return c && c->ReadForPeer(index, data);
      },
      [](sf::Packet& p) { p << HEADER; }, [&](u8, sf::Packet&& p) {
        u8 header = 0;
        p >> header;
        ASSERT_EQ(header, HEADER);
        auto chunk = ReadChunkData(p);
        ASSERT_TRUE(chunk);
        if (auto card = second_weak.lock())
          card->Deliver(*chunk);
      });

  struct Wire final : SessionCard::Remote
  {
    CardServer* server;
    void Request(std::span<const u32> chunks) override
    {
      sf::Packet p;
      WriteChunkRequest(chunks, p);
      server->OnRequest(1, p);
    }
  };
  auto wire = std::make_shared<Wire>();
  wire->server = &server;
  std::vector<u8> mine = host;
  mine[2 * CHUNK_SIZE] ^= 0x40;  // this joiner's own copy differs in one chunk
  WriteFile(Path("second.raw"), mine);
  auto second =
      SessionCard::CreateGuestCard(Path("second.raw"), first.manifest, Path("cache2"), wire);
  second_weak = second;
  EXPECT_EQ(second->MissingBytes(), 0u) << "nothing known to differ before it is read";
  EXPECT_EQ(second->UncheckedBytes(), 8 * CHUNK_SIZE);
  EXPECT_EQ(ReadAll(*second), host);
  EXPECT_EQ(second->GetStats().fetched_bytes, CHUNK_SIZE) << "only the chunk that differs";
  second->Detach();
  first.card->Detach();
}

TEST_F(MachineSDCardTest, AFetchingReadAsksForTheChunksAfterItToo)
{
  // The joiner's card differs in chunks 3-6 and 14 (a file whose start changed, prod's REX cards):
  // the read of chunk 3 asks for 4-6 in the same round trip, but not for 14 (past the read-ahead).
  const std::vector<u8> host = PatternCard(16, 2);
  std::vector<u8> guest = host;
  for (const u32 c : {3u, 4u, 5u, 6u, 14u})
    guest[c * CHUNK_SIZE + 9] ^= 0x10;
  GuestSetup g = MakeGuest(host, &guest);

  std::vector<u8> out(100);
  ASSERT_TRUE(g.card->Read(3 * CHUNK_SIZE + 10, out.data(), out.size()));
  EXPECT_TRUE(std::equal(out.begin(), out.end(), host.begin() + 3 * CHUNK_SIZE + 10));
  EXPECT_EQ(g.host->Requested(), std::vector<u32>({3, 4, 5, 6}));
  EXPECT_EQ(g.card->GetStats().waits, 1u);

  // The rest of the file reads without asking again.
  std::vector<u8> rest(3 * CHUNK_SIZE);
  ASSERT_TRUE(g.card->Read(4 * CHUNK_SIZE, rest.data(), rest.size()));
  EXPECT_TRUE(std::equal(rest.begin(), rest.end(), host.begin() + 4 * CHUNK_SIZE));
  EXPECT_EQ(g.host->Requested().size(), 4u);
  EXPECT_EQ(g.card->GetStats().waits, 1u) << "4-6 had come with 3";
  EXPECT_EQ(ReadAll(*g.card), host);
}

// ------------------------------------------------------------------------- hashes, roots, files

TEST_F(MachineSDCardTest, ChunksAreHashedWithBlake2b)
{
  // BLAKE2b-128 of nothing (RFC 7693's parameters, 16-byte output).
  const Hash h = HashChunk({});
  const Hash expected = {0xca, 0xe6, 0x69, 0x41, 0xd9, 0xef, 0xbd, 0x40,
                         0x4e, 0x4d, 0x88, 0x75, 0x8e, 0xa6, 0x76, 0x70};
  EXPECT_EQ(h, expected);
}

TEST_F(MachineSDCardTest, TheRootFollowsWhatPlaysNotFreeSpace)
{
  ASSERT_TRUE(Common::SDCardCreate(64, Path("a.raw")));
  std::vector<u8> card = ReadFile(Path("a.raw"));
  const Root a = ManifestRoot(*BuildManifest(Path("a.raw")));
  EXPECT_EQ(RootHex(a).size(), 64u);

  // Deleted data in free space: the same card.
  card[48 * MiB + 77] = 0x5A;
  WriteFile(Path("b.raw"), card);
  EXPECT_EQ(ManifestRoot(*BuildManifest(Path("b.raw"))), a);

  // A byte the card uses (its FAT): another card.
  card[32 * 512 + 100] ^= 0x01;
  WriteFile(Path("c.raw"), card);
  EXPECT_NE(ManifestRoot(*BuildManifest(Path("c.raw"))), a);
}

namespace
{
// A FAT32 directory entry: an 8.3 name, attributes, first cluster, size.
std::array<u8, 32> ShortEntry(const char (&name11)[12], u8 attr, u32 cluster, u32 size)
{
  std::array<u8, 32> e{};
  std::memcpy(e.data(), name11, 11);
  e[11] = attr;
  e[20] = static_cast<u8>(cluster >> 16);
  e[21] = static_cast<u8>(cluster >> 24);
  e[26] = static_cast<u8>(cluster);
  e[27] = static_cast<u8>(cluster >> 8);
  std::memcpy(&e[28], &size, 4);
  return e;
}

// A long name's entries, in the order they precede their short entry.
std::vector<std::array<u8, 32>> LongEntries(const std::u16string& name)
{
  const size_t pieces = (name.size() + 12) / 13;
  std::vector<std::array<u8, 32>> out;
  for (size_t k = pieces; k-- > 0;)
  {
    std::array<u8, 32> e{};
    e[0] = static_cast<u8>((k + 1) | (k + 1 == pieces ? 0x40 : 0));
    e[11] = 0x0F;
    int i = 0;
    for (const auto& [offset, n] : {std::pair{1, 5}, std::pair{14, 6}, std::pair{28, 2}})
    {
      for (int j = 0; j < n; ++j, ++i)
      {
        const size_t at = k * 13 + i;
        const char16_t ch = at < name.size() ? name[at] : at == name.size() ? 0 : 0xFFFF;
        e[offset + 2 * j] = static_cast<u8>(ch);
        e[offset + 2 * j + 1] = static_cast<u8>(ch >> 8);
      }
    }
    out.push_back(e);
  }
  return out;
}
}  // namespace

TEST_F(MachineSDCardTest, FilesAreListedWithTheirChunks)
{
  ASSERT_TRUE(Common::SDCardCreate(64, Path("card.raw")));
  std::vector<u8> card = ReadFile(Path("card.raw"));
  u32 fat_sectors;
  std::memcpy(&fat_sectors, &card[0x24], 4);
  const u64 fat = 32 * 512;
  const u64 data = (32 + 2 * u64{fat_sectors}) * 512;  // cluster 2, the root directory
  auto set_fat = [&](u32 cluster, u32 value) {
    std::memcpy(&card[fat + cluster * 4], &value, 4);
    std::memcpy(&card[fat + u64{fat_sectors} * 512 + cluster * 4], &value, 4);
  };
  auto cluster_at = [&](u32 cluster) { return data + u64{cluster - 2} * 512; };

  // Root: "Long File Name.txt" (clusters 3, 4, 5, then one 40 MiB in), a deleted entry, "SUB".
  const u32 far = static_cast<u32>((40 * MiB - data) / 512 + 2);
  size_t pos = cluster_at(2);
  for (const auto& e : LongEntries(u"Long File Name.txt"))
  {
    std::memcpy(&card[pos], e.data(), 32);
    pos += 32;
  }
  auto put = [&](const std::array<u8, 32>& e) {
    std::memcpy(&card[pos], e.data(), 32);
    pos += 32;
  };
  put(ShortEntry("LONGFI~1TXT", 0x20, 3, 1900));
  auto gone = ShortEntry("GONE    BIN", 0x20, 9, 10);
  gone[0] = 0xE5;
  put(gone);
  put(ShortEntry("SUB        ", 0x10, 6, 0));
  set_fat(3, 4);
  set_fat(4, 5);
  set_fat(5, far);
  set_fat(far, 0x0FFFFFFF);
  // SUB: ".", "..", "A.BIN" (cluster 7).
  pos = cluster_at(6);
  put(ShortEntry(".          ", 0x10, 6, 0));
  put(ShortEntry("..         ", 0x10, 0, 0));
  put(ShortEntry("A       BIN", 0x20, 7, 100));
  set_fat(6, 0x0FFFFFFF);
  set_fat(7, 0x0FFFFFFF);
  WriteFile(Path("card.raw"), card);

  File::IOFile f(Path("card.raw"), "rb");
  auto files = ListCardFiles(f, card.size());
  ASSERT_TRUE(files);
  ASSERT_EQ(files->size(), 2u);
  std::ranges::sort(*files, {}, &CardFile::path);
  EXPECT_EQ((*files)[0].path, "Long File Name.txt");
  EXPECT_EQ((*files)[0].size, 1900u);
  const u32 near_chunk = static_cast<u32>(cluster_at(3) / CHUNK_SIZE);
  const u32 far_chunk = static_cast<u32>(cluster_at(far) / CHUNK_SIZE);
  ASSERT_GE((*files)[0].runs.size(), 2u);
  EXPECT_EQ((*files)[0].runs.front().first, near_chunk);
  EXPECT_EQ((*files)[0].runs.back(), std::make_pair(far_chunk, 1u));
  EXPECT_EQ((*files)[1].path, "SUB/A.BIN");

  // Which files a chunk that differs belongs to, largest first.
  const u32 differ[] = {near_chunk, far_chunk};
  const auto touched = FilesTouching(*files, differ);
  ASSERT_FALSE(touched.empty());
  EXPECT_EQ(touched[0]->path, "Long File Name.txt");
  const u32 only_far[] = {far_chunk};
  ASSERT_EQ(FilesTouching(*files, only_far).size(), 1u);

  // The list over the wire, and a stranger's list checked.
  sf::Packet packet;
  WriteFileList(*files, packet);
  const u32 chunks = static_cast<u32>(card.size() / CHUNK_SIZE);
  auto back = ReadFileList(packet, chunks);
  ASSERT_TRUE(back);
  ASSERT_EQ(back->size(), 2u);
  EXPECT_EQ((*back)[0].path, (*files)[0].path);
  EXPECT_EQ((*back)[0].runs, (*files)[0].runs);
  sf::Packet small;
  WriteFileList(*files, small);
  EXPECT_FALSE(ReadFileList(small, far_chunk)) << "a run past the card";
  std::vector<CardFile> bad{{"a\nb", 1, {{0, 1}}}};
  sf::Packet control;
  WriteFileList(bad, control);
  EXPECT_FALSE(ReadFileList(control, chunks)) << "a control character in a path";
}
