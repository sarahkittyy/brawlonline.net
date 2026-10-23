// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Harness sessions only: report a crash (unhandled exception) with a symbolized stack on stderr
// and in dolphin.log before the process dies. Automated runs have no debugger attached and no
// crash dialog, so without this a crash is just a dropped connection. Fastmem faults never get
// here: Dolphin's own vectored handler deals with those first. abort() and std::terminate never
// reach the exception filters, so they are reported from their own handlers.

#include "Core/Harness/HarnessInternal.h"

#ifdef _WIN32
#include <Windows.h>
// DbgHelp must come after Windows.h.
#include <DbgHelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#pragma comment(lib, "dbghelp.lib")

namespace Harness::Internal
{
namespace
{
void InitSymbols()
{
  static bool s_done = false;
  if (s_done)
    return;
  s_done = true;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

std::string StackText(CONTEXT ctx)
{
  const HANDLE process = GetCurrentProcess();
  const HANDLE thread = GetCurrentThread();
  std::string out;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 48; ++i)
  {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
    {
      break;
    }
    const DWORD64 pc = frame.AddrPC.Offset;
    if (pc == 0)
      break;
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::string name = "?";
    if (SymFromAddr(process, pc, &disp, sym))
      name = fmt::format("{}+{:#x}", sym->Name, disp);
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    std::string where;
    if (SymGetLineFromAddr64(process, pc, &ldisp, &line))
      where = fmt::format(" ({}:{})", line.FileName, line.LineNumber);
    out += fmt::format("  #{:02} {:#014x} {}{}\n", i, pc, name, where);
  }
  return out;
}

void Report(const std::string& out)
{
  std::fputs(out.c_str(), stderr);
  std::fflush(stderr);
  ERROR_LOG_FMT(HARNESS, "{}", out);
}

constexpr DWORD CXX_EXCEPTION = 0xE06D7363;  // MSVC's throw

std::string ThreadName()
{
  PWSTR desc = nullptr;
  std::string name;
  if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc)
  {
    for (const wchar_t* c = desc; *c; ++c)
      name += *c < 0x80 ? static_cast<char>(*c) : '?';
    LocalFree(desc);
  }
  return name;
}

// A C++ throw: its types (from the ThrowInfo the throw passes, image relative on x64) and, for a
// std::exception, what().
std::string DescribeCxxThrow(const EXCEPTION_RECORD& rec)
{
  if (rec.NumberParameters < 4)
    return "  (no throw info)\n";
  const auto base = static_cast<uintptr_t>(rec.ExceptionInformation[3]);
  const auto* throw_info = reinterpret_cast<const s32*>(rec.ExceptionInformation[2]);
  const auto object = static_cast<uintptr_t>(rec.ExceptionInformation[1]);
  if (!throw_info || !base)
    return "  (no throw info)\n";
  // ThrowInfo: attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray.
  const auto* types = reinterpret_cast<const s32*>(base + static_cast<u32>(throw_info[3]));
  std::string out = "  types:";
  std::exception* as_std = nullptr;
  for (s32 i = 0; i < types[0] && i < 16; ++i)
  {
    // CatchableType: properties, pType, PMD {mdisp, pdisp, vdisp}, size, copy function.
    const auto* ct = reinterpret_cast<const s32*>(base + static_cast<u32>(types[1 + i]));
    // TypeDescriptor: vftable pointer, spare pointer, the decorated name.
    const char* name = reinterpret_cast<const char*>(base + static_cast<u32>(ct[1]) + 16);
    out += fmt::format(" {}", name);
    if (std::string_view(name) == ".?AVexception@std@@" && ct[3] == -1)
      as_std = reinterpret_cast<std::exception*>(object + ct[2]);
  }
  out += "\n";
  if (as_std)
    out += fmt::format("  what(): {}\n", as_std->what());
  return out;
}

void ReportCxxThrow(EXCEPTION_POINTERS* info, const char* when)
{
  static std::atomic<int> s_count{0};
  thread_local bool t_inside = false;
  if (t_inside)
    return;
  const int n = ++s_count;
  if (n > 40)
    return;
  t_inside = true;
  InitSymbols();
  std::string out = fmt::format("HARNESS CXX THROW #{} ({}): thread {} '{}'\n", n, when,
                                GetCurrentThreadId(), ThreadName());
  out += DescribeCxxThrow(*info->ExceptionRecord);
  out += StackText(*info->ContextRecord);
  Report(out);
  t_inside = false;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == CXX_EXCEPTION)
    ReportCxxThrow(info, "unhandled");
  // 0xC0000409: a fast fail / stack buffer overrun; 0xC0000374: heap corruption.
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 && code != 0xC0000374)
  {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static bool s_in_crash = false;
  if (s_in_crash)
    return EXCEPTION_CONTINUE_SEARCH;
  s_in_crash = true;

  InitSymbols();
  std::string out = fmt::format("HARNESS CRASH: exception {:#010x} at {} (thread {})\n",
                                info->ExceptionRecord->ExceptionCode,
                                info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      info->ExceptionRecord->NumberParameters >= 2)
  {
    out += fmt::format("  {} address {:#x}\n",
                       info->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                       info->ExceptionRecord->ExceptionInformation[1]);
  }
  out += StackText(*info->ContextRecord);
  Report(out);
  return EXCEPTION_CONTINUE_SEARCH;
}

// First chance: every C++ throw, caught or not, so the one that kills the process is the last.
LONG WINAPI VectoredFilter(EXCEPTION_POINTERS* info)
{
  if (info->ExceptionRecord->ExceptionCode == CXX_EXCEPTION)
  {
    ReportCxxThrow(info, "first chance");
    return EXCEPTION_CONTINUE_SEARCH;
  }
  return CrashFilter(info);
}

// A death that skips the exception filters: the stack where it began.
void ReportFromHere(const char* what)
{
  static bool s_reported = false;
  if (s_reported)
    return;
  s_reported = true;
  InitSymbols();
  CONTEXT ctx{};
  RtlCaptureContext(&ctx);
  Report(fmt::format("HARNESS CRASH: {} (thread {})\n", what, GetCurrentThreadId()) +
         StackText(ctx));
}

void OnAbort(int)
{
  ReportFromHere("abort (SIGABRT)");
}

void OnTerminate()
{
  ReportFromHere("std::terminate");
  std::abort();
}
}  // namespace

void InstallCrashReporter()
{
  // Last in the vectored chain: called only for exceptions that Dolphin's own handlers (fastmem,
  // installed first) did not resolve. Something replaces the unhandled-exception filter later.
  AddVectoredExceptionHandler(0, VectoredFilter);
  SetUnhandledExceptionFilter(CrashFilter);
  std::signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);
}
}  // namespace Harness::Internal

#else

namespace Harness::Internal
{
void InstallCrashReporter()
{
}
}  // namespace Harness::Internal

#endif
