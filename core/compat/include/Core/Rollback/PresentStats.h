// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Rollback/PresentStats.h (core/standalone only): Dolphin's
// present cadence statistics; empty outside it.

#pragma once

#include "Common/CommonTypes.h"

namespace Rollback::PresentStats
{
struct Cadence
{
  u64 presents = 0;
  double hitches = 0;
  double interval_ms_max = 0;
  double interval_ms_sum = 0, interval_ms_sq = 0;
  u64 holds = 0;
  double hold_ms_sum = 0, hold_ms_max = 0;
};
inline Cadence TakeCadence()
{
  return {};
}
inline void OnDisplayedFrameStart(bool)
{
}
inline void TagDisplayedFrame(s64, s64, s64, bool)
{
}
}  // namespace Rollback::PresentStats
