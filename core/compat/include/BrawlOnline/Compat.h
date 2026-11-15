// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Start-up of the core outside Dolphin (core/standalone only).

#pragma once

#include <string>

namespace BrawlOnline
{
// Before anything else of the core: Dolphin's user folder layout under `user_dir` (Config/,
// Online/, Logs/...) and the config layers read from its INI files (Dolphin.ini, GFX.ini,
// Logger.ini), as Dolphin's UICommon::Init does.
void InitCompat(const std::string& user_dir);
void ShutdownCompat();
}  // namespace BrawlOnline
