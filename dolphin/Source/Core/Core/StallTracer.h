// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Emulation stalls: an emulated field that does not come for STALL_MS while the emulation runs
// (a player sees the game freeze; a rollback peer waits for this machine). A monitor thread
// notices it and logs, rate-limited:
// - once the stall has lasted STALL_MS: each registered thread (the CPU, GPU and UI threads) as
//   it is right then, sampled without locks: its instruction pointer and the words on its stack
//   that point into a loaded module (Windows; elsewhere the CPU time only);
// - when the next field comes (also for a stall it could not see, the whole process stopped):
//   how long the stall was and how much CPU time each thread used
//   during it. Little CPU time means the OS did not run the process (a busy machine); a thread
//   that kept a core busy was working through it (emulation, a shader compile, a lock spin).
// Staging feedback 2026-10-12: second-long freezes on one machine only, with nothing in the log.

#pragma once

namespace Core::StallTracer
{
constexpr int STALL_MS = 250;

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

// The monitor thread, for the emulation's lifetime (EmuThread).
void Start();
void Stop();
}  // namespace Core::StallTracer
