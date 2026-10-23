// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GameplayRollback.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <picojson.h>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/OSThread.h"
#include "Core/Harness/Harness.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "InputCommon/GCAdapter.h"

namespace Gprb
{
// ---------------------------------------------------------------------------------------------
// Guest reads

bool IsMemAddr(u32 addr, u32 size)
{
  const u64 end = static_cast<u64>(addr) + size;
  if (addr >= 0x80000000u && end <= 0x81800000u)
    return true;
  if (addr >= 0x90000000u && end <= 0x94000000u)
    return true;
  return false;
}

const u8* Guest::Ptr(u32 addr, u32 size) const
{
  if (!IsMemAddr(addr, size))
    return nullptr;
  const std::span<u8> span = m_memory.GetSpanForAddress(addr);
  if (!span.data() || span.size() < size)
    return nullptr;
  return span.data();
}

std::optional<u32> Guest::U32(u32 addr) const
{
  const u8* p = Ptr(addr, 4);
  if (!p)
    return std::nullopt;
  u32 v;
  std::memcpy(&v, p, 4);
  return Common::swap32(v);
}

std::optional<u32> Guest::Ptr32(u32 addr) const
{
  const auto v = U32(addr);
  if (!v || !IsMemAddr(*v))
    return std::nullopt;
  return v;
}

std::optional<std::string> Guest::CStr(u32 addr, u32 max_len) const
{
  std::string out;
  for (u32 i = 0; i < max_len; ++i)
  {
    const u8* p = Ptr(addr + i, 1);
    if (!p)
      return std::nullopt;
    if (*p == 0)
      return out;
    out.push_back(static_cast<char>(*p));
  }
  return std::nullopt;
}

static std::optional<u32> CurrentScene(const Guest& g)
{
  const auto manager = g.Ptr32(Addr::SCENE_MANAGER_PTR);
  if (!manager)
    return std::nullopt;
  return g.Ptr32(*manager + 4);
}

std::string CurrentSceneName(const Guest& g)
{
  const auto scene = CurrentScene(g);
  if (!scene)
    return {};
  const auto name_ptr = g.Ptr32(*scene);
  if (!name_ptr)
    return {};
  return g.CStr(*name_ptr, 32).value_or("");
}

// Brawl's online waiting room (docs/game-code.md, "The waiting room"): the scMelee that
// sqNetAnyOkiraku's state 5 sets up on the Online Training stage, where the player plays alone
// while matchmaking searches. It is never an online match, so it is no match scene here: no
// rollback session starts in it, even once the opponent is connected. gmGlobalModeMelee+0x1A is
// the stage kind; state 5 writes it before the room loads, the match setup after the room.
// The stage is P+'s Training Room, which a match can be played on too (bug #35, 2026-10-16: a
// room's loser picked it, and no session ran in that game: each machine played on alone). The
// room is a one-player match (state 5's players 1-6 none); every online match has two players
// or more.
constexpr u16 STAGE_ONLINE_TRAINING = 0x37;
constexpr u32 MM_PLAYERS = 0x98, MM_PLAYER_SIZE = 0x5C, MM_PLAYER_STATE = 0x01;
constexpr u8 PLAYER_STATE_CPU = 1;  // 0 human, 1 CPU; above: no player

bool IsWaitingRoom(const Guest& g)
{
  const auto gg = g.Ptr32(Addr::GAME_GLOBAL_PTR);
  if (!gg)
    return false;
  const auto mm = g.Ptr32(*gg + Addr::GG_MODE_MELEE);
  if (!mm)
    return false;
  const u8* p = g.Ptr(*mm, MM_PLAYERS + 4 * MM_PLAYER_SIZE);
  if (!p || static_cast<u16>(p[0x1A] << 8 | p[0x1B]) != STAGE_ONLINE_TRAINING)
    return false;
  int players = 0;
  for (u32 i = 0; i < 4; ++i)
  {
    if (p[MM_PLAYERS + i * MM_PLAYER_SIZE + MM_PLAYER_STATE] <= PLAYER_STATE_CPU)
      ++players;
  }
  return players < 2;
}

bool IsSceneMelee(const Guest& g)
{
  return CurrentSceneName(g) == "scMelee" && !IsWaitingRoom(g);
}

bool IsNextSceneMelee(const Guest& g)
{
  // gfSceneManager: +0x4 the current scene, +0x8 the scene a pending change switches to.
  const auto manager = g.Ptr32(Addr::SCENE_MANAGER_PTR);
  if (!manager)
    return false;
  const auto next = g.Ptr32(*manager + 8);
  if (!next)
    return false;
  const auto name_ptr = g.Ptr32(*next);
  return name_ptr && g.CStr(*name_ptr, 32).value_or("") == "scMelee" && !IsWaitingRoom(g);
}

std::vector<HeapInfo> ReadHeapTable(const Guest& g)
{
  std::vector<HeapInfo> out;
  for (u32 i = 1; i < Addr::HEAP_INFO_COUNT; ++i)
  {
    const u32 entry = Addr::HEAP_INFOS + 16 * i;
    const auto name_p = g.U32(entry);
    const auto pool = g.U32(entry + 4);
    const auto size = g.U32(entry + 8);
    if (!name_p || !pool || !size || !IsMemAddr(*pool) || !IsMemAddr(*name_p) || *size == 0)
      continue;
    if (!IsMemAddr(*pool, *size))
      continue;
    const auto name = g.CStr(*name_p, 40);
    if (!name || name->empty())
      continue;
    if (!std::all_of(name->begin(), name->end(), [](char c) { return c >= 0x20 && c < 0x7f; }))
      continue;
    out.push_back({i, *name, *pool, *pool + *size});
  }
  return out;
}

std::vector<ModuleSection> ReadModuleSections(const Guest& g)
{
  std::vector<ModuleSection> out;
  std::vector<u32> seen;
  auto mod = g.Ptr32(Addr::OS_MODULE_LIST_HEAD);
  while (mod && seen.size() < 64 && std::find(seen.begin(), seen.end(), *mod) == seen.end())
  {
    seen.push_back(*mod);
    const u32 m = *mod;
    const auto id = g.U32(m);
    const auto next = g.U32(m + 4);
    const auto num_sections = g.U32(m + 0xC);
    const auto section_info = g.U32(m + 0x10);
    const u8* bss_sec_p = g.Ptr(m + 0x33, 1);
    if (!id || !num_sections || !section_info || !bss_sec_p || *num_sections > 64)
      break;
    for (u32 i = 0; i < *num_sections; ++i)
    {
      const auto off = g.U32(*section_info + 8 * i);
      const auto size = g.U32(*section_info + 8 * i + 4);
      if (!off || !size || *size == 0)
        continue;
      const u32 addr = *off & ~1u;
      if (!IsMemAddr(addr, *size))
        continue;
      out.push_back({*id, i, addr, *size, (*off & 1) != 0, i == *bss_sec_p});
    }
    if (!next || *next == 0)
      break;
    mod = IsMemAddr(*next) ? std::optional<u32>(*next) : std::nullopt;
  }
  return out;
}

FighterFields ReadFighterFields(const Guest& g)
{
  // ftEntryManager* at 0x80624780 -> ftEntry entries[4], stride 0x244: +0x0A u8 active instance,
  // +0x28 ftOwner*, +0x30 + 8*k {u32 kind, Fighter*}. ftOwner -> +0 data -> +0x24 damage, +0x34
  // stocks. Fighter -> +0x60 accesser -> +0xD8 enumeration -> +0x0C posture (+0x0C x, +0x10 y),
  // +0x70 status (+0x34 kind). See Rollback::CalculateDesyncChecksums.
  // The entries are not indexed by port: a match has 9, the used ones first in port order, with
  // none for an empty port (P1 + P3 + P4: entries 0, 1, 2), so each entry's player number (+0x58)
  // says whose it is (docs/nplayer/setup.md).
  //
  // Entries a match does not use keep what an earlier match left in them: after a 4-player match,
  // a 2-player match's entries 2 and 3 still carry player numbers 2 and 3 and stale owner and
  // fighter pointers, on that machine only (another machine that never played 4 players has none
  // there). Read as the match's fighters, they made the confirmed checksum differ from session
  // frame 0 between a machine that had played offline 4-player Versus and one that had not
  // (staging, 2026-10-10: "desync at frame 0"). So only the match's own entries count: the first
  // n, n = the players in the match's setup (gmGlobalModeMelee, state human or CPU), each for a
  // port the setup has, the first entry for a port winning, and never a freed one (entry id -1:
  // the 2-player match's entries 2 and 3 above; docs/gameplay-rollback-status.md, Phase 15).
  FighterFields out{};
  for (auto& f : out)
    f[0] = 0xff;
  const auto entries = g.Ptr32(0x80624780u);
  if (!entries)
    return out;
  u32 ports = 0;
  if (const auto gg = g.Ptr32(Addr::GAME_GLOBAL_PTR))
  {
    if (const auto mm = g.Ptr32(*gg + Addr::GG_MODE_MELEE))
    {
      if (const u8* p = g.Ptr(*mm, Addr::MODE_MELEE_SIZE))
      {
        for (u32 port = 0; port < 4; ++port)
        {
          const u8 state = p[0x98 + port * 0x5C + 0x01];  // gmPlayerInitData: 0 human, 1 CPU, 3 none
          if (state == 0 || state == 1)
            ports |= 1u << port;
        }
      }
    }
  }
  const u32 players = static_cast<u32>(std::popcount(ports));
  const u32 count = std::min<u32>({g.U32(0x80624784u).value_or(0), 9u, players});
  u32 seen = 0;
  for (u32 k = 0; k < count; ++k)
  {
    const u32 entry = *entries + k * 0x244u;
    // ftEntry::m_entryId (+0x04) is -1 once an entry is freed; its player number stays.
    const auto entry_id = g.U32(entry + 0x04);
    const auto player = g.U32(entry + 0x58);
    if (!entry_id || *entry_id == 0xFFFFFFFFu || !player || *player >= 4 ||
        !(ports & (1u << *player)) || (seen & (1u << *player)))
    {
      continue;
    }
    const u32 port = *player;
    seen |= 1u << port;
    auto& f = out[port];
    const u8* inst = g.Ptr(entry + 0x0a, 1);
    f[0] = inst ? *inst : 0xff;
    if (const auto owner = g.Ptr32(entry + 0x28))
    {
      if (const auto data = g.Ptr32(*owner))
      {
        f[1] = g.U32(*data + 0x24).value_or(0);
        f[2] = g.U32(*data + 0x34).value_or(0);
      }
    }
    if (f[0] < 4)
    {
      if (const auto fighter = g.Ptr32(entry + 0x34 + 8 * f[0]))
      {
        if (const auto acc = g.Ptr32(*fighter + 0x60))
        {
          if (const auto en = g.Ptr32(*acc + 0xd8))
          {
            if (const auto posture = g.Ptr32(*en + 0x0c))
            {
              f[3] = g.U32(*posture + 0x0c).value_or(0);
              f[4] = g.U32(*posture + 0x10).value_or(0);
            }
            if (const auto status = g.Ptr32(*en + 0x70))
              f[5] = g.U32(*status + 0x34).value_or(0);
          }
        }
      }
    }
  }
  return out;
}

u32 FightersCrc(const FighterFields& f)
{
  // Same byte order as Rollback::FighterChecksum (host-order u32s).
  u32 crc = Common::StartCRC32();
  for (const auto& port : f)
    crc = Common::UpdateCRC32(crc, reinterpret_cast<const u8*>(port.data()), sizeof(port));
  return crc;
}

bool IsGameSet(const Guest& g)
{
  const auto scene = CurrentScene(g);
  if (!scene || !IsSceneMelee(g))
    return false;
  const auto rule = g.Ptr32(*scene + Addr::SCM_OPERATOR_RULE_MELEE);
  if (!rule)
    return false;
  const u8* p = g.Ptr(*rule + Addr::OPR_IS_GAME_SET, 1);
  return p && *p != 0;
}

u32 RemainingFrames(const Guest& g)
{
  const auto scene = CurrentScene(g);
  if (!scene || !IsSceneMelee(g))
    return 0;
  const auto gg = g.Ptr32(Addr::GAME_GLOBAL_PTR);
  const auto mm = gg ? g.Ptr32(*gg + Addr::GG_MODE_MELEE) : std::nullopt;
  const auto limit = mm ? g.U32(*mm + Addr::MM_TIME_LIMIT) : std::nullopt;
  if (!limit || static_cast<s32>(*limit) <= 0)
    return 0;
  const auto rule = g.Ptr32(*scene + Addr::SCM_OPERATOR_RULE_MELEE);
  const u32 left = rule ? g.U32(*rule + Addr::OPR_REMAINING_FRAMES).value_or(0) : 0;
  // A timer that ran out reads 1, so that 0 always means "no time limit".
  return std::max<u32>(left, 1);
}

bool IsQuitRequested(const Guest& g)
{
  const auto scene = CurrentScene(g);
  if (!scene || !IsSceneMelee(g))
    return false;
  const auto info = g.Ptr32(*scene + Addr::SCM_OPERATOR_INFO);
  if (!info)
    return false;
  const auto name = g.Ptr32(*info);
  if (!name || g.CStr(*name, 32).value_or("") != "StOperatorInfoMelee")
    return false;
  const u8* p = g.Ptr(*info + Addr::OPI_FLAGS, 1);
  return p && (*p & Addr::OPI_QUIT) != 0;
}

// ---------------------------------------------------------------------------------------------
// Region sets

static std::optional<u32> JsonU32(const picojson::value& v)
{
  if (v.is<double>())
    return static_cast<u32>(v.get<double>());
  if (v.is<std::string>())
  {
    u32 out = 0;
    if (TryParse(v.get<std::string>(), &out))
      return out;
  }
  return std::nullopt;
}

std::optional<std::string> ParseRegionSet(const std::string& json, RegionSetSpec* out)
{
  picojson::value root;
  const std::string err = picojson::parse(root, json);
  if (!err.empty())
    return "region set JSON: " + err;
  if (!root.is<picojson::object>())
    return "region set JSON must be an object";
  const auto& obj = root.get<picojson::object>();
  RegionSetSpec spec;
  auto get = [&](const char* key) -> const picojson::value* {
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
  };
  if (const auto* v = get("name"); v && v->is<std::string>())
    spec.name = v->get<std::string>();
  if (const auto* v = get("heaps"); v && v->is<picojson::array>())
  {
    for (const auto& h : v->get<picojson::array>())
      if (h.is<std::string>())
        spec.heaps.push_back(h.get<std::string>());
  }
  if (const auto* v = get("remove_heaps"); v && v->is<picojson::array>())
  {
    for (const auto& h : v->get<picojson::array>())
      if (h.is<std::string>())
        spec.remove_heaps.push_back(h.get<std::string>());
  }
  if (const auto* v = get("rel_data"); v && v->is<bool>())
    spec.rel_data = v->get<bool>();
  // "add": [[addr, size, label], ...] or [{"addr":..,"size":..,"label":..}, ...]
  if (const auto* v = get("add"); v && v->is<picojson::array>())
  {
    for (const auto& e : v->get<picojson::array>())
    {
      Range r{};
      if (e.is<picojson::array>() && e.get<picojson::array>().size() >= 2)
      {
        const auto& a = e.get<picojson::array>();
        const auto addr = JsonU32(a[0]);
        const auto size = JsonU32(a[1]);
        if (!addr || !size)
          return "bad entry in 'add'";
        r = {*addr, *size, a.size() > 2 && a[2].is<std::string>() ? a[2].get<std::string>() : ""};
      }
      else if (e.is<picojson::object>())
      {
        const auto& o = e.get<picojson::object>();
        const auto ia = o.find("addr");
        const auto is = o.find("size");
        if (ia == o.end() || is == o.end())
          return "entry in 'add' needs addr and size";
        const auto addr = JsonU32(ia->second);
        const auto size = JsonU32(is->second);
        if (!addr || !size)
          return "bad entry in 'add'";
        const auto il = o.find("label");
        r = {*addr, *size,
             il != o.end() && il->second.is<std::string>() ? il->second.get<std::string>() : ""};
      }
      else
      {
        return "bad entry in 'add'";
      }
      if (!IsMemAddr(r.addr, r.size))
        return fmt::format("range {:#x}+{:#x} is not in MEM1/MEM2", r.addr, r.size);
      spec.add.push_back(std::move(r));
    }
  }
  if (const auto* v = get("remove"); v && v->is<picojson::array>())
  {
    for (const auto& e : v->get<picojson::array>())
    {
      if (!e.is<picojson::array>() || e.get<picojson::array>().size() < 2)
        return "bad entry in 'remove'";
      const auto& a = e.get<picojson::array>();
      const auto addr = JsonU32(a[0]);
      const auto size = JsonU32(a[1]);
      if (!addr || !size)
        return "bad entry in 'remove'";
      spec.remove.emplace_back(*addr, *size);
    }
  }
  // "exclude": [[addr, size, label], ...]: bytes a load never writes, even where they share a
  // 64-byte granule with the set (RollbackManager::BeginRegionMode).
  if (const auto* v = get("exclude"); v && v->is<picojson::array>())
  {
    for (const auto& e : v->get<picojson::array>())
    {
      if (!e.is<picojson::array>() || e.get<picojson::array>().size() < 2)
        return "bad entry in 'exclude'";
      const auto& a = e.get<picojson::array>();
      const auto addr = JsonU32(a[0]);
      const auto size = JsonU32(a[1]);
      if (!addr || !size)
        return "bad entry in 'exclude'";
      spec.exclude.push_back(
          {*addr, *size, a.size() > 2 && a[2].is<std::string>() ? a[2].get<std::string>() : ""});
    }
  }
  *out = std::move(spec);
  return std::nullopt;
}

std::optional<std::string> LoadRegionSet(const std::string& name_or_path, RegionSetSpec* out)
{
  std::string path = name_or_path;
  if (path.find('/') == std::string::npos && path.find('\\') == std::string::npos)
  {
    path = File::GetSysDirectory() + "Rollback/" + name_or_path;
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json")
      path += ".json";
  }
  std::string json;
  if (!File::ReadFileToString(path, json))
    return "cannot read region set " + path;
  auto err = ParseRegionSet(json, out);
  if (!err && out->name.empty())
    out->name = name_or_path;
  return err;
}

static std::vector<Range> SubtractHoles(std::vector<Range> in,
                                        const std::vector<std::pair<u32, u32>>& holes)
{
  for (const auto& [ha, hn] : holes)
  {
    const u64 he = static_cast<u64>(ha) + hn;
    std::vector<Range> next;
    for (const Range& r : in)
    {
      const u64 re = static_cast<u64>(r.addr) + r.size;
      if (he <= r.addr || ha >= re)
      {
        next.push_back(r);
        continue;
      }
      if (ha > r.addr)
        next.push_back({r.addr, ha - r.addr, r.label});
      if (he < re)
        next.push_back({static_cast<u32>(he), static_cast<u32>(re - he), r.label});
    }
    in = std::move(next);
  }
  return in;
}

std::vector<Range> ResolveRegionSet(const Guest& g, const RegionSetSpec& spec)
{
  std::vector<Range> rs;
  const auto heaps = ReadHeapTable(g);
  for (const auto& h : heaps)
  {
    if (std::find(spec.heaps.begin(), spec.heaps.end(), h.name) != spec.heaps.end())
      rs.push_back({h.start, h.end - h.start, h.name});
  }
  if (spec.rel_data)
  {
    for (const auto& s : ReadModuleSections(g))
    {
      if (!s.exec)
        rs.push_back({s.addr, s.size, fmt::format("rel{}.{}", s.module_id, s.bss ? "bss" : "data")});
    }
  }
  for (const auto& r : spec.add)
    rs.push_back(r);

  std::vector<std::pair<u32, u32>> holes = spec.remove;
  for (const auto& h : heaps)
  {
    if (std::find(spec.remove_heaps.begin(), spec.remove_heaps.end(), h.name) !=
        spec.remove_heaps.end())
    {
      holes.emplace_back(h.start, h.end - h.start);
    }
  }
  rs = SubtractHoles(std::move(rs), holes);

  std::sort(rs.begin(), rs.end(), [](const Range& a, const Range& b) { return a.addr < b.addr; });
  std::vector<Range> merged;
  for (const Range& r : rs)
  {
    if (r.size == 0)
      continue;
    if (!merged.empty() &&
        static_cast<u64>(merged.back().addr) + merged.back().size >= r.addr)
    {
      const u64 end = std::max<u64>(static_cast<u64>(merged.back().addr) + merged.back().size,
                                    static_cast<u64>(r.addr) + r.size);
      merged.back().size = static_cast<u32>(end - merged.back().addr);
      continue;
    }
    merged.push_back(r);
  }
  return merged;
}

// ---------------------------------------------------------------------------------------------
// Frame trace

namespace
{
constexpr size_t TRACE_CAPACITY = 1 << 16;
std::mutex s_trace_mutex;
std::vector<FrameTraceRow> s_trace(TRACE_CAPACITY);
std::atomic<bool> s_trace_forced{false};
std::atomic<bool> s_trace_disabled{false};
std::vector<std::pair<u32, u32>> s_trace_ranges;
u32 s_trace_epoch = 0;
u32 s_trace_last_frame = 0;
bool s_trace_last_valid = false;
}  // namespace

void FrameTraceConfigure(bool enabled, std::vector<std::pair<u32, u32>> ranges)
{
  std::lock_guard lk(s_trace_mutex);
  s_trace_forced = enabled;
  s_trace_disabled = !enabled;
  s_trace_ranges = std::move(ranges);
}

bool FrameTraceEnabled()
{
  if (s_trace_disabled.load(std::memory_order_relaxed))
    return false;
  static const bool env = std::getenv("PPR_FRAME_TRACE") != nullptr;
  return env || s_trace_forced.load(std::memory_order_relaxed) || Harness::IsActive();
}

u32 FrameTraceEpoch()
{
  std::lock_guard lk(s_trace_mutex);
  return s_trace_epoch;
}

void FrameTraceOnFrameEnd(Core::System& system, u32 logic_steps, bool resim)
{
  if (!FrameTraceEnabled())
    return;
  auto& memory = system.GetMemory();
  const Guest g(memory);
  if (!IsSceneMelee(g))
  {
    s_trace_last_valid = false;
    return;
  }
  FrameTraceRow row;
  row.game_frame = g.U32(Addr::GAME_FRAME + 4).value_or(0);
  row.persistent = g.U32(Addr::GAME_FRAME + 0x14).value_or(0);
  row.logic_steps = logic_steps;
  row.ticks = system.GetCoreTiming().GetTicks();
  row.rng = {g.U32(Addr::MTRAND_DEFAULT_SEED).value_or(0), g.U32(Addr::MTRAND_OTHER_SEED).value_or(0),
             g.U32(Addr::LIBC_RAND_NEXT).value_or(0)};
  row.fighters = ReadFighterFields(g);
  row.fighters_crc = FightersCrc(row.fighters);
  row.game_set = IsGameSet(g) ? 1 : 0;
  row.resim = resim;

  std::lock_guard lk(s_trace_mutex);
  if (!s_trace_ranges.empty())
  {
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    for (const auto& [a, n] : s_trace_ranges)
    {
      if (const u8* p = g.Ptr(a, n))
        XXH3_64bits_update(st, p, n);
    }
    row.range_hash = XXH3_64bits_digest(st);
    XXH3_freeState(st);
  }
  // A new match simulation: the frame counter restarted (outside a rollback, where it legitimately
  // goes back a few frames).
  if (!s_trace_last_valid || (row.game_frame + 64 < s_trace_last_frame) ||
      (row.game_frame <= 1 && s_trace_last_frame > 1 && !resim))
  {
    if (!s_trace_last_valid || row.game_frame < s_trace_last_frame)
      ++s_trace_epoch;
  }
  row.epoch = s_trace_epoch;
  s_trace_last_frame = row.game_frame;
  s_trace_last_valid = true;
  s_trace[row.game_frame % TRACE_CAPACITY] = row;
}

std::vector<FrameTraceRow> FrameTraceRows(std::optional<u32> epoch, u32 since, u32 max_rows)
{
  std::lock_guard lk(s_trace_mutex);
  const u32 want = epoch.value_or(s_trace_epoch);
  std::vector<FrameTraceRow> out;
  for (const auto& r : s_trace)
  {
    if (r.epoch == want && r.game_frame >= since && r.epoch != 0)
      out.push_back(r);
  }
  std::sort(out.begin(), out.end(),
            [](const FrameTraceRow& a, const FrameTraceRow& b) { return a.game_frame < b.game_frame; });
  if (max_rows && out.size() > max_rows)
    out.resize(max_rows);
  return out;
}

}  // namespace Gprb

