// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define DINPUT_SOURCE_NAME "DInput"

#include <windows.h>
#include <list>
#include <optional>
#include <string>

#include "InputCommon/ControllerInterface/DInput/DInput8.h"

namespace ciface::DInput
{
// BOOL CALLBACK DIEnumEffectsCallback(LPCDIEFFECTINFO pdei, LPVOID pvRef);
BOOL CALLBACK DIEnumDeviceObjectsCallback(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef);
BOOL CALLBACK DIEnumDevicesCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef);
std::string GetDeviceName(const LPDIRECTINPUTDEVICE8 device);

// The attached game controllers, or nothing if DirectInput didn't list them in time.
std::optional<std::list<DIDEVICEINSTANCE>> EnumGameControllers(IDirectInput8* idi8);
// False while an enumeration that was given up on hasn't returned.
bool IsResponding();

void Init();
void PopulateDevices(HWND hwnd);
void ChangeWindow(HWND hwnd);
void DeInit();
}  // namespace ciface::DInput
