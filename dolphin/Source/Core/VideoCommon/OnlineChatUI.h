// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The online chat window (docs/design/chat.md, docs/chat-protocol.md §6), drawn with the OSD's
// ImGui over the game: the room's or the match's chat, names to click (hide, report), the box to
// type in. Online::Chat holds the state; this is only its view.
//
// Peer text is only ever drawn with TextUnformatted / AddText, never passed as a format string or
// as a widget label (ImGui reads "##" in labels as an id).

#pragma once

struct ImFont;

namespace VideoCommon::OnlineChatUI
{
// The chat font (Noto Sans + Noto Sans JP), loaded by OnScreenUI. Under the ImGui lock.
void SetFont(ImFont* font);
// Draws the window if the player is in a chat group. Under the ImGui lock, once per frame.
void Display(float scale);
// The "Activate NetPlay Chat" hotkey: open the box (any thread).
void Activate();
// The box has the keyboard (any thread): the game gets no keyboard or mouse input meanwhile.
bool IsTyping();
}  // namespace VideoCommon::OnlineChatUI
