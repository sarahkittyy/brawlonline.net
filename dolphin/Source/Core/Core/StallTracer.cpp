// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/StallTracer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define STALL_TRACER_SIGNAL_SAMPLES 1
#include <cerrno>
#include <csignal>
#include <fstream>
#include <link.h>
#include <sys/syscall.h>
#include <ucontext.h>
#endif

namespace Core::StallTracer
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto POLL = 25ms;
constexpr auto TRACE_EVERY = 5s;  // at most one sampled trace this often (a stall's first)
constexpr int MAX_TRACES = 40;    // and this many a run
constexpr int MAX_ENDS = 300;     // "stall over" lines a run
constexpr int LONG_TRACES = 3;    // a stall's later traces: at 2 s, 8 s, 32 s
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
#ifdef STALL_TRACER_SIGNAL_SAMPLES
  long tid = 0;
#endif
};

std::mutex s_slots_mutex;  // the slots; the monitor holds it while it samples
std::array<Slot, 3> s_slots;

std::atomic<s64> s_last_field_ns{0};  // steady clock, 0: no field yet
std::atomic<u64> s_fields{0};
std::atomic<s64> s_stop_requested_ns{0};  // steady clock, 0: no stop asked for this run

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

// A thread's instruction pointer and the top of its stack, as copied while it was stopped.
struct RawSample
{
  const char* error = nullptr;  // set: nothing was sampled
  u64 ip = 0;
  size_t words = 0;
  std::array<u64, STACK_BYTES / 8> stack{};
};

// A loaded module's code: [base, base + size), offsets counted from `load` (its load address,
// as a debugger or addr2line counts them).
struct Module
{
  u64 base = 0;
  u64 size = 0;
  u64 load = 0;
  std::string name;
};

[[maybe_unused]] std::string BaseName(std::string_view path)
{
  const size_t slash = path.find_last_of("/\\");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

#ifdef _WIN32
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
    const u64 base = reinterpret_cast<u64>(info.lpBaseOfDll);
    out.push_back({base, info.SizeOfImage, base, WStringToUTF8(name)});
  }
  return out;
}

// The thread suspended only while its context and the top of its stack are copied (nothing in
// between that could wait for a lock the thread holds); the addresses are named after it runs
// again.
RawSample SampleThread(const Slot& slot)
{
  RawSample out;
  if (SuspendThread(slot.handle) == static_cast<DWORD>(-1))
  {
    out.error = "(not suspended)";
    return out;
  }
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  const bool have = GetThreadContext(slot.handle, &ctx) != 0;
  SIZE_T got = 0;
#if defined(_M_ARM64)
  const u64 ip = have ? ctx.Pc : 0, sp = have ? ctx.Sp : 0;
#else
  const u64 ip = have ? ctx.Rip : 0, sp = have ? ctx.Rsp : 0;
#endif
  if (have)
  {
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(sp), out.stack.data(),
                      sizeof(out.stack), &got);
  }
  ResumeThread(slot.handle);
  if (!have)
  {
    out.error = "(no context)";
    return out;
  }
  out.ip = ip;
  out.words = got / 8;
  return out;
}

std::string ThreadState(const Slot&)
{
  return {};
}
#elif defined(STALL_TRACER_SIGNAL_SAMPLES)
// Linux: a thread can't be suspended from outside, so a signal sent to it copies its registers
// and the top of its stack in the handler (only memcpy and atomics: async-signal-safe). A thread
// that never runs the handler in time (blocked in the kernel, uninterruptibly) is reported so;
// the handler then does nothing when it runs later.
enum SampleState : int
{
  SAMPLE_IDLE,
  SAMPLE_REQUESTED,
  SAMPLE_WRITING,
  SAMPLE_DONE,
};
std::atomic<int> s_signal_state{SAMPLE_IDLE};
std::atomic<u64> s_signal_stack_hi{0};
RawSample s_signal_sample;
bool s_signal_installed = false;  // under s_run_mutex

int SampleSignal()
{
  return SIGRTMIN + 7;
}

void OnSampleSignal(int, siginfo_t*, void* context)
{
  int expected = SAMPLE_REQUESTED;
  if (!s_signal_state.compare_exchange_strong(expected, SAMPLE_WRITING, std::memory_order_acquire))
    return;
  const int saved_errno = errno;
  const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
  const u64 ip = static_cast<u64>(uc->uc_mcontext.gregs[REG_RIP]);
  const u64 sp = static_cast<u64>(uc->uc_mcontext.gregs[REG_RSP]);
#else
  const u64 ip = uc->uc_mcontext.pc;
  const u64 sp = uc->uc_mcontext.sp;
#endif
  const u64 stack_hi = s_signal_stack_hi.load(std::memory_order_relaxed);
  size_t bytes = 0;
  if (sp != 0 && stack_hi > sp)
    bytes = static_cast<size_t>(std::min<u64>(stack_hi - sp, sizeof(s_signal_sample.stack)));
  if (bytes != 0)
    std::memcpy(s_signal_sample.stack.data(), reinterpret_cast<const void*>(sp), bytes);
  s_signal_sample.error = nullptr;
  s_signal_sample.ip = ip;
  s_signal_sample.words = bytes / 8;
  errno = saved_errno;
  s_signal_state.store(SAMPLE_DONE, std::memory_order_release);
}

