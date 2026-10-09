// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The gameplay-only session (Gprb::Session) as an Online::SessionBackend: matchmaking hands a
// connected peer over (Online::Session::Start), and this backend turns it into a Gprb::Session
// network session on the punched port (docs/backend-design.md 5.5).

#pragma once

#include <memory>
#include <string>

#include "Core/Online/OnlineSession.h"

namespace Gprb
{
struct OnlineBackendOptions
{
  int delay = 2;                     // GekkoNet input delay
  std::string region_set = "gp-v21";  // Data/Sys/Rollback/<name>.json
  bool dedupe_resim_sounds = true;
};

void SetOnlineBackendOptions(const OnlineBackendOptions& options);
std::unique_ptr<Online::SessionBackend> MakeOnlineBackend();
// Online::Session::RegisterFactory("gameplay", MakeOnlineBackend). The frontends (DolphinQt and
// DolphinNoGUI) call it at start-up, before Online::Session::SelectConfigured(); "gameplay" is the
// default [Online] SessionBackend.
void RegisterOnlineBackend();
}  // namespace Gprb
