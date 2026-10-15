// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The SD card of a whole-machine session (Other Games rooms, docs/design/brawl-mods.md §6.5).
//
// Everyone plays the session's card: the card of the player who started the session. Nothing a
// session does reaches anyone's image: every write goes to a throwaway layer (SessionCard) that
// rolls back with the machine. The starter's card has a manifest (a hash per 64 KiB chunk, and
// which chunks hold no file) and a list of its files. Before a joiner asks to join, it compares
// its own card with the manifest and, once its player agreed, fetches every chunk it has not got
// (StartSync) while the room plays on: the game never waits for the network. A read of a chunk
// the joiner still has not got (a bug) fetches it and waits, so the game still sees the same
// bytes on every machine.
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
// A read that has to fetch also asks for the chunks after it that the card lacks, up to this
// many: a game reads a file front to back, and those come in one round trip instead of one each.
constexpr u32 READ_AHEAD_CHUNKS = 8;
// A read that waits this long for the players' chunks fails (the session is broken by then).
constexpr int FETCH_TIMEOUT_MS = 60'000;
// A request with no answer after this long is sent again.
constexpr int REQUEST_RETRY_MS = 5'000;
// A card's file list: at most this many files, each path at most this long.
constexpr u32 MAX_CARD_FILES = 1u << 18;
constexpr size_t MAX_CARD_PATH = 1024;

// A chunk's hash: BLAKE2b-128 (Monocypher), cryptographic, since a chunk may come from any
// player and is checked against the host's manifest.
using Hash = std::array<u8, 16>;
// A whole card's: BLAKE2b-256 of its canonical manifest. Two cards with the same root play the
// same (their free space may differ), so rooms compare cards by it (DolphinTool sdhash).
using Root = std::array<u8, 32>;

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

// A file on a card: its path from the card's root ("rex_/pf/fighter/hero/FitHero.pac") and the
// chunks its clusters are in, as runs of consecutive chunks (first, count).
struct CardFile
{
  std::string path;
  u64 size = 0;
  std::vector<std::pair<u32, u32>> runs;
};

// The files of a FAT16/FAT32 image (with or without an MBR), long names included, or nullopt
// when it is not a FAT volume this can read.
std::optional<std::vector<CardFile>> ListCardFiles(File::IOFile& image, u64 image_size);

// Wire format of a file list; ReadFileList checks everything (a stranger's bytes): the counts,
// the paths, and every run against the card's `chunk_count`.
void WriteFileList(const std::vector<CardFile>& files, sf::Packet& packet);
std::optional<std::vector<CardFile>> ReadFileList(sf::Packet& packet, u32 chunk_count);

// The files that hold any of `chunks` (sorted, ascending), largest first.
std::vector<const CardFile*> FilesTouching(const std::vector<CardFile>& files,
                                           std::span<const u32> chunks);

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
// `progress` (any thread) hears the bytes read so far and the image's size while it is hashed
// (nothing when the cache has it).
using HashProgress = std::function<void(u64 done, u64 total)>;
std::optional<ImageHashes> HashImage(const std::string& path,
                                     const std::atomic<bool>* cancel = nullptr,
                                     const HashProgress& progress = {});
// The hashes as the cache holds them for this image now (size and modification time), or nullopt.
std::optional<ImageHashes> CachedImageHashes(const std::string& path);

// The manifest of the session starter's card image.
std::optional<Manifest> BuildManifest(const std::string& path,
                                      const std::atomic<bool>* cancel = nullptr,
                                      const HashProgress& progress = {});

// The manifest's contents in a fixed form (its size, then each chunk's zero flag and hash, not
// compressed): what players compare to agree on the card.
std::vector<u8> CanonicalManifest(const Manifest& manifest);
Root ManifestRoot(const Manifest& manifest);
std::string RootHex(const Root& root);

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
  // `image_path` is the joiner's own card (it may not exist). With `own` (its hashes, HashImage)
  // every chunk is known at once to match or not; without, a chunk the cache does not rule out is
  // checked when first read. `cache_dir` holds the file of fetched chunks, deleted with the card.
  static std::shared_ptr<SessionCard> CreateGuestCard(const std::string& image_path,
                                                      std::shared_ptr<const Manifest> manifest,
                                                      const std::string& cache_dir,
                                                      std::shared_ptr<Remote> remote,
                                                      const ImageHashes* own = nullptr);

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
  // Guest: the chunks it has not got yet, ascending.
  std::vector<u32> MissingChunks() const;

  // Guest, before the join: fetches every missing chunk now (a thread of its own), as many at
  // once as MAX_OUTSTANDING_REQUESTS, as fast as the link takes them.
  void StartSync();
  struct SyncProgress
  {
    u64 total = 0;  // the bytes missing when the sync started
    u64 done = 0;
    bool finished = false;
    bool failed = false;  // a chunk no player could send, or the card detached
  };
  SyncProgress GetSyncProgress() const;
  // Guest, after a finished sync: writes the fetched chunks into the player's own image (made the
  // session card's size), so it is the session's card from now on, and records its hashes (`own`:
  // the image's before) as the hash cache. The player chose it ("overwrite local card").
  bool WriteFetchedToImage(const ImageHashes& own, std::string* error);

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
  // Asks the players for those of `chunks` still missing and not asked for yet, without waiting.
  void RequestMissing(std::span<const u32> chunks);
  void SyncThreadFunc();
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
  u64 m_fetched_bytes = 0;
  std::atomic<u32> m_waits{0};
  std::string m_fetched_path;
  File::IOFile m_fetched_file;
  std::shared_ptr<Remote> m_remote;
  bool m_detached = false;

  // Guest: the sync before the join (StartSync), its progress under m_fetch_mutex.
  std::thread m_sync_thread;
  u64 m_sync_total = 0;
  u64 m_sync_start_fetched = 0;
  bool m_sync_finished = false;
  bool m_sync_failed = false;
  // A read had to fetch while the game ran (a bug: the sync fetches everything first).
  bool m_logged_late_fetch = false;

  // The chunks the game read (emulation thread), for ReadChunks.
  std::vector<bool> m_read_seen;
  std::vector<u32> m_read_order;

  // A second handle on the image for the other threads (ReadForPeer, CheckLocal): m_image
  // belongs to the emulation thread.
  std::mutex m_serve_mutex;
  File::IOFile m_serve_image;
};
}  // namespace Machine::SDCard
