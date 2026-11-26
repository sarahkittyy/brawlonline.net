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
// The displayed pass's picture (GameplaySession at its loop top: the pass frame, the frame whose picture its copy
// presents, GekkoNet's confirmed frame then, after a rollback). The engine's present writes the input trace's P line
// from it (EmissaryRecomp's CoreTracePresent), as Dolphin's PresentStats does at its copy.
struct DisplayedTag
{
  s64 pass_frame = -1, picture_frame = -1, picture_confirmed = -1;
  bool after_resim = false;
};
inline DisplayedTag g_displayed_tag;
inline void TagDisplayedFrame(s64 pass_frame, s64 picture_frame, s64 picture_confirmed,
                              bool after_resimulation)
{
  g_displayed_tag = {pass_frame, picture_frame, picture_confirmed, after_resimulation};
}
}  // namespace Rollback::PresentStats
