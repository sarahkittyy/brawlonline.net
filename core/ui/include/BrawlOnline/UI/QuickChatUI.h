// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Quick chat's menu (docs/design/quick-chat.md, Online/QuickChat.h), drawn over the CSS with the
// chat's ImGui and font: the open group's four messages around a D-pad, each on the side of the
// direction that sends it, the group's own arm lit in the player's colour. Sending a message closes
// it. OnlineChatUI::Display draws it while the chat window is shown, under the ImGui lock.
//
// The messages are the player's own (the launcher's Settings > Social), drawn with AddText only.

#pragma once

#include <array>
#include <string>

#include "BrawlOnline/Online/QuickChat.h"

typedef unsigned int ImU32;

namespace BrawlOnline::UI::QuickChatUI
{
struct Drawn
{
  bool shown = false;
  std::array<float, 4> rect{};  // the panel, screen pixels (x0, y0, x1, y1)
};

// Draws `view` (QuickChat::Tick's) centred on the game's picture, its text `font_px` high.
// `accent`: the player's chat colour. `notice`: a line under the messages ("" none), e.g. why the
// last message wasn't sent.
Drawn Draw(const Online::QuickChat::View& view, float scale, float font_px, ImU32 accent,
           const std::string& notice, u64 now_ms);
}  // namespace BrawlOnline::UI::QuickChatUI
