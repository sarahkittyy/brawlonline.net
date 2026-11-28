// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The input display's view (Core/Online/InputView.h): the game's pad records decoded by their
// controller type, a session's ports picked from their GameCube or Wii Remote pad, and outside one
// this machine's controller in use (the pad pressed last).

#include <array>

#include <gtest/gtest.h>

#include "BrawlOnline/Online/InputView.h"

using namespace Online::InputView;

namespace
{
using Pads = std::array<u8, PADS * PAD_SIZE>;

// Every pad with no controller (error 0xFF), as gfPadSystem has an empty one.
Pads Empty()
{
  Pads p{};
  for (u32 i = 0; i < PADS; ++i)
    p[i * PAD_SIZE + 0x38] = 0xFF;
  return p;
}

void Plug(Pads& p, u32 pad, Type type, u32 buttons = 0, s8 sx = 0, s8 sy = 0)
{
  u8* r = p.data() + pad * PAD_SIZE;
  r[0] = static_cast<u8>(buttons >> 24);
  r[1] = static_cast<u8>(buttons >> 16);
  r[2] = static_cast<u8>(buttons >> 8);
  r[3] = static_cast<u8>(buttons);
  r[0x30] = static_cast<u8>(sx);
  r[0x31] = static_cast<u8>(sy);
  r[0x38] = 0;
  r[0x3F] = static_cast<u8>(type);
}
}  // namespace

TEST(InputView, DecodesARecord)
{
  Pads p = Empty();
  Plug(p, 0, Type::GameCube, 0x1100, 127, -128);
  u8* r = p.data();
  r[0x32] = 10;
  r[0x33] = static_cast<u8>(-20);
  r[0x34] = 200;
  r[0x35] = 7;
  Pad pad;
  ASSERT_TRUE(Decode(r, &pad));
  EXPECT_EQ(pad.type, Type::GameCube);
  EXPECT_EQ(pad.buttons, 0x1100u);
  EXPECT_EQ(pad.stick_x, 127);
  EXPECT_EQ(pad.stick_y, -128);
  EXPECT_EQ(pad.sub_x, 10);
  EXPECT_EQ(pad.sub_y, -20);
  EXPECT_EQ(pad.trigger_l, 200);
  EXPECT_EQ(pad.trigger_r, 7);
  // No controller, or a type the game does not have: nothing to draw.
  EXPECT_FALSE(Decode(p.data() + PAD_SIZE, &pad));
  r[0x3F] = 4;
  EXPECT_FALSE(Decode(r, &pad));
  r[0x3F] = 0;
  r[0x3C] = 1;
  EXPECT_FALSE(Decode(r, &pad));
}

TEST(InputView, SessionPortsFromTheirGameCubeOrWiiPad)
{
  Reset();
  EXPECT_EQ(Get().frames, 0u);
  Pads p = Empty();
  Plug(p, 0, Type::GameCube, 0x100);        // port 1 on a GameCube controller
  Plug(p, 4 + 1, Type::Nunchuk, 0x200000);  // port 2 on a Wii Remote with a Nunchuk
  Plug(p, 2, Type::GameCube);               // port 3 (not shown)
  OnFrameEnd(p.data(), 0b011);
  View v = Get();
  EXPECT_EQ(v.frames, 1u);
  ASSERT_EQ(v.count, 2u);
  EXPECT_EQ(v.pads[0].port, 0);
  EXPECT_EQ(v.pads[0].pad, 0);
  EXPECT_EQ(v.pads[0].type, Type::GameCube);
  EXPECT_EQ(v.pads[1].port, 1);
  EXPECT_EQ(v.pads[1].pad, 5);
  EXPECT_EQ(v.pads[1].type, Type::Nunchuk);
  EXPECT_EQ(v.pads[1].buttons, 0x200000u);
  // Online: this player's port alone.
  OnFrameEnd(p.data(), 0b010);
  v = Get();
  EXPECT_EQ(v.frames, 2u);
  ASSERT_EQ(v.count, 1u);
  EXPECT_EQ(v.pads[0].port, 1);
  // A port with no controller in either pad: no box.
  OnFrameEnd(p.data(), 0b1000);
  EXPECT_EQ(Get().count, 0u);
}

