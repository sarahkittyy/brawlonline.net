// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/System.h (core/standalone only).

#pragma once

#include "Common/CommonTypes.h"

namespace Fifo
{
class FifoManager;
}
namespace Memory
{
class MemoryManager;
}
namespace PowerPC
{
class PowerPCManager;
struct PowerPCState;
}  // namespace PowerPC

namespace Core
{
class System
{
public:
  static System& GetInstance();

  Memory::MemoryManager& GetMemory() const;
  Fifo::FifoManager& GetFifo() const;
  PowerPC::PowerPCManager& GetPowerPC() const;
  PowerPC::PowerPCState& GetPPCState() const;
  bool IsDualCoreMode() const;
  bool IsWii() const { return true; }

private:
  System() = default;
};
}  // namespace Core
