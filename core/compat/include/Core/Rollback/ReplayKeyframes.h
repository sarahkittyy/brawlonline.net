// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Rollback/ReplayKeyframes.h (core/standalone only): replay
// seeking keyframes; none outside Dolphin yet (a replay plays from its start).

#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Gprb::Replay
{
class Keyframes
{
public:
  void Reset(std::vector<std::pair<u32, u32>>) {}
  void Clear() {}
  bool Empty() const { return true; }
  void Capture(Core::System&, s64) {}
  bool Has(s64) const { return false; }
  std::optional<s64> AtOrBefore(s64) const { return std::nullopt; }
  bool Restore(Core::System&, s64) { return false; }
  u64 Bytes() const { return 0; }
  size_t Count() const { return 0; }
};
}  // namespace Gprb::Replay
