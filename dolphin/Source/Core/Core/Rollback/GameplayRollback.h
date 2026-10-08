// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Gameplay-only (Slippi-style) rollback for Brawl / Project+: Brawl memory helpers, the gameplay
// region set, and a per-game-frame trace of gameplay observables.
//
// The session model: each player boots and uses the menus on their own. They connect at the
// character select, exchange selections, and start the same match. Only gameplay state (a
// configured region set) is saved and restored during the match. See
// docs/gameplay-rollback-status.md in the workspace.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}
namespace Memory
{
class MemoryManager;
}

namespace Gprb
{
// ---------------------------------------------------------------------------------------------
// Brawl memory helpers (host-side reads of guest memory; no CPU interaction).

namespace Addr
{
constexpr u32 SCENE_MANAGER_PTR = 0x805A0060;  // gfSceneManager*
constexpr u32 APPLICATION_PTR = 0x8059FFAC;    // gfApplication*
constexpr u32 APP_FRAME_COUNTER_OFF = 0x100;
constexpr u32 HEAP_INFOS = 0x80494958;  // HeapInfo g_HeapInfos[0x48] {name*, pool*, size, arena}
constexpr u32 HEAP_INFO_COUNT = 0x48;
constexpr u32 OS_MODULE_LIST_HEAD = 0x800030C8;
constexpr u32 GAME_FRAME = 0x901812A0;  // +4 frameCounter, +0x14 persistentFrameCounter (0x18 bytes)
constexpr u32 GAME_FRAME_SIZE = 0x18;
constexpr u32 MTRAND_DEFAULT_SEED = 0x805A00BC;  // g_mtRand {vtable, seed}
constexpr u32 MTRAND_OTHER_SEED = 0x805A0424;    // g_mtRandOther
constexpr u32 LIBC_RAND_NEXT = 0x8059FF58;       // rand()/srand() state
constexpr u32 OBJECT_SERIAL_COUNTER = 0x8059C668;
constexpr u32 GAME_GLOBAL_PTR = 0x805A00E0;  // GameGlobal*
constexpr u32 GG_MODE_MELEE = 0x08;          // -> gmGlobalModeMelee (0x320 bytes)
constexpr u32 MODE_MELEE_SIZE = 0x320;
constexpr u32 PAD_SYSTEM_PTR = 0x805A0040;  // gfPadSystem* (0x805BACC0)
constexpr u32 PAD_STATUS = 0x805BAD00;  // gfPadStatus[4], stride 0x40, what the game uses
constexpr u32 PAD_STRIDE = 0x40;
constexpr u32 SCM_OPERATOR_RULE_MELEE = 0x54;
constexpr u32 OPR_IS_GAME_SET = 0x74;
}  // namespace Addr

class Guest
{
public:
  explicit Guest(const Memory::MemoryManager& memory) : m_memory(memory) {}
  // nullptr if [addr, addr+size) is not inside one mapped region.
  const u8* Ptr(u32 addr, u32 size) const;
  std::optional<u32> U32(u32 addr) const;
  std::optional<u32> Ptr32(u32 addr) const;  // a u32 that must look like a MEM1/MEM2 pointer
  std::optional<std::string> CStr(u32 addr, u32 max_len = 64) const;

private:
  const Memory::MemoryManager& m_memory;
};

bool IsMemAddr(u32 addr, u32 size = 1);
std::string CurrentSceneName(const Guest& g);
bool IsSceneMelee(const Guest& g);

struct HeapInfo
{
  u32 id;
  std::string name;
  u32 start;
  u32 end;
};
std::vector<HeapInfo> ReadHeapTable(const Guest& g);

struct ModuleSection
{
  u32 module_id;
  u32 index;
  u32 addr;
  u32 size;
  bool exec;
  bool bss;
};
std::vector<ModuleSection> ReadModuleSections(const Guest& g);

// Per port: active instance, damage, stocks, X, Y, status kind of the active fighter (raw u32s,
// zeroes when not resolvable). The same fields Rollback::CalculateDesyncChecksums hashes.
using FighterFields = std::array<std::array<u32, 6>, 4>;
FighterFields ReadFighterFields(const Guest& g);
u32 FightersCrc(const FighterFields& f);

// scMelee's stOperatorRuleMelee "is game set" byte (0 if not in a match).
bool IsGameSet(const Guest& g);

// ---------------------------------------------------------------------------------------------
// Region set: what gameplay-only rollback saves and restores.

struct Range
{
  u32 addr;  // effective address (0x8xxxxxxx / 0x9xxxxxxx)
  u32 size;
  std::string label;
};

struct RegionSetSpec
{
  std::string name;
  std::vector<std::string> heaps;          // resolved by name from the live heap table
  bool rel_data = false;                   // every loaded REL's non-exec sections (data + bss)
  std::vector<Range> add;                  // static ranges
  std::vector<std::pair<u32, u32>> remove;  // holes punched out of everything above
  std::vector<std::string> remove_heaps;
  // Bytes never restored, even inside a granule the set touches (see RollbackManager).
  std::vector<Range> exclude;
};

// Parses the region-set JSON (Data/Sys/Rollback/*.json). Returns an error message on failure.
std::optional<std::string> ParseRegionSet(const std::string& json, RegionSetSpec* out);
// Loads Sys/Rollback/<name>.json (or a path). Returns an error message on failure.
std::optional<std::string> LoadRegionSet(const std::string& name_or_path, RegionSetSpec* out);
// Resolves the spec against the current heap table and module list: sorted, merged, holes removed.
std::vector<Range> ResolveRegionSet(const Guest& g, const RegionSetSpec& spec);

// ---------------------------------------------------------------------------------------------
// Frame trace: one row per game frame of the match (keyed by g_GameFrame.frameCounter), recorded
// at the end of the game loop iteration. Lets two instances be compared every frame without
// pausing either. Enabled while the harness is active or with PPR_FRAME_TRACE=1.

struct FrameTraceRow
{
  u32 epoch = 0;       // increments whenever a new match simulation starts
  u32 game_frame = 0;  // g_GameFrame.frameCounter
  u32 persistent = 0;  // g_GameFrame.persistentFrameCounter
  u32 logic_steps = 0;  // gameProc calls in the iteration
  u64 ticks = 0;        // CoreTiming ticks at the frame end
  std::array<u32, 3> rng{};  // g_mtRand, g_mtRandOther, libc rand
  u32 fighters_crc = 0;
  FighterFields fighters{};
  u64 range_hash = 0;  // XXH3 over the configured ranges (0 if none)
  u32 game_set = 0;
  bool resim = false;  // recorded during a rollback resimulation pass
};

void FrameTraceConfigure(bool enabled, std::vector<std::pair<u32, u32>> ranges);
bool FrameTraceEnabled();
// CPU thread, end of every game-loop iteration (BrawlbackGekkoNetFrameEnd).
void FrameTraceOnFrameEnd(Core::System& system, u32 logic_steps, bool resim);
// Rows of the current epoch (or `epoch`) with game_frame >= since, oldest first.
std::vector<FrameTraceRow> FrameTraceRows(std::optional<u32> epoch, u32 since, u32 max_rows);
u32 FrameTraceEpoch();

// ---------------------------------------------------------------------------------------------
// Game-frame-anchored pads.
//
// Brawl's pad thread (OSThread 0x805BA108: gfPadSystem::updateLow -> updateLowGC) writes the
// gfPadStatus slots (0x805BAD00 + 0x40 * port) once per frame, but at a time that is not tied to
// the main game loop. Which sample a game frame consumes therefore depends on thread timing, i.e.
// on the emulated-time phase, which differs between two machines with different menu histories
// (the "late divergence" of docs/gameplay-rollback-feasibility.md). Anchoring the input to the
// game frame removes the race: at the top of every main-loop iteration the slots of the driven
// ports are written with that frame's input, and whenever the pad thread writes them again they
// are overwritten with the same frame's input.
//
// The harness uses this to give two instances bit-identical per-frame input (record on one,
// inject into the other); the gameplay rollback session uses it for GekkoNet's input.

constexpr u32 PAD_SLOTS_SIZE = 4 * Addr::PAD_STRIDE;
using PadSlots = std::array<u8, PAD_SLOTS_SIZE>;

// Main thread, top of every game-loop iteration (any mode). `frame` is the game frame the
// iteration produces (g_GameFrame.frameCounter + 1 in a match).
void PadsOnLoopTop(Core::System& system);
// Pad thread, after updateLow and P+'s post-processing wrote the slots (GprbPadThreadLoopHook):
// keeps the newest sample (PadsLatestRaw) and puts the frame's slots back.
void PadsOnPadThreadUpdated(Core::System& system);
// The newest slots the pad thread produced (the local controllers' latest sample).
PadSlots PadsLatestRaw();

// Anchor (recorder): at every loop top the frame's slots become the pad thread's newest sample for
// the ports in `port_mask`, and stay constant for the whole main-thread frame.
void PadsSetAnchor(u32 port_mask);
void PadsSetRecording(bool enabled);
// Recorded slots (what each frame consumed at the loop top), frame >= since.
std::vector<std::pair<u32, PadSlots>> PadsRecorded(u32 since);
// Injection table: frame -> slots, for the ports in `port_mask`.
void PadsInjectClear();
void PadsInjectAdd(u32 frame, const PadSlots& slots);
void PadsInjectSetPorts(u32 port_mask);
// The injected slots of `frame` (game frame), if the table has them.
std::optional<PadSlots> PadsInjectedFor(u32 frame);
// Directly provide the slots the next loop tops and pad-thread reads use (session mode).
void PadsSetCurrent(u32 port_mask, const PadSlots& slots);
void PadsClearCurrent();
u32 PadsInjectedCount();

// ---------------------------------------------------------------------------------------------
// Interpreter instruction trace (debugging). Armed by the harness (`cpu_trace`); records, for the
// game's main thread with external interrupts enabled (so no interrupt handlers), every executed
// instruction: {u32 pc, u32 opcode, u32 effective address, u32 flags, u64 value}, where value is
// the loaded value (after a load) or the stored value (before a store). Only the interpreter
// (CPUCore = 0) records. The window is whole game-loop iterations: from the loop top of the
// iteration whose g_GameFrame.frameCounter is `first_frame` - 1 to the end of the iteration that
// reaches `first_frame` + `frames` - 1.

struct CpuTraceRecord
{
  u32 pc;
  u32 op;
  u32 ea;
  u32 flags;  // bit 0 load, bit 1 store, bit 2 float, bit 3 MSR.EE clear, bits 8-31 OSThread
  u64 value;
};
static_assert(sizeof(CpuTraceRecord) == 24);

std::optional<std::string> CpuTraceArm(const std::string& path, u32 first_frame, u32 frames,
                                       u64 max_records, bool all_threads,
                                       bool include_irq = false, u32 occurrences = 1);
struct CpuTraceStatus
{
  bool armed;
  bool recording;
  bool done;
  u64 records;
  u32 main_thread;
};
CpuTraceStatus CpuTraceGetStatus();
void CpuTraceOnLoopTop(Core::System& system);   // BrawlbackGekkoNetUnconditionalFrame
void CpuTraceOnFrameEnd(Core::System& system);  // BrawlbackGekkoNetFrameEnd

namespace detail
{
extern bool g_cpu_trace_recording;
void CpuTracePre(Core::System& system, u32 pc, u32 op);
void CpuTracePost(Core::System& system);
}  // namespace detail

inline bool CpuTraceRecording()
{
  return detail::g_cpu_trace_recording;
}

}  // namespace Gprb
