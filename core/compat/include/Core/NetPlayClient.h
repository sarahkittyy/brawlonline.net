// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/NetPlayClient.h (core/standalone only): no Dolphin netplay.

#pragma once

namespace NetPlay
{
inline bool IsNetPlayRunning()
{
  return false;
}
}  // namespace NetPlay
