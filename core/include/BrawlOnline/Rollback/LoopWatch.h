// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// What the session's loop watch (GameplaySession's WatchLoop) asks the operating system when the
// game has not run a session frame for a second: how much processor time the game's thread got
// meanwhile, and how busy the PC's processors were. A thread that got almost none while the PC was
// busy was starved (other programs took the processors); one that got a full processor ran the
// whole time (a loop in the game, a long host call); one that got none on an idle PC was waiting.

#pragma once

#include <utility>

#include "BrawlOnline/CommonTypes.h"

namespace Gprb::LoopWatch
{
// Marks the calling thread as the game's (its CPU time is then readable from any thread). Cheap
// when it is the thread already marked.
void MarkGameThread();
// The processor time (user and kernel) the marked thread has used so far, in nanoseconds; -1 if
// not known (no thread marked, or the platform cannot say).
s64 GameThreadCpuNs();
// Every processor's time so far, in nanoseconds: (busy, total); total is -1 if not known.
std::pair<s64, s64> SystemCpuNs();
}  // namespace Gprb::LoopWatch
