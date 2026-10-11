// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/HangWatchdog.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <system_error>

#include <QTimer>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/TimeUtil.h"

#ifdef _WIN32
#include <Windows.h>
// DbgHelp must come after Windows.h.
#include <DbgHelp.h>

#pragma comment(lib, "dbghelp.lib")
#endif

namespace
{
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);
constexpr int HANG_POLLS = HangWatchdog::HANG_SECONDS * 2;
constexpr int BEAT_INTERVAL_MS = 250;

#ifdef _WIN32
constexpr size_t KEPT_DUMPS = 2;

// Removes leftover .part files (a helper killed mid-write) and old dumps with their stacks. The
// names sort by time.
void CleanUpOldDumps()
{
  std::error_code error;
  std::vector<std::filesystem::path> dumps;
  for (const auto& entry : std::filesystem::directory_iterator(
           StringToPath(File::GetUserPath(D_LOGS_IDX)), error))
  {
    const std::string name = PathToString(entry.path().filename());
    if (!name.starts_with("hang-"))
      continue;
    if (name.ends_with(".part"))
      std::filesystem::remove(entry.path(), error);
    else if (name.ends_with(".dmp"))
      dumps.push_back(entry.path());
  }
  // The newest KEPT_DUMPS - 1 stay, leaving room for this run's.
  std::ranges::sort(dumps);
  for (size_t i = 0; i + (KEPT_DUMPS - 1) < dumps.size(); ++i)
  {
    std::filesystem::remove(dumps[i], error);
    std::filesystem::remove(std::filesystem::path(dumps[i]).replace_extension(".txt"), error);
  }
}

std::string WalkStack(HANDLE process, HANDLE thread)
{
  CONTEXT context{};
  context.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(thread) == static_cast<DWORD>(-1))
    return fmt::format("(SuspendThread failed: {})\n", GetLastError());
  const BOOL got_context = GetThreadContext(thread, &context);
  ResumeThread(thread);
  if (!got_context)
    return fmt::format("(GetThreadContext failed: {})\n", GetLastError());

  STACKFRAME64 frame{};
#if defined(_M_ARM64)
  constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
  frame.AddrPC.Offset = context.Pc;
  frame.AddrFrame.Offset = context.Fp;
  frame.AddrStack.Offset = context.Sp;
#else
  constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = context.Rip;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrStack.Offset = context.Rsp;
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  if (!SymInitialize(process, nullptr, TRUE))
    return fmt::format("(SymInitialize failed: {})\n", GetLastError());

  // Module+offset for every frame (an offset in Dolphin.exe is looked up in the build's PDB);
  // a name where the module has one (exports, or symbols where there are some).
  std::string out;
  alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 64; ++i)
  {
    if (!StackWalk64(machine, process, thread, &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
    {
      break;
    }
    const DWORD64 pc = frame.AddrPC.Offset;
    if (pc == 0)
      break;
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    std::string where = fmt::format("{:#x}", pc);
    if (SymGetModuleInfo64(process, pc, &module))
      where = fmt::format("{}+{:#x}", module.ModuleName, pc - module.BaseOfImage);
    auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, pc, &displacement, symbol))
      where += fmt::format(" ({}+{:#x})", symbol->Name, displacement);
    out += fmt::format("  #{:02} {}\n", i, where);
  }
  SymCleanup(process);
  return out.empty() ? "(no frames)\n" : out;
}
#endif
}  // namespace

HangWatchdog::HangWatchdog()
{
#ifdef _WIN32
  m_ui_thread_id = GetCurrentThreadId();
#endif
  m_timer = std::make_unique<QTimer>();
  m_timer->setTimerType(Qt::CoarseTimer);
  QObject::connect(m_timer.get(), &QTimer::timeout, m_timer.get(),
                   [this] { m_beats.fetch_add(1, std::memory_order_relaxed); });
  m_timer->start(BEAT_INTERVAL_MS);
  m_thread = std::thread(&HangWatchdog::Run, this);
}

HangWatchdog::~HangWatchdog()
{
  Stop();
}

