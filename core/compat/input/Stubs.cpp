// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// What Dolphin's input code needs from parts of Dolphin that core/standalone leaves out
// (BrawlOnline/DolphinPads.h).

#include <memory>

#include "Core/Host.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#include "InputCommon/ControllerInterface/InputBackend.h"
#ifdef _WIN32
#include "InputCommon/ControllerInterface/WGInput/WGInput.h"
#endif

namespace BrawlOnline::DolphinPads
{
bool HasFocus();
}

bool Host_RendererHasFocus()
{
  return BrawlOnline::DolphinPads::HasFocus();
}

bool Host_TASInputHasFocus()
{
  return false;
}

// DSU (cemuhook) servers are off unless a player turns them on in Dolphin; they are not read here.
namespace ciface::DualShockUDPClient
{
namespace
{
class NoBackend final : public ciface::InputBackend
{
public:
  using InputBackend::InputBackend;
  void PopulateDevices() override {}
};
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<NoBackend>(controller_interface);
}
}  // namespace ciface::DualShockUDPClient

#ifdef _WIN32
// Windows.Gaming.Input needs C++/WinRT, which the engine's toolchain lacks: WGInput/ devices are
// not listed (XInput, DInput and SDL are).
namespace ciface::WGInput
{
void Init()
{
}
void DeInit()
{
}
void PopulateDevices()
{
}
}  // namespace ciface::WGInput
#endif
