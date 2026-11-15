// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's VideoCommon/Present.h (core/standalone only): the presenter the
// core asks for the shown picture's shape and for a redraw while paused.

#pragma once

#include "BrawlOnline/Host.h"

namespace VideoCommon
{
class Presenter
{
public:
  float CalculateDrawAspectRatio(bool = true) const
  {
    return BrawlOnline::GetHost().DrawAspectRatio();
  }
  void RedrawLastFrame() {}
};
}  // namespace VideoCommon

// Dolphin's is a std::unique_ptr; the core only tests it and calls through it.
extern VideoCommon::Presenter* g_presenter;
