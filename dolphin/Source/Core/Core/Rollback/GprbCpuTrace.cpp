// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Interpreter instruction trace for gameplay-rollback determinism debugging (see
// GameplayRollback.h, "Interpreter instruction trace").

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/System.h"

namespace Gprb
{
namespace detail
{
bool g_cpu_trace_recording = false;
}

namespace
{
struct CpuTraceState
{
  std::mutex mutex;
  bool armed = false;
  bool done = false;
  bool all_threads = false;
  bool include_irq = false;
  u32 occurrences = 1;  // record this many runs of the window (a rollback re-runs frames)
  u32 occurrence = 0;
  std::string base_path;
  std::string path;
  u32 first_frame = 0;
  u32 last_frame = 0;
  u64 max_records = 0;
  u64 records = 0;
  u32 main_thread = 0;
  File::IOFile file;
  std::vector<CpuTraceRecord> buffer;
  // The instruction being executed (between Pre and Post).
  CpuTraceRecord cur{};
  bool cur_valid = false;
  int cur_kind = 0;  // 0 none, 1 gpr load, 2 fpr load
  u32 cur_reg = 0;
};
CpuTraceState s_ct;

void Flush()
{
  if (!s_ct.buffer.empty() && s_ct.file.IsOpen())
    s_ct.file.WriteArray(s_ct.buffer.data(), s_ct.buffer.size());
  s_ct.buffer.clear();
}

void Stop()
{
  detail::g_cpu_trace_recording = false;
  Flush();
  s_ct.file.Close();
  if (++s_ct.occurrence < s_ct.occurrences)
  {
    // Wait for the next run of the same window (after a rollback) in a new file.
    s_ct.path = s_ct.base_path + "." + std::to_string(s_ct.occurrence);
    s_ct.file.Open(s_ct.path, "wb");
    INFO_LOG_FMT(BRAWLBACK, "cpu_trace: {} records, waiting for run {}", s_ct.records, s_ct.occurrence);
    s_ct.records = 0;
    return;
  }
  s_ct.armed = false;
  s_ct.done = true;
  INFO_LOG_FMT(BRAWLBACK, "cpu_trace: {} records written to {}", s_ct.records, s_ct.path);
}
}  // namespace

std::optional<std::string> CpuTraceArm(const std::string& path, u32 first_frame, u32 frames,
                                       u64 max_records, bool all_threads, bool include_irq,
                                       u32 occurrences)
{
  std::lock_guard lk(s_ct.mutex);
  if (detail::g_cpu_trace_recording)
    return "a trace is already recording";
  if (frames == 0 || first_frame == 0)
    return "first_frame and frames must be >= 1";
  s_ct.file.Close();
  s_ct.occurrences = std::max(1u, occurrences);
  s_ct.occurrence = 0;
  s_ct.base_path = path;
  if (!s_ct.file.Open(path + (s_ct.occurrences > 1 ? ".0" : ""), "wb"))
    return "cannot open " + path;
  s_ct.path = path;
  s_ct.first_frame = first_frame;
  s_ct.last_frame = first_frame + frames - 1;
  s_ct.max_records = max_records ? max_records : 50000000;
  s_ct.records = 0;
  s_ct.all_threads = all_threads;
  s_ct.include_irq = include_irq;
  s_ct.armed = true;
  s_ct.done = false;
  s_ct.buffer.clear();
  s_ct.buffer.reserve(1 << 16);
  return std::nullopt;
}

CpuTraceStatus CpuTraceGetStatus()
{
  std::lock_guard lk(s_ct.mutex);
  return {s_ct.armed, detail::g_cpu_trace_recording, s_ct.done, s_ct.records, s_ct.main_thread};
}

void CpuTraceOnLoopTop(Core::System& system)
{
  // Diagnostics (PPR_GPRB_INTERP_FROM=<game frame>): run the JIT until that frame, then switch to
  // the interpreter (cpu_trace records only under the interpreter) and break; the harness resumes.
  static const u32 interp_from = [] {
    const char* v = std::getenv("PPR_GPRB_INTERP_FROM");
    return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
  }();
  if (interp_from && system.GetPowerPC().GetMode() != PowerPC::CoreMode::Interpreter)
  {
    const Guest gi(system.GetMemory());
    if (gi.U32(Addr::GAME_FRAME + 4).value_or(0) + 1 >= interp_from && IsSceneMelee(gi))
    {
      INFO_LOG_FMT(BRAWLBACK, "cpu_trace: switching to the interpreter at frame {}",
                   gi.U32(Addr::GAME_FRAME + 4).value_or(0) + 1);
      system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);
      system.GetCPU().Break();
    }
  }
  if (!s_ct.armed || s_ct.done || detail::g_cpu_trace_recording)
    return;
  std::lock_guard lk(s_ct.mutex);
  const Guest g(system.GetMemory());
  const u32 frame = g.U32(Addr::GAME_FRAME + 4).value_or(0);
  if (frame + 1 != s_ct.first_frame || !IsSceneMelee(g))
    return;
  s_ct.main_thread = g.U32(0x800000E4).value_or(0);
  detail::g_cpu_trace_recording = true;
  INFO_LOG_FMT(BRAWLBACK, "cpu_trace: recording from frame {} (main thread {:08x})", frame + 1,
               s_ct.main_thread);
}

