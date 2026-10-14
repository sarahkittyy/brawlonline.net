// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Spectating: this player's game uploads its games for spectators (docs/spectate-protocol.md §1,
// design docs/design/spectating.md §4).
//
// The replay recorder's writer thread hands every chunk it encodes to AddChunk (the same bytes the
// .brep file gets, never FTSP/FTRM). The Rooms thread, which owns the online connection to mm,
// calls Service every tick: it offers each game, takes mm's role (primary: send the chunks;
// witness: send digests of the frames), and sends through a pacer.
//
// Players come first. Nothing here runs on the CPU thread. Upload stops before the game feels
// anything: when the session's late frames, rollback depth or ping get worse than they were in
// the game's first 10 s, and while too much upload is still
// unacknowledged, the primary pauses (spectate-pause; mm hands the game to a witness if there is
// one) and resumes only after 10 s without any of it; after three pauses it stops for the game.
// A witness sends nothing while unhealthy either. Each game is measured against its own start, so
// a PC that always shows some late frames (a slower PC, a busy one) is not held to a stranger's
// normal; the start itself (its hitch at GO) is that normal.
//
// Everything is measured against the game's first 10 s of running (`baseline`), and nothing is
// sent during them: a match's start is when its link is busiest (the safety tests: an upload
// starting with the match tipped a link 85% full into a queue that never drained).
//
// The upload also yields to any queue on the player's uplink, as LEDBAT background transfers do:
// its own round trip to mm crosses the same queue as the game's packets, so when that round trip
// is more than `rtt_target_ms` above its lowest of the last 10 minutes (menus included, when the
// uplink is quiet), something is queued and nothing more is sent until it drains. That holds
// whatever filled the queue, the game itself included, and a game whose link is full from its
// first frame is measured against the quiet menus before it (a lowest taken within the game alone
// would already be the full queue's).
//
// All functions are thread-safe.

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Online::Spectate
{
using Clock = std::chrono::steady_clock;

struct GameInfo
{
  std::string match_id;
  std::array<u8, 16> uid{};
  u32 game = 0;  // the game number the players see
};

// What the gameplay session measured (Gprb::Session::GetLinkHealth): counters since the session
// started, so the uploader can judge any window of them.
struct Health
{
  bool running = false;     // a network session runs a match
  u64 late_frames = 0;      // displayed frames that ran late (the game felt it)
  double rollback_avg = 0;  // rollback depth in frames, averaged over about 2 s
  double ping_ms = -1;      // the worst link's average ping (-1: not measured)
  double mm_rtt_ms = -1;    // the online connection's latest round trip to mm (-1: none)
  // Outgoing reliable bytes to mm not acknowledged yet, whether on the wire or still queued in
  // ENet (Service's `unacked` is this; kept here for the record).
};

struct Outgoing
{
  std::vector<std::string> json;       // channel 0
  std::vector<std::vector<u8>> data;   // channel 1 (spectate data packets)
};

// The uploader's rules and limits.
struct Options
{
  // The largest data packet (header included) and how much may be unacknowledged: on a slow
  // uplink each packet holds the link (256 bytes at 160 kbit/s: 14 ms), so only one is in flight
  // ahead of the game's packets (the safety tests: two in flight cost the game's packets 1.7
  // packets' time at their 99th percentile at 260 kbit/s). One a round trip is still 5-8 KB/s,
  // ten times what a game produces.
  size_t packet = 256;
  double burst_seconds = 1.0;            // the pacer holds at most this many seconds of tokens
  size_t max_unacked = 256;              // nothing more is sent while this much is unacknowledged
  size_t queue_pause = 8192;             // ... and above this much the upload pauses ("queue")
  Clock::duration window = std::chrono::seconds(2);           // what the signals look back on
  // Late frames ("frame") in the window pause when they are over `rate_factor` times the game's
  // normal for the window plus the margin. (Waits for the other player's input are not counted:
  // the session can't tell them from its routine pacing polls, about 116 a second in normal play;
  // the ping and the rollbacks show that trouble.)
  double rate_factor = 2.0;
  double late_margin = 4;
  Clock::duration baseline = std::chrono::seconds(10);        // a game's normal is its first 10 s
  double rollback_margin = 1.0;          // frames above the normal that pause ("rollback")
  double ping_margin_ms = 20.0;          // ms above the normal that pause ("rtt")
  double rtt_target_ms = 25.0;           // queueing (mm round trip over its lowest) that holds
  Clock::duration resume_after = std::chrono::seconds(10);
  int max_pauses = 3;                    // the third pause stops the upload for the game
  Clock::duration digest_every = std::chrono::seconds(5);
  size_t max_digests = 30;
  size_t max_game_bytes = 2u << 20;
};

// The uploader itself, without globals or I/O (the free functions below drive one instance; the
// unit tests drive their own).
class Uploader
{
public:
  explicit Uploader(Options options = {});

  void BeginGame(const GameInfo& game);
  void AddChunk(const std::array<u8, 16>& uid, u32 type, std::vector<u8> chunk,
                std::span<const u8> raw);
  void EndGame(const std::array<u8, 16>& uid);
  void AbortGame(const std::array<u8, 16>& uid);

  // A message from mm: true if it was a spectate message (taken or ignored), false otherwise.
  bool OnMessage(const picojson::object& message);
  // What to send now. `unacked`: reliable bytes to mm not acknowledged yet.
  Outgoing Service(Clock::time_point now, bool online, size_t unacked, const Health& health);
  void ConnectionLost();

  picojson::object Status() const;

  // Tests only (harness `spectate_config`): no backoff at all, 16 KB/s in 1,200-byte packets and
  // no limit on what is unacknowledged. What the safety tests compare against: it must hurt a
  // weak uplink where the normal uploader does not.
  void SetForced(bool forced) { m_forced = forced; }

private:
  enum class Role
  {
    Offering,
    Primary,
    Witness,
    None,
  };
  struct Chunk
  {
    u32 type = 0;
    std::shared_ptr<const std::vector<u8>> bytes;
    bool frames = false;
    u32 first_frame = 0;
    std::array<u8, 8> digest{};
  };
  struct Sample
  {
    Clock::time_point at;
    u64 late = 0;
  };
  struct Game
  {
    GameInfo info;
    std::vector<Chunk> chunks;
    size_t bytes = 0;
    bool ended = false;
    bool too_big = false;
    bool offered = false;
    Role role = Role::Offering;
    u32 slot = 0;
    std::optional<u32> from;
    bool headers = true;
    double rate = 2048;
    // The primary's position: the next chunk, the bytes of it already sent, its sequence number,
    // and whether `from` was reached.
    size_t next = 0;
    size_t offset = 0;
    u32 seq = 0;
    bool reached = false;
    double tokens = 0;
    Clock::time_point refilled{};
    // The witness's digests.
    size_t digest_next = 0;
    Clock::time_point next_digest{};
    // Backoff.
    bool paused = false;
    Clock::time_point clean_since{};
    int pauses = 0;
    bool stopped = false;
    std::deque<Sample> samples;
    // The game's normal (its first `baseline` of running).
    std::optional<Clock::time_point> running_since;
    u64 late_at_start = 0;
    double base_late_per_s = 0;
    double base_rollbacks = 0, base_ping = 0;
    int base_n = 0, base_ping_n = 0;
    bool baseline_done = false;
    double rollback_sum = 0;
    u64 rollback_n = 0;
    // How long sending was held for a queue.
    double held_s = 0;
  };

  std::optional<std::string> Unhealthy(Game& g, Clock::time_point now, size_t unacked,
                                       const Health& health);
  void SendData(Game& g, Clock::time_point now, size_t unacked, Outgoing* out);
  void SendDigests(Game& g, Clock::time_point now, Outgoing* out);
  void FinishGame();

  // The lowest round trip to mm per minute over the last 10 minutes (LEDBAT's base delay): the
  // minute's index and its lowest. Kept across games; a new connection starts over.
  double RttBase() const;
  void SampleRtt(Clock::time_point now, double rtt_ms);
  std::deque<std::pair<s64, double>> m_rtt_minutes;

  Options m_options;
  bool m_forced = false;
  std::optional<Game> m_game;
  // The last game's figures, offered with the next game.
  u32 m_last_paused = 0;
  double m_last_rollbacks = 0;
};

// ---- The replay writer thread ----
void BeginGame(const GameInfo& game);
// One whole chunk as the file stores it (Rollback/ReplayFile EncodeChunk) and its raw bytes.
void AddChunk(const std::array<u8, 16>& uid, u32 type, std::vector<u8> chunk,
              std::span<const u8> raw);
void EndGame(const std::array<u8, 16>& uid);
void AbortGame(const std::array<u8, 16>& uid);

// ---- The online connection (Rooms thread) ----
// A message from mm. Returns false if it is not a spectate message (the caller handles it).
bool OnServerMessage(const std::string& packet);
Outgoing Service(Clock::time_point now, bool online, size_t unacked, const Health& health);
void OnConnectionLost();

picojson::object Status();
// Tests only: see Uploader::SetForced.
void SetForced(bool forced);
}  // namespace Online::Spectate
