// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/BranchProfile.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/Rollback/MachineSession.h"
#include "Core/System.h"

namespace BranchProfile
{
namespace
{
// CPU thread only (the JIT compiles and runs there).
std::unordered_map<u32, u32> s_follow;     // address -> instruction
std::unordered_map<u32, Counts> s_counts;  // a recording
std::string s_record_path;

void Load(const std::string& path)
{
  std::FILE* const f = std::fopen(path.c_str(), "r");
  if (!f)
  {
    WARN_LOG_FMT(DYNA_REC, "No branch profile at {}", path);
    return;
  }
  char line[128];
  while (std::fgets(line, sizeof(line), f))
  {
    unsigned address, instruction;
    if (line[0] != '#' && std::sscanf(line, "%x %x", &address, &instruction) == 2)
      s_follow[address] = instruction;
  }
  std::fclose(f);
  INFO_LOG_FMT(DYNA_REC, "Branch profile: {} branches from {}", s_follow.size(), path);
}
}  // namespace

void Init()
{
  s_follow.clear();
  s_counts.clear();

  const char* const record = std::getenv("PPR_JIT_BRANCH_RECORD");
  s_record_path = record ? record : "";
  if (!s_record_path.empty())
    return;

  const char* const profile = std::getenv("PPR_JIT_BRANCH_PROFILE");
  if (profile && std::string(profile) == "0")
    return;
  Load(profile && *profile ? profile : File::GetSysDirectory() + "Rollback/jit-branches.txt");
}

void Shutdown()
{
  if (!s_record_path.empty())
  {
    if (std::FILE* const f = std::fopen(s_record_path.c_str(), "w"))
    {
      std::fprintf(f, "# address instruction taken not_taken\n");
      for (const auto& [address, counts] : s_counts)
      {
        std::fprintf(f, "%08x %08x %u %u\n", address, counts.instruction, counts.taken,
                     counts.not_taken);
      }
      std::fclose(f);
    }
  }
  s_follow.clear();
  s_counts.clear();
}

bool IsActive()
{
  if ((s_follow.empty() && s_record_path.empty()) || Machine::IsArmed())
    return false;
  if (Core::System::GetInstance().GetMovie().IsMovieActive())
    return false;
  return !NetPlay::IsNetPlayRunning() || NetPlay::IsInRollbackMode();
}

bool FollowsTaken(u32 address, u32 instruction)
{
  const auto it = s_follow.find(address);
  return it != s_follow.end() && it->second == instruction;
}

Counts* Recording(u32 address, u32 instruction)
{
  if (s_record_path.empty())
    return nullptr;
  Counts& counts = s_counts[address];
  if (counts.instruction != instruction)
  {
    // Other code is here now (a module loaded where another was).
    counts = Counts{};
    counts.instruction = instruction;
  }
  return &counts;
}
}  // namespace BranchProfile