void CpuTraceOnFrameEnd(Core::System& system)
{
  if (!detail::g_cpu_trace_recording)
    return;
  std::lock_guard lk(s_ct.mutex);
  const Guest g(system.GetMemory());
  const u32 frame = g.U32(Addr::GAME_FRAME + 4).value_or(0);
  if (frame >= s_ct.last_frame)
    Stop();
}

namespace detail
{
void CpuTracePre(Core::System& system, u32 pc, u32 op)
{
  auto& ppc = system.GetPPCState();
  s_ct.cur_valid = false;
  if (!ppc.msr.EE && !s_ct.include_irq)
    return;
  auto& memory = system.GetMemory();
  const u32 thread = memory.Read_U32(0x000000E4);
  if (!s_ct.all_threads && thread != s_ct.main_thread)
    return;
  CpuTraceRecord r{pc, op, 0, ((thread & 0xFFFFFF) << 8) | (ppc.msr.EE ? 0u : 8u), 0};
  const u32 opcd = op >> 26;
  const u32 rd = (op >> 21) & 31;
  const u32 ra = (op >> 16) & 31;
  const u32 rb = (op >> 11) & 31;
  const s32 simm = static_cast<s16>(op & 0xFFFF);
  auto base = [&](bool update) -> u32 { return (ra == 0 && !update) ? 0u : ppc.gpr[ra]; };
  // 1 gpr load, 2 fpr load, 3 gpr store, 4 fpr store (single), 5 fpr store (double/ps),
  // 6 lmw/stmw (address only)
  int kind = 0;
  bool update = false;
  bool dform = true;
  switch (opcd)
  {
  case 32: case 34: case 40: case 42: kind = 1; break;
  case 33: case 35: case 41: case 43: kind = 1; update = true; break;
  case 36: case 38: case 44: kind = 3; break;
  case 37: case 39: case 45: kind = 3; update = true; break;
  case 48: case 50: kind = 2; break;
  case 49: case 51: kind = 2; update = true; break;
  case 52: kind = 4; break;
  case 53: kind = 4; update = true; break;
  case 54: kind = 5; break;
  case 55: kind = 5; update = true; break;
  case 46: case 47: kind = 6; break;
  case 56: case 57: case 60: case 61:
  {
    // psq_l / psq_lu / psq_st / psq_stu: 12-bit displacement.
    const s32 d12 = static_cast<s32>(static_cast<s16>(static_cast<u16>((op & 0xFFF) << 4))) >> 4;
    r.ea = base(opcd == 57 || opcd == 61) + static_cast<u32>(d12);
    kind = (opcd < 60) ? 2 : 5;
    dform = false;
    break;
  }
  case 31:
  {
    dform = false;
    const u32 xo = (op >> 1) & 0x3FF;
    switch (xo)
    {
    case 23: case 87: case 279: case 343: case 534: case 790: kind = 1; break;
    case 55: case 119: case 311: case 375: kind = 1; update = true; break;
    case 151: case 215: case 407: case 662: case 918: kind = 3; break;
    case 183: case 247: case 439: kind = 3; update = true; break;
    case 535: case 599: kind = 2; break;
    case 567: case 631: kind = 2; update = true; break;
    case 663: kind = 4; break;
    case 695: kind = 4; update = true; break;
    case 727: case 983: kind = 5; break;
    case 759: kind = 5; update = true; break;
    default: break;
    }
    if (kind)
      r.ea = base(update) + ppc.gpr[rb];
    break;
  }
  default:
    dform = false;
    break;
  }
  if (kind && dform)
    r.ea = base(update) + static_cast<u32>(simm);
  if (kind == 1 || kind == 2)
    r.flags |= 1;
  if (kind >= 3 && kind <= 5)
    r.flags |= 2;
  if (kind == 2 || kind == 4 || kind == 5)
    r.flags |= 4;
  if (kind == 6)
    r.flags |= (opcd == 46) ? 1 : 2;
  if (kind == 3)
    r.value = ppc.gpr[rd];
  else if (kind == 4 || kind == 5)
    r.value = ppc.ps[rd].PS0AsU64();
  s_ct.cur = r;
  s_ct.cur_valid = true;
  s_ct.cur_kind = (kind == 1 || kind == 2) ? kind : 0;
  s_ct.cur_reg = rd;
}

void CpuTracePost(Core::System& system)
{
  if (!s_ct.cur_valid)
    return;
  s_ct.cur_valid = false;
  auto& ppc = system.GetPPCState();
  if (s_ct.cur_kind == 1)
    s_ct.cur.value = ppc.gpr[s_ct.cur_reg];
  else if (s_ct.cur_kind == 2)
    s_ct.cur.value = ppc.ps[s_ct.cur_reg].PS0AsU64();
  s_ct.buffer.push_back(s_ct.cur);
  ++s_ct.records;
  if (s_ct.buffer.size() >= (1 << 16))
    Flush();
  if (s_ct.records >= s_ct.max_records)
  {
    std::lock_guard lk(s_ct.mutex);
    Stop();
  }
}
}  // namespace detail

}  // namespace Gprb
