// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MemoryUtil.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <fmt/format.h>

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#ifdef _WIN32
#include <windows.h>
#include "Common/StringUtil.h"
#else
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(_M_ARM_64) && defined(__APPLE__)
#include <pthread.h>
#endif
#if defined __APPLE__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
#include <sys/sysctl.h>
#elif defined __HAIKU__
#include <OS.h>
#else
#include <sys/sysinfo.h>
#endif
#endif

namespace Common
{
#if defined(_WIN32) && defined(_M_X86_64)
namespace
{
// After each executable block (past the size its caller asked for): a function table of one entry
// covering the block, its unwind info (no unwind codes, an exception handler) and a jump to that
// handler, which an unwind info names by its offset from the block.
struct UnwindTail
{
  RUNTIME_FUNCTION function;
  // UNWIND_INFO: version 1, UNW_FLAG_EHANDLER; no prolog, no unwind codes, no frame register.
  u8 unwind_info[4];
  u32 handler_offset;
  u32 padding;
  // jmp [rip + 0], then the handler's address.
  u8 thunk[6];
  u8 handler_address[8];
};

std::atomic<ExecutableMemoryExceptionHandler> s_exception_handler{nullptr};

size_t UnwindTailOffset(size_t size)
{
  return (size + 15) & ~size_t{15};
}

EXCEPTION_DISPOSITION ExecutableMemoryFrameHandler(EXCEPTION_RECORD* record, ULONG64,
                                                   CONTEXT* context, DISPATCHER_CONTEXT*)
{
  // Only while looking for a handler (not unwinding: nothing past here gets that far).
  if (!(record->ExceptionFlags & EXCEPTION_UNWIND))
  {
    if (const ExecutableMemoryExceptionHandler handler = s_exception_handler.load())
    {
      EXCEPTION_POINTERS pointers{record, context};
      handler(&pointers);
    }
  }
  return ExceptionContinueSearch;
}

// Gives the block the unwind tail. With no unwind codes, an unwind from the block's code goes on
// as it did with no function table (the return address at the stack pointer).
void AddUnwindTail(u8* block, size_t size)
{
  auto* tail = reinterpret_cast<UnwindTail*>(block + UnwindTailOffset(size));
  const auto offset = [block](const void* at) {
    return static_cast<DWORD>(static_cast<const u8*>(at) - block);
  };
  tail->unwind_info[0] = 1 | (UNW_FLAG_EHANDLER << 3);
  tail->handler_offset = offset(tail->thunk);
  tail->thunk[0] = 0xFF;
  tail->thunk[1] = 0x25;
  std::memset(tail->thunk + 2, 0, 4);
  const auto handler = reinterpret_cast<uintptr_t>(&ExecutableMemoryFrameHandler);
  std::memcpy(tail->handler_address, &handler, sizeof(handler));
  tail->function.BeginAddress = 0;
  tail->function.EndAddress = static_cast<DWORD>(size);
  tail->function.UnwindData = offset(tail->unwind_info);
  if (!RtlAddFunctionTable(&tail->function, 1, reinterpret_cast<DWORD64>(block)))
    WARN_LOG_FMT(COMMON, "Could not add the function table of executable memory at {}",
                 fmt::ptr(block));
}

// Removes the block's function table, if it has one (memory that is not executable has none).
void RemoveUnwindTail(void* block, size_t size)
{
  auto* tail = reinterpret_cast<UnwindTail*>(static_cast<u8*>(block) + UnwindTailOffset(size));
  DWORD64 base = 0;
  if (RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(block), &base, nullptr) ==
          &tail->function &&
      base == reinterpret_cast<DWORD64>(block))
  {
    RtlDeleteFunctionTable(&tail->function);
  }
}
}  // namespace

void SetExecutableMemoryExceptionHandler(ExecutableMemoryExceptionHandler handler)
{
  s_exception_handler.store(handler);
}
#endif

// This is purposely not a full wrapper for virtualalloc/mmap, but it
// provides exactly the primitive operations that Dolphin needs.

