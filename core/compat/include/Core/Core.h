// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Core.h (core/standalone only): the part the core uses.

#pragma once

#include <functional>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Core
{
class System;

enum class State
{
  Uninitialized,
  Paused,
  Running,
  Stopping,
  Starting,
};

// The core's hooks run on the game's thread already: the guard only names the system.
class CPUThreadGuard final
{
public:
  explicit CPUThreadGuard(System& system) : m_system(system) {}
  CPUThreadGuard(const CPUThreadGuard&) = delete;
  CPUThreadGuard& operator=(const CPUThreadGuard&) = delete;
  System& GetSystem() const { return m_system; }

private:
  System& m_system;
};

using StateChangedCallbackFunc = std::function<void(Core::State)>;
[[nodiscard]] Common::EventHook AddOnStateChangedCallback(StateChangedCallbackFunc callback);

void Stop(System& system);
void QueueHostJob(std::function<void(System&)> job, bool run_during_stop = false);
}  // namespace Core
