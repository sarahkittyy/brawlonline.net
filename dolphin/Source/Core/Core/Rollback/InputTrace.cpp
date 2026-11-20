// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/InputTrace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <fmt/format.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Rollback::InputTrace
{
namespace
{
std::mutex s_mutex;
u32 s_unflushed = 0;

std::FILE* File()
{
  static std::FILE* const file = [] {
    const char* env = std::getenv("PPR_GPRB_INPUT_TRACE");
    if (!env || !*env)
      return static_cast<std::FILE*>(nullptr);
    std::string path = env;
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    if (const size_t at = path.find("%p"); at != std::string::npos)
      path.replace(at, 2, std::to_string(pid));
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f)
      std::fprintf(f, "H %d %lld\n", pid, static_cast<long long>(Us(Clock::now())));
    return f;
  }();
  return file;
}
}  // namespace

bool Enabled()
{
  return File() != nullptr;
}

s64 Us(Clock::time_point t)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}

void Write(const std::string& line)
{
  std::FILE* f = File();
  if (!f)
    return;
  std::lock_guard lk(s_mutex);
  std::fwrite(line.data(), 1, line.size(), f);
  std::fputc('\n', f);
  if (++s_unflushed >= 600)
  {
    std::fflush(f);
    s_unflushed = 0;
  }
}

void Flush()
{
  std::FILE* f = File();
  if (!f)
    return;
  std::lock_guard lk(s_mutex);
  std::fflush(f);
  s_unflushed = 0;
}
}  // namespace Rollback::InputTrace
