// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The SD card in a netplay session.
//
// Nothing a session does reaches anyone's SD card image: every write goes to a throwaway layer
// (SessionCard), and only the host keeps it, and only when the host chose "Load and Write Host's
// Save Data" (written back when the game ends). Like the NAND saves, the card everyone plays on is
// the host's when the host loads its save data: the host sends a manifest of its card (a hash per
// 64 KiB chunk, and which chunks hold no file), a guest compares it with its own card and plays
// on the host's chunks wherever the two differ. Those are fetched from the host when the game
// first reads them (the emulated read waits, so the game sees the same bytes on every machine),
// a few before the game boots, and the rest in the background at a gentle rate.
//
// The host is a stranger in a public lobby: everything it sends is checked (sizes, indices, every
// chunk against its hash in the manifest), and none of it ever reaches the guest's own files. A
// guest is a stranger to the host too: it can only ask for chunks of the card the host chose to
// share, and the host never sends a chunk that holds no file (deleted data stays private).

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <SFML/Network/Packet.hpp>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/SDIO/SDCardStorage.h"

namespace NetPlay::SDCard
{
constexpr u64 CHUNK_SIZE = 64 * 1024;
// SDHC cards go up to 32 GiB; anything claiming more than this is refused.
constexpr u64 MAX_CARD_SIZE = 64ull << 30;
constexpr u32 MAX_CHUNKS = static_cast<u32>(MAX_CARD_SIZE / CHUNK_SIZE);
// Chunks one request may ask for.
constexpr u32 MAX_REQUEST_CHUNKS = 64;
// Fetched before the game boots, so its first reads (the FAT, the game's own files) rarely wait.
constexpr u64 PREFETCH_BYTES = 32ull << 20;
// The background stream stops after this much; reads still fetch what they need.
constexpr u64 BACKGROUND_CAP_BYTES = 2ull << 30;
// The background stream's rate, so it never crowds out the game's own traffic.
constexpr u64 BACKGROUND_BYTES_PER_SECOND = 256 * 1024;
// A read that waits this long for the host's chunks fails (the session is broken by then).
constexpr int FETCH_TIMEOUT_MS = 60'000;
// A request with no answer after this long is sent again.
constexpr int REQUEST_RETRY_MS = 5'000;

using Hash = std::array<u8, 16>;

struct ChunkInfo
{
  // No file uses any of the chunk, or it is all zero bytes: the session reads zeros.
  bool zero = false;
  Hash hash{};
};

struct Manifest
{
  u64 size = 0;
  std::vector<ChunkInfo> chunks;

  u32 ChunkCount() const { return static_cast<u32>(chunks.size()); }
  // The chunk's length: CHUNK_SIZE, except the last chunk of a card that is not a multiple.
  u64 ChunkBytes(u32 index) const;
};

u32 ChunkCountFor(u64 size);
Hash HashChunk(std::span<const u8> data);

// The chunks of a FAT16/FAT32 image (with or without an MBR) that no cluster in use touches.
// Chunks outside the data area (MBR, reserved sectors, FATs, FAT16 root directory) are in use.
// nullopt when the image is not a FAT volume this can read: then every chunk counts as in use.
std::optional<std::vector<bool>> FindFreeChunks(File::IOFile& image, u64 image_size);

// The hashes of an image's chunks, and which are all zero. Taken from a cache in the User
// folder when the image's size and modification time are unchanged since it was last hashed.
struct ImageHashes
{
  u64 size = 0;
  std::vector<Hash> hashes;
  std::vector<bool> zero;
};
std::optional<ImageHashes> HashImage(const std::string& path,
                                     const std::atomic<bool>* cancel = nullptr);

// The host's manifest of its card image.
std::optional<Manifest> BuildManifest(const std::string& path,
                                      const std::atomic<bool>* cancel = nullptr);

// Wire format. ReadManifest checks everything (a stranger's packet) and returns nullopt for a
// malformed one.
void WriteManifest(const Manifest& manifest, sf::Packet& packet);
std::optional<Manifest> ReadManifest(sf::Packet& packet);

// A chunk request: the chunk indices, at most MAX_REQUEST_CHUNKS.
void WriteChunkRequest(std::span<const u32> chunks, sf::Packet& packet);
std::optional<std::vector<u32>> ReadChunkRequest(sf::Packet& packet);

// A chunk's data; `data` empty when the host could not send it.
struct ChunkData
{
  u32 index = 0;
  bool ok = false;
  std::vector<u8> data;
};
void WriteChunkData(u32 index, std::span<const u8> data, sf::Packet& packet);
void WriteChunkUnavailable(u32 index, sf::Packet& packet);
std::optional<ChunkData> ReadChunkData(sf::Packet& packet);

// The host's side: answers guests' chunk requests from its card image on a thread of its own, so
// disk reads never hold up the netplay thread. Only chunks the manifest says hold data are sent,
// and only while they still match their hash.
class CardServer
{
public:
  using SendFunction = std::function<void(u8 pid, sf::Packet&& packet)>;

