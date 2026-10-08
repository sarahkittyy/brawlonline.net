// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Perf.h"
#include <Common/Logging/Log.h>

namespace Rollback
{
#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI) && defined(ROLLBACK_PROFILE_SUPERLUMINAL)
PerformanceAPI_Functions g_perf_api = {};
PerformanceAPI_ModuleHandle moduleHandle = {};
#endif

void PerfInit()
{
#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI) && defined(ROLLBACK_PROFILE_SUPERLUMINAL)
  if (moduleHandle != NULL)
    return;
  moduleHandle = PerformanceAPI_LoadFrom(
      L"C:\\Program Files\\Superluminal\\Performance\\API\\dll\\x64\\PerformanceAPI.dll",
      &g_perf_api);
  if (moduleHandle == NULL)
  {
    moduleHandle = PerformanceAPI_LoadFrom(L"PerformanceAPI.dll", &g_perf_api);
  }
  if (moduleHandle == NULL)
  {
    WARN_LOG_FMT(BRAWLBACK, "Failed to find superluminal api");
  }
#endif
}

}  // namespace Rollback
