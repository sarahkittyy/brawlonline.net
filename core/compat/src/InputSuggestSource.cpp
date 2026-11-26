// Copyright 2026 brawlonline.net
// SPDX-License-Identifier: GPL-2.0-or-later

// core/standalone's controllers for input suggestions (Core/Rollback/InputSuggestSource.h): no
// adapter report stream and no sampler thread of the core's. The engine reads its controllers
// itself and hands each look to Gprb::Suggest::Sample (EmissaryRecomp: on its game thread, which
// owns them, while it waits for the next field).

#include "Core/Rollback/InputSuggestSource.h"

namespace Gprb::Suggest::Source
{
void SetAdapterReportHook(ReportHook)
{
}

bool HasSamplerThread()
{
  return false;
}

void SamplerThreadStarts()
{
}

std::optional<GCPadStatus> SampleMapped(int)
{
  return std::nullopt;
}
}  // namespace Gprb::Suggest::Source