namespace Gprb
{
// ---------------------------------------------------------------------------------------------
// Host controller reads on the throttle's schedule

namespace
{
// A re-run takes the last read before it only while that is this recent: after a wait for the
// peer (seconds, emulated time standing still) it is read anew.
constexpr auto HOST_PAD_LATCH_MAX_AGE = std::chrono::milliseconds(100);
// How far behind schedule a read may come and still count as on time (the throttle's wake-up).
constexpr auto HOST_PAD_LATE = std::chrono::milliseconds(1);
// Further behind than this (a field), a read takes the controller as it is now; it still stands
// for its due instant. A PC that cannot keep full speed runs up to MaxFallback (100 ms and more)
// behind its schedule on every frame, and the history gave its player the controller of that long
// ago (156 ms from read to use at delay 2, against 52 ms read now). The few ms a frame comes late
// after a rollback stay within it, so bug #70's regular samples are unchanged.
// PPR_GPRB_PAD_FAR_LATE_MS sets it (0: never).
std::chrono::microseconds HostPadFarLate()
{
  static const std::chrono::microseconds value = [] {
    const char* env = std::getenv("PPR_GPRB_PAD_FAR_LATE_MS");
    if (env && *env)
    {
      const double ms = std::atof(env);
      // (not microseconds::max(): compared with a steady_clock duration it overflows)
      return ms <= 0 ? std::chrono::microseconds(std::chrono::hours(24)) :
                       std::chrono::microseconds(static_cast<s64>(ms * 1000));
    }
    return std::chrono::microseconds(16683);
  }();
  return value;
}

struct HostPadRead
{
  bool valid = false;
  GCPadStatus status{};
  Clock::time_point instant{};  // the time it stands for
  Clock::time_point data_at{};  // when that state was the controller's (instant, or later if far late)
};

struct HostPads
{
  std::atomic<bool> scheduled{false};
  std::atomic<u32> generation{0};  // SetHostPadsScheduled(true) calls
  // CPU thread only:
  u32 seen_generation = 0;
  std::array<HostPadRead, 4> last{};
  std::array<Clock::time_point, 4> instant{};  // each channel's last read (PadsLatestSampleAgeMs)
  HostPadReadStats stats{};
};
HostPads s_host_pads;

// Diagnostics (PPR_GPRB_PAD_READ_LOG=<file>, %p in it a number of the process's own): one line per read of a host controller in a session
// and per pad-thread pass, times in us of the steady clock:
//   R chan now due|- resim kind buttons instant
//   P now resim instant0 slot0-buttons
std::FILE* PadReadLog()
{
  static std::FILE* const f = [] {
    const char* env = std::getenv("PPR_GPRB_PAD_READ_LOG");
    if (!env || !*env)
      return static_cast<std::FILE*>(nullptr);
    std::string path = env;  // %p: a number of this process's own (the time of the first line)
    if (const size_t at = path.find("%p"); at != std::string::npos)
      path.replace(at, 2, std::to_string(Clock::now().time_since_epoch().count()));
    return std::fopen(path.c_str(), "w");
  }();
  return f;
}
long long Us(Clock::time_point t)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}
void LogRead(int chan, Clock::time_point now, std::optional<Clock::time_point> due, bool resim,
             const char* kind, const GCPadStatus& st, Clock::time_point instant)
{
  if (std::FILE* f = PadReadLog())
  {
    std::fprintf(f, "R %d %lld %lld %d %s %04x %lld\n", chan, Us(now), due ? Us(*due) : -1LL,
                 resim ? 1 : 0, kind, st.button, Us(instant));
  }
}

std::array<Clock::time_point, 4> HostPadReadInstants()
{
  return s_host_pads.instant;
}

GCPadStatus ReadHostPadNow(int chan, HostPadSource source)
{
  if (source == HostPadSource::Adapter)
  {
    GCPadStatus status = GCAdapter::Input(chan);
    Harness::ApplyPadOverride(chan, &status);
    return status;
  }
  return Pad::GetStatus(chan);
}

// The state at `t` (a recent past), if something kept it.
std::optional<GCPadStatus> ReadHostPadAt(int chan, HostPadSource source, Clock::time_point t)
{
  if (auto overridden = Harness::PadOverrideAt(chan, t))
    return overridden;
  if (source == HostPadSource::Adapter)
    return GCAdapter::InputAt(chan, t);
  return std::nullopt;
}
}  // namespace

