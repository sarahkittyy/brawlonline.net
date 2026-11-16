// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Input trace (PPR_GPRB_INPUT_TRACE=<path>, "%p" in it the process ID): what a network gameplay
// session did with the local controller and when each picture reached the screen, for the
// harness's input fidelity check (harness/tools/input_fidelity.py, ppharness/inputoracle.py).
// One line per event, times in microseconds of the host's steady clock (the clock the harness's
// pad_tape rows use), frames as game frames (the session's start frame + GekkoNet's frame):
//
//   H pid now_us
//   L game_frame instant_us data_us kind how src bytes0-3 bytes30-35
//       a local input went to GekkoNet for game_frame: the controller read it was made of stands
//       for instant_us, and the state it carries was the controller's at data_us (they differ for
//       a read far behind schedule or a late read with no past); kind is that read's
//       (now/hist/live/far/latch/relatch/deferred...), how the way it went (looptop, deferred,
//       deferred-declined, deferred-now, flush), src the pad it is (decode: the host decode of
//       the read, pass: the pad thread's pass)
//   U now_us first_frame count load_back confirmed
//       a GekkoNet update that runs `count` frames from first_frame, after loading back
//       load_back frames; confirmed is GekkoNet's confirmed frame then
//   W begin_us end_us reason
//       the session waited on the host (peer: for the peer's inputs; sync: time sync)
//   P pass_frame picture_frame picture_confirmed after_rollback copy_us done_us
//       a picture reached the screen: copied in the presented pass of pass_frame, drawn by the
//       pass before it (picture_frame; after a rollback the last re-run), when GekkoNet's
//       confirmed frame was picture_confirmed; done_us is when the present returned
//
// Any thread; lines are buffered and written under a mutex.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

namespace Rollback::InputTrace
{
bool Enabled();
s64 Us(Clock::time_point t);
void Write(const std::string& line);
// Writes out what is buffered (the session's end).
void Flush();
}  // namespace Rollback::InputTrace
