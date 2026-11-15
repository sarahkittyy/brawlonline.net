// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Harness/Harness.h (core/standalone only): Dolphin's test
// harness is not there.

#pragma once

#include <optional>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Harness
{
inline bool IsActive()
{
  return false;
}
inline void ApplyPadOverride(int, GCPadStatus*)
{
}
inline std::optional<GCPadStatus> PadOverrideAt(int, Clock::time_point)
{
  return std::nullopt;
}
}  // namespace Harness
