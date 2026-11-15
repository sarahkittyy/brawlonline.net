// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's VideoCommon/AsyncRequests.h (core/standalone only): an event for
// Dolphin's GPU thread while the CPU is stopped runs right away.

#pragma once

#include <functional>

class AsyncRequests
{
public:
  static AsyncRequests* GetInstance()
  {
    static AsyncRequests s;
    return &s;
  }
  void PushEventWhileStopped(std::function<void()> f) { f(); }
};
