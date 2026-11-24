// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The input display (Sarah's board, card 775: "the kind streamers and players use to show their
// inputs"): the controller a player plays on, drawn over the game with what it holds this frame:
// a GameCube controller, a Wii Remote held sideways, a Wii Remote with a Nunchuk or a Classic
// Controller, each with its buttons, sticks (where they point), triggers (how far, on GameCube and
// Classic) and D-pad. Online and offline this machine's own controller; in a replay or a spectated
// game one box per player, side by side. What it shows is Online::InputView's (the game's own pad
// records, read at each shown frame): display only, it never touches inputs or the game.
//
// Off by default; [Online] InputDisplay, InputDisplayX/Y/Width (the launcher's Settings > Game >
// Input Display, whose preview draws the same boxes: launcher/src/renderer/pages/settings/
// game_settings/input_display_preview.tsx follows the layouts in InputDisplayUI.cpp). Drawn under
// the other windows (chat, replay overlay), in the game's picture (Hooks::game_rect). Shared by
// Dolphin and EmissaryRecomp (core/ui).

#pragma once

#include <string>

namespace BrawlOnline::UI::InputDisplay
{
// Under the ImGui lock, once per frame. `scale`: the OSD's (outlines and text are this much
// thicker); the box's size is the setting's, in the game's picture.
void Display(float scale);

// For the harness: what the last frame drew, as a JSON object: {"enabled", "frames", "boxes":
// [{"x", "y", "w", "h" (window pixels), "port", "pad", "type" ("gc", "classic", "wiimote",
// "nunchuk"), "buttons", "stick": [x, y], "sub": [x, y], "triggers": [l, r], "pressed": [names]}]}.
std::string Status();
}  // namespace BrawlOnline::UI::InputDisplay
