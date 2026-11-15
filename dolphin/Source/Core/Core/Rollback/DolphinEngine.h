// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Dolphin as the online core's engine (BrawlOnline::Engine, docs/design/brawl-online-core.md §2):
// the emulated CPU at the session's hooks in Brawl's main loop.

#pragma once

namespace Rollback
{
// Makes Dolphin the online core's engine. Already done when the program loads; Core::Init and the
// unit tests call it too.
void RegisterDolphinEngine();
}  // namespace Rollback
