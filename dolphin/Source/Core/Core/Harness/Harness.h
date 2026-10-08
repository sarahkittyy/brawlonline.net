// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Automation harness: a line-delimited JSON control server that lets scripts drive Dolphin with
// no human present. The wire protocol is documented in docs/harness-protocol.md.
//
// When the harness is not enabled (no --harness-port and no PPR_HARNESS_PORT), every hook below
// reduces to a single relaxed atomic load.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

struct BootParameters;
struct GCPadStatus;

namespace Core
{
class System;
}

namespace Harness
{
constexpr int PROTOCOL_VERSION = 1;

// Provided by the frontend (DolphinNoGUI or DolphinQt). All callbacks are invoked on the host
// thread (from a Core::QueueHostJob job).
struct HostCallbacks
{
  // Leave the frontend's main loop so the process shuts down cleanly with exit code 0.
  std::function<void()> request_quit;
  // Boot the given parameters (used by netplay to start the game). Returns false on failure.
  std::function<bool(std::unique_ptr<BootParameters>)> boot;
  // Whether the harness may create its own headless netplay session (NoGUI only).
  bool supports_netplay = false;
  // Whether the frontend wants the process to keep running when emulation stops. Informational.
  std::string frontend_name;
};

// Resolves the port from the command line value (may be empty) and the PPR_HARNESS_PORT
// environment variable. Returns nullopt when the harness should stay disabled.
std::optional<u16> ResolvePort(const std::string& command_line_value);

// Starts the server thread listening on 127.0.0.1:<port>. Also applies the harness session
// defaults (audio muted unless PPR_HARNESS_AUDIO=1). Must be called after config is loaded.
bool Start(u16 port, HostCallbacks callbacks);
// Stops the server thread and tears down any harness-owned netplay session. Host thread only.
void Shutdown();

namespace detail
{
extern std::atomic<bool> g_active;
extern std::atomic<u32> g_pad_override_mask;
void ApplyPadOverrideSlow(int port, GCPadStatus* status);
void OnSIPollSlow();
void OnGamePadReadSlow();
void OnVIFieldSlow(Core::System& system);
}  // namespace detail

inline bool IsActive()
{
  return detail::g_active.load(std::memory_order_relaxed);
}

// Called wherever a local GameCube controller's status is produced (Pad::GetStatus and the
// GameCube adapter paths). Replaces the status if the harness overrides that local port.
inline void ApplyPadOverride(int port, GCPadStatus* status)
{
  if (detail::g_pad_override_mask.load(std::memory_order_relaxed) & (1u << (port & 3)))
    detail::ApplyPadOverrideSlow(port, status);
}

// Whether the harness currently overrides (or has a pad script scheduled on) a local port.
inline bool IsPadOverridden(int port)
{
  return (detail::g_pad_override_mask.load(std::memory_order_relaxed) & (1u << (port & 3))) != 0;
}

// CPU thread: once per SI poll (SerialInterfaceManager::UpdateDevices), before devices are read.
inline void OnSIPoll()
{
  if (IsActive())
    detail::OnSIPollSlow();
}

// CPU thread: right after the game finished reading its pads for one game frame (Brawl's
// gfPadSystem::updateLow -> updateLowGC; not called for rollback resimulation passes).
inline void OnGamePadRead()
{
  if (IsActive())
    detail::OnGamePadReadSlow();
}

// CPU thread: once per VI field boundary.
inline void OnVIField(Core::System& system)
{
  if (IsActive())
    detail::OnVIFieldSlow(system);
}
}  // namespace Harness
