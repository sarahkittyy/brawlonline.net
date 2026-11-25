// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's Core/Host.h (core/standalone only): the two questions Dolphin's
// DInput keyboard and mouse ask (BrawlOnline/DolphinPads.h, SetFocusQuery).

#pragma once

bool Host_RendererHasFocus();
bool Host_TASInputHasFocus();
