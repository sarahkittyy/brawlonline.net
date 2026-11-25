// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/HW/WiimoteReal/WiimoteReal.h (core/standalone only): the
// engine finds real Wii Remotes itself, Dolphin's ControllerInterface lists none.

#pragma once

namespace WiimoteReal
{
inline void PopulateDevices()
{
}
}  // namespace WiimoteReal
