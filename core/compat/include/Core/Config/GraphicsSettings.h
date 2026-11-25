// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Config/GraphicsSettings.h (core/standalone only): the GFX keys
// the core reads, with Dolphin's names (GFX.ini) and defaults.

#pragma once

#include "Common/Config/Config.h"
#include "VideoCommon/VideoConfig.h"

namespace Config
{
extern const Info<bool> GFX_SHOW_NETPLAY_PING;
extern const Info<StereoMode> GFX_STEREO_MODE;
extern const Info<bool> GFX_HACK_IMMEDIATE_XFB;
}  // namespace Config
