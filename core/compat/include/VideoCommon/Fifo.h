// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's VideoCommon/Fifo.h (core/standalone only): Dolphin's GPU thread and
// its determinism; the recomp draws on its own terms.

#pragma once

namespace Fifo
{
enum class SyncGPUReason
{
  Other,
};

class FifoManager
{
public:
  void SetRollbackSessionDeterminism(bool) {}
  bool UseDeterministicGPUThread() const { return false; }
  void GpuMaySleep() {}
  void SyncGPU(SyncGPUReason, bool = true) {}
};
}  // namespace Fifo
