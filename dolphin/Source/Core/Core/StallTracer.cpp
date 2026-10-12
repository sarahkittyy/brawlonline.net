// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/StallTracer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Core.h"
#include "Core/System.h"

#ifdef _WIN32
#include <Windows.h>
#include <psapi.h>
#include "Common/StringUtil.h"
#else
#include <pthread.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#endif

namespace Core::StallTracer
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto POLL = 25ms;
constexpr auto TRACE_EVERY = 5s;  // at most one sampled trace this often
constexpr int MAX_TRACES = 40;    // and this many a run
constexpr int MAX_ENDS = 300;     // "stall over" lines a run
constexpr int STACK_BYTES = 4096;
constexpr int STACK_WORDS_SHOWN = 12;
constexpr std::array<const char*, 3> ROLE_NAMES{"cpu", "gpu", "ui"};

struct Slot
{
  bool set = false;
#ifdef _WIN32
  HANDLE handle = nullptr;
#else
  pthread_t thread{};
#endif
};

std::mutex s_slots_mutex;  // the slots; the monitor holds it while it samples
std::array<Slot, 3> s_slots;

std::atomic<s64> s_last_field_ns{0};  // steady clock, 0: no field yet
std::atomic<u64> s_fields{0};

std::mutex s_run_mutex;
std::condition_variable s_run_cv;
bool s_running = false;
std::thread s_thread;

s64 NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

// Under s_slots_mutex: the thread's CPU time (user + kernel) in ms, -1 unknown.
double CpuMs(const Slot& slot)
{
  if (!slot.set)
    return -1;
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(slot.handle, &created, &exited, &kernel, &user))
    return -1;
  const auto ticks = [](const FILETIME& t) {
    return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#elif defined(__APPLE__)
  const mach_port_t port = pthread_mach_thread_np(slot.thread);
  thread_basic_info_data_t info;
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) !=
      KERN_SUCCESS)
  {
    return -1;
  }
  return (info.user_time.seconds + info.system_time.seconds) * 1000.0 +
         (info.user_time.microseconds + info.system_time.microseconds) / 1000.0;
#else
  clockid_t cid;
  timespec ts;
  if (pthread_getcpuclockid(slot.thread, &cid) != 0 || clock_gettime(cid, &ts) != 0)
    return -1;
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

std::array<double, 3> CpuTimes()
{
  std::array<double, 3> out{};
  std::lock_guard lk(s_slots_mutex);
  for (size_t i = 0; i < s_slots.size(); ++i)
    out[i] = CpuMs(s_slots[i]);
  return out;
}

#ifdef _WIN32
struct Module
{
  u64 base = 0;
  u64 size = 0;
  std::string name;
};

std::vector<Module> Modules()
{
  std::vector<Module> out;
  HMODULE mods[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    return out;
  const size_t n = std::min<size_t>(needed / sizeof(HMODULE), std::size(mods));
  for (size_t i = 0; i < n; ++i)
  {
    MODULEINFO info{};
    wchar_t name[MAX_PATH];
    if (!GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof(info)) ||
        !GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH))
    {
      continue;
    }
    out.push_back({reinterpret_cast<u64>(info.lpBaseOfDll), info.SizeOfImage, WStringToUTF8(name)});
  }
  return out;
}

std::string Where(const std::vector<Module>& mods, u64 addr)
{
  for (const Module& m : mods)
  {
    if (addr >= m.base && addr < m.base + m.size)
      return fmt::format("{}+{:#x}", m.name, addr - m.base);
  }
  return fmt::format("{:#x}", addr);
}

// The thread suspended only while its context and the top of its stack are copied (nothing in
// between that could wait for a lock the thread holds); the addresses are named after it runs
// again.
std::string Sample(HANDLE thread, const std::vector<Module>& mods)
{
  if (SuspendThread(thread) == static_cast<DWORD>(-1))
    return "(not suspended)";
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  const bool have = GetThreadContext(thread, &ctx) != 0;
  std::array<u64, STACK_BYTES / 8> stack{};
  SIZE_T got = 0;
#if defined(_M_ARM64)
  const u64 ip = have ? ctx.Pc : 0, sp = have ? ctx.Sp : 0;
#else
  const u64 ip = have ? ctx.Rip : 0, sp = have ? ctx.Rsp : 0;
#endif
  if (have)
  {
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(sp), stack.data(),
                      sizeof(stack), &got);
  }
  ResumeThread(thread);
  if (!have)
    return "(no context)";
  std::string out = "at " + Where(mods, ip);
  int shown = 0;
  for (size_t i = 0; i < got / 8 && shown < STACK_WORDS_SHOWN; ++i)
  {
    const u64 w = stack[i];
    const bool in_module = std::any_of(mods.begin(), mods.end(), [w](const Module& m) {
      return w >= m.base && w < m.base + m.size;
    });
    if (!in_module)
      continue;
    out += (shown == 0 ? "; stack " : " < ") + Where(mods, w);
    ++shown;
  }
  return out;
}
#endif

