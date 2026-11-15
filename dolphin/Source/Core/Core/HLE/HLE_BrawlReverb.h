// Copyright 2026 Brawl Online
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace HLE_BrawlReverb
{
// Brawl's AUX reverb (main.dol 0x80202A58) in C++ (see the .cpp).
void ReverbHiHook(const Core::CPUThreadGuard& guard);
// Its return (the blr at 0x80202F6C): PPR_REVERB_HLE=check compares there.
void ReverbHiReturnHook(const Core::CPUThreadGuard& guard);
u32 FunctionAddress();
u32 ReturnAddress();
}  // namespace HLE_BrawlReverb
