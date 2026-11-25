// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Rollback/MachineSession.h (core/standalone only): Other Games'
// whole-machine sessions are Dolphin's; outside it none is ever armed.

#pragma once

namespace Machine
{
constexpr int MAX_PLAYERS = 4;
constexpr int AUTO_DELAY = -1;  // Normal
constexpr int AUTO_DELAY_AGGRESSIVE = -2;
constexpr int AUTO_DELAY_SMOOTH = -3;
constexpr int AutoDelayLevel(int delay)
{
  return delay == AUTO_DELAY_AGGRESSIVE ? -1 : delay == AUTO_DELAY_SMOOTH ? 1 : 0;
}
constexpr int DelayFromSetting(int setting)
{
  return setting > 0   ? setting :
         setting == -3 ? 0 :
         setting == -1 ? AUTO_DELAY_AGGRESSIVE :
         setting == -2 ? AUTO_DELAY_SMOOTH :
                         AUTO_DELAY;
}

inline bool IsArmed()
{
  return false;
}
inline void SetInputDelay(int)
{
}
inline int CurrentInputDelay()
{
  return AUTO_DELAY;
}
}  // namespace Machine
