// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/InputDisplayFeed.h"

#include <algorithm>
#include <cmath>
#include <variant>

#include "BrawlOnline/Online/InputView.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/Rollback/MachineSession.h"
#include "InputCommon/GCPadStatus.h"

namespace InputDisplayFeed
{
namespace
{
using Online::InputView::Pad;
using Online::InputView::Type;

// gfPadStatus's buttons (Online/InputView.h): the D-pad's as a GameCube's, the Wii ones above.
constexpr u32 D_LEFT = 0x1, D_RIGHT = 0x2, D_DOWN = 0x4, D_UP = 0x8;
constexpr u32 Z = 0x10, R = 0x20, L = 0x40, A = 0x100, B = 0x200, X = 0x400, Y = 0x800;
constexpr u32 START = 0x1000, ZL = 0x4000, ZR = 0x8000;
constexpr u32 WII_1 = 0x10000, WII_2 = 0x20000, WII_C = 0x40000, WII_MINUS = 0x80000,
              WII_A = 0x200000, WII_B = 0x400000;

// A GameCube controller's buttons are the record's already (the D-pad, Z, R, L, A, B, X, Y,
// START), without the origin bits.
constexpr u16 GC_BUTTONS = 0x1F7F;

// A Wii stick's raw value around its centre, as the record's -100..100: 100 where Dolphin's
// emulated stick puts its gate (`gate` of the way from the centre to each end, rounded as its raw
// value is), a real one's about the same.
s8 WiiAxis(int raw, int center, int range, float gate)
{
  const float to = std::round((raw >= center ? range - center : center) * gate);
  const float v = to > 0 ? std::round((raw - center) * 100.0f / to) : 0.0f;
  return static_cast<s8>(std::clamp(v, -100.0f, 100.0f));
}

void Map(u16 from, u16 bit, u32 to, u32* buttons)
{
  if (from & bit)
    *buttons |= to;
}

bool Skipped()
{
  // A rollback's re-run frames were shown already.
  return Machine::IsResimulationPass();
}
}  // namespace

void OnGameCubeRead(int port, const GCPadStatus& status)
{
  if (port < 0 || port >= 4 || Skipped())
    return;
  Pad p;
  p.type = Type::GameCube;
  p.buttons = status.button & GC_BUTTONS;
  p.stick_x = static_cast<s8>(status.stickX - GCPadStatus::MAIN_STICK_CENTER_X);
  p.stick_y = static_cast<s8>(status.stickY - GCPadStatus::MAIN_STICK_CENTER_Y);
  p.sub_x = static_cast<s8>(status.substickX - GCPadStatus::C_STICK_CENTER_X);
  p.sub_y = static_cast<s8>(status.substickY - GCPadStatus::C_STICK_CENTER_Y);
  p.trigger_l = status.triggerLeft;
  p.trigger_r = status.triggerRight;
  Online::InputView::SetHardwarePad(static_cast<u32>(port), p);
}

void OnWiimoteRead(int index, const WiimoteEmu::DesiredWiimoteState& state)
{
  if (index < 0 || index >= 4 || Skipped())
    return;
  using WiimoteEmu::Classic;
  using WiimoteEmu::Nunchuk;
  using WiimoteEmu::Wiimote;
  const u16 wii = state.buttons.hex;
  Pad p;

  if (const auto* cc = std::get_if<Classic::DataFormat>(&state.extension.data))
  {
    p.type = Type::Classic;
    const u16 b = cc->GetButtons();
    Map(b, Classic::BUTTON_A, A, &p.buttons);
    Map(b, Classic::BUTTON_B, B, &p.buttons);
    Map(b, Classic::BUTTON_X, X, &p.buttons);
    Map(b, Classic::BUTTON_Y, Y, &p.buttons);
    Map(b, Classic::TRIGGER_L, L, &p.buttons);
    Map(b, Classic::TRIGGER_R, R, &p.buttons);
    Map(b, Classic::BUTTON_ZL, ZL, &p.buttons);
    Map(b, Classic::BUTTON_ZR, ZR, &p.buttons);
    Map(b, Classic::BUTTON_PLUS, START, &p.buttons);
    Map(b, Classic::BUTTON_MINUS, WII_MINUS, &p.buttons);
    Map(b, Classic::PAD_LEFT, D_LEFT, &p.buttons);
    Map(b, Classic::PAD_RIGHT, D_RIGHT, &p.buttons);
    Map(b, Classic::PAD_DOWN, D_DOWN, &p.buttons);
    Map(b, Classic::PAD_UP, D_UP, &p.buttons);
    constexpr float gate = static_cast<float>(Classic::STICK_GATE_RADIUS) / Classic::CAL_STICK_RADIUS;
    const auto left = cc->GetLeftStick().value;
    const auto right = cc->GetRightStick().value;
    p.stick_x = WiiAxis(left.x, Classic::LEFT_STICK_CENTER, Classic::LEFT_STICK_RANGE, gate);
    p.stick_y = WiiAxis(left.y, Classic::LEFT_STICK_CENTER, Classic::LEFT_STICK_RANGE, gate);
    p.sub_x = WiiAxis(right.x, Classic::RIGHT_STICK_CENTER, Classic::RIGHT_STICK_RANGE, gate);
    p.sub_y = WiiAxis(right.y, Classic::RIGHT_STICK_CENTER, Classic::RIGHT_STICK_RANGE, gate);
    p.trigger_l = static_cast<u8>(cc->GetLeftTrigger().value * 255 / Classic::TRIGGER_RANGE);
    p.trigger_r = static_cast<u8>(cc->GetRightTrigger().value * 255 / Classic::TRIGGER_RANGE);
  }
  else
  {
    Map(wii, Wiimote::BUTTON_A, WII_A, &p.buttons);
    Map(wii, Wiimote::BUTTON_B, WII_B, &p.buttons);
    Map(wii, Wiimote::BUTTON_ONE, WII_1, &p.buttons);
    Map(wii, Wiimote::BUTTON_TWO, WII_2, &p.buttons);
    Map(wii, Wiimote::BUTTON_PLUS, START, &p.buttons);
    Map(wii, Wiimote::BUTTON_MINUS, WII_MINUS, &p.buttons);
    if (const auto* nc = std::get_if<Nunchuk::DataFormat>(&state.extension.data))
    {
      // Upright, its D-pad as a GameCube's.
      p.type = Type::Nunchuk;
      Map(wii, Wiimote::PAD_LEFT, D_LEFT, &p.buttons);
      Map(wii, Wiimote::PAD_RIGHT, D_RIGHT, &p.buttons);
      Map(wii, Wiimote::PAD_DOWN, D_DOWN, &p.buttons);
      Map(wii, Wiimote::PAD_UP, D_UP, &p.buttons);
      Map(nc->GetButtons(), Nunchuk::BUTTON_C, WII_C, &p.buttons);
      Map(nc->GetButtons(), Nunchuk::BUTTON_Z, Z, &p.buttons);
      const auto stick = nc->GetStick().value;
      constexpr float gate = static_cast<float>(Nunchuk::STICK_GATE_RADIUS) / Nunchuk::STICK_RADIUS;
      p.stick_x = WiiAxis(stick.x, Nunchuk::STICK_CENTER, Nunchuk::STICK_RANGE, gate);
      p.stick_y = WiiAxis(stick.y, Nunchuk::STICK_CENTER, Nunchuk::STICK_RANGE, gate);
    }
    else
    {
      // Alone (or with an extension the display has no layout for), held sideways: the D-pad as
      // the player holds it, the remote's up to their left.
      p.type = Type::WiiRemote;
      Map(wii, Wiimote::PAD_UP, D_LEFT, &p.buttons);
      Map(wii, Wiimote::PAD_DOWN, D_RIGHT, &p.buttons);
      Map(wii, Wiimote::PAD_LEFT, D_DOWN, &p.buttons);
      Map(wii, Wiimote::PAD_RIGHT, D_UP, &p.buttons);
    }
  }
  Online::InputView::SetHardwarePad(static_cast<u32>(4 + index), p);
}

void OnViField()
{
  if (Skipped())
    return;
  Online::InputView::OnHardwareField(Machine::InputDisplayPads());
}
}  // namespace InputDisplayFeed
