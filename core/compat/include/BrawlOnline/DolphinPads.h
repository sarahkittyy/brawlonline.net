// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The controllers as set up in Dolphin's Controllers settings, for an engine that is not Dolphin
// (core/standalone, docs/design/brawl-recompiled.md "Dolphin's controls"). The netplay User
// folder's Config/GCPadNew.ini is loaded by Dolphin's own GCPad class (Core/HW/GCPadEmu.cpp) over
// Dolphin's own input code (InputCommon: expressions, sticks, modifiers, dead zones, calibration)
// and its Windows devices (DInput keyboard and mouse, DInput joysticks, XInput); the engine adds
// the devices it opens itself (SDL gamepads, as Dolphin's SDL backend names them). A port then
// reads as it would in Dolphin.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "InputCommon/GCPadStatus.h"

namespace ciface::Core
{
class Device;
}

namespace BrawlOnline::DolphinPads
{
// Dolphin's SIDevice values the engine acts on (Core/HW/SI/SI_Device.h).
inline constexpr int SI_NONE = 0;
inline constexpr int SI_STANDARD_CONTROLLER = 6;
inline constexpr int SI_GC_ADAPTER = 12;

// Starts Dolphin's device backends (once; `window` is the engine's native window, an HWND on
// Windows, for DInput's keyboard) and loads `user_folder`/Config/GCPadNew.ini and Dolphin.ini's
// [Core] SIDevice0-3 and [Input] BackgroundInput. False when there is no GCPadNew.ini: Dolphin has
// never saved a controller set-up there, and the engine keeps its own controls.
bool Load(const std::string& user_folder, void* window);
// True after a Load that found GCPadNew.ini.
bool Active();
// Load again when GCPadNew.ini or Dolphin.ini changed since (Dolphin's Controllers window saved).
// True when it loaded.
bool ReloadIfChanged();

// What Dolphin plugs into GameCube port `port` (0-3): SI_STANDARD_CONTROLLER, SI_GC_ADAPTER, ...
int SiDevice(int port);

// Reads every device once (Dolphin's ControllerInterface::UpdateInput), with the input gate as
// Dolphin sets it at each field: open when the game window has focus or BackgroundInput is on.
void UpdateInput(bool window_has_focus);
// Dolphin's GCPad::GetInput for `port` (a standard controller), as of the last UpdateInput.
GCPadStatus GetStatus(int port);
// Dolphin's GCPad::SetOutput: the port's Rumble/Motor expression.
void SetRumble(int port, double strength);

// The port's device line ("DInput/0/Keyboard Mouse"), for the engine's menu.
std::string DefaultDevice(int port);

// Devices the engine opens itself (SDL gamepads). Added devices get Dolphin's ids (the first free
// one for their source and name) and the configs are bound again.
void AddDevice(std::shared_ptr<ciface::Core::Device> device);
void RemoveDevices(const std::function<bool(const ciface::Core::Device*)>& which);

// With SDL (the parent's BRAWL_ONLINE_SDL3_TARGET): adds the joysticks SDL lists now that are not
// listed yet, as Dolphin's SDL backend does (SDL/<id>/<name>), and removes the ones that went.
// Call before UpdateInput, on the thread that reads the pads.
void SyncSdlDevices();

// The engine's answer to Host_RendererHasFocus (DInput's mouse centring asks it).
void SetFocusQuery(std::function<bool()> has_focus);

void Shutdown();
}  // namespace BrawlOnline::DolphinPads
