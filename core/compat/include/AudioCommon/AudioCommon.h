// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's AudioCommon/AudioCommon.h (core/standalone only).

#pragma once

namespace Core
{
class System;
}

namespace AudioCommon
{
inline void UpdateSoundStream(Core::System&)
{
}
}  // namespace AudioCommon
