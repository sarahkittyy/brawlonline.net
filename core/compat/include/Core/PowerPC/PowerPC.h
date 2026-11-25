// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/PowerPC/PowerPC.h (core/standalone only): what the core's
// diagnostics read (the loop-stall report). The engine owns the real registers; these read zero.

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

constexpr int SPR_LR = 8;
constexpr int SPR_CTR = 9;

namespace PowerPC
{
struct PowerPCState
{
  u32 pc = 0;
  u32 npc = 0;
  u32 gpr[32]{};
  u32 spr[1024]{};
  struct
  {
    bool EE = false;
  } msr;
};

// The guest's OS threads (Dolphin's debugger): none listed outside Dolphin.
class ThreadEntry
{
public:
  virtual ~ThreadEntry() = default;
  virtual u32 GetAddress() const = 0;
};

class DebugInterface
{
public:
  std::vector<std::unique_ptr<ThreadEntry>> GetThreads(const Core::CPUThreadGuard&) const
  {
    return {};
  }
};

class PowerPCManager
{
public:
  DebugInterface& GetDebugInterface() { return m_debug; }

private:
  DebugInterface m_debug;
};
}  // namespace PowerPC
