// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CrashReporter.h"

#ifdef _WIN32
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <intrin.h>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/StringUtil.h"

#include "DolphinQt/HangWatchdog.h"

namespace CrashReporter
{
namespace
{
constexpr DWORD CXX_EXCEPTION = 0xE06D7363;  // MSVC's throw
constexpr ULONG_PTR CXX_MAGIC = 0x19930520;
constexpr size_t KEPT_DUMPS = 2;

// The last C++ exception thrown on each thread, kept at the throw (the first chance): by the time
// std::terminate or abort() runs, nothing says what it was. Fixed buffers: the throw may be a
// std::bad_alloc.
struct LastThrow
{
  char types[256];
  char what[512];
};
thread_local LastThrow t_last_throw{};

// Set at Install: nothing is allocated while crashing.
wchar_t s_exe[2048];
wchar_t s_logs[2048];
std::atomic<bool> s_reported{false};
LPTOP_LEVEL_EXCEPTION_FILTER s_previous_filter = nullptr;
using SignalHandler = void (*)(int);
SignalHandler s_previous_abort = nullptr;

void Append(char* buffer, size_t size, const char* text)
{
  const size_t used = std::strlen(buffer);
  if (used + 1 < size)
    strncat_s(buffer, size, text, _TRUNCATE);
}

// The ThrowInfo a throw passes (image relative on x64): its types' decorated names
// (".?AVbad_alloc@std@@") and, for a std::exception, what(). No C++ objects here: __try.
void RecordThrow(const EXCEPTION_RECORD* rec)
{
  // A rethrow (`throw;`) passes no ThrowInfo: the one recorded is still the exception.
  if (rec->NumberParameters < 4 || rec->ExceptionInformation[0] != CXX_MAGIC ||
      !rec->ExceptionInformation[2])
  {
    return;
  }
  LastThrow& last = t_last_throw;
  last.types[0] = '\0';
  last.what[0] = '\0';
  __try
  {
    const auto base = static_cast<uintptr_t>(rec->ExceptionInformation[3]);
    const auto* throw_info = reinterpret_cast<const INT32*>(rec->ExceptionInformation[2]);
    const auto object = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
    // ThrowInfo: attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray.
    const auto* types = reinterpret_cast<const INT32*>(base + static_cast<UINT32>(throw_info[3]));
    const std::exception* as_std = nullptr;
    for (INT32 i = 0; i < types[0] && i < 8; ++i)
    {
      // CatchableType: properties, pType, PMD {mdisp, pdisp, vdisp}, size, copy function.
      const auto* ct = reinterpret_cast<const INT32*>(base + static_cast<UINT32>(types[1 + i]));
      // TypeDescriptor: vftable pointer, spare pointer, the decorated name.
      const char* name = reinterpret_cast<const char*>(base + static_cast<UINT32>(ct[1]) + 16);
      if (i > 0)
        Append(last.types, sizeof(last.types), " ");
      Append(last.types, sizeof(last.types), name);
      if (std::strcmp(name, ".?AVexception@std@@") == 0 && ct[3] == -1)
        as_std = reinterpret_cast<const std::exception*>(object + ct[2]);
    }
    if (as_std)
      strncpy_s(last.what, as_std->what(), _TRUNCATE);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    Append(last.types, sizeof(last.types), " (unreadable)");
  }
}

LONG WINAPI OnFirstChance(EXCEPTION_POINTERS* info)
{
  if (info->ExceptionRecord->ExceptionCode == CXX_EXCEPTION)
    RecordThrow(info->ExceptionRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

void ThreadName(char* out, size_t size)
{
  out[0] = '\0';
  PWSTR description = nullptr;
  if (FAILED(GetThreadDescription(GetCurrentThread(), &description)) || !description)
    return;
  size_t n = 0;
  for (const wchar_t* c = description; *c && n + 1 < size; ++c)
    out[n++] = *c < 0x80 ? static_cast<char>(*c) : '?';
  out[n] = '\0';
  LocalFree(description);
}

// "Dolphin.exe+0x1234" for an address in a module.
void ModuleOffset(const void* address, char* out, size_t size)
{
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH];
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(address), &module) ||
      !GetModuleFileNameW(module, path, MAX_PATH))
  {
    std::snprintf(out, size, "%p", address);
    return;
  }
  const wchar_t* name = std::wcsrchr(path, L'\\');
  name = name ? name + 1 : path;
  std::snprintf(out, size, "%ls+0x%llx", name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                reinterpret_cast<uintptr_t>(module)));
}

