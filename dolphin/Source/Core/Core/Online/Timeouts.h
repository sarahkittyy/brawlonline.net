// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Slippi's online timeouts, in one place so every session backend uses the same values.
// docs/backend-design.md 5.6 has the sources and the full disconnect flow.

#pragma once

#include "Common/CommonTypes.h"

namespace Online
{
// SlippiNetplayClient::ThreadFunc: "u64 timeout = 8000;" (the P2P connect window).
constexpr u32 P2P_CONNECT_TIMEOUT_MS = 8000;

// Slippi's ROLLBACK_MAX_FRAMES (SlippiNetplay.h).
constexpr int SLIPPI_ROLLBACK_MAX_FRAMES = 7;
// CEXISlippi::shouldSkipOnlineFrame: the opponent is force-disconnected once
// `stall_frame_counts[i] > 60 * 7`, i.e. on the 421st consecutive halted frame.
constexpr int SLIPPI_STALL_DISCONNECT_FRAMES = 60 * 7 + 1;
// Slippi's frame time in microseconds (EXI_DeviceSlippi.cpp `frame_time = 16683`).
constexpr u32 SLIPPI_FRAME_US = 16683;

// How long a peer may stay silent before the session drops it, matching Slippi in-match:
// - the game halts once it is ROLLBACK_MAX_FRAMES frames past the newest remote input, and the
//   newest input already carries the remote's delay, so it halts `input_delay + 7 + 1` frames
//   after the last packet;
// - it then force-disconnects on the 421st halted frame.
// That is (input_delay + 429) frames: 7.19 s at the default delay of 2.
// (Slippi's ENet peer timeout, 5-10 s depending on RTT, can fire first or later; the stall counter
// is the deterministic rule, and the one the UI shows.)
constexpr u32 PeerSilenceTimeoutMs(int input_delay)
{
  const int frames = (input_delay < 0 ? 0 : input_delay) + SLIPPI_ROLLBACK_MAX_FRAMES + 1 +
                     SLIPPI_STALL_DISCONNECT_FRAMES;
  return static_cast<u32>((static_cast<u64>(frames) * SLIPPI_FRAME_US + 999) / 1000);
}
static_assert(PeerSilenceTimeoutMs(2) == 7191);
}  // namespace Online
