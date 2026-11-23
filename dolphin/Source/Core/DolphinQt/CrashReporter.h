// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A crash on Windows (an unhandled exception, abort() and so std::terminate, which an exception
// that leaves a std::thread ends in, or a C runtime "invalid parameter") writes what it was, the
// last C++ exception thrown on the crashing thread and the crashing thread's stack to dolphin.log,
// flushes the log, and writes Logs/crash-<time>.dmp (every thread's stack) and .txt (the crashing
// thread's), through the hang watchdog's dump helper (a second Dolphin.exe). The launcher keeps a
// crashed run's dolphin.log, and a bug report sends the Logs folder. Before this a crash ended the
// process with c0000409 or e06d7363 and the log stopped where its last write had got to, seconds
// before (bug report #324: four crashes in REX rooms, nothing in the logs about any of them).
// Elsewhere nothing (Linux has its own signal handler in Main.cpp).
namespace CrashReporter
{
// After the User folder is set (the dumps go in its Logs folder). Chains to the handlers that
// were there (the harness's).
void Install();
}  // namespace CrashReporter
