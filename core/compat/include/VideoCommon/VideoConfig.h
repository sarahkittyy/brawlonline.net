// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's VideoCommon/VideoConfig.h (core/standalone only).

#pragma once

enum class StereoMode : int
{
  Off,
  SBS,
  TAB,
  Anaglyph,
  QuadBuffer,
  Passive,
};
