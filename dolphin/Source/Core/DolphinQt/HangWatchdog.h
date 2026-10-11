// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"

class QTimer;

// Notices a frozen UI: the UI thread not getting back to its event loop for HANG_SECONDS (the
// window is "Not responding"; a player closing it is how Dolphin ends with 0xCFFFFFFF). On
// Windows it writes Logs/hang-<time>.dmp, a minidump with every thread's stack, and
// Logs/hang-<time>.txt, the UI thread's stack, which also goes in dolphin.log; the launcher's
// "Download logs" sends the Logs folder. One dump a run (the two newest are kept); a UI that
// recovers is logged too. Elsewhere only the log lines.
//
// Made on the UI thread before the main window (its constructor can hang too) and stopped once
// the event loop has returned (shutdown runs without one).
class HangWatchdog final
{
public:
  static constexpr int HANG_SECONDS = 8;

  HangWatchdog();
  ~HangWatchdog();
  HangWatchdog(const HangWatchdog&) = delete;
  HangWatchdog& operator=(const HangWatchdog&) = delete;

  void Stop();

#ifdef _WIN32
  // `Dolphin.exe --write-hang-dump <pid> <tid> <dump path> <stack path>`: writes the minidump of
  // process <pid> and the stack of its thread <tid>. Done from a second process, as nothing in
  // the hung one can be relied on: MiniDumpWriteDump or symbol loading in the process itself can
  // block on a lock (the heap's, the loader's) that the hung thread holds.
  static constexpr const char* DUMP_HELPER_SWITCH = "--write-hang-dump";
  static int RunDumpHelper(const std::vector<std::string>& args);
#endif

private:
  void Run();
  void OnHang();

  std::unique_ptr<QTimer> m_timer;
  std::atomic<u64> m_beats{0};
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop = false;
  std::thread m_thread;
#ifdef _WIN32
  u32 m_ui_thread_id = 0;
  bool m_dumped = false;
#endif
};