struct Crash
{
  const char* text;
  const char* helper;
  const wchar_t* base;
};
Crash s_crash{};

// What happened and the crashing thread's stack (the helper's .txt): to dolphin.log, flushed, and
// at the top of the .txt.
DWORD WINAPI LogCrash(void*)
{
  try
  {
    const std::string txt = WStringToUTF8(s_crash.base) + ".txt";
    std::string stack;
    if (!File::ReadFileToString(txt, stack))
      stack = "(none)\n";
    File::WriteStringToFile(txt, fmt::format("{}\n{}.\n{}", s_crash.text, s_crash.helper, stack));
    ERROR_LOG_FMT(CORE, "{} {}; the crashing thread's stack:\n{}", s_crash.text, s_crash.helper,
                  stack);
    if (auto* log = Common::Log::LogManager::GetInstance())
      log->Flush();
  }
  catch (...)
  {
  }
  return 0;
}

// Has the dump helper write the dump and the crashing thread's stack, then logs what happened with
// that stack. Once a process: the handlers chain (an unhandled C++ exception goes on to
// std::terminate and abort()).
void Report(const char* what, EXCEPTION_POINTERS* info)
{
  if (s_reported.exchange(true))
    return;

  SYSTEMTIME now;
  GetLocalTime(&now);
  static wchar_t base[2200];
  swprintf_s(base, L"%lscrash-%04u%02u%02u-%02u%02u%02u", s_logs, now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond);

  char thread_name[64];
  ThreadName(thread_name, sizeof(thread_name));
  static char text[2048];
  std::snprintf(text, sizeof(text),
                "Dolphin crashed: %s, on thread %lu '%s'. The last C++ exception thrown on this "
                "thread: %s%s%s%s. Its dump: crash-%04u%02u%02u-%02u%02u%02u.dmp in the Logs "
                "folder;",
                what, GetCurrentThreadId(), thread_name,
                t_last_throw.types[0] ? t_last_throw.types : "none",
                t_last_throw.what[0] ? " (what(): \"" : "", t_last_throw.what,
                t_last_throw.what[0] ? "\")" : "", now.wYear, now.wMonth, now.wDay, now.wHour,
                now.wMinute, now.wSecond);

  static wchar_t command_line[8192];
  swprintf_s(command_line, L"\"%ls\" %hs %lu %lu \"%ls.dmp\" \"%ls.txt\" %llx", s_exe,
             HangWatchdog::DUMP_HELPER_SWITCH, GetCurrentProcessId(), GetCurrentThreadId(), base,
             base, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info)));
  STARTUPINFOW startup_info{.cb = sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process_info{};
  const char* helper = "the dump helper did not start";
  if (CreateProcessW(nullptr, command_line, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                     nullptr, &startup_info, &process_info))
  {
    CloseHandle(process_info.hThread);
    if (WaitForSingleObject(process_info.hProcess, 60'000) == WAIT_OBJECT_0)
    {
      helper = "the dump helper finished";
    }
    else
    {
      TerminateProcess(process_info.hProcess, 1);
      helper = "the dump helper took over 60 s and was stopped";
    }
    CloseHandle(process_info.hProcess);
  }

  s_crash = {text, helper, base};
  // On a thread of its own, for at most a few seconds: the crashing thread may hold the log's lock
  // (or the heap's), and a crash must not become a frozen window.
  if (const HANDLE thread = CreateThread(nullptr, 0, LogCrash, nullptr, 0, nullptr))
  {
    WaitForSingleObject(thread, 10'000);
    CloseHandle(thread);
  }
}

const char* ExceptionName(DWORD code)
{
  switch (code)
  {
  case EXCEPTION_ACCESS_VIOLATION:
    return "access violation";
  case EXCEPTION_STACK_OVERFLOW:
    return "stack overflow";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "illegal instruction";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "integer divide by zero";
  case EXCEPTION_PRIV_INSTRUCTION:
    return "privileged instruction";
  case EXCEPTION_IN_PAGE_ERROR:
    return "in-page error (a file mapped in could not be read)";
  case CXX_EXCEPTION:
    return "an unhandled C++ exception";
  case 0xC0000374:
    return "heap corruption";
  case 0xC0000409:
    return "fast fail";
  default:
    return "exception";
  }
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info)
{
  const EXCEPTION_RECORD& rec = *info->ExceptionRecord;
  char where[300];
  ModuleOffset(rec.ExceptionAddress, where, sizeof(where));
  char what[512];
  std::snprintf(what, sizeof(what), "%s (0x%08lx) at %s", ExceptionName(rec.ExceptionCode),
                rec.ExceptionCode, where);
  if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2)
  {
    char access[96];
    std::snprintf(access, sizeof(access), ", %s 0x%llx",
                  rec.ExceptionInformation[0] == 1 ? "writing" :
                  rec.ExceptionInformation[0] == 8 ? "executing" :
                                                     "reading",
                  static_cast<unsigned long long>(rec.ExceptionInformation[1]));
    Append(what, sizeof(what), access);
  }
  Report(what, info);
  return s_previous_filter ? s_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int sig)
{
  Report("abort() (std::terminate ends in it: an exception that left a thread or a noexcept "
         "function)",
         nullptr);
  if (s_previous_abort && s_previous_abort != SIG_DFL && s_previous_abort != SIG_IGN)
    s_previous_abort(sig);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
  Report("the C runtime was passed an invalid parameter", nullptr);
  // What the C runtime's own handler does in a release build.
  __fastfail(FAST_FAIL_INVALID_ARG);
}

// The newest KEPT_DUMPS - 1 stay, leaving room for this run's. The launcher's crash-<time>.log
// copies share the prefix: only .dmp and their .txt are this one's.
void CleanUpOldDumps()
{
  std::error_code error;
  std::vector<std::filesystem::path> dumps;
  for (const auto& entry : std::filesystem::directory_iterator(
           StringToPath(File::GetUserPath(D_LOGS_IDX)), error))
  {
    const std::string name = PathToString(entry.path().filename());
    if (!name.starts_with("crash-"))
      continue;
    if (name.ends_with(".dmp.part"))
      std::filesystem::remove(entry.path(), error);
    else if (name.ends_with(".dmp"))
      dumps.push_back(entry.path());
  }
  std::ranges::sort(dumps);
  for (size_t i = 0; i + (KEPT_DUMPS - 1) < dumps.size(); ++i)
  {
    std::filesystem::remove(dumps[i], error);
    std::filesystem::remove(std::filesystem::path(dumps[i]).replace_extension(".txt"), error);
  }
}
}  // namespace

void Install()
{
  static bool s_installed = false;
  if (s_installed)
    return;
  s_installed = true;

  const std::wstring exe = UTF8ToWString(File::GetExePath());
  const std::wstring logs = UTF8ToWString(File::GetUserPath(D_LOGS_IDX));
  wcsncpy_s(s_exe, exe.c_str(), _TRUNCATE);
  wcsncpy_s(s_logs, logs.c_str(), _TRUNCATE);
  try
  {
    CleanUpOldDumps();
  }
  catch (...)
  {
  }

  // Last in the vectored chain: after fastmem's, which resolves its own faults first.
  AddVectoredExceptionHandler(0, OnFirstChance);
  s_previous_filter = SetUnhandledExceptionFilter(OnUnhandled);
  const SignalHandler previous_abort = std::signal(SIGABRT, OnAbort);
  if (previous_abort != SIG_ERR)
    s_previous_abort = previous_abort;
  _set_invalid_parameter_handler(OnInvalidParameter);
}
}  // namespace CrashReporter

#else

namespace CrashReporter
{
void Install()
{
}
}  // namespace CrashReporter

#endif
