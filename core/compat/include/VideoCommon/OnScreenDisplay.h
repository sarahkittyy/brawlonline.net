// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Compat stand-in for Dolphin's VideoCommon/OnScreenDisplay.h (core/standalone only): both are the
// shared on-screen messages (core/ui). A build without the UI sends them to the host
// (BrawlOnline::Host::OnScreenMessage, compat/src/Compat.cpp).

#pragma once

#include "BrawlOnline/UI/OnScreenDisplay.h"