  CardServer(const std::string& image_path, std::shared_ptr<const Manifest> manifest,
             SendFunction send);
  ~CardServer();
  CardServer(const CardServer&) = delete;
  CardServer& operator=(const CardServer&) = delete;

  // From the netplay thread: a guest's request (the packet past its sub-ID).
  void OnRequest(u8 pid, sf::Packet& packet);

private:
  void ThreadFunc();

  // Requests queued per guest; more than this many waiting chunks and new ones are dropped (the
  // guest asks again).
  static constexpr size_t MAX_QUEUED_PER_PLAYER = 256;

  std::string m_image_path;
  std::shared_ptr<const Manifest> m_manifest;
  SendFunction m_send;

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<std::pair<u8, u32>> m_queue;
  std::map<u8, size_t> m_queued_per_player;
  bool m_stop = false;
  std::thread m_thread;
};

// The storage the emulated SD card uses for one netplay session.
class SessionCard final : public IOS::HLE::SDCardStorage
{
public:
  // How a guest's card reaches the host. Request() may be called from any thread.
  class Remote
  {
  public:
    virtual ~Remote() = default;
    virtual void Request(std::span<const u32> chunks) = 0;
    // From the emulation thread while a read waits for chunks: keeps the session's network
    // traffic going (GekkoNet), so the other players don't take the wait for a disconnect.
    virtual void KeepAlive() {}
  };

  enum class Mode
  {
    // Each player's own card (the host does not load its save data, or plays alone).
    OwnCard,
    // The host's card, on the host: its own image, with the chunks the manifest zeroes read as zero.
    HostCard,
    // The host's card, on a guest: its own image where it matches the host's, else fetched.
    GuestCard,
  };

  static std::shared_ptr<SessionCard> CreateOwnCard(const std::string& image_path,
                                                    bool write_back);
  static std::shared_ptr<SessionCard> CreateHostCard(const std::string& image_path,
                                                     std::shared_ptr<const Manifest> manifest,
                                                     bool write_back);
  // `local` is the guest's own image's hashes (nullopt: it has none). `cache_dir` holds the file
  // of fetched chunks, deleted with the card.
  static std::shared_ptr<SessionCard>
  CreateGuestCard(const std::string& image_path, std::shared_ptr<const Manifest> manifest,
                  const std::optional<ImageHashes>& local, const std::string& cache_dir,
                  std::shared_ptr<Remote> remote);

  ~SessionCard() override;
  SessionCard(const SessionCard&) = delete;
  SessionCard& operator=(const SessionCard&) = delete;

  Mode GetMode() const { return m_mode; }

  bool IsOpen() const override;
  u64 GetSize() const override;
  bool Read(u64 offset, u8* dst, u64 size) override;
  bool Write(u64 offset, const u8* src, u64 size) override;
  u64 GetWriteSeq() const override;
  void RewindTo(u64 write_seq) override;

  // Guest: a chunk from the host (any thread). Checked against the manifest; a bad one is
  // dropped. An unavailable chunk fails the reads waiting for it.
  void Deliver(const ChunkData& chunk);
  // Guest: the chunks of the host's card that are neither all zero nor the same on this card.
  u64 MissingBytes() const;