void* AllocateExecutableMemory(size_t size)
{
#if defined(_WIN32) && defined(_M_X86_64)
  void* ptr = VirtualAlloc(nullptr, UnwindTailOffset(size) + sizeof(UnwindTail), MEM_COMMIT,
                           PAGE_EXECUTE_READWRITE);
  if (ptr)
    AddUnwindTail(static_cast<u8*>(ptr), size);
#elif defined(_WIN32)
  void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
  int map_flags = MAP_ANON | MAP_PRIVATE;
#if defined(__APPLE__)
  map_flags |= MAP_JIT;
#endif
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, map_flags, -1, 0);
  if (ptr == MAP_FAILED)
    ptr = nullptr;
#endif

  if (ptr == nullptr)
    PanicAlertFmt("Failed to allocate executable memory");

  return ptr;
}
// This function is used to provide a counter for the JITPageWrite*Execute*
// functions to enable nesting. The static variable is wrapped in a a function
// to allow those functions to be called inside of the constructor of a static
// variable portably.
//
// The variable is thread_local as the W^X mode is specific to each running thread.
static int& JITPageWriteNestCounter()
{
  static thread_local int nest_counter = 0;
  return nest_counter;
}

// Certain platforms (Mac OS on ARM) enforce that a single thread can only have write or
// execute permissions to pages at any given point of time. The two below functions
// are used to toggle between having write permissions or execute permissions.
//
// The default state of these allocations in Dolphin is for them to be executable,
// but not writeable. So, functions that are updating these pages should wrap their
// writes like below:

// JITPageWriteEnableExecuteDisable();
// PrepareInstructionStreamForJIT();
// JITPageWriteDisableExecuteEnable();

// These functions can be nested, in which case execution will only be enabled
// after the call to the JITPageWriteDisableExecuteEnable from the top most
// nesting level. Example:

// [JIT page is in execute mode for the thread]
// JITPageWriteEnableExecuteDisable();
//   [JIT page is in write mode for the thread]
//   JITPageWriteEnableExecuteDisable();
//     [JIT page is in write mode for the thread]
//   JITPageWriteDisableExecuteEnable();
//   [JIT page is in write mode for the thread]
// JITPageWriteDisableExecuteEnable();
// [JIT page is in execute mode for the thread]

// Allows a thread to write to executable memory, but not execute the data.
void JITPageWriteEnableExecuteDisable()
{
#if defined(_M_ARM_64) && defined(__APPLE__)
  if (JITPageWriteNestCounter() == 0)
  {
    pthread_jit_write_protect_np(0);
  }
#endif
  JITPageWriteNestCounter()++;
}
// Allows a thread to execute memory allocated for execution, but not write to it.
void JITPageWriteDisableExecuteEnable()
{
  JITPageWriteNestCounter()--;

  // Sanity check the NestCounter to identify underflow
  // This can indicate the calls to JITPageWriteDisableExecuteEnable()
  // are not matched with previous calls to JITPageWriteEnableExecuteDisable()
  if (JITPageWriteNestCounter() < 0)
    PanicAlertFmt("JITPageWriteNestCounter() underflowed");

#if defined(_M_ARM_64) && defined(__APPLE__)
  if (JITPageWriteNestCounter() == 0)
  {
    pthread_jit_write_protect_np(1);
  }
#endif
}

void* AllocateMemoryPages(size_t size)
{
#ifdef _WIN32
  void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
#else
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

  if (ptr == MAP_FAILED)
    ptr = nullptr;
#endif

  if (ptr == nullptr)
    PanicAlertFmt("Failed to allocate raw memory");

  return ptr;
}

void* AllocateAlignedMemory(size_t size, size_t alignment)
{
#ifdef _WIN32
  void* ptr = _aligned_malloc(size, alignment);
#else
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0)
    ERROR_LOG_FMT(MEMMAP, "Failed to allocate aligned memory");
#endif

  if (ptr == nullptr)
    PanicAlertFmt("Failed to allocate aligned memory");

  return ptr;
}

bool FreeMemoryPages(void* ptr, size_t size)
{
  if (ptr)
  {
#ifdef _WIN32
#ifdef _M_X86_64
    RemoveUnwindTail(ptr, size);
#endif
    if (!VirtualFree(ptr, 0, MEM_RELEASE))
    {
      PanicAlertFmt("FreeMemoryPages failed!\nVirtualFree: {}", GetLastErrorString());
      return false;
    }
#else
    if (munmap(ptr, size) != 0)
    {
      PanicAlertFmt("FreeMemoryPages failed!\nmunmap: {}", LastStrerrorString());
      return false;
    }
#endif
  }
  return true;
}

