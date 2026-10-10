// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_Misc.h"

#include <chrono>
#include <thread>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/GeckoCode.h"
#include "Core/HW/CPU.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Memmap.h"
#include "Core/Harness/Harness.h"
#include "Core/Host.h"
#include "Core/NetPlayClient.h"
#include "Core/Online/GameBridge.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/GameplayRollback.h"
#include "Core/Rollback/GameplaySession.h"
#include "Core/Rollback/PresentStats.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"

namespace HLE_Misc
{
static bool IsShortcutResimulationPass();

// If you just want to kill a function, one of the three following are usually appropriate.
// According to the PPC ABI, the return value is always in r3.
void UnimplementedFunction(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  ppc_state.npc = LR(ppc_state);
}

void HBReload(const Core::CPUThreadGuard& guard)
{
  // There isn't much we can do. Just stop cleanly.
  auto& system = guard.GetSystem();
  system.GetCPU().Break();
  Host_Message(HostMessageID::WMUserStop);
}

void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  auto& jit_interface = system.GetJitInterface();

  // Work around the codehandler not properly invalidating the icache, but
  // only the first few frames.
  // (Project M uses a conditional to only apply patches after something has
  // been read into memory, or such, so we do the first 5 frames.  More
  // robust alternative would be to actually detect memory writes, but that
  // would be even uglier.)
  u32 gch_gameid = PowerPC::MMU::HostRead<u32>(guard, Gecko::INSTALLER_BASE_ADDRESS);
  if (gch_gameid - Gecko::MAGIC_GAMEID == 5)
  {
    return;
  }
  else if (gch_gameid - Gecko::MAGIC_GAMEID > 5)
  {
    gch_gameid = Gecko::MAGIC_GAMEID;
  }
  PowerPC::MMU::HostWrite<u32>(guard, gch_gameid + 1, Gecko::INSTALLER_BASE_ADDRESS);

  ppc_state.iCache.Reset(jit_interface);
}

// Because Dolphin messes around with the CPU state instead of patching the game binary, we
// need a way to branch into the GCH from an arbitrary PC address. Branching is easy, returning
// back is the hard part. This HLE function acts as a trampoline that restores the original LR, SP,
// and PC before the magic, invisible BL instruction happened.
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  // Stack frame is built in GeckoCode.cpp, Gecko::RunCodeHandler.
  const u32 SP = ppc_state.gpr[1];
  ppc_state.gpr[1] = PowerPC::MMU::HostRead<u32>(guard, SP + 8);
  ppc_state.npc = PowerPC::MMU::HostRead<u32>(guard, SP + 12);
  LR(ppc_state) = PowerPC::MMU::HostRead<u32>(guard, SP + 16);
  ppc_state.cr.Set(PowerPC::MMU::HostRead<u32>(guard, SP + 20));
  for (int i = 0; i < 14; ++i)
  {
    ppc_state.ps[i].SetBoth(
        PowerPC::MMU::HostRead<u64>(guard, SP + 24 + 2 * i * sizeof(u64)),
        PowerPC::MMU::HostRead<u64>(guard, SP + 24 + (2 * i + 1) * sizeof(u64)));
  }
}

static constexpr u32 BRAWL_UNCONDITIONAL_HOOK_ADDR = 0x800171b4;
static constexpr u32 BRAWL_GAME_LOOP_HOOK_ADDR = 0x80017344;
static constexpr u32 BRAWL_GAME_LOOP_CONDITION_ADDR = 0x800173a4;
static constexpr u32 BRAWL_GAMEPROC_CALLSITE_ADDR = 0x80017350;
static constexpr u32 BRAWL_GAMEPROC_CALLSITE_NEXT_ADDR = 0x80017354;
static constexpr u32 BRAWL_APP_SCENE_MANAGER_OFFSET = 0xd4;
static constexpr u32 BRAWL_SCENE_MANAGER_CURRENT_SCENE_OFFSET = 0x4;
static constexpr u32 BRAWL_SCENE_NAME_OFFSET = 0;

bool IsBootScene(const Core::CPUThreadGuard& guard, u32 app_ptr)
{
  static constexpr char boot_scene[] = "scBoot";
  const u32 scene_manager =
      PowerPC::MMU::HostRead<u32>(guard, app_ptr + BRAWL_APP_SCENE_MANAGER_OFFSET);
  if (scene_manager == 0)
    return false;

  const u32 current_scene = PowerPC::MMU::HostRead<u32>(
      guard, scene_manager + BRAWL_SCENE_MANAGER_CURRENT_SCENE_OFFSET);
  if (current_scene == 0)
    return false;

  const u32 scene_name = PowerPC::MMU::HostRead<u32>(guard, current_scene + BRAWL_SCENE_NAME_OFFSET);
  if (scene_name == 0)
    return false;

  for (u32 index = 0; boot_scene[index] != '\0'; ++index)
  {
    if (PowerPC::MMU::HostRead<u8>(guard, scene_name + index) != boot_scene[index])
      return false;
  }
  return true;
}

