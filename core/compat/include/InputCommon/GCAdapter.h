// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's InputCommon/GCAdapter.h (core/standalone only): the host reads its
// own controllers (the adapter included), so a read "at" an instant is the read now.

#pragma once

#include <optional>

#include "BrawlOnline/Host.h"
#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace GCAdapter
{
inline GCPadStatus Input(int chan)
{
  return BrawlOnline::GetHost().LocalPad(chan);
}
inline std::optional<GCPadStatus> InputAt(int, Clock::time_point)
{
  return std::nullopt;
}
}  // namespace GCAdapter
