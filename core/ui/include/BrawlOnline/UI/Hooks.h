// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Brawl Online's in-game UI (core/ui: the on-screen messages, the chat window, the replay
// overlay), drawn with ImGui by both engines (docs/design/brawl-online-core.md §11): Dolphin in
// its OSD (VideoCommon/OnScreenUI), EmissaryRecomp in its own ImGui frame. What only an engine
// knows comes through these hooks, set once by the engine before its first frame; an unset hook
// does what the comment says.

#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "Common/CommonTypes.h"

namespace BrawlOnline::UI
{
// A texture the engine made of a message's icon (Dolphin: an achievement's badge).
class IconTexture
{
public:
  virtual ~IconTexture() = default;
  // ImGui's ImTextureID for it.
  virtual u64 Id() const = 0;
  virtual float Width() const = 0;
  virtual float Height() const = 0;
};

struct Rect
{
  int left = 0, top = 0, right = 0, bottom = 0;
};

struct Hooks
{
  // Where the game's picture is in the window, in the window's pixels (Dolphin: the presenter's
  // target rectangle). Unset or empty: the whole window.
  std::function<Rect()> game_rect;
  // Whether on-screen messages are drawn (Dolphin: [Interface] OnScreenDisplayMessages). Unset:
  // they are.
  std::function<bool()> show_messages;
  // The chat box has the keyboard: the game must get no keyboard or mouse input meanwhile
  // (Dolphin: ciface::Core::SetKeyboardMouseBlocked). Called every frame drawn, with the state.
  std::function<void(bool)> block_keyboard_mouse;
  // A texture for a message's icon (the engine's own type, opaque here), on the thread that draws.
  // Unset or null: the message is drawn without it.
  std::function<std::unique_ptr<IconTexture>(const void* icon)> make_icon;
  // Every message added (any thread), as OSD::AddMessage got it: the recomp also logs them.
  std::function<void(std::string_view text, u32 ms, u32 argb)> message_added;
};

// Any thread: the hooks are replaced whole (an engine sets them once, before its first frame).
void SetHooks(Hooks hooks);
const Hooks& GetHooks();
}  // namespace BrawlOnline::UI
