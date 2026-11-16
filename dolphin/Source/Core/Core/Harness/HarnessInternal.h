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
// The port's buttons count VI fields from now: buttons[i] is bit i of the count of `rate_hz`
// periods since the call, worked out from the host clock whenever the port is read (also for an
// earlier time: PadOverrideAt). A time base with no jitter for timing tests. Until pad_set or
// pad_clear.
void PadCounter(int port, double rate_hz, std::vector<u16> buttons);
// pad_tape: the port's state as a function of host time, for input timing tests
// (harness/tools/input_fidelity.py). Each row takes effect at its host time and lasts until the
// next; the port is read whenever the game reads it, also for an earlier time (PadOverrideAt), as
// the device model allows:
enum class PadTapeModel
{
  Ideal,    // the state at any time, past ones included (what every harness pad does)
  Mapped,   // a pad mapped in Dolphin (SDL, XInput, a keyboard): read now, no past
  Adapter,  // the Wii U adapter: the last report by then (one per period from phase, each latency
            // after its controller read), and its last `history` reports for past instants
            // (GCAdapter::InputAt keeps 128: 128 ms at 1000 Hz, 1 s at the stock 125 Hz)
};
struct PadTapeRow
{
  Clock::time_point at;
  GCPadStatus pad;
};
struct PadTapeSpec
{
  std::vector<PadTapeRow> rows;  // in time order
  PadTapeModel model = PadTapeModel::Ideal;
  Clock::duration period{};
  Clock::time_point phase{};
  Clock::duration latency{};
  u32 history = 128;  // Adapter: reports kept for past instants
  Clock::time_point submitted{};  // set by PadTape
};
// Replaces the port's override (pad_set, pad_counter, a script) until pad_set, pad_script,
// pad_counter, pad_clear or PadTapeClear. Returns the host time it took effect.
Clock::time_point PadTape(int port, PadTapeSpec tape);
// More rows at the tape's end (none before its last row); the model stays. An error message when
// the port has no tape.
std::optional<std::string> PadTapeAppend(int port, std::vector<PadTapeRow> rows);
// Ends the tape; the port keeps presenting the tape's current state (as pad_set).
void PadTapeClear(int port);
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
// The rumble a local port got since the boot: motor starts (off -> on) and whether it is on.
struct RumbleStatus
{
  u64 starts;
  bool on;
};
RumbleStatus PadRumbleStatus(int port);

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
