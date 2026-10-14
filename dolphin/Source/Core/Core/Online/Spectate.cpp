// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/Spectate.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <fmt/format.h>
#include <mbedtls/sha256.h>

#include "Common/Logging/Log.h"
#include "Core/Rollback/PeerData.h"
#include "Core/Rollback/ReplayFile.h"

namespace Online::Spectate
{
namespace
{
using namespace std::chrono_literals;
namespace Replay = Gprb::Replay;

// mm's messages are small; anything bigger is not one of them.
constexpr size_t MAX_MESSAGE = 4096;
constexpr size_t DATA_HEADER = 16;
// The pacer's rate is mm's to choose, within these.
constexpr double MIN_RATE = 256;
constexpr double MAX_RATE = 8192;

std::string Hex(std::span<const u8> bytes)
{
  std::string s;
  s.reserve(bytes.size() * 2);
  for (const u8 b : bytes)
    s += fmt::format("{:02x}", b);
  return s;
}

void PutU32(std::vector<u8>& out, u32 v)
{
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 GetU32(std::span<const u8> b, size_t at)
{
  return u32(b[at]) | u32(b[at + 1]) << 8 | u32(b[at + 2]) << 16 | u32(b[at + 3]) << 24;
}

bool IsHeader(u32 type)
{
  return type == Replay::CHUNK_INFO || type == Replay::CHUNK_SETP || type == Replay::FourCC("SYNC");
}

// The chunk types spectators get (never FTSP/FTRM).
bool Streamed(u32 type)
{
  return IsHeader(type) || type == Replay::CHUNK_FRMS || type == Replay::CHUNK_EVNT ||
         type == Replay::CHUNK_ENDG;
}

std::string Json(picojson::object o)
{
  return picojson::value(std::move(o)).serialize();
}

std::optional<double> Num(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<double>() || !std::isfinite(it->second.get<double>()))
    return std::nullopt;
  return it->second.get<double>();
}

std::optional<std::string> Str(const picojson::object& o, const char* key, size_t max)
{
  const auto it = o.find(key);
  if (it == o.end() || !it->second.is<std::string>() || it->second.get<std::string>().size() > max)
    return std::nullopt;
  return it->second.get<std::string>();
}

std::optional<u32> U32(const picojson::object& o, const char* key)
{
  const auto n = Num(o, key);
  if (!n || *n < 0 || *n > 4294967295.0 || std::floor(*n) != *n)
    return std::nullopt;
  return static_cast<u32>(*n);
}
}  // namespace

Uploader::Uploader(Options options) : m_options(options)
{
}

void Uploader::FinishGame()
{
  if (!m_game)
    return;
  m_last_paused = static_cast<u32>(m_game->pauses);
  m_last_rollbacks = m_game->rollback_n ? m_game->rollback_sum / m_game->rollback_n : 0.0;
  m_game.reset();
}

void Uploader::BeginGame(const GameInfo& game)
{
  FinishGame();
  m_game.emplace();
  m_game->info = game;
}

void Uploader::AddChunk(const std::array<u8, 16>& uid, u32 type, std::vector<u8> chunk,
                        std::span<const u8> raw)
{
  if (!m_game || m_game->info.uid != uid || !Streamed(type) || m_game->too_big)
    return;
  Game& g = *m_game;
  if (g.bytes + chunk.size() > m_options.max_game_bytes)
  {
    g.too_big = true;
    WARN_LOG_FMT(NETPLAY, "Spectate: game {} is over {} bytes; its upload stops", g.info.game,
                 m_options.max_game_bytes);
    return;
  }
  Chunk c;
  c.type = type;
  if (type == Replay::CHUNK_FRMS && raw.size() >= 8)
  {
    c.frames = true;
    c.first_frame = GetU32(raw, 0);
    std::array<u8, 32> sha{};
    mbedtls_sha256_ret(raw.data(), raw.size(), sha.data(), 0);
    std::copy_n(sha.begin(), c.digest.size(), c.digest.begin());
  }
  g.bytes += chunk.size();
  c.bytes = std::make_shared<const std::vector<u8>>(std::move(chunk));
  g.chunks.push_back(std::move(c));
}

void Uploader::EndGame(const std::array<u8, 16>& uid)
{
  if (m_game && m_game->info.uid == uid)
    m_game->ended = true;
}

void Uploader::AbortGame(const std::array<u8, 16>& uid)
{
  if (m_game && m_game->info.uid == uid)
    m_game.reset();
}

double Uploader::RttBase() const
{
  double base = -1;
  for (const auto& [minute, rtt] : m_rtt_minutes)
    base = base < 0 ? rtt : std::min(base, rtt);
  return base;
}

void Uploader::SampleRtt(Clock::time_point now, double rtt_ms)
{
  if (rtt_ms <= 0)
    return;
  const s64 minute =
      std::chrono::duration_cast<std::chrono::minutes>(now.time_since_epoch()).count();
  if (m_rtt_minutes.empty() || m_rtt_minutes.back().first != minute)
    m_rtt_minutes.emplace_back(minute, rtt_ms);
  else
    m_rtt_minutes.back().second = std::min(m_rtt_minutes.back().second, rtt_ms);
  while (!m_rtt_minutes.empty() && m_rtt_minutes.front().first <= minute - 10)
    m_rtt_minutes.pop_front();
}

void Uploader::ConnectionLost()
{
  // Another connection may take another route: its round trip is learned anew.
  m_rtt_minutes.clear();
  // mm forgot this connection's slots: offer again once connected (mm treats it as a new offer).
  if (!m_game)
    return;
  m_game->offered = false;
  m_game->role = Role::Offering;
  m_game->slot = 0;
}

bool Uploader::OnMessage(const picojson::object& o)
{
  const auto type = Str(o, "type", 32);
  if (!type || !type->starts_with("spectate-"))
    return false;
  if (*type == "spectate-error")
  {
    const auto op = Str(o, "op", 32);
    const auto error = Str(o, "error", 200);
    WARN_LOG_FMT(NETPLAY, "Spectate: mm refused {}: {}", op.value_or("?"), error.value_or("?"));
    return true;
  }
  if (*type != "spectate-role")
    return true;
  const auto uid = Str(o, "gameUid", 32);
  const auto role = Str(o, "role", 16);
  if (!m_game || !uid || *uid != Hex(m_game->info.uid) || !role)
    return true;
  Game& g = *m_game;
  if (*role == "none")
  {
    g.role = Role::None;
    g.slot = 0;
    INFO_LOG_FMT(NETPLAY, "Spectate: game {}: no upload", g.info.game);
    return true;
  }
  const auto slot = U32(o, "slot");
  if (!slot || *slot == 0 || (*role != "primary" && *role != "witness"))
    return true;
  g.slot = *slot;
  if (*role == "witness")
  {
    g.role = Role::Witness;
    g.digest_next = 0;
    g.next_digest = {};
  }
  else
  {
    g.role = Role::Primary;
    const auto it = o.find("from");
    g.from = it != o.end() && it->second.is<double>() ? U32(o, "from") : std::nullopt;
    const auto headers = o.find("headers");
    g.headers = headers == o.end() || !headers->second.is<bool>() || headers->second.get<bool>();
    g.rate = std::clamp(Num(o, "rate").value_or(2048), MIN_RATE, MAX_RATE);
    g.next = 0;
    g.offset = 0;
    g.seq = 0;
    g.reached = !g.from;
    g.tokens = g.rate * m_options.burst_seconds;
    g.refilled = {};
    // A fresh role (a takeover back to this player): its earlier pauses are history.
    g.paused = false;
    g.stopped = false;
  }
  INFO_LOG_FMT(NETPLAY, "Spectate: game {}: {} (slot {}{})", g.info.game, *role, g.slot,
               g.from ? fmt::format(", from frame {}", *g.from) : "");
  return true;
}

std::optional<std::string> Uploader::Unhealthy(Game& g, Clock::time_point now, size_t unacked,
                                               const Health& health)
{
  if (unacked > m_options.queue_pause)
    return "queue";
  // During the game's first `baseline` its normal is being learned: only the queue counts.
  if (!health.running || g.samples.empty() || !g.baseline_done)
    return std::nullopt;
  // The counters' growth since the newest sample at least `window` old (Service keeps them),
  // against the game's normal over as long.
  const Sample& oldest = g.samples.front();
  const double seconds = std::chrono::duration<double>(now - oldest.at).count();
  const auto worse = [&](u64 count, double per_s, double margin) {
    return static_cast<double>(count) > m_options.rate_factor * per_s * seconds + margin;
  };
  if (worse(health.late_frames - oldest.late, g.base_late_per_s, m_options.late_margin))
    return "frame";
  if (health.rollback_avg > g.base_rollbacks + m_options.rollback_margin)
    return "rollback";
  if (g.base_ping_n > 0 && health.ping_ms > 0 &&
      health.ping_ms > g.base_ping + m_options.ping_margin_ms)
  {
    return "rtt";
  }
  return std::nullopt;
}

void Uploader::SendData(Game& g, Clock::time_point now, size_t unacked, Outgoing* out)
{
  const double rate = m_forced ? 16384.0 : g.rate;
  const size_t packet = m_forced ? 1200 : m_options.packet;
  const size_t max_unacked = m_forced ? (1u << 20) : m_options.max_unacked;
  if (g.refilled != Clock::time_point{})
  {
    const double dt = std::chrono::duration<double>(now - g.refilled).count();
    g.tokens = std::min(g.tokens + rate * dt, rate * m_options.burst_seconds);
  }
  g.refilled = now;
  size_t in_flight = unacked;
  while (g.next < g.chunks.size() && in_flight < max_unacked)
  {
    const Chunk& c = g.chunks[g.next];
    if (g.offset == 0)
    {
      // What this upload skips: the headers mm already has (a takeover), and everything before
      // the frame mm needs next.
      bool skip = false;
      if (IsHeader(c.type))
      {
        skip = !g.headers;
      }
      else if (!g.reached)
      {
        if (c.frames && c.first_frame == *g.from)
        {
          g.reached = true;
        }
        else if (c.frames && c.first_frame > *g.from)
        {
          WARN_LOG_FMT(NETPLAY, "Spectate: game {}: no chunk starts at frame {}; stopping",
                       g.info.game, *g.from);
          out->json.push_back(Json({{"type", picojson::value("spectate-stop")},
                                    {"slot", picojson::value(static_cast<double>(g.slot))},
                                    {"reason", picojson::value("other")}}));
          g.stopped = true;
          return;
        }
        else
        {
          skip = true;
        }
      }
      if (skip)
      {
        ++g.next;
        continue;
      }
    }
    const size_t room = packet - DATA_HEADER;
    const size_t n = std::min(room, c.bytes->size() - g.offset);
    if (g.tokens < static_cast<double>(n + DATA_HEADER))
      return;
    std::vector<u8> p;
    p.reserve(DATA_HEADER + n);
    PutU32(p, g.slot);
    PutU32(p, g.seq);
    PutU32(p, static_cast<u32>(g.offset));
    PutU32(p, static_cast<u32>(c.bytes->size()));
    p.insert(p.end(), c.bytes->begin() + g.offset, c.bytes->begin() + g.offset + n);
    out->data.push_back(std::move(p));
    g.tokens -= static_cast<double>(n + DATA_HEADER);
    in_flight += n + DATA_HEADER;
    g.offset += n;
    if (g.offset == c.bytes->size())
    {
      g.offset = 0;
      ++g.next;
      ++g.seq;
    }
  }
}

void Uploader::SendDigests(Game& g, Clock::time_point now, Outgoing* out)
{
  if (now < g.next_digest)
    return;
  g.next_digest = now + m_options.digest_every;
  picojson::array list;
  while (g.digest_next < g.chunks.size() && list.size() < m_options.max_digests)
  {
    const Chunk& c = g.chunks[g.digest_next++];
    if (!c.frames)
      continue;
    list.emplace_back(picojson::array{picojson::value(static_cast<double>(c.first_frame)),
                                      picojson::value(Hex(c.digest))});
  }
  if (list.empty())
    return;
  out->json.push_back(Json({{"type", picojson::value("spectate-digest")},
                            {"slot", picojson::value(static_cast<double>(g.slot))},
                            {"frms", picojson::value(std::move(list))}}));
}

Outgoing Uploader::Service(Clock::time_point now, bool online, size_t unacked, const Health& health)
{
  Outgoing out;
  // The round trip to mm is sampled always, games or not: the menus are when the uplink is quiet.
  if (online)
    SampleRtt(now, health.mm_rtt_ms);
  if (!m_game)
    return out;
  Game& g = *m_game;

  // The game's normal: its first `baseline` of running; the average rollback depth over the game
  // goes with the next offer.
  if (health.running)
  {
    if (!g.running_since)
    {
      g.running_since = now;
      g.late_at_start = health.late_frames;
    }
    if (!g.baseline_done)
    {
      g.base_rollbacks = (g.base_rollbacks * g.base_n + health.rollback_avg) / (g.base_n + 1);
      ++g.base_n;
      if (health.ping_ms > 0)
      {
        g.base_ping = (g.base_ping * g.base_ping_n + health.ping_ms) / (g.base_ping_n + 1);
        ++g.base_ping_n;
      }
      g.baseline_done = now - *g.running_since >= m_options.baseline;
      if (g.baseline_done)
      {
        const double s = std::chrono::duration<double>(now - *g.running_since).count();
        g.base_late_per_s = (health.late_frames - g.late_at_start) / s;
        INFO_LOG_FMT(NETPLAY,
                     "Spectate: game {}'s normal: {:.1f} late frames/s, rollback depth {:.2f}, "
                     "ping {:.0f} ms",
                     g.info.game, g.base_late_per_s, g.base_rollbacks,
                     g.base_ping_n ? g.base_ping : -1.0);
      }
    }
    g.rollback_sum += health.rollback_avg;
    ++g.rollback_n;
    // The late-frame counter, every tick from the game's start (whatever the role):
    // kept back to the newest sample at least `window` old.
    g.samples.push_back({now, health.late_frames});
    while (g.samples.size() >= 2 && now - g.samples[1].at >= m_options.window)
      g.samples.pop_front();
  }
  else
  {
    g.samples.clear();
  }

  if (!online)
    return out;
  if (!g.offered)
  {
    g.offered = true;
    out.json.push_back(Json({{"type", picojson::value("spectate-offer")},
                             {"matchId", picojson::value(g.info.match_id)},
                             {"gameUid", picojson::value(Hex(g.info.uid))},
                             {"game", picojson::value(static_cast<double>(g.info.game))},
                             {"paused", picojson::value(static_cast<double>(m_last_paused))},
                             {"rollbacks", picojson::value(m_last_rollbacks)}}));
    INFO_LOG_FMT(NETPLAY, "Spectate: offered game {} of {}", g.info.game, g.info.match_id);
  }

  if (g.role != Role::Primary && g.role != Role::Witness)
    return out;
  const auto slot = picojson::value(static_cast<double>(g.slot));
  const auto reason = m_forced ? std::nullopt : Unhealthy(g, now, unacked, health);
  if (g.role == Role::Witness)
  {
    if (!reason)
      SendDigests(g, now, &out);
    return out;
  }
  if (g.stopped)
    return out;
  if (g.too_big)
  {
    g.stopped = true;
    out.json.push_back(Json({{"type", picojson::value("spectate-stop")},
                             {"slot", slot},
                             {"reason", picojson::value("other")}}));
    return out;
  }
  if (reason)
  {
    g.clean_since = now;
    if (!g.paused)
    {
      g.paused = true;
      ++g.pauses;
      const bool stop = g.pauses >= m_options.max_pauses;
      INFO_LOG_FMT(NETPLAY, "Spectate: game {}: upload {} ({}, pause {})", g.info.game,
                   stop ? "stopped" : "paused", *reason, g.pauses);
      out.json.push_back(Json({{"type", picojson::value(stop ? "spectate-stop" : "spectate-pause")},
                               {"slot", slot},
                               {"reason", picojson::value(*reason)}}));
      if (stop)
        g.stopped = true;
    }
    return out;
  }
  if (g.paused)
  {
    if (now - g.clean_since < m_options.resume_after)
      return out;
    g.paused = false;
    INFO_LOG_FMT(NETPLAY, "Spectate: game {}: upload resumed", g.info.game);
    out.json.push_back(Json({{"type", picojson::value("spectate-resume")}, {"slot", slot}}));
  }
  if (m_forced)
  {
    SendData(g, now, unacked, &out);
    return out;
  }
  // Nothing during the game's first `baseline` of running (the start is its busiest time).
  if (health.running && !g.baseline_done)
    return out;
  // A queue on the uplink: hold until it drains (the pacer's tokens wait, capped at its burst).
  const double base = RttBase();
  if (health.mm_rtt_ms > 0 && base > 0 && health.mm_rtt_ms > base + m_options.rtt_target_ms)
  {
    if (g.refilled != Clock::time_point{})
      g.held_s += std::chrono::duration<double>(now - g.refilled).count();
    g.refilled = now;
    return out;
  }
  SendData(g, now, unacked, &out);
  return out;
}

picojson::object Uploader::Status() const
{
  picojson::object o;
  if (!m_game)
  {
    o["game"] = picojson::value();
    return o;
  }
  const Game& g = *m_game;
  const char* role = g.role == Role::Primary ? "primary" :
                     g.role == Role::Witness ? "witness" :
                     g.role == Role::None    ? "none" :
                                               "offering";
  o["game"] = picojson::value(static_cast<double>(g.info.game));
  o["role"] = picojson::value(role);
  o["slot"] = picojson::value(static_cast<double>(g.slot));
  o["chunks"] = picojson::value(static_cast<double>(g.chunks.size()));
  o["sent"] = picojson::value(static_cast<double>(g.next));
  o["bytes"] = picojson::value(static_cast<double>(g.bytes));
  o["paused"] = picojson::value(g.paused);
  o["pauses"] = picojson::value(static_cast<double>(g.pauses));
  o["stopped"] = picojson::value(g.stopped);
  o["ended"] = picojson::value(g.ended);
  o["forced"] = picojson::value(m_forced);
  o["held_s"] = picojson::value(g.held_s);
  o["mm_rtt_base"] = picojson::value(RttBase());
  return o;
}

// ---- The process's uploader ----

namespace
{
std::mutex s_mutex;
Uploader& The()
{
  static Uploader s_uploader;
  return s_uploader;
}
}  // namespace

void BeginGame(const GameInfo& game)
{
  std::lock_guard lk(s_mutex);
  The().BeginGame(game);
}

void AddChunk(const std::array<u8, 16>& uid, u32 type, std::vector<u8> chunk,
              std::span<const u8> raw)
{
  std::lock_guard lk(s_mutex);
  The().AddChunk(uid, type, std::move(chunk), raw);
}

void EndGame(const std::array<u8, 16>& uid)
{
  std::lock_guard lk(s_mutex);
  The().EndGame(uid);
}

void AbortGame(const std::array<u8, 16>& uid)
{
  std::lock_guard lk(s_mutex);
  The().AbortGame(uid);
}

bool OnServerMessage(const std::string& packet)
{
  // A cheap look first: everything else on the connection is the Rooms and Chat code's.
  if (packet.find("\"spectate-") == std::string::npos || packet.size() > MAX_MESSAGE)
    return false;
  if (!Gprb::PeerData::JsonSafeToParse(packet))
    return false;
  picojson::value root;
  if (!picojson::parse(root, packet).empty() || !root.is<picojson::object>())
    return false;
  std::lock_guard lk(s_mutex);
  return The().OnMessage(root.get<picojson::object>());
}

Outgoing Service(Clock::time_point now, bool online, size_t unacked, const Health& health)
{
  std::lock_guard lk(s_mutex);
  return The().Service(now, online, unacked, health);
}

void OnConnectionLost()
{
  std::lock_guard lk(s_mutex);
  The().ConnectionLost();
}

picojson::object Status()
{
  std::lock_guard lk(s_mutex);
  return The().Status();
}

void SetForced(bool forced)
{
  std::lock_guard lk(s_mutex);
  The().SetForced(forced);
}
}  // namespace Online::Spectate