void SetHostPadsScheduled(bool scheduled)
{
  if (scheduled)
    s_host_pads.generation.fetch_add(1, std::memory_order_relaxed);
  s_host_pads.scheduled.store(scheduled, std::memory_order_relaxed);
}

GCPadStatus ReadHostPad(Core::System& system, int chan, HostPadSource source)
{
  HostPads& h = s_host_pads;
  const Clock::time_point now = Clock::now();
  if (chan < 0 || chan >= 4 || !h.scheduled.load(std::memory_order_relaxed))
  {
    if (chan >= 0 && chan < 4)
      h.instant[chan] = now;
    return ReadHostPadNow(chan, source);
  }
  if (const u32 gen = h.generation.load(std::memory_order_relaxed); gen != h.seen_generation)
  {
    h.seen_generation = gen;
    h.last = {};  // nothing from an earlier session
  }

  auto& ct = system.GetCoreTiming();
  HostPadRead& last = h.last[chan];
  if (ct.IsRollbackResimulating())
  {
    if (last.valid && now - last.data_at <= HOST_PAD_LATCH_MAX_AGE)
    {
      ++h.stats.latched;
      h.instant[chan] = last.instant;
      LogRead(chan, now, std::nullopt, true, "latch", last.status, last.instant);
      return last.status;
    }
    // The first read of this re-run stands for it from here on.
    ++h.stats.relatched;
    last = {true, ReadHostPadNow(chan, source), now, now};
    h.instant[chan] = now;
    LogRead(chan, now, std::nullopt, true, "relatch", last.status, now);
    return last.status;
  }

  GCPadStatus status{};
  Clock::time_point instant = now;
  Clock::time_point data_at = now;
  const char* kind = "now";
  const std::optional<Clock::time_point> due = ct.ScheduledHostTime();
  if (due && *due < now)
  {
    const auto behind = now - *due;
    if (behind > HostPadFarLate())
    {
      // Far behind: the controller now, standing for its due instant.
      status = ReadHostPadNow(chan, source);
      instant = *due;
      kind = "far";
      ++h.stats.late_far;
    }
    else if (const std::optional<GCPadStatus> then = ReadHostPadAt(chan, source, *due))
    {
      status = *then;
      instant = data_at = *due;
      kind = "hist";
      if (behind > HOST_PAD_LATE)
        ++h.stats.late_history;
      else
        ++h.stats.on_time;
    }
    else
    {
      status = ReadHostPadNow(chan, source);
      kind = "live";
      if (behind > HOST_PAD_LATE)
        ++h.stats.late_live;
      else
        ++h.stats.on_time;
    }
    if (behind > HOST_PAD_LATE)
    {
      h.stats.late_max_ms = std::max(
          h.stats.late_max_ms, std::chrono::duration<double, std::milli>(behind).count());
    }
  }
  else
  {
    status = ReadHostPadNow(chan, source);
    ++h.stats.on_time;
  }
  last = {true, status, instant, data_at};
  h.instant[chan] = instant;
  LogRead(chan, now, due, false, kind, status, instant);
  return status;
}