void HangWatchdog::Stop()
{
  {
    std::lock_guard lk(m_mutex);
    m_stop = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();
  m_timer.reset();
}

void HangWatchdog::Run()
{
  Common::SetCurrentThreadName("Hang Watchdog");
#ifdef _WIN32
  CleanUpOldDumps();
#endif

  // Polls, not time: a machine waking from sleep has its clock jump but has missed no beats.
  u64 last_beats = m_beats.load(std::memory_order_relaxed);
  int stalled_polls = 0;
  std::unique_lock lk(m_mutex);
  while (!m_cv.wait_for(lk, POLL_INTERVAL, [this] { return m_stop; }))
  {
    const u64 beats = m_beats.load(std::memory_order_relaxed);
    if (beats != last_beats)
    {
      if (stalled_polls >= HANG_POLLS)
      {
        NOTICE_LOG_FMT(CORE, "Hang watchdog: the UI thread is running again after about {} s",
                       stalled_polls * POLL_INTERVAL.count() / 1000);
      }
      last_beats = beats;
      stalled_polls = 0;
      continue;
    }
    if (++stalled_polls == HANG_POLLS)
    {
      lk.unlock();
      OnHang();
      lk.lock();
    }
  }
}

void HangWatchdog::OnHang()
{
#ifdef _WIN32
  // Stopped in a debugger, not hung.
  if (IsDebuggerPresent())
    return;
  if (m_dumped)
  {
    WARN_LOG_FMT(CORE,
                 "Hang watchdog: the UI thread has not run its event loop for {} s (the window "
                 "is not responding); this run's dump is already written",
                 HANG_SECONDS);
    return;
  }
  m_dumped = true;

  const auto now = Common::LocalTime(std::time(nullptr));
  const std::string base = File::GetUserPath(D_LOGS_IDX) +
                           (now ? fmt::format("hang-{:%Y%m%d-%H%M%S}", *now) : "hang");
  const std::string dump_path = base + ".dmp";
  const std::string stack_path = base + ".txt";

  std::wstring command_line = UTF8ToWString(
      fmt::format("\"{}\" {} {} {} \"{}\" \"{}\"", File::GetExePath(), DUMP_HELPER_SWITCH,
                  GetCurrentProcessId(), m_ui_thread_id, dump_path, stack_path));
  STARTUPINFOW startup_info{.cb = sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process_info{};
  std::string helper_result;
  if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                     nullptr, nullptr, &startup_info, &process_info))
  {
    CloseHandle(process_info.hThread);
    if (WaitForSingleObject(process_info.hProcess, 60'000) != WAIT_OBJECT_0)
    {
      TerminateProcess(process_info.hProcess, 1);
      helper_result = "the dump helper took over 60 s and was stopped; ";
    }
    CloseHandle(process_info.hProcess);
  }
  else
  {
    helper_result = fmt::format("the dump helper did not start ({}); ", GetLastError());
  }

  std::string stack;
  if (!File::ReadFileToString(stack_path, stack))
    stack = "(none)\n";
  WARN_LOG_FMT(CORE,
               "Hang watchdog: the UI thread has not run its event loop for {} s (the window is "
               "not responding); {}dump: {}. The UI thread's stack:\n{}",
               HANG_SECONDS, helper_result, File::Exists(dump_path) ? dump_path : "not written",
               stack);
#else
  WARN_LOG_FMT(CORE,
               "Hang watchdog: the UI thread has not run its event loop for {} s (the window is "
               "not responding)",
               HANG_SECONDS);
#endif
}

#ifdef _WIN32
int HangWatchdog::RunDumpHelper(const std::vector<std::string>& args)
{
  DWORD pid = 0;
  DWORD tid = 0;
  if (args.size() != 6 || !TryParse(args[2], &pid) || !TryParse(args[3], &tid))
    return 1;
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
  if (!process)
    return 2;

  // The dump first: it is what matters, and the stack walk below can be slow (symbol loading).
  int result = 0;
  const std::wstring dump_path = UTF8ToWString(args[4]);
  const std::wstring part_path = dump_path + L".part";
  const HANDLE file = CreateFileW(part_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE)
  {
    // Every thread's stack and the memory its pointers lead to, thread times and handles (what
    // a deadlock waits on); no heap. A few MB.
    const auto type =
        static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                   MiniDumpWithHandleData | MiniDumpWithUnloadedModules);
    const BOOL written = MiniDumpWriteDump(process, pid, file, type, nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (!written || !MoveFileExW(part_path.c_str(), dump_path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
      DeleteFileW(part_path.c_str());
      result = 3;
    }
  }
  else
  {
    result = 3;
  }

  const HANDLE thread =
      OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  const std::string stack = thread ? WalkStack(process, thread) :
                                     fmt::format("(OpenThread failed: {})\n", GetLastError());
  if (thread)
    CloseHandle(thread);
  File::WriteStringToFile(args[5], stack);
  CloseHandle(process);
  return result;
}
#endif
