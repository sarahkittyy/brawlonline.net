// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/Rollback/LoopWatch.h"

#include <atomic>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#elif defined(__linux__)
#include <cstdio>
#include <ctime>
#include <pthread.h>
#endif

namespace Gprb::LoopWatch
{
namespace
{
// Which marking the calling thread made (0: none); a different thread's marking replaces it.
std::atomic<unsigned> s_generation{0};
thread_local unsigned t_marked = 0;

#if defined(_WIN32)
// A handle of the marked thread. Earlier ones stay open: another thread may be reading one, and a
// process marks a new game thread only once per game started.
std::atomic<HANDLE> s_thread{nullptr};

s64 FileTimeNs(const FILETIME& t)
{
  return static_cast<s64>((static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;
}
#elif defined(__APPLE__)
std::atomic<mach_port_t> s_thread{MACH_PORT_NULL};
#elif defined(__linux__)
std::atomic<bool> s_have_clock{false};
std::atomic<clockid_t> s_clock{};
#endif
}  // namespace

void MarkGameThread()
{
  if (t_marked != 0 && t_marked == s_generation.load(std::memory_order_relaxed))
    return;
#if defined(_WIN32)
  HANDLE h = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                      THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
  {
    s_thread.store(h);
  }
#elif defined(__APPLE__)
  s_thread.store(pthread_mach_thread_np(pthread_self()));
#elif defined(__linux__)
  clockid_t c;
  if (pthread_getcpuclockid(pthread_self(), &c) == 0)
  {
    s_clock.store(c);
    s_have_clock.store(true);
  }
#endif
  t_marked = s_generation.fetch_add(1) + 1;
}

s64 GameThreadCpuNs()
{
#if defined(_WIN32)
  const HANDLE h = s_thread.load();
  FILETIME created, exited, kernel, user;
  if (!h || !GetThreadTimes(h, &created, &exited, &kernel, &user))
    return -1;
  return FileTimeNs(kernel) + FileTimeNs(user);
#elif defined(__APPLE__)
  const mach_port_t port = s_thread.load();
  if (port == MACH_PORT_NULL)
    return -1;
  thread_basic_info_data_t info;
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) !=
      KERN_SUCCESS)
  {
    return -1;
  }
  return (static_cast<s64>(info.user_time.seconds) + info.system_time.seconds) * 1'000'000'000 +
         (static_cast<s64>(info.user_time.microseconds) + info.system_time.microseconds) * 1000;
#elif defined(__linux__)
  timespec ts;
  if (!s_have_clock.load() || clock_gettime(s_clock.load(), &ts) != 0)
    return -1;
  return static_cast<s64>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#else
  return -1;
#endif
}

std::pair<s64, s64> SystemCpuNs()
{
#if defined(_WIN32)
  // The kernel time includes the idle time.
  FILETIME idle, kernel, user;
  if (!GetSystemTimes(&idle, &kernel, &user))
    return {-1, -1};
  const s64 total = FileTimeNs(kernel) + FileTimeNs(user);
  return {total - FileTimeNs(idle), total};
#elif defined(__APPLE__)
  host_cpu_load_info_data_t info;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info),
                      &count) != KERN_SUCCESS)
  {
    return {-1, -1};
  }
  // Scheduler ticks (10 ms each): only their ratio is used.
  const s64 idle = info.cpu_ticks[CPU_STATE_IDLE];
  const s64 total = idle + info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM] +
                    info.cpu_ticks[CPU_STATE_NICE];
  return {(total - idle) * 10'000'000, total * 10'000'000};
#elif defined(__linux__)
  // /proc/stat's first line: user nice system idle iowait irq softirq steal, in clock ticks (only
  // their ratio is used).
  FILE* f = std::fopen("/proc/stat", "r");
  if (!f)
    return {-1, -1};
  unsigned long long v[8] = {};
  const int n = std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                            &v[3], &v[4], &v[5], &v[6], &v[7]);
  std::fclose(f);
  if (n < 4)
    return {-1, -1};
  s64 total = 0;
  for (const unsigned long long x : v)
    total += static_cast<s64>(x);
  const s64 idle = static_cast<s64>(v[3] + v[4]);
  return {(total - idle) * 10'000'000, total * 10'000'000};
#else
  return {-1, -1};
#endif
}
}  // namespace Gprb::LoopWatch