// Under s_run_mutex (Start).
void InstallSampleSignal()
{
  if (s_signal_installed)
    return;
  struct sigaction sa{};
  sa.sa_sigaction = OnSampleSignal;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  s_signal_installed = sigaction(SampleSignal(), &sa, nullptr) == 0;
}

int CollectModule(dl_phdr_info* info, size_t, void* data)
{
  auto* out = static_cast<std::vector<Module>*>(data);
  const std::string name = BaseName(info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name :
                                                                            "dolphin");
  for (int i = 0; i < info->dlpi_phnum; ++i)
  {
    const auto& ph = info->dlpi_phdr[i];
    if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X) != 0)
      out->push_back({info->dlpi_addr + ph.p_vaddr, ph.p_memsz, info->dlpi_addr, name});
  }
  return 0;
}

std::vector<Module> Modules()
{
  std::vector<Module> out;
  dl_iterate_phdr(CollectModule, &out);
  return out;
}

RawSample SampleThread(const Slot& slot)
{
  RawSample out;
  if (!s_signal_installed)
  {
    out.error = "(no sampling signal)";
    return out;
  }
  u64 stack_hi = 0;
  pthread_attr_t attr;
  if (pthread_getattr_np(slot.thread, &attr) == 0)
  {
    void* addr = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &addr, &size) == 0)
      stack_hi = reinterpret_cast<u64>(addr) + size;
    pthread_attr_destroy(&attr);
  }
  s_signal_stack_hi.store(stack_hi, std::memory_order_relaxed);
  s_signal_state.store(SAMPLE_REQUESTED, std::memory_order_release);
  if (pthread_kill(slot.thread, SampleSignal()) != 0)
  {
    s_signal_state.store(SAMPLE_IDLE, std::memory_order_relaxed);
    out.error = "(not signalled)";
    return out;
  }
  const auto deadline = Clock::now() + 200ms;
  while (true)
  {
    const int state = s_signal_state.load(std::memory_order_acquire);
    if (state == SAMPLE_DONE)
    {
      out = s_signal_sample;
      s_signal_state.store(SAMPLE_IDLE, std::memory_order_relaxed);
      return out;
    }
    if (Clock::now() >= deadline)
    {
      int expected = SAMPLE_REQUESTED;
      if (s_signal_state.compare_exchange_strong(expected, SAMPLE_IDLE))
      {
        out.error = "(did not take the signal in 200 ms)";
        return out;
      }
      // Being written: the handler finishes without waiting on anything.
    }
    std::this_thread::sleep_for(1ms);
  }
}

std::string ReadProcLine(long tid, const char* what)
{
  std::ifstream f(fmt::format("/proc/self/task/{}/{}", tid, what));
  std::string line;
  std::getline(f, line);
  return line;
}

// What the kernel says of it: its state (R running, S sleeping, D uninterruptible), the kernel
// function it waits in and the syscall it is in.
std::string ThreadState(const Slot& slot)
{
  if (slot.tid == 0)
    return {};
  std::string out;
  const std::string stat = ReadProcLine(slot.tid, "stat");
  const size_t paren = stat.rfind(')');
  if (paren != std::string::npos && paren + 2 < stat.size())
    out += fmt::format("state {}", stat[paren + 2]);
  const std::string wchan = ReadProcLine(slot.tid, "wchan");
  if (!wchan.empty() && wchan != "0")
    out += fmt::format("{}wchan {}", out.empty() ? "" : ", ", wchan);
  const std::string syscall_line = ReadProcLine(slot.tid, "syscall");
  if (!syscall_line.empty())
  {
    const std::string_view first = std::string_view(syscall_line).substr(0, syscall_line.find(' '));
    out += fmt::format("{}{}", out.empty() ? "" : ", ",
                       first == "running" ? std::string("in user code") :
                       first == "-1"      ? std::string("not in a syscall") :
                                            fmt::format("syscall {}", first));
  }
  return out.empty() ? out : " (" + out + ")";
}
#else
std::vector<Module> Modules()
{
  return {};
}

RawSample SampleThread(const Slot&)
{
  RawSample out;
  out.error = "(not sampled)";
  return out;
}

std::string ThreadState(const Slot&)
{
  return {};
}
#endif

[[maybe_unused]] std::string Where(const std::vector<Module>& mods, u64 addr)
{
  for (const Module& m : mods)
  {
    if (addr >= m.base && addr < m.base + m.size)
      return fmt::format("{}+{:#x}", m.name, addr - m.load);
  }
  return fmt::format("{:#x}", addr);
}

