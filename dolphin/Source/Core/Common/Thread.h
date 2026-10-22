// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <thread>

#ifndef _WIN32
#include <tuple>
#endif

// Don't include Common.h here as it will break LogManager
#include "Common/CommonTypes.h"

// This may not be defined outside _WIN32
#ifndef _WIN32
#ifndef INFINITE
#define INFINITE 0xffffffff
#endif
#endif

namespace Common
{
int CurrentThreadId();

void SetThreadAffinity(std::thread::native_handle_type thread, u32 mask);
void SetCurrentThreadAffinity(u32 mask);

void SleepCurrentThread(int ms);
void SwitchCurrentThread();  // On Linux, this is equal to sleep 1ms

// Use this function during a spin-wait to make the current thread
// relax while another thread is working. This may be more efficient
// than using events because event functions use kernel calls.
inline void YieldCPU()
{
  std::this_thread::yield();
}

void SetCurrentThreadName(const char* name);

// Lowers the current thread below the emulator's own threads (work that may fall behind, such as
// writing replays): when the machine is short of CPU, the OS runs the game first. Windows: lowest
// priority; macOS: the utility QoS class; Linux: nice 10 for this thread.
void SetCurrentThreadBackgroundPriority();

#ifdef _WIN32
// Runs the messages that other threads sent (SendMessage) to this thread's windows, and nothing
// else: no posted messages, no Qt events. For a UI thread waiting on the emulation: DXGI's
// SetFullscreenState and Present on the video thread send the render window a message and wait
// for the answer, while the CPU thread waits for the video thread (bug reports #86, #92).
void AnswerSentMessages();
#endif

#ifndef _WIN32
// Returns the lowest address of the stack and the size of the stack
std::tuple<void*, size_t> GetCurrentThreadStack();
#endif

}  // namespace Common
