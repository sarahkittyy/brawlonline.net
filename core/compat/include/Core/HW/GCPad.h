// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/HW/GCPad.h (core/standalone only): the local controller is
// the host's.

#pragma once

#include "BrawlOnline/Host.h"
#include "InputCommon/GCPadStatus.h"

class InputConfig;

namespace Pad
{
// Dolphin's Controllers window's config set: core/standalone has none (GCPadEmu.cpp asks).
inline InputConfig* GetConfig()
{
  return nullptr;
}

inline GCPadStatus GetStatus(int pad_num)
{
  return BrawlOnline::GetHost().LocalPad(pad_num);
}
}  // namespace Pad
