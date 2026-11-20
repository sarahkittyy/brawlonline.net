// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/UI/Hooks.h"

#include <atomic>
#include <utility>

namespace BrawlOnline::UI
{
namespace
{
const Hooks s_none;
// Messages can come from any thread while the engine sets its hooks: each set is a new object,
// published whole and never freed (an engine sets them once or twice a run).
std::atomic<const Hooks*> s_hooks{&s_none};
}  // namespace

void SetHooks(Hooks hooks)
{
  s_hooks.store(new Hooks(std::move(hooks)), std::memory_order_release);
}

const Hooks& GetHooks()
{
  return *s_hooks.load(std::memory_order_acquire);
}
}  // namespace BrawlOnline::UI