[[maybe_unused]] std::string Describe(const RawSample& s, const std::vector<Module>& mods)
{
  if (s.error)
    return s.error;
  std::string out = "at " + Where(mods, s.ip);
  int shown = 0;
  for (size_t i = 0; i < s.words && shown < STACK_WORDS_SHOWN; ++i)
  {
    const u64 w = s.stack[i];
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

constexpr bool SAMPLES_STACKS =
#if defined(_WIN32) || defined(STALL_TRACER_SIGNAL_SAMPLES)
    true;
#else
    false;
#endif

void Trace(std::string_view headline, u64 field, const std::array<double, 3>& cpu_at_field)
{
  const std::array<double, 3> cpu = CpuTimes();
  std::string line = fmt::format("{} (after field {})", headline, field);
  const std::vector<Module> mods = Modules();
  std::lock_guard lk(s_slots_mutex);
  for (size_t i = 0; i < s_slots.size(); ++i)
  {
    if (!s_slots[i].set)
      continue;
    line += fmt::format("\n  {} thread: {:.0f} ms CPU since the field", ROLE_NAMES[i],
                        cpu[i] >= 0 && cpu_at_field[i] >= 0 ? cpu[i] - cpu_at_field[i] : -1.0);
    if constexpr (SAMPLES_STACKS)
    {
      // Sampled first: reading /proc after it does not move the thread's registers.
      const RawSample sample = SampleThread(s_slots[i]);
      line += ThreadState(s_slots[i]) + ", " + Describe(sample, mods);
    }
  }
  WARN_LOG_FMT(CORE, "{}", line);
}

// The emulation did not stop in STOP_HANG_MS: where the threads are, then out (TerminateProcess /
// _Exit: nothing more runs in the process, its stuck threads' locks included; the log file is
// written through at every line).
[[noreturn]] void ExitStuckStop(u64 field, const std::array<double, 3>& cpu_at_field)
{
  Trace(fmt::format("The emulation did not stop {} s after the stop was asked for; Dolphin exits",
                    STOP_HANG_MS / 1000),
        field, cpu_at_field);
  ERROR_LOG_FMT(CORE, "Dolphin exits (code {}): the emulation is stuck", STOP_HANG_EXIT_CODE);
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), STOP_HANG_EXIT_CODE);
#endif
  std::_Exit(STOP_HANG_EXIT_CODE);
}

void Monitor()
{
  Common::SetCurrentThreadName("Stall tracer");
  auto& system = Core::System::GetInstance();
  u64 seen_field = 0;
  s64 seen_at = 0;
  std::array<double, 3> cpu_at_field{-1, -1, -1};
  bool in_stall = false;
  s64 next_trace_ms = 0;  // in a stall: when its next long-stall trace is due
  int long_traces = 0;    // and how many it has had
  int traces = 0, ends = 0;
  std::optional<Clock::time_point> last_trace;
  std::unique_lock lk(s_run_mutex);
  while (s_running)
  {
    s_run_cv.wait_for(lk, POLL, [] { return !s_running; });
    if (!s_running)
      break;

    const s64 stop_at = s_stop_requested_ns.load(std::memory_order_relaxed);
    if (stop_at != 0 && NowNs() - stop_at >= static_cast<s64>(STOP_HANG_MS) * 1'000'000)
    {
      lk.unlock();
      ExitStuckStop(s_fields.load(std::memory_order_relaxed), cpu_at_field);
    }

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
    if (!in_stall)
    {
      if (stalled_ms < STALL_MS)
        continue;
      in_stall = true;
      next_trace_ms = LONG_STALL_MS;
      long_traces = 0;
      // A stall's first trace is rate-limited (many short stalls); a long one is traced anyway.
      const auto now = Clock::now();
      if (traces < MAX_TRACES && (!last_trace || now - *last_trace >= TRACE_EVERY))
      {
        last_trace = now;
        ++traces;
        lk.unlock();
        Trace(fmt::format("Stall: no emulated field for {} ms", stalled_ms), field, cpu_at_field);
        lk.lock();
      }
      continue;
    }
    if (long_traces >= LONG_TRACES || stalled_ms < next_trace_ms || traces >= MAX_TRACES)
      continue;
    ++long_traces;
    next_trace_ms *= 4;
    last_trace = Clock::now();
    ++traces;
    lk.unlock();
    Trace(fmt::format("Stall goes on: no emulated field for {} ms", stalled_ms), field,
          cpu_at_field);
    lk.lock();
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
#ifdef STALL_TRACER_SIGNAL_SAMPLES
  slot.tid = static_cast<long>(syscall(SYS_gettid));
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

void OnStopRequested()
{
  s64 none = 0;
  s_stop_requested_ns.compare_exchange_strong(none, NowNs(), std::memory_order_relaxed);
}

void Start()
{
  std::lock_guard lk(s_run_mutex);
  if (s_running)
    return;
#ifdef STALL_TRACER_SIGNAL_SAMPLES
  InstallSampleSignal();
#endif
  s_running = true;
  s_last_field_ns.store(0, std::memory_order_relaxed);
  s_stop_requested_ns.store(0, std::memory_order_relaxed);
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
