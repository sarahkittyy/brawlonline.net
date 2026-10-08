// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Harness sessions only: report a crash (unhandled exception) with a symbolized stack on stderr
// and in dolphin.log before the process dies. Automated runs have no debugger attached and no
// crash dialog, so without this a crash is just a dropped connection. Fastmem faults never get
// here: Dolphin's own vectored handler deals with those first.

#include "Core/Harness/HarnessInternal.h"

#ifdef _WIN32
#include <Windows.h>
// DbgHelp must come after Windows.h.
#include <DbgHelp.h>

#include <cstdio>
#include <string>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

#pragma comment(lib, "dbghelp.lib")

namespace Harness::Internal
{
namespace
{
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409)
  {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static bool s_in_crash = false;
  if (s_in_crash)
    return EXCEPTION_CONTINUE_SEARCH;
  s_in_crash = true;

  const HANDLE process = GetCurrentProcess();
  const HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(process, nullptr, TRUE);

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

  CONTEXT ctx = *info->ContextRecord;
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
  std::fputs(out.c_str(), stderr);
  std::fflush(stderr);
  ERROR_LOG_FMT(HARNESS, "{}", out);
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashReporter()
{
  // Last in the vectored chain: called only for exceptions that Dolphin's own handlers (fastmem,
  // installed first) did not resolve. Something replaces the unhandled-exception filter later.
  AddVectoredExceptionHandler(0, CrashFilter);
  SetUnhandledExceptionFilter(CrashFilter);
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
