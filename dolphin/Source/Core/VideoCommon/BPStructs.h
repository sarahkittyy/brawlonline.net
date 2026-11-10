// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

void BPInit();
void BPReload();

// Draws a game makes once and copies into a texture, armed from the CPU thread by an HLE hook
// (Brawl's name tags in any Brawl build: HLE_Misc.cpp, BrawlTagRenderHook). From ArmNoSkipDraws
// until the GPU thread runs the texture copy into the address SetNoSkipDrawsCopyDest gave, a draw
// whose shader is not compiled yet waits for it instead of being skipped, as with the P+ plugin's
// BPMEM_NO_SKIP_DRAWS mark. Nothing of it is in the emulated machine's state.
void ArmNoSkipDraws();
void SetNoSkipDrawsCopyDest(u32 guest_address);
// GPU thread: armed (VertexManagerBase::UpdatePipelineObject).
bool NoSkipDrawsArmed();