TEST(InputView, OutsideASessionTheControllerPressedLast)
{
  Reset();
  Pads p = Empty();
  Plug(p, 0, Type::GameCube);
  Plug(p, 4, Type::Classic);
  // Nothing pressed yet: the first with a controller.
  OnFrameEnd(p.data(), 0);
  ASSERT_EQ(Get().count, 1u);
  EXPECT_EQ(Get().pads[0].pad, 0);
  EXPECT_EQ(Get().pads[0].port, -1);
  // The Classic Controller's stick moves: the display goes to it, and stays when it is let go.
  Plug(p, 4, Type::Classic, 0, 90, 0);
  OnFrameEnd(p.data(), 0);
  EXPECT_EQ(Get().pads[0].pad, 4);
  EXPECT_EQ(Get().pads[0].type, Type::Classic);
  Plug(p, 4, Type::Classic);
  OnFrameEnd(p.data(), 0);
  EXPECT_EQ(Get().pads[0].pad, 4);
  // A stick resting off-centre is not a press.
  Plug(p, 0, Type::GameCube, 0, 12, -9);
  OnFrameEnd(p.data(), 0);
  EXPECT_EQ(Get().pads[0].pad, 4);
  // While it is held, another controller's press does not take it; after, it does.
  Plug(p, 4, Type::Classic, 0x100);
  Plug(p, 0, Type::GameCube, 0x200);
  OnFrameEnd(p.data(), 0);
  EXPECT_EQ(Get().pads[0].pad, 4);
  Plug(p, 4, Type::Classic);
  OnFrameEnd(p.data(), 0);
  EXPECT_EQ(Get().pads[0].pad, 0);
  // Its controller gone (unplugged, a Wii Remote reconnecting): the next one with a controller,
  // until it is back.
  p[0x38] = 0xFF;
  OnFrameEnd(p.data(), 0);
  EXPECT_EQ(Get().pads[0].pad, 4);
  Plug(p, 0, Type::GameCube);
  OnFrameEnd(p.data(), 0);
  EXPECT_EQ(Get().pads[0].pad, 0);
}

// Any other game (Other Games): what the hardware gave the game, at each VI field.
TEST(InputView, OtherGamesTheHardwaresPads)
{
  Reset();
  Pad gc;
  gc.buttons = 0x100;
  gc.stick_x = 127;
  SetHardwarePad(0, gc);
  Pad wii;
  wii.type = Type::Nunchuk;
  SetHardwarePad(4 + 2, wii);
  // Outside a session: the controller pressed last.
  OnHardwareField(0);
  View v = Get();
  EXPECT_EQ(v.frames, 1u);
  ASSERT_EQ(v.count, 1u);
  EXPECT_EQ(v.pads[0].pad, 0);
  EXPECT_EQ(v.pads[0].port, -1);
  EXPECT_EQ(v.pads[0].buttons, 0x100u);
  EXPECT_EQ(v.pads[0].stick_x, 127);
  // A whole-machine session: the pads it names, by their port.
  OnHardwareField(1u << (4 + 2));
  v = Get();
  ASSERT_EQ(v.count, 1u);
  EXPECT_EQ(v.pads[0].pad, 6);
  EXPECT_EQ(v.pads[0].port, 2);
  EXPECT_EQ(v.pads[0].type, Type::Nunchuk);
  OnHardwareField(0b1000001);
  v = Get();
  ASSERT_EQ(v.count, 2u);
  EXPECT_EQ(v.pads[0].port, 0);
  EXPECT_EQ(v.pads[1].port, 2);
  // Brawl's own pads stand over the hardware's while it reads them, and half a second after.
  Pads p = Empty();
  Plug(p, 1, Type::GameCube, 0x200);
  OnFrameEnd(p.data(), 0b10);
  for (int i = 0; i < 30; ++i)
    OnHardwareField(0);
  EXPECT_EQ(Get().pads[0].pad, 1);
  OnHardwareField(0);
  EXPECT_EQ(Get().pads[0].pad, 0);
  // A pad the game stops reading (no controller there): gone after two seconds.
  for (int i = 0; i < 120; ++i)
    OnHardwareField(1u << 0);
  EXPECT_EQ(Get().count, 0u);
  // And a new boot forgets the last game's.
  SetHardwarePad(0, gc);
  Reset();
  OnHardwareField(0);
  EXPECT_EQ(Get().count, 0u);
}
