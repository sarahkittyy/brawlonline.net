// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Common::FPU
{
enum RoundMode : u32
{
  ROUND_NEAR = 0,
  ROUND_CHOP = 1,
  ROUND_UP = 2,
  ROUND_DOWN = 3
};

void SetSIMDMode(RoundMode rounding_mode, bool non_ieee_mode);

/*
 * There are two different flavors of float to int conversion:
 * _mm_cvtps_epi32() and _mm_cvttps_epi32().
 *
 * The first rounds according to the MXCSR rounding bits.
 * The second one always uses round towards zero.
 */
void SaveSIMDState();
void LoadSIMDState();
void LoadDefaultSIMDState();

u64 GetSIMDState();
void SetSIMDState(u64 state);

// The host's default FPU mode for the rest of the scope, then the mode from before. For host
// computations on the CPU thread whose results reach the guest (DVD timing): under the guest's
// mode (its rounding and flushing, and FPCR.AH on arm64, see FloatUtils.h) the same C++ code gives
// different results on different hosts.
class ScopedDefaultSIMDState
{
public:
  ScopedDefaultSIMDState() : m_saved(GetSIMDState()) { LoadDefaultSIMDState(); }
  ~ScopedDefaultSIMDState() { SetSIMDState(m_saved); }
  ScopedDefaultSIMDState(const ScopedDefaultSIMDState&) = delete;
  ScopedDefaultSIMDState& operator=(const ScopedDefaultSIMDState&) = delete;

private:
  u64 m_saved;
};
}  // namespace Common::FPU
