// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Online/RoomWatchInput.h"

#include "Common/CommonTypes.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/Harness/Harness.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/InputConfig.h"

namespace Online::RoomWatchInput
{
// START or B on the host's controllers as they are now: the GameCube ports (mapped or a Wii U
// adapter's) and the emulated Wii Remotes' B and +. A watch that is buffering stands still between
// two frames, and so does the game's pad thread: its newest sample is from before (bug report
// #171, a sitter's view stuck while the players' upload paused, where START and B did nothing).
bool HostLeaveDown()
{
  if (!Pad::IsInitialized())
    return false;
  // As the CPU thread before an SI poll: the controllers count only while the game window has
  // focus, unless background input is on.
  Core::UpdateInputGate(!Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT),
                        Config::Get(Config::MAIN_LOCK_CURSOR));
  for (int chan = 0; chan < 4; ++chan)
  {
    const SerialInterface::SIDevices type = Config::Get(Config::GetInfoForSIDevice(chan));
    GCPadStatus pad{};
    if (type == SerialInterface::SIDEVICE_WIIU_ADAPTER)
    {
      pad = GCAdapter::Peek(chan);
      Harness::ApplyPadOverride(chan, &pad);
    }
    else if (SerialInterface::SIDevice_IsGCController(type))
    {
      pad = Pad::GetStatus(chan);
    }
    else
    {
      continue;
    }
    if (pad.isConnected && (pad.button & (PAD_BUTTON_START | PAD_BUTTON_B)) != 0)
      return true;
  }
  if (!Core::System::GetInstance().IsWii() || !Wiimote::GetConfig() ||
      Wiimote::GetConfig()->ControllersNeedToBeCreated())
  {
    return false;
  }
  // The Wii Remote's buttons group: A, B, 1, 2, -, +, HOME.
  static constexpr u16 WIIMOTE_LEAVE[] = {0, 1, 0, 0, 0, 1, 0};
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  for (int i = 0; i < 4; ++i)
  {
    if (Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::Emulated)
      continue;
    const auto* buttons = static_cast<const ControllerEmu::Buttons*>(
        Wiimote::GetWiimoteGroup(i, WiimoteEmu::WiimoteGroup::Buttons));
    u16 down = 0;
    buttons->GetState(&down, WIIMOTE_LEAVE);
    if (down != 0)
      return true;
  }
  return false;
}
}  // namespace Online::RoomWatchInput
