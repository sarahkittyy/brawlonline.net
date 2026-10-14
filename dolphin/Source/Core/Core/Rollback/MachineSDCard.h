// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The SD card of a whole-machine session (Other Games rooms, docs/design/brawl-mods.md §6.5).
//
// Everyone plays the session's card: the card of the player who started the session. Nothing a
// session does reaches anyone's image: every write goes to a throwaway layer (SessionCard) that
// rolls back with the machine. The starter's card has a manifest (a hash per 64 KiB chunk, and
// which chunks hold no file), which joiners get with the machine image. A joiner plays its own
// card's chunks where they match the manifest (each checked the first time the game reads it)
// and fetches the others from the players when the game first reads them (the emulated read
// waits, so the game sees the same bytes on every machine), the chunks the game read before the
// join first, in the background.
//
// Every player is a stranger to the others: everything they send is checked (sizes, indices,
// every chunk against its hash in the manifest), and none of it reaches anyone's own files. A
// player only serves chunks of the session's card that hold data and match their hash: what
// differs on its own card, and deleted data, is never sent.

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

namespace Machine::SDCard
{
constexpr u64 CHUNK_SIZE = 64 * 1024;
// SDHC cards go up to 32 GiB; anything claiming more than this is refused.
constexpr u64 MAX_CARD_SIZE = 64ull << 30;
constexpr u32 MAX_CHUNKS = static_cast<u32>(MAX_CARD_SIZE / CHUNK_SIZE);
// Chunks one request may ask for.
constexpr u32 MAX_REQUEST_CHUNKS = 64;
// The background stream stops after this much; reads still fetch what they need.
constexpr u64 BACKGROUND_CAP_BYTES = 2ull << 30;
// The background stream's rate, so it never crowds out the game's own traffic.
constexpr u64 BACKGROUND_BYTES_PER_SECOND = 256 * 1024;
// A read that waits this long for the players' chunks fails (the session is broken by then).
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

// The manifest of the session starter's card image.
std::optional<Manifest> BuildManifest(const std::string& path,
                                      const std::atomic<bool>* cancel = nullptr);

// The manifest's contents in a fixed form (its size, then each chunk's zero flag and hash, not
// compressed): what players compare to agree on the card.
std::vector<u8> CanonicalManifest(const Manifest& manifest);

// Wire format. ReadManifest checks everything (a stranger's bytes) and returns nullopt for a
// malformed one.
void WriteManifest(const Manifest& manifest, sf::Packet& packet);
std::optional<Manifest> ReadManifest(sf::Packet& packet);

// A chunk request: the chunk indices, at most MAX_REQUEST_CHUNKS.
void WriteChunkRequest(std::span<const u32> chunks, sf::Packet& packet);
std::optional<std::vector<u32>> ReadChunkRequest(sf::Packet& packet);

// A chunk's data; `data` empty when the player asked has not got it.
struct ChunkData
{
  u32 index = 0;
  bool ok = false;
  std::vector<u8> data;
};
void WriteChunkData(u32 index, std::span<const u8> data, sf::Packet& packet);
void WriteChunkUnavailable(u32 index, sf::Packet& packet);
std::optional<ChunkData> ReadChunkData(sf::Packet& packet);

// Answers the other players' chunk requests on a thread of its own, so disk reads never hold up
// the network thread. Only chunks the manifest says hold data are sent, and only while they match
// their hash.
class CardServer
{
public:
  using SendFunction = std::function<void(u8 player, sf::Packet&& packet)>;
  // Reads a chunk of the card (the server's thread); false when this machine has not got it.
  using ReadFunction = std::function<bool(u32 index, std::span<u8> data)>;
  // Starts each chunk's packet (whatever goes in front of WriteChunkData).
  using HeaderFunction = std::function<void(sf::Packet& packet)>;

  CardServer(std::shared_ptr<const Manifest> manifest, ReadFunction read, HeaderFunction header,
             SendFunction send);
  ~CardServer();
  CardServer(const CardServer&) = delete;
  CardServer& operator=(const CardServer&) = delete;

  // From the network thread: a player's request.
  void OnRequest(u8 player, sf::Packet& packet);

private:
  void ThreadFunc();

  // Requests queued per player; more than this many waiting chunks and new ones are dropped (the
  // player asks again).
  static constexpr size_t MAX_QUEUED_PER_PLAYER = 256;

  std::shared_ptr<const Manifest> m_manifest;
  ReadFunction m_read;
  HeaderFunction m_header;
  SendFunction m_send;

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<std::pair<u8, u32>> m_queue;
  std::map<u8, size_t> m_queued_per_player;
  bool m_stop = false;
  std::thread m_thread;
};

// The storage the emulated SD card uses in a session.
class SessionCard final : public IOS::HLE::SDCardStorage
{
public:
  // How a joiner's card reaches the other players. Request() may be called from any thread.
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
    // The player's own card (a session of a game without a card, or before a joiner's image).
    OwnCard,
    // The session's card, on its starter: its own image, the chunks the manifest zeroes read as
    // zero.
    HostCard,
    // The session's card, on a joiner: its own image where it matches the manifest, else fetched.
    GuestCard,
  };