void Trace(s64 stalled_ms, u64 field, const std::array<double, 3>& cpu_at_field)
{
  const std::array<double, 3> cpu = CpuTimes();
  std::string line = fmt::format("Stall: no emulated field for {} ms (after field {})", stalled_ms, field);
#ifdef _WIN32
  const std::vector<Module> mods = Modules();
#endif
  std::lock_guard lk(s_slots_mutex);
  for (size_t i = 0; i < s_slots.size(); ++i)
  {
    if (!s_slots[i].set)
      continue;
    line += fmt::format("\n  {} thread: {:.0f} ms CPU since the field", ROLE_NAMES[i],
                        cpu[i] >= 0 && cpu_at_field[i] >= 0 ? cpu[i] - cpu_at_field[i] : -1.0);
#ifdef _WIN32
    line += ", " + Sample(s_slots[i].handle, mods);
#endif
  }
  WARN_LOG_FMT(CORE, "{}", line);
}

void Monitor()
{
  Common::SetCurrentThreadName("Stall tracer");
  auto& system = Core::System::GetInstance();
  u64 seen_field = 0;
  s64 seen_at = 0;
  std::array<double, 3> cpu_at_field{-1, -1, -1};
  bool in_stall = false;
  int traces = 0, ends = 0;
  std::optional<Clock::time_point> last_trace;
  std::unique_lock lk(s_run_mutex);
  while (s_running)
  {
    s_run_cv.wait_for(lk, POLL, [] { return !s_running; });
    if (!s_running)
      break;
    const u64 field = s_fields.load(std::memory_order_relaxed);
    const s64 at = s_last_field_ns.load(std::memory_order_relaxed);
    if (field != seen_field)
    {
      // A stall seen while it lasted, or found afterwards (the whole process stopped, this
      // thread too: the gap between the two fields says so).
      const bool late = seen_at != 0 && at - seen_at >= static_cast<s64>(STALL_MS) * 1'000'000;
      if ((in_stall || late) && ends < MAX_ENDS)
      {
        // The stall's length, field to field, and the CPU time each thread used in it.
        const auto cpu = CpuTimes();
        std::string used;
        for (size_t i = 0; i < cpu.size(); ++i)
        {
          if (cpu[i] >= 0 && cpu_at_field[i] >= 0)
            used += fmt::format(" {} {:.0f} ms", ROLE_NAMES[i], cpu[i] - cpu_at_field[i]);
        }
        WARN_LOG_FMT(CORE, "Stall over: {} ms without an emulated field; CPU time in it:{}",
                     (at - seen_at) / 1'000'000, used.empty() ? " unknown" : used);
        ++ends;
      }
      in_stall = false;
      seen_field = field;
      seen_at = at;
      cpu_at_field = CpuTimes();
      continue;
    }
    // Only while the emulation runs (not paused, booting or stopping) and has made a field.
    if (seen_at == 0 || Core::GetState(system) != Core::State::Running)
    {
      seen_at = 0;
      seen_field = field;
      in_stall = false;
      continue;
    }
    const s64 stalled_ms = (NowNs() - seen_at) / 1'000'000;
    if (in_stall || stalled_ms < STALL_MS)
      continue;
    in_stall = true;
    const auto now = Clock::now();
    if (traces < MAX_TRACES && (!last_trace || now - *last_trace >= TRACE_EVERY))
    {
      last_trace = now;
      ++traces;
      lk.unlock();
      Trace(stalled_ms, field, cpu_at_field);
      lk.lock();
    }
  }
}
}  // namespace

void RegisterThread(Role role)
{
  std::lock_guard lk(s_slots_mutex);
  Slot& slot = s_slots[static_cast<size_t>(role)];
#ifdef _WIN32
  if (slot.set && slot.handle)
    CloseHandle(slot.handle);
  slot.handle = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &slot.handle,
                       THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                       FALSE, 0))
  {
    slot.set = false;
    return;
  }
#else
  slot.thread = pthread_self();
#endif
  slot.set = true;
}

void UnregisterThread(Role role)
{
  std::lock_guard lk(s_slots_mutex);
  Slot& slot = s_slots[static_cast<size_t>(role)];
#ifdef _WIN32
  if (slot.set && slot.handle)
    CloseHandle(slot.handle);
  slot.handle = nullptr;
#endif
  slot.set = false;
}

void OnField()
{
  s_last_field_ns.store(NowNs(), std::memory_order_relaxed);
  s_fields.fetch_add(1, std::memory_order_relaxed);
}

void Start()
{
  std::lock_guard lk(s_run_mutex);
  if (s_running)
    return;
  s_running = true;
  s_last_field_ns.store(0, std::memory_order_relaxed);
  s_thread = std::thread(Monitor);
}

void Stop()
{
  {
    std::lock_guard lk(s_run_mutex);
    if (!s_running)
      return;
    s_running = false;
  }
  s_run_cv.notify_all();
  if (s_thread.joinable())
    s_thread.join();
}
}  // namespace Core::StallTracer
