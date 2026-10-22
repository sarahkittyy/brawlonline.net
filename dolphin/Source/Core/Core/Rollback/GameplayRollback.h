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
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
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
// gfPadSystem's system pads (+0x40), what the game uses: gfPadStatus[8], stride 0x40, GameCube
// ports 0-3 then Wii Remotes 0-3 (a Wii Remote alone, with a Nunchuk or with a Classic
// Controller). A match player's gmPlayerInitData controller number is the pad + 1.
constexpr u32 PAD_STATUS = 0x805BAD00;
constexpr u32 PAD_STRIDE = 0x40;
constexpr u32 WII_PADS = 4;  // the first Wii Remote's pad
// gfPadStatus +0x38 the error (0 none, 0xFF no controller), +0x3C the controller type (u32:
// 0 GameCube, 1 Classic Controller, 2 Wii Remote, 3 Nunchuk; the game's gfPadType).
constexpr u32 PAD_ERROR_OFF = 0x38;
constexpr u32 PAD_TYPE_OFF = 0x3F;  // the type's low byte
constexpr u32 PAD_THREAD = 0x805BA108;           // OSThread of Brawl's pad thread (object at -8)
constexpr u32 PAD_THREAD_RUN = 0x805BA440;       // u8: the thread's loop runs while non-zero
constexpr u32 PAD_SYS_THREAD_OFF = 0xB54;        // gfPadSystem+0xB54: the thread the alarm resumes
constexpr u32 OS_ALARM_QUEUE = 0x805A0678;       // {head, tail} of the OS alarm queue (r13-0x3DA8)
constexpr u32 SCM_OPERATOR_RULE_MELEE = 0x54;
constexpr u32 OPR_IS_GAME_SET = 0x74;
constexpr u32 OPR_REMAINING_FRAMES = 0xE0;  // m_remainingFrameTime: the match timer, in frames
constexpr u32 MM_TIME_LIMIT = 0x20;         // gmGlobalModeMelee: the time limit (s32 frames, 0 none)
// scMelee's stOperatorInfoMelee (its gfTask name at +0) and its flags byte: 0x80 running, 0x40 stop
// requested, 0x30 the pause screen's L+R+A+START quit (sora_melee text+0x256F80), 0x20 its other
// quit (+0x256FB0).
constexpr u32 SCM_OPERATOR_INFO = 0x68;
constexpr u32 OPI_FLAGS = 0x11B;
constexpr u8 OPI_QUIT = 0x30;
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
bool IsNextSceneMelee(const Guest& g);  // a scene change to scMelee is pending
bool IsSceneMelee(const Guest& g);      // not the online waiting room's scMelee (IsWaitingRoom)
bool IsWaitingRoom(const Guest& g);     // the match setup is Brawl's online waiting room

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
// The match timer in frames (stOperatorRuleMelee m_remainingFrameTime; one that ran out reads 1),
// 0 when the match has no time limit (gmGlobalModeMelee's) or none is running.
u32 RemainingFrames(const Guest& g);
// A pause-screen quit is chosen (online the plugin marks it and keeps the game paused until the
// session has ended: docs/game-code.md, the pause-screen quit).
bool IsQuitRequested(const Guest& g);

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

// The players' input, one gfPadStatus per in-game port (0-3). A record whose controller type is
// a Wii one (1-3) is written into the port's Wii Remote pad (WII_PADS + port) and the port's
// GameCube pad is left with no controller; any other into the GameCube pad, the Wii Remote pad
// left with none. So a player keeps the controller they locked in with on every machine, and no
// pad of the eight holds this machine's own controllers while the slots are in force.
constexpr u32 PAD_SLOTS_SIZE = 4 * Addr::PAD_STRIDE;
using PadSlots = std::array<u8, PAD_SLOTS_SIZE>;
// All eight pads as the pad thread made them (GameCube 0-3, Wii Remotes 4-7): the local controllers.
constexpr u32 RAW_PADS_SIZE = 8 * Addr::PAD_STRIDE;
using RawPads = std::array<u8, RAW_PADS_SIZE>;
// A record's controller type is a Wii Remote's (alone, Nunchuk, Classic Controller).
bool IsWiiPad(const u8* pad);

// Main thread, top of every game-loop iteration (any mode). `frame` is the game frame the
// iteration produces (g_GameFrame.frameCounter + 1 in a match).
void PadsOnLoopTop(Core::System& system);
// Pad thread, after updateLow and P+'s post-processing wrote the slots (GprbPadThreadLoopHook):
// keeps the newest sample (PadsLatestRaw) and puts the frame's slots back.
void PadsOnPadThreadUpdated(Core::System& system);
// The newest pads the pad thread produced (the local controllers' latest sample).
RawPads PadsLatestRaw();
// The pads of the newest of the pad thread's recent passes whose read of pad `chan` stands for an
// instant at or before `t`, and that instant in `*sampled_at`; the newest pass when none is that
// old. A GameCube port's read is the SI read behind it (ReadHostPad); a Wii Remote's (`chan` 4-7)
// is the pass itself, as Wii Remote reports come over the emulated Bluetooth on their own clock.
// A network session's local input (CaptureLocalInputs).
RawPads PadsSampledBefore(int chan, Clock::time_point t, Clock::time_point* sampled_at);

