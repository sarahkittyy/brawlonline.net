// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The online chat window (docs/design/chat.md, docs/chat-protocol.md §6), drawn with the OSD's
// ImGui over the game: the room's or the match's chat, names to click (ignore, report), the box to
// type in. Online::Chat holds the state; this is only its view.
//
// Peer text is only ever drawn with TextUnformatted / AddText, never passed as a format string or
// as a widget label (ImGui reads "##" in labels as an id).

#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

struct ImFont;

namespace VideoCommon::OnlineChatUI
{
// The system clipboard, for the box's Cut, Copy and Paste (menu and Ctrl+X/C/V). Without one,
// ImGui's own: Win32's on Windows, else a clipboard inside Dolphin only. DolphinQt gives Qt's.
// Both functions are called on the thread that draws ImGui, under the ImGui lock, and must not
// block on the GUI thread.
struct Clipboard
{
  std::function<std::string()> get;
  std::function<void(const std::string&)> set;
};
// Any thread, before or after the renderer starts.
void SetClipboard(Clipboard clipboard);
// Points a new ImGui context at it (OnScreenUI, under the ImGui lock).
void InstallClipboard();
// The chat font (Noto Sans + Noto Sans JP), loaded by OnScreenUI. Under the ImGui lock.
void SetFont(ImFont* font);
// Draws the window if the player is in a chat group. Under the ImGui lock, once per frame.
void Display(float scale);
// The "Activate NetPlay Chat" hotkey: open the box (any thread).
void Activate();
// The box has the keyboard (any thread): the game gets no keyboard or mouse input meanwhile.
bool IsTyping();

// ---- What each frame drew, while recording (harness `chat_ui`, UnitTests/VideoCommon/
// OnlineChatUITest.cpp) ----
struct TestFrame
{
  struct Name
  {
    std::string uid;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // screen pixels
  };
  int frame = 0;  // ImGui's frame count
  float display_w = 0, display_h = 0;
  float mouse_x = 0, mouse_y = 0;
  int mouse_down = 0;  // bit 0 left, 1 right, 2 middle
  bool shown = false;  // the window was drawn
  std::vector<Name> names;  // the clickable names
  bool member_menu_open = false;
  std::string menu_uid;
  std::array<float, 4> ignore_item{};  // the menu's Ignore / Unignore item (x0, y0, x1, y1)
  float x = 0, y = 0, w = 0, h = 0;
  bool collapsed = false;
  bool focused = false;
  float alpha = 1;
  bool typing = false;
};
// Any thread. At most the last 600 frames are kept.
void SetRecording(bool on);
std::vector<TestFrame> TakeRecordedFrames();
// Forgets the window's state and records each frame from now on (ImGui lock).
void ResetForTesting();
TestFrame LastFrameForTesting();
}  // namespace VideoCommon::OnlineChatUI
