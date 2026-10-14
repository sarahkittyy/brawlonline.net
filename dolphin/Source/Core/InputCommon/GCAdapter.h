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
