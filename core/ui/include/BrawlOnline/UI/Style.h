// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The fonts and style Brawl Online's in-game UI is drawn with: Dolphin's OSD's (OnScreenUI), used
// by both engines (core/ui).

#pragma once

#include <string>

struct ImFont;
struct ImFontAtlas;
struct ImGuiStyle;

namespace BrawlOnline::UI
{
// The OSD's font size before the scale, Dolphin's [Settings] OSDFontSize default.
constexpr int DEFAULT_FONT_SIZE = 13;

struct Fonts
{
  ImFont* osd = nullptr;   // the messages and every window's text but the chat's
  ImFont* chat = nullptr;  // the chat and the replay overlay (names in any script); null: none
};

// Adds the fonts to `atlas`: the OSD font (`user_osd_font`, a player's own (Dolphin:
// User/Load/OSD_Font.ttf), else OSD_Font.ttf in `resources`, else ImGui's own) and the chat font
// (Noto Sans with Noto Sans JP merged in for kana and kanji, from `resources`). Glyphs are
// rasterized when first drawn, and only these fonts' glyphs exist (others draw as the fallback).
// `resources` ends with a separator. With `replace` the OSD font is the atlas's first, its
// default (Dolphin: it clears the atlas first); without, they are added after the engine's own.
// Always stb_truetype, as Dolphin's, also where the atlas uses FreeType (EmissaryRecomp).
Fonts AddFonts(ImFontAtlas* atlas, const std::string& user_osd_font, const std::string& resources,
               bool replace);

// The OSD's style at `scale` (the screen's DPI scale; Dolphin's backbuffer scale): ImGui's
// default (dark) style, rounded windows, everything scaled.
void SetStyle(ImGuiStyle& style, float scale);
}  // namespace BrawlOnline::UI
