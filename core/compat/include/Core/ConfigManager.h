// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/ConfigManager.h (core/standalone only): the running disc.

#pragma once

#include <string>

#include "BrawlOnline/Host.h"
#include "Common/CommonTypes.h"

class SConfig
{
public:
  static SConfig& GetInstance()
  {
    static SConfig s;
    return s;
  }
  std::string GetGameID() const { return BrawlOnline::GetHost().GameId(); }
  u16 GetRevision() const { return static_cast<u16>(BrawlOnline::GetHost().Revision()); }
};