  struct Stats
  {
    u64 size = 0;
    u64 missing_bytes = 0;  // still to fetch
    u64 fetched_bytes = 0;
    u32 waits = 0;          // reads and writes that waited for the host's chunks
    u64 writes = 0;         // the session's write counter
  };
  Stats GetStats() const;
  // Guest, before the game boots: fetches up to `budget` bytes of missing chunks, lowest first.
  // False if the host stopped answering or the card was detached.
  bool Prefetch(u64 budget, const std::atomic<bool>* cancel,
                const std::function<void(u64 done, u64 total)>& progress = {});
  // Guest, once the game runs: streams the rest in the background, up to BACKGROUND_CAP_BYTES.
  void StartBackgroundFetch();
  // The connection to the host is gone (or the game stopped): waiting reads fail, no more fetches.
  void Detach();

  // At the end of the emulation: writes the session's writes into the image when this card keeps
  // them (the host with "Load and Write Host's Save Data"). True if the image changed.
  bool Finish();

  // Whether Dolphin's SD folder sync should pack the folder into the image at boot (the host's
  // card was already packed before its manifest was built, and a guest's own card is only a cache).
  bool PacksFolderAtBoot() const { return m_mode == Mode::OwnCard; }

  // For tests: the clock rollback history is kept by (default: the emulated time).
  void SetClockForTesting(std::function<u64()> clock, u64 history_ticks);

private:
  struct UndoEntry
  {
    u64 seq;
    u64 time;
    u32 chunk;
    std::shared_ptr<const std::vector<u8>> previous;  // nullptr: the chunk had no write before
  };

  enum class Source : u8
  {
    Zero,
    Local,
    Fetched,
    Missing,
  };

  SessionCard(Mode mode, const std::string& image_path, std::shared_ptr<const Manifest> manifest,
              bool write_back);

  u32 ChunkCount() const;
  u64 ChunkBytes(u32 index) const;
  // The chunk's current contents (the session's writes included), fetching it if needed.
  bool ReadChunk(u32 index, u8* dst);
  bool ReadBase(u32 index, u8* dst);
  // Waits until every listed chunk has arrived (guest). False on timeout, detach or a chunk the
  // host could not send.
  bool WaitForChunks(std::span<const u32> chunks, bool keep_alive);
  void BackgroundThreadFunc();
  void PruneHistory(u64 now);

  const Mode m_mode;
  const std::string m_image_path;
  const std::shared_ptr<const Manifest> m_manifest;
  const bool m_write_back;
  u64 m_size = 0;

  // The image (read only), for the chunks this machine has.
  File::IOFile m_image;

  // The session's writes and their history (emulation thread only).
  std::map<u32, std::shared_ptr<const std::vector<u8>>> m_overlay;
  std::deque<UndoEntry> m_history;
  u64 m_write_seq = 0;
  u64 m_pruned_seq = 0;
  std::function<u64()> m_clock;
  u64 m_history_ticks = 0;

  // Guest: where each chunk comes from, and the fetched chunks (guarded by m_fetch_mutex).
  mutable std::mutex m_fetch_mutex;
  std::condition_variable m_fetch_cv;
  std::vector<Source> m_sources;
  std::map<u32, u64> m_fetched_offsets;      // chunk -> offset in m_fetched_file
  std::map<u32, u64> m_requested_at;         // chunk -> when it was last requested (ms)
  std::set<u32> m_unavailable;
  std::map<u32, int> m_bad_deliveries;
  std::vector<u32> m_missing;                // the missing chunks at creation, ascending
  u64 m_fetched_bytes = 0;
  std::atomic<u32> m_waits{0};
  std::string m_fetched_path;
  File::IOFile m_fetched_file;
  std::shared_ptr<Remote> m_remote;
  bool m_detached = false;

  std::thread m_background_thread;
  std::atomic<bool> m_stop_background{false};
};
}  // namespace NetPlay::SDCard
