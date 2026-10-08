// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Internal interfaces shared between the harness translation units. Not for use outside
// Core/Harness.

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Harness/Harness.h"
#include "Core/Online/NetPlaySession.h"
#include "InputCommon/GCPadStatus.h"

namespace Harness::Internal
{
const HostCallbacks& GetHost();

// Number of VI fields since the last boot.
u64 GetFrame();
// Number of game pad polls since the last boot (see GetPollSourceName()).
u64 GetInputPolls();
// Number of SI polls since the last boot.
u64 GetSIPolls();
// "game" (Brawl's per-frame pad read), "si" (every SI poll) or "unknown" (before the first poll).
const char* GetPollSourceName();

// Blocks until pred() returns true or the timeout expires. pred is re-evaluated whenever a VI
// field passes, an SI poll happens, or the emulation state changes. Returns pred()'s final value.
bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout);
void NotifyWaiters();

// Pad overrides. Ports are local controller ports (0-3).
void PadSet(int port, const GCPadStatus& status);
void PadClear(int port);
struct ScriptTiming
{
  u64 starts_at;
  u64 ends_at;
};
// start: nullopt means "next poll". Returns an error message on failure.
std::optional<std::string> PadScript(int port, std::vector<GCPadStatus> timeline,
                                     std::optional<u64> start, ScriptTiming* timing);
struct ScriptStatus
{
  bool active;
  u64 remaining;
};
ScriptStatus PadScriptStatus(int port);

// Frame advance: request a CPU break once the frame counter reaches `target`.
void RequestBreakAtFrame(u64 target);
void CancelBreakAtFrame();

// Netplay: the headless session lives in Core/Online (NetPlaySession.h), shared with online
// play; the harness drives it directly (netplay_host, netplay_join, ...).
namespace NetPlaySession = ::Online::NetPlaySession;

// Online (HarnessOnline.cpp): registers the "record" session backend factory, a test backend
// that records the hand-off and holds the P2P link (online_session_backend backend=record).
void RegisterOnlineTestBackends();

// Crash reporter (HarnessCrash.cpp): symbolized stack on stderr/log for unhandled exceptions.
void InstallCrashReporter();

// Server (HarnessServer.cpp).
bool StartServer(u16 port);
void StopServer();
}  // namespace Harness::Internal