void FreeAlignedMemory(void* ptr)
{
  if (ptr)
  {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
  }
}

bool ReadProtectMemory(void* ptr, size_t size)
{
#ifdef _WIN32
  DWORD oldValue;
  if (!VirtualProtect(ptr, size, PAGE_NOACCESS, &oldValue))
  {
    PanicAlertFmt("ReadProtectMemory failed!\nVirtualProtect: {}", GetLastErrorString());
    return false;
  }
#else
  if (mprotect(ptr, size, PROT_NONE) != 0)
  {
    PanicAlertFmt("ReadProtectMemory failed!\nmprotect: {}", LastStrerrorString());
    return false;
  }
#endif
  return true;
}

bool WriteProtectMemory(void* ptr, size_t size, bool allowExecute)
{
#ifdef _WIN32
  DWORD oldValue;
  if (!VirtualProtect(ptr, size, allowExecute ? PAGE_EXECUTE_READ : PAGE_READONLY, &oldValue))
  {
    PanicAlertFmt("WriteProtectMemory failed!\nVirtualProtect: {}", GetLastErrorString());
    return false;
  }
#elif !(defined(_M_ARM_64) && defined(__APPLE__))
  // MacOS 11.2 on ARM does not allow for changing the access permissions of pages
  // that were marked executable, instead it uses the protections offered by MAP_JIT
  // for write protection.
  if (mprotect(ptr, size, allowExecute ? (PROT_READ | PROT_EXEC) : PROT_READ) != 0)
  {
    PanicAlertFmt("WriteProtectMemory failed!\nmprotect: {}", LastStrerrorString());
    return false;
  }
#endif
  return true;
}

bool UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute)
{
#ifdef _WIN32
  DWORD oldValue;
  if (!VirtualProtect(ptr, size, allowExecute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &oldValue))
  {
    PanicAlertFmt("UnWriteProtectMemory failed!\nVirtualProtect: {}", GetLastErrorString());
    return false;
  }
#elif !(defined(_M_ARM_64) && defined(__APPLE__))
  // MacOS 11.2 on ARM does not allow for changing the access permissions of pages
  // that were marked executable, instead it uses the protections offered by MAP_JIT
  // for write protection.
  if (mprotect(ptr, size,
               allowExecute ? (PROT_READ | PROT_WRITE | PROT_EXEC) : PROT_WRITE | PROT_READ) != 0)
  {
    PanicAlertFmt("UnWriteProtectMemory failed!\nmprotect: {}", LastStrerrorString());
    return false;
  }
#endif
  return true;
}

size_t MemPhysical()
{
#ifdef _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  return memInfo.ullTotalPhys;
#elif defined __APPLE__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
  int mib[2];
  size_t physical_memory;
  mib[0] = CTL_HW;
#ifdef __APPLE__
  mib[1] = HW_MEMSIZE;
#elif defined __FreeBSD__
  mib[1] = HW_REALMEM;
#elif defined __OpenBSD__ || defined __NetBSD__
  mib[1] = HW_PHYSMEM64;
#endif
  size_t length = sizeof(size_t);
  sysctl(mib, 2, &physical_memory, &length, nullptr, 0);
  return physical_memory;
#elif defined __HAIKU__
  system_info sysinfo;
  get_system_info(&sysinfo);
  return static_cast<size_t>(sysinfo.max_pages * B_PAGE_SIZE);
#else
  struct sysinfo memInfo;
  sysinfo(&memInfo);
  return (size_t)memInfo.totalram * memInfo.mem_unit;
#endif
}

size_t PageSize()
{
#ifdef _WIN32
  SYSTEM_INFO sysInfo;
  GetSystemInfo(&sysInfo);
  return sysInfo.dwPageSize;
#else
  return sysconf(_SC_PAGESIZE);
#endif
}

// https://stackoverflow.com/a/66077507
void* GetPageAddress(void* address, std::size_t alignment) noexcept
{
  void* aligned = address;
  std::size_t space = alignment;
  std::align(alignment, 1, aligned, space);
  return address == aligned ? aligned : static_cast<char*>(aligned) - alignment;
}

}  // namespace Common
