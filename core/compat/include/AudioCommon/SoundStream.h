// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's AudioCommon/SoundStream.h (core/standalone only).

#pragma once

class SoundStream
{
public:
  void SetVolume(int) {}
};