void HostPadLogLine(const char* kind, long long a, long long b, long long c)
{
  if (std::FILE* f = PadReadLog())
    std::fprintf(f, "%s %lld %lld %lld %lld\n", kind, Us(Clock::now()), a, b, c);
}

HostPadReadStats TakeHostPadReadStats()
{
  return std::exchange(s_host_pads.stats, HostPadReadStats{});
}

// ---------------------------------------------------------------------------------------------
// Game-frame-anchored pads

namespace
{
constexpr size_t PADS_CAPACITY = 1 << 16;
struct PadsState
{
  std::mutex mutex;
  bool recording = false;
  std::vector<std::pair<u32, PadSlots>> recorded = std::vector<std::pair<u32, PadSlots>>(PADS_CAPACITY);
  std::vector<std::pair<u32, PadSlots>> inject = std::vector<std::pair<u32, PadSlots>>(PADS_CAPACITY);
  std::vector<bool> inject_valid = std::vector<bool>(PADS_CAPACITY, false);
  u32 inject_ports = 0;
  u32 injected = 0;
  // Slots in force for the frame the main thread is producing (written at the loop top, re-applied
  // after every pad-thread write).
  bool current_valid = false;
  u32 current_ports = 0;
  PadSlots current{};
  // Session mode (PadsSetCurrent): applied regardless of the frame number.
  bool session = false;
  u32 anchor_ports = 0;
  bool have_latest = false;
  RawPads latest{};
  std::chrono::steady_clock::time_point latest_at{};
  // The last passes' pads and the instants the reads behind them stand for (ReadHostPad for the
  // GameCube ports; the pass itself for the Wii Remotes).
  struct Pass
  {
    RawPads pads{};
    std::array<Clock::time_point, 4> sampled_at{};
    Clock::time_point at{};
  };
  std::array<Pass, 4> recent{};
  size_t recent_next = 0, recent_n = 0;
  u64 latest_ticks = 0;  // emulated time of that pass (the watchdog's clock)
  u32 latest_app_frame = 0;  // gfApplication's frame counter then (the watchdog's report)
  u64 passes = 0;
};
PadsState s_pads;

// A pad with no controller, as the pad thread leaves an empty GameCube port or a Wii Remote that is
// not connected (error 0xFF, the port's own type).
std::array<u8, Addr::PAD_STRIDE> EmptyPad(bool wii)
{
  std::array<u8, Addr::PAD_STRIDE> pad{};
  pad[Addr::PAD_ERROR_OFF] = 0xFF;
  pad[Addr::PAD_TYPE_OFF] = wii ? 2 : 0;
  return pad;
}

// Each port's record into its GameCube or Wii Remote pad (PadSlots), the other one empty.
void WriteSlots(Memory::MemoryManager& memory, u32 port_mask, const PadSlots& slots)
{
  static const auto empty_gc = EmptyPad(false);
  static const auto empty_wii = EmptyPad(true);
  for (u32 port = 0; port < 4; ++port)
  {
    if (!(port_mask & (1u << port)))
      continue;
    const u8* rec = slots.data() + port * Addr::PAD_STRIDE;
    const bool wii = IsWiiPad(rec);
    const u32 gc_addr = Addr::PAD_STATUS + port * Addr::PAD_STRIDE;
    const u32 wii_addr = Addr::PAD_STATUS + (Addr::WII_PADS + port) * Addr::PAD_STRIDE;
    memory.CopyToEmu(wii ? wii_addr : gc_addr, rec, Addr::PAD_STRIDE);
    memory.CopyToEmu(wii ? gc_addr : wii_addr, wii ? empty_gc.data() : empty_wii.data(),
                     Addr::PAD_STRIDE);
  }
}

// The players' records from the pads in memory: each port's GameCube pad, or its Wii Remote pad
// when that one has a controller and the GameCube pad has none (what WriteSlots wrote).
PadSlots SlotsFromPads(const u8* pads)
{
  PadSlots slots;
  for (u32 port = 0; port < 4; ++port)
  {
    const u8* gc = pads + port * Addr::PAD_STRIDE;
    const u8* wii = pads + (Addr::WII_PADS + port) * Addr::PAD_STRIDE;
    const bool use_wii = gc[Addr::PAD_ERROR_OFF] != 0 && wii[Addr::PAD_ERROR_OFF] == 0;
    std::memcpy(slots.data() + port * Addr::PAD_STRIDE, use_wii ? wii : gc, Addr::PAD_STRIDE);
  }
  return slots;
}
}  // namespace

