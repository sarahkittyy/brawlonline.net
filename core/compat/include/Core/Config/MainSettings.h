// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Config/MainSettings.h (core/standalone only): the Main keys
// the core reads, with Dolphin's names (Dolphin.ini) and defaults.

#pragma once

#include <string>

#include "Common/Config/Config.h"

namespace Config
{
extern const Info<std::string> MAIN_GPU_DETERMINISM_MODE;
}  // namespace Config
