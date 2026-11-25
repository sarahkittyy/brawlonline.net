// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Harness/Harness.h (core/standalone only): Dolphin's test
// harness is not there; the host says whether its own one is on (Host::HarnessActive).

#pragma once

#include <optional>

#include "BrawlOnline/Host.h"
#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Harness
{
inline bool IsActive()
{
  return BrawlOnline::GetHost().HarnessActive();
}
inline void ApplyPadOverride(int, GCPadStatus*)
{
}
inline std::optional<GCPadStatus> PadOverrideAt(int, Clock::time_point)
{
  return std::nullopt;
}
// The host's harness drives its pads itself (Host::LocalPad): no port is overridden here.
inline bool IsPadOverridden(int)
{
  return false;
}
}  // namespace Harness