bool IsWiiPad(const u8* pad)
{
  const u8 type = pad[Addr::PAD_TYPE_OFF];
  return pad[Addr::PAD_TYPE_OFF - 3] == 0 && pad[Addr::PAD_TYPE_OFF - 2] == 0 &&
         pad[Addr::PAD_TYPE_OFF - 1] == 0 && type >= 1 && type <= 3;
}

void PadsOnLoopTop(Core::System& system)
{
  auto& memory = system.GetMemory();
  const Guest g(memory);
  const u32 frame = g.U32(Addr::GAME_FRAME + 4).value_or(0) + 1;
  std::lock_guard lk(s_pads.mutex);
  if (!s_pads.session)
  {
    const size_t idx = frame % PADS_CAPACITY;
    if (s_pads.inject_ports && s_pads.inject_valid[idx] && s_pads.inject[idx].first == frame)
    {
      s_pads.current = s_pads.inject[idx].second;
      s_pads.current_ports = s_pads.inject_ports;
      s_pads.current_valid = true;
      ++s_pads.injected;
    }
    else if (s_pads.anchor_ports)
    {
      if (!s_pads.have_latest)
      {
        memory.CopyFromEmu(s_pads.latest.data(), Addr::PAD_STATUS, s_pads.latest.size());
        s_pads.have_latest = true;
      }
      // The GameCube ports (the harness's anchor).
      std::memcpy(s_pads.current.data(), s_pads.latest.data(), s_pads.current.size());
      s_pads.current_ports = s_pads.anchor_ports;
      s_pads.current_valid = true;
    }
    else
    {
      s_pads.current_valid = false;
    }
  }
  if (s_pads.current_valid)
  {
    WriteSlots(memory, s_pads.current_ports, s_pads.current);
    // gfPadSystem::updateGame fills the game pads from a queue of the pad thread's raw samples
    // while bit 0x08 of gfPadSystem+0x34 is set (as in a match), not from the slots. The queue is
    // filled asynchronously, so which sample a frame pops depends on timing too. With the bit
    // clear it copies the game pads from the slots, i.e. from this frame's anchored input.
    if (const auto pad_system = g.Ptr32(Addr::PAD_SYSTEM_PTR))
    {
      if (u8* flags = memory.GetPointerForRange(*pad_system + 0x34, 1); flags && (*flags & 0x08))
      {
        const u8 cleared = *flags & ~0x08;
        memory.CopyToEmu(*pad_system + 0x34, &cleared, 1);
      }
    }
  }
  if (s_pads.recording && IsSceneMelee(g))
  {
    RawPads pads;
    memory.CopyFromEmu(pads.data(), Addr::PAD_STATUS, pads.size());
    s_pads.recorded[frame % PADS_CAPACITY] = {frame, SlotsFromPads(pads.data())};
  }
}

