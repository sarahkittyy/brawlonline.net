// Copyright 2026 brawlonline.net
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Where input suggestions (Core/Rollback/InputSuggest.h) get their looks at the local controller
// between the game's reads: the engine's controller code.
// - Dolphin (InputSuggestSource.cpp): the Wii U adapter's reports, and for a controller mapped in
//   Dolphin a sampler thread of InputSuggest's that updates the input devices and reads the pad.
// - core/standalone (core/compat/src/InputSuggestSource.cpp): neither. The engine reads its own
//   controllers where it can (EmissaryRecomp: on its game thread, while it waits for the next
//   field) and hands each look to Suggest::Sample.

#include <optional>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Gprb::Suggest::Source
{
using ReportHook = void (*)(int chan, const GCPadStatus& st, Clock::time_point t);
// The adapter's reports go to `hook` from now on as well (nullptr: no more).
void SetAdapterReportHook(ReportHook hook);
// Whether InputSuggest runs its sampler thread for mapped controllers (else the engine samples).
bool HasSamplerThread();
// On the sampler thread, once at its start.
void SamplerThreadStarts();
// On the sampler thread: SI channel `chan`'s mapped controller now (the devices updated, the input
// gate as the CPU thread sets it before an SI poll), or nothing while none can be read.
std::optional<GCPadStatus> SampleMapped(int chan);
}  // namespace Gprb::Suggest::Source
