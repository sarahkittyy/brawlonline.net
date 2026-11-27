// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

namespace Common
{
void* AllocateExecutableMemory(size_t size);

#if defined(_WIN32) && defined(_M_X86_64)
// The code a JIT writes has no unwind data of its own, so on Windows an exception that reaches it
// unhandled (a C++ throw from a function JIT code called, a fault in it) used to end the process
// without the unhandled exception filter: the exception's dispatch can't get past it (bug report
// #373). Executable memory now has a handler of its own there, and it calls `handler` (with the
// exception's EXCEPTION_POINTERS) before the dispatch goes on, and fails as before.
using ExecutableMemoryExceptionHandler = void (*)(void* exception_pointers);
void SetExecutableMemoryExceptionHandler(ExecutableMemoryExceptionHandler handler);
#endif

// These two functions control the executable/writable state of the W^X memory
// allocations. More detailed documentation about them is in the .cpp file.
// In general where applicable the ScopedJITPageWriteAndNoExecute wrapper
// should be used to prevent bugs from not pairing up the calls properly.

// Allows a thread to write to executable memory, but not execute the data.
void JITPageWriteEnableExecuteDisable();
// Allows a thread to execute memory allocated for execution, but not write to it.
void JITPageWriteDisableExecuteEnable();
// RAII Wrapper around JITPageWrite*Execute*(). When this is in scope the thread can
// write to executable memory but not execute it.
struct ScopedJITPageWriteAndNoExecute
{
  ScopedJITPageWriteAndNoExecute() { JITPageWriteEnableExecuteDisable(); }
  ~ScopedJITPageWriteAndNoExecute() { JITPageWriteDisableExecuteEnable(); }
};
void* AllocateMemoryPages(size_t size);
bool FreeMemoryPages(void* ptr, size_t size);
void* AllocateAlignedMemory(size_t size, size_t alignment);
void FreeAlignedMemory(void* ptr);
bool ReadProtectMemory(void* ptr, size_t size);
bool WriteProtectMemory(void* ptr, size_t size, bool executable = false);
bool UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute = false);
size_t MemPhysical();
// Brawlback
size_t PageSize();
void* GetPageAddress(void* address, std::size_t alignment) noexcept;

}  // namespace Common