void PadsOnPadThreadUpdated(Core::System& system)
{
  auto& memory = system.GetMemory();
  std::lock_guard lk(s_pads.mutex);
  memory.CopyFromEmu(s_pads.latest.data(), Addr::PAD_STATUS, s_pads.latest.size());
  s_pads.have_latest = true;
  s_pads.latest_at = std::chrono::steady_clock::now();
  {
    auto& pass = s_pads.recent[s_pads.recent_next];
    pass.pads = s_pads.latest;
    pass.sampled_at = HostPadReadInstants();
    pass.at = s_pads.latest_at;
    s_pads.recent_next = (s_pads.recent_next + 1) % s_pads.recent.size();
    s_pads.recent_n = std::min(s_pads.recent_n + 1, s_pads.recent.size());
    if (std::FILE* f = PadReadLog())
    {
      std::fprintf(f, "P %lld %d %lld %02x%02x%02x%02x\n", Us(s_pads.latest_at),
                   system.GetCoreTiming().IsRollbackResimulating() ? 1 : 0, Us(pass.sampled_at[0]),
                   pass.pads[0], pass.pads[1], pass.pads[2], pass.pads[3]);
      std::fflush(f);
    }
  }
  s_pads.latest_ticks = system.GetCoreTiming().GetTicks();
  ++s_pads.passes;
  {
    const Guest g(memory);
    if (const auto app = g.Ptr32(Addr::APPLICATION_PTR))
      s_pads.latest_app_frame = g.U32(*app + Addr::APP_FRAME_COUNTER_OFF).value_or(0);
  }
  if (s_pads.current_valid)
    WriteSlots(memory, s_pads.current_ports, s_pads.current);
}

