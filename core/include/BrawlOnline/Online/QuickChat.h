// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Quick chat (docs/design/quick-chat.md): 16 messages the player sets in the launcher (Settings >
// Social), sent with a GameCube controller's D-pad on the online CSS. A first press opens that
// direction's group of four, a second press sends the message in its direction to the chat (as if
// typed: Online::Chat::Send) and closes the group, or just closes it if that slot is unset. The
// window is core/ui QuickChatUI.
//
// On the CSS the D-pad moves the hand like the stick (measured 2026-11-26); while quick chat can be
// used it is taken out of what the game reads, and B too while a group is open (B closes it):
// - the CSS of a room, or of a Direct / Unranked / Ranked set (LOCAL.screen Room or OnlineBusy on
//   scSelctCharacter), with the room's rules menu closed;
// - the chat window shown (in a chat group, [Online] DisableChat off, not a spectator's chat);
// - at least one message set.
// Only GameCube controllers (Dolphin's mapped ones and the Wii U adapter's, EmissaryRecomp's):
// the read that reaches the game outside a gameplay session (Gprb::ReadHostPad). A Wii Remote's
// D-pad is its stick, and is never taken.
//
// All functions are thread-safe.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Online::QuickChat
{
// Directions, the D-pad's, clockwise from up. A message is messages[4 * group + direction].
enum Direction : u8
{
  Up = 0,
  Right = 1,
  Down = 2,
  Left = 3,
};
constexpr int DIRECTIONS = 4;
constexpr size_t MESSAGES = 16;
using Messages = std::array<std::string, MESSAGES>;

// An open group closes after this long without a press.
constexpr u64 OPEN_MS = 3000;
// The chat window's word that quick chat can be used is good for this long (UI frames).
constexpr u64 SHOWN_STALE_MS = 500;

// quick-chat.json in the online folder (the launcher writes it, atomically): {"version": 1,
// "messages": [16 strings]}. Each message is cleaned as chat text (ChatText::Clean); one that
// cleans to nothing is no message. nullopt: not that.
std::optional<Messages> Parse(std::string_view json);
// Re-reads <online_dir>quick-chat.json when its time or size changed (Rooms' file poll). A missing
// file is no messages; a damaged one keeps what was read before.
void PollFile(const std::string& online_dir);
void SetMessages(const Messages& messages);
Messages GetMessages();

// CPU thread, each frame (GameBridge): the game shows a CSS quick chat works on.
void SetOnCss(bool on_css);
// The chat window, each frame it draws (UI thread): it is shown, for a room's or a match's group.
void SetChatShown(bool shown, u64 now_ms);

// A GameCube controller's read on its way to the game (GameCube PAD_BUTTON_* bits): notes the D-pad
// presses and takes the D-pad (and B while a group is open) out when quick chat can be used. Any
// thread (Dolphin's CPU thread, EmissaryRecomp's pad read).
void FilterPad(int chan, u16* buttons, u64 now_ms);

// What the window draws.
struct View
{
  enum class State : u8
  {
    Closed,
    Open,  // a group is open: `group`
  };
  State state = State::Closed;
  int group = 0;
  u64 since_ms = 0;  // when it opened
  u64 last_press_ms = 0;
  Messages messages;
};
// UI thread, each frame: takes the presses since the last call, sends a chosen message with
// Online::Chat::Send (its result in *result, else untouched) and returns what to draw.
enum class Result : u8
{
  None,
  Sent,
  NoGroup,
  NoReaders,
  TooFast,
};
View Tick(u64 now_ms, Result* result);

// ---- Tests (UnitTests/Core/Online/QuickChatTest.cpp, harness `quick_chat`) ----
// Forgets everything (messages, presses, state). `send` stands in for Online::Chat::Send.
void ResetForTesting(Result (*send)(const std::string& text) = nullptr);
// A D-pad press (Direction) or B (-1), as FilterPad would note it.
void PressForTesting(int direction);
// Whether FilterPad takes the D-pad now.
bool Usable(u64 now_ms);
}  // namespace Online::QuickChat
