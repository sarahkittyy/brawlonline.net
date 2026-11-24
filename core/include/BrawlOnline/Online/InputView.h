// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The input display's view of the game (core/ui InputDisplayUI draws it; Sarah's board, card 775):
// the controllers to show and what they hold this frame, as Brawl itself has them. Display only:
// nothing here writes to the game or changes what any machine simulates.
//
// The source is gfPadSystem's eight system pads (0x805BAD00, gfPadStatus, 0x40 bytes each: GameCube
// ports 0-3, then Wii Remotes 0-3), the same in Dolphin and EmissaryRecomp, offline and online: in
// a gameplay session (a match online, a replay, a spectated game) they hold the session's input of
// every port, the frame's own, in the port's GameCube or Wii Remote pad by its controller type
// (GameplayRollback WriteSlots); elsewhere the pad thread's newest sample of this machine's
// controllers. Read at the end of each frame that is shown (never a rollback's re-run): what the
// game played that frame, input delay included.
//
// What a record holds, by its controller type (+0x3C, gfPadType; measured with Dolphin's emulated
// controllers, 2026-11-24, harness/tests/test_input_display.py):
//   GameCube (0): A 0x100, B 0x200, X 0x400, Y 0x800, START 0x1000, Z 0x10, L 0x40, R 0x20, the
//     D-pad left 0x1, right 0x2, down 0x4, up 0x8; the stick +0x30/+0x31 and C-stick +0x32/+0x33
//     (s8, -128..127), the analog triggers +0x34/+0x35 (u8, 0-255).
//   Classic Controller (1): a 0x100, b 0x200, x 0x400, y 0x800, L 0x40, R 0x20, ZL 0x4000 (with
//     0x10), ZR 0x8000 (with 0x10), PLUS 0x100000 (with 0x1000), the D-pad as a GameCube's; the
//     left stick +0x30, the right stick +0x32 (-100..100), the analog triggers +0x34/+0x35.
//   Wii Remote alone, held sideways (2): A 0x200000, B 0x400000 (with 0x40), 1 0x10000 (with
//     0x200), 2 0x20000 (with 0x100), PLUS 0x1000; the D-pad turned as the player holds it (their
//     left is 0x1, right 0x2, down 0x4, up 0x8).
//   Wii Remote with a Nunchuk (3): the Wii Remote's A 0x200000 (with 0x100), B 0x400000 (with
//     0x200), 1 0x10000, 2 0x20000, PLUS 0x1000, its D-pad upright (as a GameCube's); the Nunchuk's
//     C 0x40000 (with 0x400) and Z 0x10, its stick +0x30 (-100..100).
//   MINUS (0x80000) and HOME never reach the record in a match.

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"

namespace Online::InputView
{
constexpr u32 PAD_SIZE = 0x40;
constexpr u32 PADS = 8;  // GameCube 0-3, Wii Remotes 4-7
constexpr u32 MAX_SHOWN = 4;

enum class Type : u8
{
  GameCube = 0,
  Classic = 1,
  WiiRemote = 2,
  Nunchuk = 3,
};

// One controller as the display draws it.
struct Pad
{
  int port = -1;  // the in-game port it plays (0-3), -1: this machine's controller outside a match
  int pad = 0;    // gfPadSystem's pad (0-7)
  Type type = Type::GameCube;
  u32 buttons = 0;  // the record's buttons (+0x00)
  s8 stick_x = 0, stick_y = 0;
  s8 sub_x = 0, sub_y = 0;  // GameCube C-stick, Classic Controller right stick
  u8 trigger_l = 0, trigger_r = 0;
};

struct View
{
  u32 frames = 0;  // frames seen (0: none yet: draw nothing)
  u32 count = 0;
  std::array<Pad, MAX_SHOWN> pads{};
};

// Decodes one record (guest byte order). False when the pad has no controller (its error +0x38 is
// not NONE) or an unknown type.
bool Decode(const u8* record, Pad* out);

// CPU thread, at the end of a frame that is shown. `pads`: the eight system pads (PADS * PAD_SIZE
// bytes, guest order). `ports`: the in-game ports to show (a bit each; their GameCube or Wii Remote
// pad, whichever has a controller); 0: this machine's controller, the pad pressed last.
void OnFrameEnd(const u8* pads, u32 ports);
// Reads the eight pads from guest memory (when gfPadSystem exists) and calls OnFrameEnd.
void SampleGuest(u32 ports);
// Any thread: the last frame's view.
View Get();
// A new game (the core's reset at boot): nothing to show until its first frame.
void Reset();

// input-display.json in the launcher's online folder (Settings > Game > Input Display, changed
// while the game runs): {"version": 1, "enabled": bool, "x", "y", "width": per mille of the game's
// picture, "everyone": bool (optional)}. Taken into the [Online] InputDisplay* settings
// (Dolphin.ini, which the launcher writes too) and deleted. Rooms' file poll calls it.
void PollLauncherFile(const std::string& online_dir);
}  // namespace Online::InputView
