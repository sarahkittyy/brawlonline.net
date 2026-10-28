// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace GCAdapter
{
void Init();
void ResetRumble();
void Shutdown();
void SetAdapterCallback(std::function<void(void)> func);

// P+ change: for poll rate display
bool IsReadingAtReducedRate();
double ReadRate();

// Buttons have PAD_GET_ORIGIN set on new connection
// Netplay and CSIDevice_GCAdapter make use of this.
GCPadStatus Input(int chan);
// The state the controller in this port had at host time `t` (a recent past), from the read
// thread's timestamped payloads: the last one that arrived at or before `t`. Empty when `t` is
// older than the payloads kept, or none arrived yet (the caller reads Input). As Input otherwise:
// the origin on a new connection, a neutral state without an adapter.
std::optional<GCPadStatus> InputAt(int chan, Clock::time_point t);
// The controller in this port now, read from outside the emulated game (a room watch standing
// still): unlike Input, a new connection's origin stays for the game's next read (nothing pressed
// meanwhile). A neutral state without an adapter.
GCPadStatus Peek(int chan);

void Output(int chan, u8 rumble_command);
bool IsDetected(const char** error_message);
bool DeviceConnected(int chan);
void ResetDeviceType(int chan);
// The controller in this port at rest (its state when it was plugged in, or when the adapter was
// first read): what CSIDevice_GCAdapter tells the game as the controller's origin. Empty when no
// controller is in the port.
std::optional<GCPadStatus> Origin(int chan);

// Callable from any thread. Returns 0 when the adapter is not detected.
double GetCurrentPollRate();

}  // namespace GCAdapter
