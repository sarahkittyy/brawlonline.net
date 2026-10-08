// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GameplayRollback.h"

#include <algorithm>
#include <atomic>
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
#include "Core/CoreTiming.h"
#include "Core/Harness/Harness.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

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

bool IsSceneMelee(const Guest& g)
{
  return CurrentSceneName(g) == "scMelee";
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
  FighterFields out{};
  const auto entries = g.Ptr32(0x80624780u);
  if (!entries)
    return out;
  for (u32 port = 0; port < 4; ++port)
  {
    auto& f = out[port];
    const u32 entry = *entries + port * 0x244u;
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
  PadSlots latest{};
};
PadsState s_pads;

void WriteSlots(Memory::MemoryManager& memory, u32 port_mask, const PadSlots& slots)
{
  for (u32 port = 0; port < 4; ++port)
  {
    if (!(port_mask & (1u << port)))
      continue;
    memory.CopyToEmu(Addr::PAD_STATUS + port * Addr::PAD_STRIDE,
                     slots.data() + port * Addr::PAD_STRIDE, Addr::PAD_STRIDE);
  }
}
}  // namespace

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
      s_pads.current = s_pads.latest;
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
    PadSlots slots;
    memory.CopyFromEmu(slots.data(), Addr::PAD_STATUS, slots.size());
    s_pads.recorded[frame % PADS_CAPACITY] = {frame, slots};
  }
}

void PadsOnPadThreadUpdated(Core::System& system)
{
  auto& memory = system.GetMemory();
  std::lock_guard lk(s_pads.mutex);
  memory.CopyFromEmu(s_pads.latest.data(), Addr::PAD_STATUS, s_pads.latest.size());
  s_pads.have_latest = true;
  if (s_pads.current_valid)
    WriteSlots(memory, s_pads.current_ports, s_pads.current);
}

PadSlots PadsLatestRaw()
{
  std::lock_guard lk(s_pads.mutex);
  return s_pads.latest;
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