RawPads PadsLatestRaw()
{
  std::lock_guard lk(s_pads.mutex);
  return s_pads.latest;
}

RawPads PadsSampledBefore(int chan, Clock::time_point t, Clock::time_point* sampled_at)
{
  std::lock_guard lk(s_pads.mutex);
  *sampled_at = {};
  if (chan < 0 || chan >= 8 || s_pads.recent_n == 0)
    return s_pads.latest;
  const auto instant = [chan](const PadsState::Pass& pass) {
    return chan < 4 ? pass.sampled_at[chan] : pass.at;
  };
  const size_t size = s_pads.recent.size();
  for (size_t i = 0; i < s_pads.recent_n; ++i)
  {
    const auto& pass = s_pads.recent[(s_pads.recent_next + size - 1 - i) % size];
    if (instant(pass) != Clock::time_point{} && instant(pass) <= t)
    {
      *sampled_at = instant(pass);
      return pass.pads;
    }
  }
  const auto& newest = s_pads.recent[(s_pads.recent_next + size - 1) % size];
  *sampled_at = instant(newest);
  return newest.pads;
}

namespace
{
struct PadWatch
{
  bool stalled = false;
  u32 reports = 0;
};
PadWatch s_pad_watch;
constexpr u32 PAD_WATCH_MAX_REPORTS = 8;  // a run

// The return addresses of the guest stack at `sp` (back chain), oldest call last.
std::string GuestBacktrace(const Core::CPUThreadGuard& guard, u32 sp, int depth)
{
  std::string out;
  for (int i = 0; i < depth && PowerPC::MMU::HostIsRAMAddress(guard, sp); ++i)
  {
    const u32 next = PowerPC::MMU::HostRead<u32>(guard, sp);
    if (next <= sp || !PowerPC::MMU::HostIsRAMAddress(guard, next + 4))
      break;
    out += fmt::format(" {:08x}", PowerPC::MMU::HostRead<u32>(guard, next + 4));
    sp = next;
  }
  return out;
}

std::string ThreadReport(const Core::CPUThreadGuard& guard, u32 addr)
{
  const Core::Debug::OSThreadView view(guard, addr);
  const auto& t = view.Data();
  return fmt::format("{:08x} state {} suspend {} prio {}/{} queue {:08x} srr0 {:08x} lr {:08x} r3 {:08x} "
                     "msr {:08x} stack{}",
                     addr, t.state, t.suspend, t.effective_priority, t.base_priority, t.queue_addr,
                     t.context.srr0, t.context.lr, t.context.gpr[3], t.context.srr1,
                     GuestBacktrace(guard, t.context.gpr[1], 12));
}

void ReportPadStall(const Core::CPUThreadGuard& guard, double seconds)
{
  auto& system = guard.GetSystem();
  const auto rd = [&](u32 a) {
    return PowerPC::MMU::HostIsRAMAddress(guard, a) ? PowerPC::MMU::HostRead<u32>(guard, a) : 0u;
  };
  const Guest g(system.GetMemory());
  const u32 pad_system = rd(Addr::PAD_SYSTEM_PTR);
  WARN_LOG_FMT(BRAWLBACK,
               "pad watchdog: the pad thread made no pass for {:.1f} s (scene {}): run flag {}, "
               "gfPadSystem {:08x} thread {:08x}, current thread {:08x}",
               seconds, CurrentSceneName(g),
               PowerPC::MMU::HostIsRAMAddress(guard, Addr::PAD_THREAD_RUN) ?
                   PowerPC::MMU::HostRead<u8>(guard, Addr::PAD_THREAD_RUN) :
                   0,
               pad_system, pad_system ? rd(pad_system + Addr::PAD_SYS_THREAD_OFF) : 0, rd(0x800000E4));
  WARN_LOG_FMT(BRAWLBACK, "pad watchdog: pad thread {}", ThreadReport(guard, Addr::PAD_THREAD));

  // What the controllers did up to the stop: the pad slots of the thread's last pass (gfPadStatus,
  // per port: current, current2, held, pressed, released, pressed2, then the sticks and triggers)
  // and the sys pads' held buttons now (gfPadSystem+0x244 + 0x40 * port, +8).
  RawPads last_slots;
  u64 passes;
  u32 last_frame;
  {
    std::lock_guard lk(s_pads.mutex);
    last_slots = s_pads.latest;
    passes = s_pads.passes;
    last_frame = s_pads.latest_app_frame;
  }
  std::string slots;
  for (u32 port = 0; port < 4; ++port)
  {
    slots += fmt::format(" P{}", port + 1);
    for (u32 i = 0; i < 0x20; i += 4)
    {
      const u8* b = last_slots.data() + port * Addr::PAD_STRIDE + i;
      slots += fmt::format(" {:02x}{:02x}{:02x}{:02x}", b[0], b[1], b[2], b[3]);
    }
  }
  std::string held;
  for (u32 port = 0; pad_system && port < 4; ++port)
    held += fmt::format(" P{} {:08x}", port + 1, rd(pad_system + 0x244 + 0x40 * port + 8));
  u32 now_frame = 0;
  if (const auto app = g.Ptr32(Addr::APPLICATION_PTR))
    now_frame = g.U32(*app + Addr::APP_FRAME_COUNTER_OFF).value_or(0);
  WARN_LOG_FMT(BRAWLBACK, "pad watchdog: {} passes, the last at app frame {} (now {}); its pad slots:{}", passes,
               last_frame, now_frame, slots);
  WARN_LOG_FMT(BRAWLBACK, "pad watchdog: sys pads held now:{}", held);

  // The OS alarm queue (OSAlarm: handler +0, fire u64 +8, prev +0x10, next +0x14, period u64 +0x18).
  // "in": timebase ticks from now to the fire time, from the low words (the game's time base and
  // Dolphin's differed by one in the high word; good for +-35 s); the full values are logged too.
  const u64 now_tb = system.GetSystemTimers().GetFakeTimeBase();
  const auto in = [now_tb](u64 fire) { return s32(u32(fire) - u32(now_tb)); };
  std::string alarms;
  bool pad_alarm_queued = false;
  u32 a = rd(Addr::OS_ALARM_QUEUE), prev = 0;
  for (int i = 0; a && i < 64 && PowerPC::MMU::HostIsRAMAddress(guard, a + 0x1F); ++i)
  {
    const u64 fire = u64{rd(a + 8)} << 32 | rd(a + 12);
    const u64 period = u64{rd(a + 0x18)} << 32 | rd(a + 0x1C);
    alarms += fmt::format(" [{:08x} handler {:08x} fire {:x} in {} period {}{}]", a, rd(a), fire, in(fire),
                          period, rd(a + 0x10) == prev ? "" : " BAD-PREV");
    pad_alarm_queued |= a == pad_system;
    prev = a;
    a = rd(a + 0x14);
  }
  WARN_LOG_FMT(BRAWLBACK, "pad watchdog: alarm queue (now {:x}, tail {:08x}, last {:08x}){}", now_tb,
               rd(Addr::OS_ALARM_QUEUE + 4), prev, alarms);
  if (pad_system)
  {
    const u64 fire = u64{rd(pad_system + 8)} << 32 | rd(pad_system + 12);
    WARN_LOG_FMT(BRAWLBACK,
                 "pad watchdog: pad alarm {:08x} {}: handler {:08x} fire in {} ({:x}) prev {:08x} next {:08x} "
                 "period {}",
                 pad_system, pad_alarm_queued ? "queued" : "NOT QUEUED", rd(pad_system), in(fire), fire,
                 rd(pad_system + 0x10), rd(pad_system + 0x14),
                 u64{rd(pad_system + 0x18)} << 32 | rd(pad_system + 0x1C));
  }
  for (const auto& t : system.GetPowerPC().GetDebugInterface().GetThreads(guard))
    WARN_LOG_FMT(BRAWLBACK, "pad watchdog: thread {}", ThreadReport(guard, t->GetAddress()));
}
}  // namespace

