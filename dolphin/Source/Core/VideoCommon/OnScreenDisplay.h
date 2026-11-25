// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Dolphin's on-screen messages are Brawl Online's shared ones (core/ui), which EmissaryRecomp draws
// too. A message's icon is a VideoCommon::CustomTextureData::ArraySlice::Level (an achievement's
// badge), made a texture by OnScreenUI's BrawlOnline::UI::Hooks::make_icon.

#pragma once

#include "BrawlOnline/UI/OnScreenDisplay.h"
