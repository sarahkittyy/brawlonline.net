// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/Engine.h"

#include <cstdio>
#include <cstdlib>

namespace BrawlOnline
{
namespace
{
Engine* s_engine = nullptr;
}

Engine& GetEngine()
{
  if (!s_engine)
  {
    std::fputs("BrawlOnline: no engine set (SetEngine at start-up)\n", stderr);
    std::abort();
  }
  return *s_engine;
}

void SetEngine(Engine* engine)
{
  s_engine = engine;
}

EngineIdentity ThisEngine()
{
  if (!s_engine)
    return {"unknown", ""};
  return s_engine->Identity();
}
}  // namespace BrawlOnline