void PadsWatchdog(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  u64 last;
  {
    std::lock_guard lk(s_pads.mutex);
    last = s_pads.latest_ticks;
  }
  const u64 now = system.GetCoreTiming().GetTicks();
  if (last == 0 || now < last)  // no pass yet, or a savestate went back in time
    return;
  const double seconds = double(now - last) / system.GetSystemTimers().GetTicksPerSecond();
  if (!s_pad_watch.stalled)
  {
    if (seconds < 1.0)
      return;
    s_pad_watch.stalled = true;
    if (s_pad_watch.reports++ < PAD_WATCH_MAX_REPORTS)
      ReportPadStall(guard, seconds);
  }
  else if (seconds < 0.1)
  {
    s_pad_watch.stalled = false;
    WARN_LOG_FMT(BRAWLBACK, "pad watchdog: the pad thread runs again");
  }
}

void PadsSetAnchor(u32 port_mask)
{
  std::lock_guard lk(s_pads.mutex);
  s_pads.anchor_ports = port_mask & 0xF;
  if (!s_pads.anchor_ports && !s_pads.session && !s_pads.inject_ports)
    s_pads.current_valid = false;
}

void PadsSetRecording(bool enabled)
{
  std::lock_guard lk(s_pads.mutex);
  s_pads.recording = enabled;
  if (enabled)
    std::fill(s_pads.recorded.begin(), s_pads.recorded.end(), std::pair<u32, PadSlots>{});
}

std::vector<std::pair<u32, PadSlots>> PadsRecorded(u32 since)
{
  std::lock_guard lk(s_pads.mutex);
  std::vector<std::pair<u32, PadSlots>> out;
  for (const auto& e : s_pads.recorded)
  {
    if (e.first != 0 && e.first >= since)
      out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

void PadsInjectClear()
{
  std::lock_guard lk(s_pads.mutex);
  std::fill(s_pads.inject_valid.begin(), s_pads.inject_valid.end(), false);
  s_pads.inject_ports = 0;
  s_pads.injected = 0;
  s_pads.current_valid = false;
}

void PadsInjectAdd(u32 frame, const PadSlots& slots)
{
  std::lock_guard lk(s_pads.mutex);
  const size_t idx = frame % PADS_CAPACITY;
  s_pads.inject[idx] = {frame, slots};
  s_pads.inject_valid[idx] = true;
}

std::optional<PadSlots> PadsInjectedFor(u32 frame)
{
  std::lock_guard lk(s_pads.mutex);
  const size_t idx = frame % PADS_CAPACITY;
  if (!s_pads.inject_valid[idx] || s_pads.inject[idx].first != frame)
    return std::nullopt;
  return s_pads.inject[idx].second;
}

void PadsInjectSetPorts(u32 port_mask)
{
  std::lock_guard lk(s_pads.mutex);
  s_pads.inject_ports = port_mask & 0xF;
}

void PadsSetCurrent(u32 port_mask, const PadSlots& slots)
{
  std::lock_guard lk(s_pads.mutex);
  s_pads.session = true;
  s_pads.current = slots;
  s_pads.current_ports = port_mask & 0xF;
  s_pads.current_valid = true;
}

void PadsClearCurrent()
{
  std::lock_guard lk(s_pads.mutex);
  s_pads.session = false;
  s_pads.current_valid = false;
}

u32 PadsInjectedCount()
{
  std::lock_guard lk(s_pads.mutex);
  return s_pads.injected;
}
}  // namespace Gprb
