// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Emulation stalls: an emulated field that does not come for STALL_MS while the emulation runs
// (a player sees the game freeze; a rollback peer waits for this machine). A monitor thread
// notices it and logs, rate-limited:
// - once the stall has lasted STALL_MS, and again at LONG_STALL_MS steps while it lasts: each
//   registered thread (the CPU, GPU and UI threads) as it is right then, sampled without locks:
//   its instruction pointer and the words on its stack that point into a loaded module's code
//   (Windows: suspended; Linux x86-64 / arm64: a signal whose handler copies them, and the
//   thread's state, kernel wait channel and syscall from /proc; elsewhere the CPU time only);
// - when the next field comes (also for a stall it could not see, the whole process stopped):
//   how long the stall was and how much CPU time each thread used during it. A thread that kept
//   a core busy was working through it (emulation, a shader compile, a lock spin); little CPU
//   time on every thread means they all waited: for something (the sample's stacks show a wait
//   function and who called it) or for the OS to run them at all (a busy machine: the stacks
//   show ordinary work, and the process got no CPU).
// Staging feedback 2026-10-12: second-long freezes on one machine only, with nothing in the log.
//
// A stop that does not finish: when the emulation has not stopped STOP_HANG_MS after a stop was
// asked for (the player closed the game window of a frozen game), the threads are traced once
// more and Dolphin exits with STOP_HANG_EXIT_CODE. Player report 2026-10-14 (Linux, AMD): the
// GPU thread kept a core busy and no field came again; closing the window did nothing and the
// player had to force quit, three times.

#pragma once

namespace Core::StallTracer
{
constexpr int STALL_MS = 250;
constexpr int LONG_STALL_MS = 2000;  // traced again at 2 s, 8 s and 32 s
constexpr int STOP_HANG_MS = 10000;
constexpr int STOP_HANG_EXIT_CODE = 70;

enum class Role
{
  CPU = 0,  // the CPU thread (CPU-GPU in single core)
  GPU = 1,  // the video thread (dual core)
  UI = 2,   // the host's thread that started the emulation
};

// The calling thread, as `role`, until UnregisterThread from the same thread.
void RegisterThread(Role role);
void UnregisterThread(Role role);

// The CPU thread at every emulated field (Callback_NewField). Lock-free.
void OnField();

// Core::Stop, before anything that could wait: starts the STOP_HANG_MS countdown (the first
// call of a run; the monitor ends with the emulation). Lock-free.
void OnStopRequested();

// The monitor thread, for the emulation's lifetime (EmuThread).
void Start();
void Stop();
}  // namespace Core::StallTracer