// GekkoNet has no frame for us yet: the peer is behind (prediction window exhausted) or the
// session hasn't started. Wait on the host instead of spinning the game loop. A guest-side spin
// runs guest code, so emulated time - and with it VI interrupts, IO completions and the game's
// other threads - advanced by a host-speed-dependent amount between two frames, which neither the
// peer nor a later resimulation of those frames reproduces (e.g. a file load finishing a frame
// earlier on one side). Returns false if the wait was given up (emulation is pausing or stopping,
// or netplay ended), in which case the caller falls back to the guest-side spin.
static bool WaitForGekkoFrames(Core::System& system)
{
  auto& cpu = system.GetCPU();
  bool waited = false;
  while (!NetPlay::IsTimeSynced() || NetPlay::GetFramesToAdvance() == 0)
  {
    if (cpu.GetState() != CPU::State::Running || !NetPlay::IsNetPlayRunning() ||
        !NetPlay::IsInRollbackMode())
    {
      return false;
    }
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(250));
    NetPlay::OnFrameStart();
  }
  if (waited)
    system.GetCoreTiming().ResetThrottleToNow();
  return true;
}

void BrawlbackGekkoNetUnconditionalFrame(const Core::CPUThreadGuard& guard)
{
  static bool gekko_sync_activated = false;
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  // A running gameplay session arms the trace itself, after any load has rewound the frame.
  if (!Gprb::Session::IsRunning())
    Gprb::CpuTraceOnLoopTop(system);
  Gprb::PadsOnLoopTop(system);
  // Gameplay-only rollback session (Core/Rollback/GameplaySession.h). Never together with a
  // Dolphin netplay rollback session.
  if (!NetPlay::IsNetPlayRunning() && Gprb::Session::OnLoopTop(guard))
    return;

  // Check if rollback mode is active
  const bool netplay_running = NetPlay::IsNetPlayRunning();
  const bool rollback_mode = NetPlay::IsInRollbackMode();

  auto& core_timing = system.GetCoreTiming();

  if (!netplay_running || !rollback_mode)
  {
    gekko_sync_activated = false;
    NetPlay::SetGekkoCpuStalled(false);
    core_timing.SetRollbackResimulating(false);
    core_timing.SetRollbackSpeedAdjustment(1.0);
    // Not in rollback mode - execute original instruction and continue normally
    // Execute the original instruction: li r25, 0x1
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
    return;
  }

  static constexpr u32 BRAWL_LOOP_END_ADDR = 0x80017508;

  // Start synchronized frame advancement once Brawl reaches scBoot. Keep it active after the
  // boot scene transitions so GekkoNet continues exchanging inputs in later scenes.
  if (!gekko_sync_activated && !IsBootScene(guard, ppc_state.gpr[23]))
  {
    NetPlay::SetGekkoCpuStalled(false);
    ppc_state.gpr[25] = 1;
    ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
    return;
  }
  gekko_sync_activated = true;

  // Take the snapshot requested by the previous iteration's FrameEnd hook here, at the top of the
  // loop, which is exactly where loads resume. Only hooks ran since that iteration ended, so the
  // memory is the same as at FrameEnd, but the CPU's position in the CoreTiming slice
  // (ppc_state.downcount) now matches what a load restores: the JIT blocks between FrameEnd and
  // the loop top consume a few cycles that a load at the loop top would otherwise skip, leaving
  // every resimulation a few cycles (one timebase tick) early.
  NetPlay::PerformQueuedFrameSave(system);

  int current_iteration = NetPlay::GetCurrentIteration();

  if (current_iteration == 0)
  {
    // Must run every spin even while stalled below - this is what actually polls GekkoNet and
    // lets its handshake with the peer (and thus IsTimeSynced()) ever progress.
    NetPlay::OnFrameStart();
    if (!NetPlay::IsTimeSynced() || NetPlay::GetFramesToAdvance() == 0)
    {
      NetPlay::SetGekkoCpuStalled(true);
      if (!WaitForGekkoFrames(system) && NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
        NetPlay::NoteGekkoStallFallback();
    }
  }

  if (!NetPlay::IsTimeSynced())
  {
    NetPlay::SetGekkoCpuStalled(true);
    core_timing.SetRollbackResimulating(false);
    // GekkoNet's handshake with the peer hasn't finished yet - spin here without advancing any
    // game logic instead of letting the local game run ahead into the match on its own, which
    // is what let host and guest load in at noticeably different real times.
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  int total_iterations = NetPlay::GetFramesToAdvance();

  if (total_iterations == 0)
  {
    NetPlay::SetGekkoCpuStalled(true);
    core_timing.SetRollbackResimulating(false);
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  if (current_iteration >= total_iterations)
  {
    NetPlay::SetGekkoCpuStalled(true);
    core_timing.SetRollbackResimulating(false);
    // Execute the original instruction: li r25, 0x1
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  NetPlay::SetGekkoCpuStalled(false);
  const bool is_resimulation_pass = IsResimulationPass();
  NetPlay::SetGekkoResimulationPass(is_resimulation_pass);
  // Re-running frames that were already shown must not cost wall-clock time at emulated speed,
  // or every rollback makes this peer fall further behind (and roll back even more).
  core_timing.SetRollbackResimulating(is_resimulation_pass);
  core_timing.SetRollbackSpeedAdjustment(NetPlay::GetTimeSyncSpeedFactor());
  // The same flag keeps this iteration's VI field off the screen (VideoInterface::OutputField).
  if (!is_resimulation_pass)
  {
    Rollback::PresentStats::OnDisplayedFrameStart(total_iterations > 1);
    NetPlay::OnDisplayedFrameStart();
  }

  // Execute the original instruction: li r25, 0x1
  ppc_state.gpr[25] = (s16)0x1;

  // Continue to next instruction
  ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
}

void BrawlbackGekkoNetFrameEnd(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  static constexpr u32 BRAWL_FRAME_END_NEXT_ADDR = 0x80017508;

  if (Gprb::Session::OnFrameEnd(guard))
    return;

  // The game<->Dolphin mailbox (docs/backend-design.md 5.2): serviced here, at the frame
  // boundary, and only outside netplay sessions (GameBridge checks) and gameplay sessions (the
  // return above).
  Online::GameBridge::OnFrameEnd(guard);

  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
  {
    // Execute the original instruction: stw r0,0x100(r23)
    const u32 addr = ppc_state.gpr[23] + 0x100;
    PowerPC::MMU::HostWrite<u32>(guard, ppc_state.gpr[0], addr);
    ppc_state.npc = BRAWL_FRAME_END_NEXT_ADDR;
    Gprb::FrameTraceOnFrameEnd(system, ppc_state.gpr[24], false);
    Gprb::CpuTraceOnFrameEnd(system);
    return;
  }

  // Execute the original instruction first: stw r0,0x100(r23). Loads resume at the top of the
  // loop (0x800171b4), and the only thing between here and there is the LoopEnd hook, so a save
  // taken after this store captures exactly the state a load resumes from. Saving before it
  // dropped this iteration's gfApplication frame-counter increment from every snapshot, so each
  // rollback left that counter one behind the peer.
  const u32 addr = ppc_state.gpr[23] + 0x100;
  PowerPC::MMU::HostWrite<u32>(guard, ppc_state.gpr[0], addr);
  ppc_state.npc = BRAWL_FRAME_END_NEXT_ADDR;

  // The save itself happens at the top of the next pass through the loop (see
  // BrawlbackGekkoNetUnconditionalFrame), r24 = gameProc calls this iteration.
  NetPlay::QueueFrameSave(NetPlay::GetCurrentIteration(), ppc_state.gpr[24]);
}

void BrawlbackGekkoNetLoopEnd(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  static constexpr u32 BRAWL_LOOP_START_ADDR = 0x800171b4;

  if (Gprb::Session::OnLoopEnd(guard))
    return;

  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
  {
    ppc_state.npc = BRAWL_LOOP_START_ADDR;
    return;
  }

  int total_iterations = NetPlay::GetFramesToAdvance();

  if (total_iterations == 0)
  {
    ppc_state.npc = BRAWL_LOOP_START_ADDR;
    return;
  }

  int current_iteration = NetPlay::GetCurrentIteration();

  current_iteration++;

  if (current_iteration >= total_iterations)
  {
    NetPlay::SetCurrentIteration(0);
  }
  else
  {
    NetPlay::SetCurrentIteration(current_iteration);
  }

  ppc_state.npc = BRAWL_LOOP_START_ADDR;
}

// `bl OSSleepThread` inside DVDCancel's wait loop (dvd.o @ 0x801fb1a8). DVDCancel spins here,
// re-checking the DI command block's state field (at +0xc) until it reaches a terminal value
// (0, -1, 10, or a small set of command-specific codes), relying on the real DI completion
// interrupt to eventually call OSWakeupThread on this same queue. Across a rollback, the
// CoreTiming event driving that interrupt for the in-flight command can end up permanently
// lost/desynced from the resimulated command block, so the wakeup never arrives and the game
// hangs here forever.
static constexpr u32 BRAWL_DVDCANCEL_SLEEP_CALL_ADDR = 0x801fb1a8;
static constexpr u32 BRAWL_DVDCANCEL_LOOP_TOP_ADDR = 0x801fb134;
static constexpr u32 BRAWL_OSSLEEPTHREAD_ADDR = 0x801e1790;
static constexpr u32 BRAWL_DVD_CMD_BLOCK_STATE_OFFSET = 0xc;
static constexpr u32 BRAWL_DVD_STATE_DONE = 10;

void BrawlbackDVDCancelSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() && IsShortcutResimulationPass())
  {
    // Force the command block straight to "done" instead of actually sleeping, so DVDCancel's
    // loop condition (re-checked where we jump back to) is satisfied on its own rather than
    // waiting on a wakeup that may never come.
    const u32 command_block = ppc_state.gpr[30];
    if (command_block != 0)
    {
      PowerPC::MMU::HostWrite<u32>(guard, BRAWL_DVD_STATE_DONE,
                                   command_block + BRAWL_DVD_CMD_BLOCK_STATE_OFFSET);
    }
    ppc_state.npc = BRAWL_DVDCANCEL_LOOP_TOP_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_DVDCANCEL_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `bl OSSleepThread` inside nw4r::snd::detail::TaskManager::CancelTask's wait loop
// (snd_TaskManager.o @ 0x801cff38). CancelTask spins here waiting for the "currently executing
// task" pointer (at TaskManager+0x24) to stop matching the task being cancelled, relying on that
// task's own completion callback to advance/clear the pointer and call OSWakeupThread on this
// same queue (TaskManager+0x34). Same class of bug as DVDCancel: rollback resimulation can lose
// whatever real completion would normally clear it, hanging the game here forever.
static constexpr u32 BRAWL_CANCELTASK_SLEEP_CALL_ADDR = 0x801cff38;
static constexpr u32 BRAWL_CANCELTASK_LOOP_TOP_ADDR = 0x801cff3c;
static constexpr u32 BRAWL_CANCELTASK_CURRENT_TASK_OFFSET = 0x24;

void BrawlbackCancelTaskSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() && IsShortcutResimulationPass())
  {
    // Clear the "currently executing task" pointer instead of sleeping, so the loop condition
    // (re-checked where we jump back to) no longer matches and exits on its own.
    const u32 task_manager = ppc_state.gpr[25];
    if (task_manager != 0)
    {
      PowerPC::MMU::HostWrite<u32>(guard, 0, task_manager + BRAWL_CANCELTASK_CURRENT_TASK_OFFSET);
    }
    ppc_state.npc = BRAWL_CANCELTASK_LOOP_TOP_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_CANCELTASK_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `bl OSSleepThread` inside GXDrawDone's wait loop (GXMisc.o @ 0x801f0ac0). GXDrawDone pushes a
// GX_DRAWDONE token and waits for the GPU's real PE-finish interrupt to set the done flag
// (r13-0x3b60). mainLoopSub calls this unconditionally at the top of every iteration to wait for
// the *previous* iteration's draw, but BrawlbackSkipResimRenderHook skips issuing any draw at all
// for throwaway resimulation iterations, so that interrupt can arrive late (or never) relative to
// a compressed resim burst, hanging here forever.
static constexpr u32 BRAWL_GXDRAWDONE_SLEEP_CALL_ADDR = 0x801f0ac0;
static constexpr u32 BRAWL_GXDRAWDONE_RECHECK_ADDR = 0x801f0ac4;
static constexpr u32 BRAWL_GXDRAWDONE_FLAG_R13_OFFSET = 0xffffc4a0u;  // -0x3b60

void BrawlbackGXDrawDoneSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() && IsShortcutResimulationPass())
  {
    // Force the done flag instead of sleeping, so the loop condition (re-checked where we jump
    // back to) is satisfied on its own rather than waiting on an interrupt for a draw we skipped.
    PowerPC::MMU::HostWrite<u8>(guard, 1, ppc_state.gpr[13] + BRAWL_GXDRAWDONE_FLAG_R13_OFFSET);
    ppc_state.npc = BRAWL_GXDRAWDONE_RECHECK_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_GXDRAWDONE_RECHECK_ADDR;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `bl OSSleepThread` inside DVDReadPrio's wait loop (dvdfs.o @ 0x801f68ec). Waits on the same DI
// queue as DVDCancel for its own read command's cb.state field to reach a terminal value. Forcing
// that state here (as we do for DVDCancel) is unsafe for a real data read: the actual RAM copy and
// cb.state update only happen once DVDThread::FinishRead's CoreTiming event runs, so forcing
// completion early can hand the game uninitialized memory it believes is valid file data. The
// real fix is in DVDInterface::ScheduleReads, which now schedules read completion at a negligible
// delay during rollback netplay so that event - and the real DI interrupt/OSWakeupThread it
// triggers - always arrives promptly instead of racing rollback resimulation. This hook just
// replicates the original `bl OSSleepThread` unconditionally.
static constexpr u32 BRAWL_DVDREADPRIO_SLEEP_CALL_ADDR = 0x801f68ec;

void BrawlbackDVDReadPrioSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  LR(ppc_state) = BRAWL_DVDREADPRIO_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `subi r3, r13, 0x3bf8` inside VIWaitForRetrace's wait loop (vi.o @ 0x801e894c), the setup for
// `bl OSSleepThread`. VIWaitForRetrace spins here waiting for __VIRetraceCount (r13-0x3bd4) to
// increment, relying on __VIRetraceHandler - driven by Dolphin's real VI/present timing on its
// own thread, not our compressed per-iteration CPU loop - to eventually call OSWakeupThread. Only
// bypass during a resimulation pass: those iterations never present, so nothing would ever wake
// this up. The final iteration of every update DOES present, so let it use the real wait - that's
// also what paces the match to 60fps; bypassing it unconditionally left rollback netplay running
// as fast as the CPU could go. Advance the count ourselves and let the existing reload-and-compare
// take its own normal exit path, so OSSleepThread/the thread queue are never touched mid-resim.
//
// Do NOT try to replace the real wait with a guest-side busy-wait (re-enabling MSR[EE] and looping
// on the reload-and-compare): that re-dispatches this HLE hook every spin, which tanked the frame
// rate and desynced.
static constexpr u32 BRAWL_VIWAITFORRETRACE_SLEEP_CALL_ADDR = 0x801e894c;
static constexpr u32 BRAWL_VIWAITFORRETRACE_RECHECK_ADDR = 0x801e8954;
static constexpr u32 BRAWL_VIWAITFORRETRACE_COUNT_R13_OFFSET = 0xffffc42cu;  // -0x3bd4

void BrawlbackVIWaitForRetraceSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (IsShortcutResimulationPass())
  {
    const u32 count_addr = ppc_state.gpr[13] + BRAWL_VIWAITFORRETRACE_COUNT_R13_OFFSET;
    const u32 count = PowerPC::MMU::HostRead<u32>(guard, count_addr) + 1;
    PowerPC::MMU::HostWrite<u32>(guard, count, count_addr);
    ppc_state.npc = BRAWL_VIWAITFORRETRACE_RECHECK_ADDR;
    return;
  }

  // subi	r3, r13, 15352
  ppc_state.gpr[3] = ppc_state.gpr[13] - 15352;
  ppc_state.npc = BRAWL_VIWAITFORRETRACE_SLEEP_CALL_ADDR + 4;
}

// `bl VIWaitForRetrace` inside gfFrameBuffer::sync's busy-wait loop (gf_framebuffer.o @
// 0x80023b1c). sync() scans a small ring of framebuffer slots for one whose busy flag
// (slot+0xc) is clear, and if none are free, calls VIWaitForRetrace and rescans in a do-while
// loop. That flag is only ever cleared by drawDoneCallback, a real GX draw-done completion
// callback - which resimulation iterations never trigger since they skip rendering entirely. The
// final iteration of every update does render, so let its wait behave normally there (it also
// provides the real-time pacing that keeps rollback netplay at 60fps). Clear every slot's busy
// flag ourselves only during a resim pass, so the rescan that follows finds one free immediately.
static constexpr u32 BRAWL_FRAMEBUFFER_SYNC_VIWAIT_CALL_ADDR = 0x80023b1c;
static constexpr u32 BRAWL_FRAMEBUFFER_SYNC_VIWAIT_RETURN_ADDR = 0x80023b20;
static constexpr u32 BRAWL_FRAMEBUFFER_SLOT_COUNT_OFFSET = 0x4;
static constexpr u32 BRAWL_FRAMEBUFFER_SLOT_STRIDE = 0x8;
static constexpr u32 BRAWL_FRAMEBUFFER_SLOT_BUSY_OFFSET = 0xc;

void BrawlbackFrameBufferSyncWaitHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (IsShortcutResimulationPass())
  {
    const u32 this_ptr = ppc_state.gpr[31];
    const u8 slot_count = PowerPC::MMU::HostRead<u8>(guard, this_ptr + BRAWL_FRAMEBUFFER_SLOT_COUNT_OFFSET);
    for (u8 i = 0; i < slot_count; ++i)
    {
      const u32 slot_addr = this_ptr + static_cast<u32>(i) * BRAWL_FRAMEBUFFER_SLOT_STRIDE;
      PowerPC::MMU::HostWrite<u32>(guard, 0, slot_addr + BRAWL_FRAMEBUFFER_SLOT_BUSY_OFFSET);
    }
    ppc_state.npc = BRAWL_FRAMEBUFFER_SYNC_VIWAIT_RETURN_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl VIWaitForRetrace` we replaced.
  LR(ppc_state) = BRAWL_FRAMEBUFFER_SYNC_VIWAIT_RETURN_ADDR;
  ppc_state.npc = 0x801e892c;
}

// Resimulated iterations used to take shortcuts: skip the render, skip VIWaitForRetrace
// (bumping __VIRetraceCount by hand), force GX draw-done / DVD / sound-task completions and refuse
// sound allocations. That made a resimulated frame span almost no emulated time and no VI
// interrupt, while the original frame spanned a full field: anything driven by VI interrupts or by
// emulated time (the game's own frame counter, VI callbacks, IO completions feeding file loads,
// the game's other threads) then advanced differently on the peer that rolled back, and the two
// simulations drifted apart. Resimulated frames now run exactly like the original ones (only the
// pad source differs), unthrottled; these shortcuts stay off.
static constexpr bool kResimulationShortcuts = false;

static bool IsShortcutResimulationPass()
{
  return kResimulationShortcuts && IsResimulationPass();
}

bool IsResimulationPass()
{
  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
    return false;

  const int frames_to_advance = NetPlay::GetFramesToAdvance();
  const int current_iteration = NetPlay::GetCurrentIteration();
  return frames_to_advance > 1 && current_iteration != frames_to_advance - 1;
}

// updateLowGC calls PADRead and writes its results into gfPadSystem+0x40. During resimulation,
// that would overwrite GekkoNet's input for the iteration before updateLow queues it, so skip
// only the physical poll in that case. Normal passes still perform the original call and capture
// its completed sample at the following instruction.
static constexpr u32 BRAWL_PAD_UPDATELOWGC_CALL_ADDR = 0x80029464;
static constexpr u32 BRAWL_PAD_UPDATELOWGC_ADDR = 0x80029578;
static constexpr u32 BRAWL_PAD_UPDATELOWGC_RETURN_ADDR = 0x80029468;

void BrawlbackSkipResimPadThreadReadHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();

  // Resimulated frames used to skip the pad read entirely, so the PAD/SI library's own state
  // (transfer timestamps, the thread that services it) and the CPU time the read takes differed
  // from the frame's first run. Read the pads on every pass; the post-read hook then replaces the
  // players' slots with GekkoNet's input either way.
  if (IsShortcutResimulationPass())
  {
    ppc_state.npc = BRAWL_PAD_UPDATELOWGC_RETURN_ADDR;
    return;
  }

  LR(ppc_state) = BRAWL_PAD_UPDATELOWGC_RETURN_ADDR;
  ppc_state.npc = BRAWL_PAD_UPDATELOWGC_ADDR;
}

void BrawlbackCapturePadThreadReadHook(const Core::CPUThreadGuard& guard)
{
  // updateLowGC just wrote the pad sample into gfPadSystem+0x40. On the presented pass that is the
  // live controller: keep it for GekkoNet (submitted at the next frame start, with the session's
  // input delay). Then, on every pass, replace every player's pad with GekkoNet's input for this
  // frame. Otherwise the local player's own game would use the live sample on this frame while
  // the peer (and any later resimulation of this frame) uses the delayed one.
  if (!IsResimulationPass())
  {
    Harness::OnGamePadRead();
    NetPlay::CaptureGekkoPadInput(guard.GetSystem());
  }
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() && NetPlay::IsTimeSynced() &&
      NetPlay::GetFramesToAdvance() > 0)
  {
    NetPlay::InjectPadsForIteration(NetPlay::GetCurrentIteration());
    NetPlay::RecordPadHistory(NetPlay::GetCurrentIteration());
  }
}

// Gameplay-only rollback: the pad thread's loop (0x8002ba84, `lbz r0,0x340(r31)` right after
// `bl updateLow`). P+ replaced updateLow's blr with a branch into its own post-processing, which
// returns here, so this is the first point after every write the pad thread makes to the
// gfPadStatus slots. See Gprb::PadsOnPadThreadUpdated. A Start hook: the original instruction
// runs afterwards.
void GprbPadThreadLoopHook(const Core::CPUThreadGuard& guard)
{
  Gprb::PadsOnPadThreadUpdated(guard.GetSystem());
}

// Brawl's main loop paces the game logic by emulated time: every pass computes r20 = VI fields
// elapsed since the previous pass (1-3), subtracts rate * r20 from an accumulator at
// gfApplication+0xFC (rate = u16 at +0xF8, 60) and then calls gameProc while the accumulator is
// <= 0, adding 60 per call (r24 = calls, 0-3): catch-up frame skipping. A gameplay-only rollback
// session cannot replay that, because it does not rewind emulated time, and two peers with
// different timing would map the same session frame to different game frames. While a session
// drives the loop, every pass runs exactly one logic step (lag slows the game down instead).
//   0x800172d0  mr r3,r26 (before consumeFrameCounter(r20))
//   0x8001730c  lhz r3,0xF8(r23) (before the accumulator update)
// Both are Start hooks; the original instruction runs afterwards.
void GprbPacingElapsedHook(const Core::CPUThreadGuard& guard)
{
  if (Gprb::Session::DrivesLoop())
    guard.GetSystem().GetPPCState().gpr[20] = 1;
}

// mtRand::generate entry, patched only with PPR_GPRB_RNG_LOG: logs every call during a session
// pass (which generator, the caller, the state before the call), to find the first call two
// peers make differently.
void GprbRngTraceHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  const u32 self = ppc.gpr[3];
  const u32 state = PowerPC::MMU::HostRead<u32>(guard, self + 4);  // +0 is the vtable
  // Two callers up the stack: generate is a leaf, so r1 is still its caller's frame.
  std::array<u32, 8> callers{ppc.spr[SPR_LR]};
  u32 sp = ppc.gpr[1];
  for (size_t i = 1; i < callers.size(); ++i)
  {
    if (!PowerPC::MMU::HostIsRAMAddress(guard, sp))
      break;
    sp = PowerPC::MMU::HostRead<u32>(guard, sp);
    if (!PowerPC::MMU::HostIsRAMAddress(guard, sp + 4))
      break;
    callers[i] = PowerPC::MMU::HostRead<u32>(guard, sp + 4);
  }
  Gprb::Session::OnRngCall(self, callers, state);
}

void GprbProbeHook(const Core::CPUThreadGuard& guard)
{
  const auto& ppc = guard.GetSystem().GetPPCState();
  Gprb::Session::OnProbe(ppc.pc, {ppc.gpr[3], ppc.gpr[4], ppc.gpr[5], ppc.gpr[6], ppc.gpr[12],
                                  ppc.spr[SPR_CTR], ppc.spr[SPR_LR]});
}

void GprbPacingStepsHook(const Core::CPUThreadGuard& guard)
{
  if (!Gprb::Session::DrivesLoop())
    return;
  auto& ppc = guard.GetSystem().GetPPCState();
  ppc.gpr[20] = 1;
  const u32 app = ppc.gpr[23];
  const u16 rate = PowerPC::MMU::HostRead<u16>(guard, app + 0xF8);
  // accumulator - rate * 1 == 0 -> exactly one gameProc call, accumulator back to 60.
  PowerPC::MMU::HostWrite<u32>(guard, rate, app + 0xFC);
}

// Render dispatch branch in gfApplication::mainLoopSub (@ 0x80017404), run once per pass through
// the Brawlback-hooked frame loop. Bit 0x10 of the app object's flags byte (+0xed) picks between
// the normal 3D scene render (renderNormal, 0x8001789c) and an overlay/menu render path - both
// converge at 0x8001746c.
//
// Brawlback skipped both paths for resimulated iterations. That is one of the resimulation
// shortcuts that are now off (kResimulationShortcuts): the render pass also updates game memory
// (model and effect state, the frame buffer ring), so skipping it made a resimulated frame differ
// from its first run. Resimulated frames now render in full, and their output is kept off the
// screen at the presenter instead (Core/Rollback/PresentStats.h). With the shortcut off, this hook
// only replicates the instructions it replaces:
//   0x80017404 lbz r0,0xed(r23); 0x80017408 rlwinm. r0,r0,28,31,31; 0x8001740c beq 0x80017464
// It leaves r0 and cr0 unset, which is safe: both targets start with a call (0x80017410 bl,
// 0x80017464 mr r3,r23 / bl renderNormal), and r0 and cr0 are volatile across calls.
static constexpr u32 BRAWL_RENDER_DISPATCH_ADDR = 0x80017404;
static constexpr u32 BRAWL_RENDER_NORMAL_BRANCH_ADDR = 0x80017464;
static constexpr u32 BRAWL_RENDER_ALT_BRANCH_ADDR = 0x80017410;
static constexpr u32 BRAWL_RENDER_DISPATCH_CONVERGE_ADDR = 0x8001746c;
static constexpr u32 BRAWL_APP_FLAGS_OFFSET = 0xed;
static constexpr u8 BRAWL_APP_ALT_RENDER_FLAG = 0x10;

void BrawlbackSkipResimRenderHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (IsShortcutResimulationPass())
  {
    // Not the newest frame this pass - skip straight past both render paths.
    ppc_state.npc = BRAWL_RENDER_DISPATCH_CONVERGE_ADDR;
    return;
  }

  // Replicate the original `lbz r0,0xed(r23); rlwinm. r0,r0,0x1c,0x1f,0x1f; beq ...` we replaced.
  const u32 app_ptr = ppc_state.gpr[23];
  const u8 flags = PowerPC::MMU::HostRead<u8>(guard, app_ptr + BRAWL_APP_FLAGS_OFFSET);
  ppc_state.npc = (flags & BRAWL_APP_ALT_RENDER_FLAG) ? BRAWL_RENDER_ALT_BRANCH_ADDR :
                                                        BRAWL_RENDER_NORMAL_BRANCH_ADDR;
}

// The three `bl detail_AllocXXXSound` call sites inside
// SoundArchivePlayer::detail_SetupSound (snd_SoundArchivePlayer.o). Each hands out a wave/seq/strm
// sound object from a small fixed-size pool that is only restored by the next rollback's
// RollbackManager::LoadFrame, not per resimulation iteration. Letting throwaway resim iterations
// actually consume pool slots exhausts/corrupts the pool within a few frames, after which
// PrepareWaveSoundImpl (and its seq/strm equivalents) runs a virtual call through a stale or
// never-constructed object's vtable and crashes (e.g. the bctrl at 0x801ca1f4). Skip the alloc
// call during resim passes and fake the same "no channel available" (null) return the pool
// already produces when genuinely exhausted - every caller already handles that safely.
static constexpr u32 BRAWL_WAVESOUND_ALLOC_CALL_ADDR = 0x801c9bfc;
static constexpr u32 BRAWL_ALLOC_WAVE_SOUND_ADDR = 0x801cb8ac;
static constexpr u32 BRAWL_SEQSOUND_ALLOC_CALL_ADDR = 0x801c9aac;
static constexpr u32 BRAWL_ALLOC_SEQ_SOUND_ADDR = 0x801cb13c;
static constexpr u32 BRAWL_STRMSOUND_ALLOC_CALL_ADDR = 0x801c9b54;
static constexpr u32 BRAWL_ALLOC_STRM_SOUND_ADDR = 0x801cb4f4;
// detail_SetupSound's success path: `mr r3,r28; mr r4,r24; bl SoundHandle::detail_AttachSound;
// li r3,0` (all three sound types join here).
static constexpr u32 BRAWL_SETUP_SOUND_ATTACH_ADDR = 0x801c9c70;
// nw4r::snd::detail::BasicSound::Stop(int fade_frames) (vtable +0x18 of every sound type); its first
// instruction is `stwu r1, -48(r1)`.
static constexpr u32 BRAWL_BASIC_SOUND_STOP_ADDR = 0x801bc684;

// Start hook at BRAWL_SETUP_SOUND_ATTACH_ADDR: the game's handle (r28) gets the sound (r24) with
// sound id r29. The gameplay session's sound bookkeeping records it.
void GprbStageCreateHook(const Core::CPUThreadGuard& guard)
{
  Gprb::Session::OnStageCreate(guard);
}

void GprbSoundAttachHook(const Core::CPUThreadGuard& guard)
{
  const auto& ppc = guard.GetSystem().GetPPCState();
  Gprb::Session::OnSoundAttached(guard, ppc.gpr[28], ppc.gpr[24], ppc.gpr[29]);
}

// Replace hook at BasicSound::Stop: the sound r3 is not stopped when the gameplay session says it
// has not started yet in the timeline a resimulated pass runs (Gprb::Session::OnSoundStop).
void GprbSoundStopHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc = guard.GetSystem().GetPPCState();
  if (Gprb::Session::OnSoundStop(guard, ppc.gpr[3]))
  {
    ppc.npc = LR(ppc);
    return;
  }
  // Run the replaced instruction, stwu r1, -48(r1), and go on with the function.
  PowerPC::MMU::HostWrite<u32>(guard, ppc.gpr[1], ppc.gpr[1] - 48);
  ppc.gpr[1] -= 48;
  ppc.npc = BRAWL_BASIC_SOUND_STOP_ADDR + 4;
}

void BrawlbackSkipResimSoundAlloc(const Core::CPUThreadGuard& guard, u32 call_addr,
                                  u32 target_addr)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();

  // r8 is the sound id at all three call sites (fn_801C9950: mr r8, r29 before the bl), r28 the
  // game's SoundHandle (detail_SetupSound's second argument).
  const auto decision = Gprb::Session::OnSoundAlloc(guard, ppc_state.gpr[8], ppc_state.gpr[28]);
  if (decision.action == Gprb::Session::SoundAllocAction::Reattach)
  {
    // The sound an earlier run of this frame started is still playing: skip the allocation and
    // the setup and continue at detail_SetupSound's success path, which attaches the handle to it
    // (r28 handle, r24 sound) and returns START_SUCCESS.
    ppc_state.gpr[24] = decision.sound;
    ppc_state.npc = BRAWL_SETUP_SOUND_ATTACH_ADDR;
    return;
  }
  if (IsShortcutResimulationPass() || decision.action == Gprb::Session::SoundAllocAction::NoChannel)
  {
    ppc_state.gpr[3] = 0;
    ppc_state.npc = call_addr + 4;
    return;
  }

  // Not a resim pass: replicate the original `bl` we replaced.
  LR(ppc_state) = call_addr + 4;
  ppc_state.npc = target_addr;
}

void BrawlbackSkipResimWaveSoundAllocHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimSoundAlloc(guard, BRAWL_WAVESOUND_ALLOC_CALL_ADDR, BRAWL_ALLOC_WAVE_SOUND_ADDR);
}

void BrawlbackSkipResimSeqSoundAllocHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimSoundAlloc(guard, BRAWL_SEQSOUND_ALLOC_CALL_ADDR, BRAWL_ALLOC_SEQ_SOUND_ADDR);
}

void BrawlbackSkipResimStrmSoundAllocHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimSoundAlloc(guard, BRAWL_STRMSOUND_ALLOC_CALL_ADDR, BRAWL_ALLOC_STRM_SOUND_ADDR);
}

// Synchronize global PRNG seeds (g_mtRand and g_randSeed) across netplay peers.
// The original game seeds PRNGs at match start (sqMelee::start) and screen transitions using OSGetTick(),
// which returns local hardware CPU tick counts that differ between clients.
// By overriding r3 on calls to srand (0x803f8c5c) and srandi (0x8003fb4c) with the session RTC seed,
// both clients enter matches and random selections with identical PRNG state.
void BrawlbackSyncCharSelectRandomSeedHook(const Core::CPUThreadGuard& guard)
{
  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
    return;

  const u64 session_rtc = NetPlay::GetInitialRTCValue();
  auto& ppc_state = guard.GetSystem().GetPPCState();
  // r3 contains the seed value passed to srand/srandi; override it with our deterministic session RTC seed.
  ppc_state.gpr[3] = static_cast<u32>(session_rtc);
}
}  // namespace HLE_Misc
