// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/EnumMap.h"

// The main point of this is to allow other parts of dolphin to set ImGui's key map without
// having to import ImGui headers.
// But the idea is that it can be expanded in the future with more keys to support more things.
enum class DolphinKey
{
  Tab,
  LeftArrow,
  RightArrow,
  UpArrow,
  DownArrow,
  PageUp,
  PageDown,
  Home,
  End,
  Insert,
  Delete,
  Backspace,
  Space,
  Enter,
  Escape,
  KeyPadEnter,
  A,  // for text edit CTRL+A: select all
  C,  // for text edit CTRL+C: copy
  V,  // for text edit CTRL+V: paste
  X,  // for text edit CTRL+X: cut
  Y,  // for text edit CTRL+Y: redo
  Z,  // for text edit CTRL+Z: undo
  W,  // for the online chat's CTRL+W: delete the word before the cursor
};

using DolphinKeyMap = Common::EnumMap<int, DolphinKey::W>;

// The modifier keys held, as the physical keys (on macOS, Ctrl is the Control key and Super is
// Command; ImGui swaps them itself for its shortcuts).
enum DolphinKeyModifier : unsigned
{
  DolphinKeyModifier_Ctrl = 1u << 0,
  DolphinKeyModifier_Shift = 1u << 1,
  DolphinKeyModifier_Alt = 1u << 2,
  DolphinKeyModifier_Super = 1u << 3,
};