  static std::shared_ptr<SessionCard> CreateOwnCard(const std::string& image_path);
  static std::shared_ptr<SessionCard> CreateHostCard(const std::string& image_path,
                                                     std::shared_ptr<const Manifest> manifest);
  // `image_path` is the joiner's own card (it may not exist), checked chunk by chunk as the game
  // reads it. `cache_dir` holds the file of fetched chunks, deleted with the card.
  static std::shared_ptr<SessionCard> CreateGuestCard(const std::string& image_path,
                                                      std::shared_ptr<const Manifest> manifest,
                                                      const std::string& cache_dir,
                                                      std::shared_ptr<Remote> remote);

  ~SessionCard() override;
  SessionCard(const SessionCard&) = delete;
  SessionCard& operator=(const SessionCard&) = delete;

  Mode GetMode() const { return m_mode; }
  // The session's card (host and guest cards), or nullptr (the player's own card).
  const std::shared_ptr<const Manifest>& GetManifest() const { return m_manifest; }

  bool IsOpen() const override;
  u64 GetSize() const override;
  bool Read(u64 offset, u8* dst, u64 size) override;
  bool Write(u64 offset, const u8* src, u64 size) override;
  u64 GetWriteSeq() const override;
  void RewindTo(u64 write_seq) override;

  // Guest: a chunk from a player (any thread). Checked against the manifest; a bad one is
  // dropped. An unavailable chunk fails the reads waiting for it.
  void Deliver(const ChunkData& chunk);
  // Guest: the chunks known to need fetching (this card has none there, or it was checked and
  // differs), and those of this card not checked yet.
  u64 MissingBytes() const;
  u64 UncheckedBytes() const;

  struct Stats
  {
    u64 size = 0;
    u64 missing_bytes = 0;  // still to fetch
    u64 fetched_bytes = 0;
    u32 waits = 0;          // reads and writes that waited for the players' chunks
    u64 writes = 0;         // the session's write counter
  };
  Stats GetStats() const;
  // Guest, once the game runs: streams the missing chunks in the background, up to
  // BACKGROUND_CAP_BYTES, the chunks in `first` (those the game is likely to read soon: checked
  // first if this card has them) before the others.
  void StartBackgroundFetch(std::span<const u32> first = {});

  // Another player's request (any thread): the chunk as this machine has it before the session's
  // writes (its image where it matches the session's card, or fetched), or false when it has not
  // got it. Host and guest cards only; the caller checks it against the manifest before sending.
  bool ReadForPeer(u32 index, std::span<u8> data);
  // The chunks the game has read so far, in the order it first read them, at most `max` (the
  // emulation thread): a player joining later fetches these first.
  std::vector<u32> ReadChunks(size_t max) const;
  // The session is gone (or the game stopped): waiting reads fail, no more fetches.
  void Detach();

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
    Local,      // this card's chunk, checked against the manifest
    Unchecked,  // this card has the chunk: checked when first needed
    Fetched,
    Missing,
  };

  SessionCard(Mode mode, const std::string& image_path, std::shared_ptr<const Manifest> manifest);

  u32 ChunkCount() const;
  u64 ChunkBytes(u32 index) const;
  // Guest: compares this card's chunks among `chunks` that are not checked yet with the manifest
  // (any thread): each becomes Local or Missing.
  void CheckLocal(std::span<const u32> chunks);
  // The chunk's current contents (the session's writes included), fetching it if needed.
  bool ReadChunk(u32 index, u8* dst);
  bool ReadBase(u32 index, u8* dst);
  // Waits until every listed chunk has arrived (guest). False on timeout, detach or a chunk no
  // player could send.
  bool WaitForChunks(std::span<const u32> chunks, bool keep_alive);
  void BackgroundThreadFunc();
  void PruneHistory(u64 now);

  const Mode m_mode;
  const std::string m_image_path;
  const std::shared_ptr<const Manifest> m_manifest;
  u64 m_size = 0;

  // The image (read only), for the chunks this machine has (emulation thread).
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
  std::vector<u32> m_background_order;       // m_missing, the chunks asked for first in front
  u64 m_fetched_bytes = 0;
  std::atomic<u32> m_waits{0};
  std::string m_fetched_path;
  File::IOFile m_fetched_file;
  std::shared_ptr<Remote> m_remote;
  bool m_detached = false;

  std::thread m_background_thread;
  std::atomic<bool> m_stop_background{false};

  // The chunks the game read (emulation thread), for ReadChunks.
  std::vector<bool> m_read_seen;
  std::vector<u32> m_read_order;

  // A second handle on the image for the other threads (ReadForPeer, CheckLocal): m_image
  // belongs to the emulation thread.
  std::mutex m_serve_mutex;
  File::IOFile m_serve_image;
};
}  // namespace Machine::SDCard