// Pad thread watchdog (main thread, every loop top, any mode). The pad thread suspends itself after
// every pass and the pad alarm (an OSAlarm at gfPadSystem, period one field) resumes it; players
// have lost their controller for good after a ranked stage select (2026-10-11: the thread made no
// pass again, menus and match alike). Once no pass came for a second of emulated time, logs one
// report of what keeps it stopped: the pad thread (state, suspend count, wait queue, backtrace), its
// run flag and gfPadSystem's thread pointer, the pad alarm and the OS alarm queue, and every guest
// thread; and again when passes resume. Emulated time, so a paused emulation reports nothing.
void PadsWatchdog(const Core::CPUThreadGuard& guard);

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
// Host controller reads on the throttle's schedule (network sessions; bug report #70: "against
// players over 100 ping my L button feels unresponsive half the time ... full hopping when I try
// to wavedash").
//
// The local player's input for a frame is a pad slot Brawl's pad thread made (CaptureLocalInputs),
// from the SI poll a field before the frame's loop top. The throttle sleeps at that poll (Rush
// Frame Presentation, present pacing), so the controller is read at its due time, one field after
// the last. Two things broke that with rollbacks (harness/tests/test_diag_bug70.py, 57 ms each
// way, auto delay 2, 60% of frames rolled back: 12% of the frames took the same controller sample
// as the frame before and 12% skipped one; on a LAN 0.1%; presses moved by a frame against each
// other, a jump held a frame too long, a press held one frame lost):
// - A rollback runs its re-run passes and the presented one inside one real frame, with real SI
//   polls in each (region snapshots do not rewind the ticks), and the poll the next frame's input
//   came from was a live read the re-run's length late (~4 ms at depth 2).
// - The pad thread's pass and the loop top come at nearly the same emulated time, in either order
//   (a region load puts the guest's alarm times back, not the ticks), so the newest pass was made
//   from the poll just before the loop top on some frames and from the one a field before on
//   others: a sample taken twice, then one skipped (PadsSampledBefore fixes that side).
//
// With SetHostPadsScheduled on, every read of a local controller (CSIDevice_GCController and
// CSIDevice_GCAdapter::GetPadStatus) stands for an instant on the throttle's schedule:
// - in a rollback's re-run: the last read before it, the one taken at the frame's due time
//   (unless older than 100 ms, after a long wait for the peer: then read now);
// - behind schedule (the instant was due earlier, CoreTimingManager::ScheduledHostTime): the
//   state the controller had then, from a timestamped history: the Wii U adapter's read thread
//   keeps one (GCAdapter::InputAt), and the harness (Harness::PadOverrideAt). Pads mapped in
//   Dolphin (keyboard, XInput, SDL...) are read when called and have no past, so such a read is
//   taken now ("late live"). The polls that make a local input are not behind schedule in a
//   session (the re-run's are latched, the others come after the throttle's sleep): only a
//   frame that ran longer than a field makes one late. A sampler thread polling every input
//   backend would give them a past, but ControllerInterface updates its devices under one mutex,
//   so it would hold up the SI poll itself;
// - otherwise (on schedule or ahead of it): now, as before.
// Each frame's sample is then one field after the last, rolled back or not, and as old as on a
// frame without a rollback (the loop top's due time minus a field; ~17.5 ms at the loop top,
// against ~16 ms on a LAN and 12-14 ms under rollbacks before, when the late reads were fresher).
enum class HostPadSource
{
  Mapped,   // Dolphin's controller mapping (Pad::GetStatus)
  Adapter,  // the Wii U GameCube adapter (GCAdapter::Input)
};
// A network session's frames run (on) or not (off: every read is taken now).
void SetHostPadsScheduled(bool scheduled);
// CPU thread: SI channel `chan`'s host controller, as above.
GCPadStatus ReadHostPad(Core::System& system, int chan, HostPadSource source);

struct HostPadReadStats
{
  u64 on_time = 0;       // read now, on schedule (or ahead of it)
  u64 late_history = 0;  // more than 1 ms behind schedule: from the history
  u64 late_live = 0;     // more than 1 ms behind schedule, no history: read now
  u64 latched = 0;       // in a re-run: the last read before it
  u64 relatched = 0;     // in a re-run, the last read too old: read now
  double late_max_ms = 0;
};
// Since the last call.
HostPadReadStats TakeHostPadReadStats();
// Diagnostics (PPR_GPRB_PAD_READ_LOG): a line of the session's own (loop tops, captures).
void HostPadLogLine(const char* kind, long long a, long long b, long long c);

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
