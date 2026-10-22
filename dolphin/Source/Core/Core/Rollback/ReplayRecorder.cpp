// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ReplayRecorder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Common/TimeUtil.h"
#include "Common/Version.h"
#include "Core/Brawlback/include/brawlback-common/BrawlbackConstants.h"
#include "Core/Config/OnlineSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/Online/GameSetup.h"
#include "Core/Online/Ranked.h"
#include "Core/Online/Spectate.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Gprb::Replay
{
namespace
{
// ---- The writer thread: one for the process, shared by every recording. ----
// Jobs run in order. The CPU thread only queues them; above MAX_QUEUED bytes (a disk that stopped
// for a long time) the recording stops being written instead of growing without bound.
constexpr size_t MAX_QUEUED = 64ull << 20;

struct Job
{
  std::function<void()> run;
  size_t bytes = 0;
};

class WriterThread
{
public:
  ~WriterThread()
  {
    {
      std::lock_guard lk(m_mutex);
      m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
      m_thread.join();
  }

  bool Push(Job job)
  {
    std::lock_guard lk(m_mutex);
    if (m_queued + job.bytes > MAX_QUEUED)
      return false;
    m_queued += job.bytes;
    m_jobs.push_back(std::move(job));
    if (!m_thread.joinable())
      m_thread = std::thread([this] { Loop(); });
    m_cv.notify_one();
    return true;
  }

  void Flush()
  {
    std::unique_lock lk(m_mutex);
    m_idle_cv.wait(lk, [this] { return m_jobs.empty() && !m_busy; });
  }

private:
  void Loop()
  {
    Common::SetCurrentThreadName("Replay writer");
    Common::SetCurrentThreadBackgroundPriority();
    std::unique_lock lk(m_mutex);
    while (true)
    {
      m_cv.wait(lk, [this] { return m_quit || !m_jobs.empty(); });
      if (m_jobs.empty())
        return;
      Job job = std::move(m_jobs.front());
      m_jobs.pop_front();
      m_busy = true;
      lk.unlock();
      job.run();
      lk.lock();
      m_busy = false;
      m_queued -= job.bytes;
      if (m_jobs.empty())
        m_idle_cv.notify_all();
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_cv, m_idle_cv;
  std::deque<Job> m_jobs;
  size_t m_queued = 0;
  bool m_busy = false;
  bool m_quit = false;
  std::thread m_thread;
};

WriterThread& Writers()
{
  static WriterThread s_writers;
  return s_writers;
}

// One recording's outputs, shared by its jobs: the file (once a write fails, the rest of the file
// is skipped) and the spectate upload.
struct Output
{
  Writer writer;
  std::string path;
  bool save = true;
  bool stream = false;
  std::array<u8, 16> uid{};
  bool failed = false;
  bool warned = false;
};

void Fail(Output& out, std::string_view what)
{
  if (out.failed)
    return;
  out.failed = true;
  out.writer.Close();
  ERROR_LOG_FMT(BRAWLBACK, "replay: {} ({}); this game's replay stops here", what, out.path);
  OSD::AddMessage("The replay of this game could not be saved.", OSD::Duration::VERY_LONG,
                  OSD::Color::YELLOW);
}

// ---- The recording (CPU thread, under the session's mutex) ----

constexpr s64 FINAL_BEHIND = MAX_ROLLBACK_FRAMES + 1;
constexpr size_t RING = 64;

struct RingFrame
{
  s64 frame = -1;
  bool input = false;
  bool saved = false;
  Slots pads{};
  u32 checksum = 0;
  Parts parts{};
  std::vector<u8> fighters;  // every match port's blocks, in port order
};

struct Recording
{
  bool active = false;
  bool overflowed = false;
  bool save = true;  // to a file: fighter memory is read and recorded only then
  std::shared_ptr<Output> out;
  u32 ports = 0;
  int num_ports = 0;
  s64 next_emit = -1;  // the next frame to hand to the writer (-1: the first frame seen)
  s64 newest = -1;
  s64 first_frame = -1;  // the first frame recorded
  std::array<RingFrame, RING> ring{};
  FrameChunk frames;
  FighterChunk fighters;
  picojson::array events;
  std::string path;
};

Recording s_rec;
std::mutex s_path_mutex;
std::string s_last_path;

const char* ModeName(const MatchMeta& m)
{
  if (m.room)
    return "room";
  switch (m.mode)
  {
  case 0:
    return "ranked";
  case 1:
    return "unranked";
  case 2:
    return "direct";
  case 3:
    return "teams";
  default:
    return "unknown";
  }
}

const char* ModeLabel(const MatchMeta& m)
{
  if (m.room)
    return "Room";
  switch (m.mode)
  {
  case 0:
    return "Ranked";
  case 1:
    return "Unranked";
  case 2:
    return "Direct";
  case 3:
    return "Teams";
  default:
    return "Online";
  }
}

std::string IsoUtcNow(int* utc_offset_min)
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &t);
#else
  gmtime_r(&t, &utc);
#endif
  if (const auto local = Common::LocalTime(t))
  {
    // The offset of local time from UTC, from the two broken-down times.
    std::tm l = *local, u = utc;
    l.tm_isdst = u.tm_isdst = 0;
    *utc_offset_min = static_cast<int>(std::difftime(std::mktime(&l), std::mktime(&u)) / 60);
  }
  return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", utc.tm_year + 1900, utc.tm_mon + 1,
                     utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, ms);
}

std::string PlayerLabel(const BeginInfo& info, int port)
{
  std::string name = FileNamePart(info.meta.players[port].name, 16);
  if (name.empty())
    name = fmt::format("P{}", port + 1);
  return fmt::format("{} ({})", name, Online::GameSetup::CharacterName(info.char_kind[port]));
}

// "2026-10-12 1403 Ranked G2 - Alice (Fox) vs Bob (Marth)": the local time the game started, the
// mode, the game number and who played what (teams joined with " + ").
std::string FileStem(const BeginInfo& info)
{
  std::string when = "0000-00-00 0000";
  if (const auto t = Common::LocalTime(std::time(nullptr)))
  {
    when = fmt::format("{:04}-{:02}-{:02} {:02}{:02}", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                       t->tm_hour, t->tm_min);
  }
  std::vector<std::string> sides;
  if (info.teams)
  {
    for (u8 team = 0; team < 3; ++team)
    {
      std::string side;
      for (int port = 0; port < PORTS; ++port)
      {
        if (!((info.match_ports >> port) & 1) || info.team[port] != team)
          continue;
        side += (side.empty() ? "" : " + ") + PlayerLabel(info, port);
      }
      if (!side.empty())
        sides.push_back(std::move(side));
    }
  }
  else
  {
    for (int port = 0; port < PORTS; ++port)
    {
      if ((info.match_ports >> port) & 1)
        sides.push_back(PlayerLabel(info, port));
    }
  }
  std::string who;
  for (const auto& s : sides)
    who += (who.empty() ? "" : " vs ") + s;
  return fmt::format("{} {} G{} - {}", when, ModeLabel(info.meta), std::max<u32>(info.game, 1), who);
}

// Dolphin's own replay folder, inside its User folder: the default, and where a game goes when the
// replay folder refuses the file (the launcher lists it then: replays/own_replay_folder.ts).
std::string OwnReplayDir()
{
  return ::File::GetUserPath(D_USER_IDX) + "Replays";
}

std::string ReplayDir(std::string dir)
{
  if (dir.empty())
    dir = OwnReplayDir();
  while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
    dir.pop_back();
  if (Config::Get(Config::ONLINE_REPLAY_MONTHLY_FOLDERS))
  {
    if (const auto t = Common::LocalTime(std::time(nullptr)))
      dir += fmt::format("/{:04}-{:02}", t->tm_year + 1900, t->tm_mon + 1);
  }
  return dir + "/";
}

std::string ReplayDir()
{
  return ReplayDir(Config::Get(Config::ONLINE_REPLAY_DIR));
}

// The first free name for stem in dir.
std::string FreePath(const std::string& dir, const std::string& stem)
{
  std::string path = dir + stem + EXTENSION;
  for (int n = 2; ::File::Exists(path) && n < 100; ++n)
    path = fmt::format("{}{} ({}){}", dir, stem, n, EXTENSION);
  return path;
}

std::string InfoJson(const BeginInfo& info)
{
  picojson::object o;
  o["v"] = picojson::value(1.0);
  int offset = 0;
  o["startTime"] = picojson::value(IsoUtcNow(&offset));
  o["utcOffsetMin"] = picojson::value(static_cast<double>(offset));
  o["mode"] = picojson::value(ModeName(info.meta));
  o["matchId"] = picojson::value(info.meta.match_id);
  o["gameIndex"] = picojson::value(static_cast<double>(info.game));
  o["sessionGame"] = picojson::value(static_cast<double>(info.session_game));
  o["isTeams"] = picojson::value(info.teams);
  o["stageId"] = picojson::value(static_cast<double>(info.stage));
  o["asl"] = picojson::value(static_cast<double>(info.asl));
  picojson::array players;
  for (int port = 0; port < PORTS; ++port)
  {
    if (!((info.match_ports >> port) & 1))
      continue;
    const PlayerMeta& m = info.meta.players[port];
    picojson::object p;
    p["port"] = picojson::value(static_cast<double>(port + 1));
    p["uid"] = picojson::value(m.uid);
    p["displayName"] = picojson::value(m.name);
    p["connectCode"] = picojson::value(m.code);
    p["characterId"] = picojson::value(static_cast<double>(info.char_kind[port]));
    p["character"] = picojson::value(Online::GameSetup::CharacterName(info.char_kind[port]));
    p["costume"] = picojson::value(static_cast<double>(info.costume[port]));
    p["teamId"] = info.teams ? picojson::value(static_cast<double>(info.team[port])) : picojson::value();
    players.emplace_back(p);
  }
  o["players"] = picojson::value(players);
  if (!info.meta.rules.empty())
    o["rules"] = picojson::value(info.meta.rules);
  picojson::object recorder;
  recorder["port"] = picojson::value(static_cast<double>(info.local_port + 1));
  recorder["role"] = picojson::value(info.host ? "host" : "joiner");
  o["recorder"] = picojson::value(recorder);
  picojson::object build;
  build["dolphin"] = picojson::value(Common::GetScmDescStr());
  build["sim"] = picojson::value(static_cast<double>(info.sim_version));
  build["game"] = picojson::value(SConfig::GetInstance().GetGameID());
  build["revision"] = picojson::value(static_cast<double>(SConfig::GetInstance().GetRevision()));
  // The Project+ release (the launcher's, Config ONLINE_PPLUS_RELEASE): "v" and up to 24 of
  // [0-9a-z.], else left out.
  if (const std::string pplus = Config::Get(Config::ONLINE_PPLUS_RELEASE);
      pplus.size() >= 2 && pplus.size() <= 25 && pplus[0] == 'v' &&
      std::all_of(pplus.begin() + 1, pplus.end(),
                  [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.'; }))
  {
    build["pplus"] = picojson::value(pplus);
  }
  o["build"] = picojson::value(build);
  picojson::object session;
  session["regionSet"] = picojson::value(info.region_set);
  session["delay"] = picojson::value(static_cast<double>(info.input_delay));
  session["startFrame"] = picojson::value(static_cast<double>(info.start_frame));
  o["session"] = picojson::value(session);
  return picojson::value(o).serialize();
}

std::vector<u8> Bytes(const std::string& s)
{
  return {s.begin(), s.end()};
}

// Queues a job for this recording's file; on overflow the recording stops being written.
void Queue(std::function<void(Output&)> fn, size_t bytes)
{
  if (!s_rec.out || s_rec.overflowed)
    return;
  std::shared_ptr<Output> out = s_rec.out;
  if (!Writers().Push({[out, fn = std::move(fn)] { fn(*out); }, bytes}))
  {
    s_rec.overflowed = true;
    ERROR_LOG_FMT(BRAWLBACK, "replay: the disk is not keeping up; the rest of this game is not saved");
  }
}

// Encodes a chunk once: into the file when saving, to the spectate upload when streaming (never
// fighter memory).
void Put(Output& out, u32 type, u32 flags, std::span<const u8> raw, std::string_view what)
{
  const bool to_file = out.save && !out.failed;
  const bool to_stream = out.stream && type != CHUNK_FTSP && type != CHUNK_FTRM;
  if (!to_file && !to_stream)
    return;
  auto chunk = EncodeChunk(type, flags, raw);
  if (!chunk)
  {
    if (to_file)
      Fail(out, what);
    return;
  }
  if (to_file && !out.writer.AppendChunk(*chunk))
    Fail(out, what);
  if (to_stream)
    Online::Spectate::AddChunk(out.uid, type, std::move(*chunk), raw);
}

void QueueChunks(bool all)
{
  // A whole chunk every FRAMES_PER_CHUNK frames, and at the end whatever is left.
  if (s_rec.frames.pads.empty() || (!all && s_rec.frames.pads.size() < FRAMES_PER_CHUNK))
    return;
  FrameChunk frames = std::move(s_rec.frames);
  FighterChunk fighters = std::move(s_rec.fighters);
  picojson::array events = std::move(s_rec.events);
  s_rec.frames = {};
  s_rec.fighters = {};
  s_rec.events.clear();
  const size_t bytes = frames.pads.size() * sizeof(Slots) +
                       (fighters.data.empty() ? 0 : fighters.data.size() * fighters.data[0].size());
  Queue(
      [frames = std::move(frames), fighters = std::move(fighters),
       events = std::move(events)](Output& out) {
        const auto f = EncodeFrames(frames);
        Put(out, CHUNK_FRMS, FLAG_DEFLATE | FLAG_CRITICAL, f, "cannot write frames");
        if (out.save && fighters.count && !fighters.data.empty())
        {
          const auto m = EncodeFighters(fighters);
          Put(out, CHUNK_FTRM, FLAG_DEFLATE, m, "cannot write fighter memory");
        }
        if (!events.empty())
        {
          picojson::object o;
          o["events"] = picojson::value(events);
          const auto e = Bytes(picojson::value(o).serialize());
          Put(out, CHUNK_EVNT, FLAG_DEFLATE, e, "cannot write events");
        }
      },
      bytes);
}

// Hands frame `f` (final) to the chunk being built.
void EmitFrame(s64 f)
{
  RingFrame& r = s_rec.ring[static_cast<size_t>(f) % RING];
  if (r.frame != f || !r.input)
  {
    // Never expected: a frame that ran without a pass this recorder saw.
    WARN_LOG_FMT(BRAWLBACK, "replay: frame {} has no input; the recording stops here", f);
    s_rec.overflowed = true;
    return;
  }
  if (s_rec.frames.pads.empty())
  {
    s_rec.frames.first_frame = static_cast<u32>(f);
    s_rec.frames.ports = static_cast<u8>(s_rec.ports);
    s_rec.fighters = {};
    if (s_rec.save)
    {
      s_rec.fighters.first_frame = static_cast<u32>(f);
      s_rec.fighters.ports = static_cast<u8>(s_rec.ports);
      s_rec.fighters.block = FighterBlockBytes();
      s_rec.fighters.data.assign(s_rec.num_ports, {});
    }
  }
  s_rec.frames.pads.push_back(r.pads);
  s_rec.frames.checksums.push_back(r.saved ? r.checksum : 0);
  if (r.saved && f % PARTS_EVERY == 0)
    s_rec.frames.parts.push_back(r.parts);
  if (s_rec.save)
  {
    const u32 block = FighterBlockBytes();
    for (int i = 0; i < s_rec.num_ports; ++i)
    {
      auto& d = s_rec.fighters.data[i];
      if (r.saved && r.fighters.size() == static_cast<size_t>(s_rec.num_ports) * block)
        d.insert(d.end(), r.fighters.begin() + i * block, r.fighters.begin() + (i + 1) * block);
      else
        d.insert(d.end(), block, 0);
    }
    ++s_rec.fighters.count;
  }
  QueueChunks(false);
}
}  // namespace

// ---- Fighter memory (FTSP) ----

// From docs/brawl-memory-map.md ("Per player") and BrawlHeaders' soModuleAccesser: a module of the
// active Fighter is [[[fighter + 0x60] + 0xD8] + 4 * index], its size the Impl class's (where the
// headers have one). Raw bytes: what they mean is decoded later (stats), against the P+ version
// the file names.
const std::vector<FighterBlock>& FighterBlocks()
{
  static const std::vector<FighterBlock> blocks = [] {
    std::vector<FighterBlock> b{
        {"entry", false, {0x00}, 0x80},             // ftEntry: ids, active instance, port, kind
        {"owner", false, {0x28, 0x00, 0x00}, 0x100},  // ftOwner's data: damage +0x24, stocks +0x34
        {"fighter", true, {0x00}, 0x180},           // Fighter: ftKind +0x110
    };
    const auto module = [&](const char* name, u32 index, u32 size) {
      b.push_back({name, true, {0x60, 0xD8, 4 * index, 0x00}, size});
    };
    module("motion", 2, 0x170);
    module("posture", 3, 0x78);
    module("ground", 4, 0x58);
    module("situation", 5, 0x3C);
    module("collisionAttack", 7, 0xA0);
    module("collisionHit", 8, 0x80);
    module("collisionShield", 9, 0x54);
    module("damage", 14, 0xAC);
    module("stop", 17, 0x24);
    module("controller", 23, 0x14);
    module("workManage", 25, 0x30);
    module("status", 28, 0xA4);
    module("kinetic", 31, 0x20);
    return b;
  }();
  return blocks;
}

u32 FighterBlockBytes()
{
  static const u32 bytes = [] {
    u32 n = 0;
    for (const auto& b : FighterBlocks())
      n += b.size;
    return n;
  }();
  return bytes;
}

std::string FighterSpecJson()
{
  picojson::object o;
  o["v"] = picojson::value(1.0);
  o["source"] = picojson::value(
      "per frame and port: these blocks of guest memory (big-endian) after the frame ran, "
      "concatenated; a block starts at the port's ftEntry, or at its active Fighter (ftEntry + "
      "0x34 + 8 * [ftEntry + 0x0A]); every chain step but the last adds and reads a pointer, "
      "the last only adds; zeros where a pointer does not resolve");
  picojson::array blocks;
  u32 offset = 0;
  for (const auto& b : FighterBlocks())
  {
    picojson::object e;
    e["name"] = picojson::value(b.name);
    e["base"] = picojson::value(b.from_fighter ? "fighter" : "entry");
    picojson::array chain;
    for (u32 c : b.chain)
      chain.emplace_back(static_cast<double>(c));
    e["chain"] = picojson::value(chain);
    e["offset"] = picojson::value(static_cast<double>(offset));
    e["size"] = picojson::value(static_cast<double>(b.size));
    blocks.emplace_back(e);
    offset += b.size;
  }
  o["blocks"] = picojson::value(blocks);
  o["bytes"] = picojson::value(static_cast<double>(offset));
  return picojson::value(o).serialize();
}

void ReadFighterMemory(Core::System& system, int port, u8* out)
{
  std::fill(out, out + FighterBlockBytes(), u8(0));
  const Guest g(system.GetMemory());
  // ftEntryManager (docs/brawl-memory-map.md): entries at [0x80624780], count at +4, stride 0x244.
  constexpr u32 ENTRY_MANAGER = 0x80624780;
  constexpr u32 ENTRY_SIZE = 0x244;
  const auto entries = g.Ptr32(ENTRY_MANAGER);
  const u32 count = std::min<u32>(g.U32(ENTRY_MANAGER + 4).value_or(0), 16);
  std::optional<u32> entry;
  for (u32 k = 0; entries && k < count && !entry; ++k)
  {
    const u32 e = *entries + k * ENTRY_SIZE;
    if (g.U32(e + 0x58) == static_cast<u32>(port) && g.Ptr32(e + 0x28))
      entry = e;
  }
  if (!entry)
    return;
  const u32 active = g.Ptr(*entry + 0x0A, 1) ? (*g.Ptr(*entry + 0x0A, 1) & 3) : 0;
  const auto fighter = g.Ptr32(*entry + 0x34 + 8 * active);
  u32 at = 0;
  for (const auto& b : FighterBlocks())
  {
    std::optional<u32> a = b.from_fighter ? fighter : entry;
    for (size_t i = 0; a && i + 1 < b.chain.size(); ++i)
      a = g.Ptr32(*a + b.chain[i]);
    if (a)
    {
      if (const u8* p = g.Ptr(*a + b.chain.back(), b.size))
        std::copy(p, p + b.size, out + at);
    }
    at += b.size;
  }
}

// ---- Recording ----

bool Saving()
{
  return Config::Get(Config::ONLINE_SAVE_REPLAYS);
}

bool Streamed(const MatchMeta& meta)
{
  // Only a match mm made can be streamed (its id names it to mm).
  if (meta.match_id.empty())
    return false;
  const bool ranked = meta.mode == 0 && !meta.room;
  return ranked || Config::Get(Config::ONLINE_STREAM_GAMES);
}

bool Enabled(const MatchMeta& meta)
{
  return Saving() || Streamed(meta);
}

bool Begin(Core::System& system, const BeginInfo& info)
{
  if (s_rec.active)
    Abort();
  const bool save = Saving();
  const bool stream = Streamed(info.meta);
  if (!save && !stream)
    return false;
  s_rec = {};
  s_rec.save = save;
  s_rec.ports = info.match_ports & 0xF;
  for (int p = 0; p < PORTS; ++p)
    s_rec.num_ports += (s_rec.ports >> p) & 1;
  if (s_rec.num_ports == 0)
    return false;
  std::string dir, path, stem, fallback_dir;
  if (save)
  {
    dir = ReplayDir();
    stem = FileStem(info);
    path = FreePath(dir, stem);
    fallback_dir = ReplayDir(OwnReplayDir());
    if (fallback_dir == dir)
      fallback_dir.clear();
    std::lock_guard lk(s_path_mutex);
    s_last_path = path;
  }
  s_rec.path = path;
  s_rec.out = std::make_shared<Output>();
  s_rec.out->path = path;
  s_rec.out->save = save;
  s_rec.out->stream = stream;
  s_rec.out->uid = info.uid;
  s_rec.active = true;
  const std::string info_json = InfoJson(info);
  const std::string setup_json = info.setup_json;
  const std::string spec_json = save ? FighterSpecJson() : std::string();
  const bool ranked = info.meta.mode == 0 && !info.meta.room;
  const Online::Spectate::GameInfo game{info.meta.match_id, info.uid, info.game,
                                        Config::Get(Config::ONLINE_STREAM_GAMES),
                                        ranked ? Online::Ranked::GameIndexOf(info.session_game) : 0};
  Queue(
      [dir, stem, fallback_dir, info_json, setup_json, spec_json, game](Output& out) {
        if (out.save)
        {
          ::File::CreateFullPath(dir);
          std::string error;
          bool open = out.writer.Open(out.path, game.uid, &error);
          // A folder that refuses us (Windows' Controlled folder access or an antivirus guarding
          // Documents, bug report #96: every game of two players lost) keeps no replay at all, so
          // the game goes to Dolphin's own folder, which the launcher then lists.
          if (!open && !fallback_dir.empty())
          {
            WARN_LOG_FMT(BRAWLBACK, "replay: {} ({}); saving it in {} instead", error, out.path,
                         fallback_dir);
            ::File::CreateFullPath(fallback_dir);
            std::string path = FreePath(fallback_dir, stem);
            {
              std::lock_guard lk(s_path_mutex);
              if (s_last_path == out.path)
                s_last_path = path;
            }
            out.path = std::move(path);
            open = out.writer.Open(out.path, game.uid, &error);
            // Once a run: it says so at every game's start otherwise.
            static std::atomic_bool s_told;
            if (open && !s_told.exchange(true))
            {
              OSD::AddMessage("Your replay folder refused the replay: it is saved in Brawl Online's "
                              "own folder instead.",
                              OSD::Duration::VERY_LONG, OSD::Color::YELLOW);
            }
          }
          if (!open)
            Fail(out, fmt::format("cannot create the replay file: {}", error));
        }
        if (out.stream)
          Online::Spectate::BeginGame(game);
        // Deflated: spectators get nothing of a game before its headers, and the uploader sends
        // one small packet a round trip (raw, they were 8.5 KB, 36 round trips; deflated 1.7 KB).
        // Every reader inflates any chunk with FLAG_DEFLATE.
        Put(out, CHUNK_INFO, FLAG_DEFLATE, Bytes(info_json), "cannot write the replay header");
        Put(out, CHUNK_SETP, FLAG_DEFLATE | FLAG_CRITICAL, Bytes(setup_json),
            "cannot write the replay header");
        if (out.save)
          Put(out, CHUNK_FTSP, 0, Bytes(spec_json), "cannot write the replay header");
        if (out.save && !out.failed)
          NOTICE_LOG_FMT(BRAWLBACK, "replay: recording to {}", out.path);
      },
      info_json.size() + setup_json.size());
  (void)system;
  return true;
}

bool Active()
{
  return s_rec.active;
}

void OnPass(s64 frame, const Slots& slots)
{
  if (!s_rec.active || frame < 0)
    return;
  RingFrame& r = s_rec.ring[static_cast<size_t>(frame) % RING];
  if (r.frame != frame)
  {
    r = {};
    r.frame = frame;
  }
  r.pads = slots;
  r.input = true;
  if (s_rec.next_emit < 0)
  {
    s_rec.next_emit = frame;
    s_rec.first_frame = frame;
  }
  s_rec.newest = std::max(s_rec.newest, frame);
}

void OnSave(Core::System& system, s64 frame, u32 checksum, const Parts& parts)
{
  if (!s_rec.active || frame < 0)
    return;
  RingFrame& r = s_rec.ring[static_cast<size_t>(frame) % RING];
  if (r.frame != frame)
    return;  // a save without a pass this recording saw (frame 0's initial save)
  r.checksum = checksum;
  r.parts = parts;
  r.parts.frame = static_cast<u32>(frame);
  r.saved = true;
  if (!s_rec.save)
    return;  // only streamed: no fighter memory
  const u32 block = FighterBlockBytes();
  r.fighters.resize(static_cast<size_t>(s_rec.num_ports) * block);
  int i = 0;
  for (int port = 0; port < PORTS; ++port)
  {
    if ((s_rec.ports >> port) & 1)
      ReadFighterMemory(system, port, r.fighters.data() + static_cast<size_t>(i++) * block);
  }
}

void Advance(s64 newest_frame)
{
  if (!s_rec.active || s_rec.next_emit < 0 || s_rec.overflowed)
    return;
  const s64 final_frame = newest_frame - FINAL_BEHIND;
  while (s_rec.next_emit <= final_frame && !s_rec.overflowed)
    EmitFrame(s_rec.next_emit++);
}

void AddEvent(s64 frame, picojson::object event)
{
  if (!s_rec.active || s_rec.events.size() >= 200)
    return;
  event["frame"] = picojson::value(static_cast<double>(std::max<s64>(frame, 0)));
  s_rec.events.emplace_back(event);
}

void End(const EndInfo& info)
{
  if (!s_rec.active)
    return;
  if (s_rec.newest < 0)
  {
    // It ended before its first frame ran (in the countdown): no replay.
    Abort();
    return;
  }
  // Every frame that ran: the last few may not have been confirmed (finalFrame says which were).
  const s64 final_frame = s_rec.next_emit - 1;
  while (s_rec.next_emit >= 0 && s_rec.next_emit <= s_rec.newest && !s_rec.overflowed)
    EmitFrame(s_rec.next_emit++);
  QueueChunks(true);
  const s64 last = s_rec.next_emit - 1;
  picojson::object o;
  o["endMethod"] = picojson::value(info.method);
  o["gameSetFrame"] = picojson::value(static_cast<double>(info.game_set_frame));
  o["firstFrame"] = picojson::value(static_cast<double>(s_rec.first_frame));
  o["lastFrame"] = picojson::value(static_cast<double>(last));
  o["finalFrame"] = picojson::value(static_cast<double>(std::max<s64>(final_frame, -1)));
  o["complete"] = picojson::value(!s_rec.overflowed);
  o["winnerPort"] = info.winner < PORTS ? picojson::value(static_cast<double>(info.winner + 1)) :
                    info.winner == 0xFE ? picojson::value("draw") :
                                          picojson::value();
  if (info.quitter < PORTS)
    o["quitterPort"] = picojson::value(static_cast<double>(info.quitter + 1));
  picojson::array players;
  for (int port = 0; port < PORTS; ++port)
  {
    if (!info.present[port])
      continue;
    picojson::object p;
    p["port"] = picojson::value(static_cast<double>(port + 1));
    p["stocks"] = picojson::value(static_cast<double>(info.stocks[port]));
    p["damage"] = picojson::value(static_cast<double>(info.damage[port]));
    if (info.place[port])
      p["place"] = picojson::value(static_cast<double>(info.place[port]));
    p["isWinner"] = picojson::value(info.winner == port);
    players.emplace_back(p);
  }
  o["players"] = picojson::value(players);
  o["rollbacks"] = picojson::value(static_cast<double>(info.rollbacks));
  o["maxRollback"] = picojson::value(static_cast<double>(info.max_rollback));
  o["desyncs"] = picojson::value(static_cast<double>(info.desyncs));
  if (info.perf.frames)
  {
    // Rounded: three digits are all anyone reads (and the ENDG stays short).
    const auto round = [](double v, double scale) { return std::round(v * scale) / scale; };
    picojson::object perf;
    perf["frames"] = picojson::value(static_cast<double>(info.perf.frames));
    perf["fps"] = picojson::value(round(info.perf.fps, 100));
    perf["ownLate"] = picojson::value(round(info.perf.own_late, 1000));
    perf["followed"] = picojson::value(round(info.perf.followed, 1000));
    if (info.perf.followed > 0)
      perf["peerFps"] = picojson::value(round(info.perf.peer_fps, 100));
    perf["slow"] = picojson::value(info.perf.slow);
    perf["peerSlow"] = picojson::value(info.perf.peer_slow);
    if (!info.perf.delay_profile.empty())
    {
      perf["delay"] = picojson::value(static_cast<double>(info.perf.delay));
      perf["delayProfile"] = picojson::value(info.perf.delay_profile);
      perf["delayAsked"] = picojson::value(info.perf.delay_asked);
      perf["delayFixed"] = picojson::value(info.perf.delay_fixed);
      perf["rttStart"] = picojson::value(round(info.perf.rtt_start, 10));
      perf["ping"] = picojson::value(round(info.perf.ping, 10));
      perf["jitter"] = picojson::value(round(info.perf.jitter, 10));
      perf["rollbacks"] = picojson::value(round(info.perf.rollbacks, 1000));
      perf["depth"] = picojson::value(round(info.perf.depth, 100));
      perf["rollbackMs"] = picojson::value(round(info.perf.rollback_ms, 100));
      perf["lateAfterRollback"] = picojson::value(round(info.perf.late_after_rollback, 1000));
      perf["hitches"] = picojson::value(round(info.perf.hitches, 1000));
      perf["waits"] = picojson::value(static_cast<double>(info.perf.waits));
      perf["waitMs"] = picojson::value(round(info.perf.wait_ms, 1));
      perf["resimMs"] = picojson::value(round(info.perf.resim_ms, 100));
    }
    o["perf"] = picojson::value(std::move(perf));
  }
  const std::string endg = picojson::value(o).serialize();
  const u32 last_frame = static_cast<u32>(std::max<s64>(last, 0));
  if (s_rec.out && !s_rec.overflowed)
  {
    Queue(
        [endg, last_frame](Output& out) {
          if (out.stream)
          {
            const auto raw = Bytes(endg);
            if (auto chunk = EncodeChunk(CHUNK_ENDG, 0, raw))
              Online::Spectate::AddChunk(out.uid, CHUNK_ENDG, std::move(*chunk), raw);
            Online::Spectate::EndGame(out.uid);
          }
          if (!out.save || out.failed)
            return;
          if (!out.writer.Finish(Bytes(endg), last_frame))
            return Fail(out, "cannot finish the replay");
          NOTICE_LOG_FMT(BRAWLBACK, "replay: saved {}", out.path);
        },
        endg.size());
  }
  s_rec.active = false;
  s_rec.out.reset();
}

void Abort()
{
  if (!s_rec.active)
    return;
  // The header may be on its way to the disk: the file is removed once the writer gets here.
  if (s_rec.out)
  {
    Queue(
        [](Output& out) {
          if (out.stream)
            Online::Spectate::AbortGame(out.uid);
          if (!out.save)
            return;
          out.writer.Close();
          ::File::Delete(out.path);
        },
        0);
  }
  s_rec = {};
}

std::string CurrentPath()
{
  std::lock_guard lk(s_path_mutex);
  return s_last_path;
}

void Flush()
{
  Writers().Flush();
}
}  // namespace Gprb::Replay
