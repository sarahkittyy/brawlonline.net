// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

// Brawl's gfPadStatus (gf_pad.o). Layout/offsets are load-bearing: this is memcpy'd directly
// into gfPadSystem's raw pad slots (gfPadSystem+0x40, stride 0x40), so it must stay byte-for-byte
// identical to what updateLowGC/updateLowWii produce.
namespace gfPadError
{
enum PadError : s8
{
  NONE = 0,
  NO_CONTROLLER = -1,
  NOT_READY = -2,
  TRANSFER = -3,
};
}

namespace gfPadType
{
enum PadType
{
  GCC = 0,
  WII_CLASSIC = 1,
  WIIMOTE = 2,
  NUNCHUK = 3,
};
}

union gfPadButtons
{
  enum Buttons : u32
  {
    DLeft = 0x1,
    DDown = 0x2,
    DRight = 0x4,
    DUp = 0x8,
    Z = 0x10,
    R = 0x20,
    L = 0x40,
    A = 0x100,
    B = 0x200,
    X = 0x400,
    Y = 0x800,
    Start = 0x1000,
  };
  u32 bits;
};

struct gfPadStatus
{
  gfPadButtons m_buttonsCurrentFrame;          // 0x0
  gfPadButtons m_buttonsCurrentFrame2;         // 0x4
  gfPadButtons m_buttonsHeld;                  // 0x8
  gfPadButtons m_buttonsPressedThisFrame;      // 0xC
  gfPadButtons m_buttonsReleasedThisFrame;     // 0x10
  gfPadButtons m_buttonsPressedThisFrame2;     // 0x14

  // Wiimote motion tracking floats. Always 0 for a GameCube controller.
  float _0x18;
  float _0x1c;
  float _0x20;
  float _0x24;
  float _0x28;
  float _0x2c;

  s8 m_stickX;
  s8 m_stickY;
  s8 m_subStickX;
  s8 m_subStickY;
  s8 m_lTriggerAnalog;
  s8 m_rTriggerAnalog;
  s8 _0x36;

  s8 _0x37;
  gfPadError::PadError m_error;
  s8 _0x39;
  s8 _0x3a;
  s8 _0x3b;

  gfPadType::PadType m_controllerType;
};
static_assert(sizeof(gfPadStatus) == 0x40, "gfPadStatus is the wrong size!");
