// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "BrawlOnline/UI/Style.h"

#include <imgui.h>
#ifdef IMGUI_ENABLE_FREETYPE
#include <imgui_internal.h>  // ImFontAtlasGetFontLoaderForStbTruetype
#endif

#include "Common/FileUtil.h"

namespace BrawlOnline::UI
{
namespace
{
ImFontConfig Config()
{
  ImFontConfig config;
#ifdef IMGUI_ENABLE_FREETYPE
  config.FontLoader = ImFontAtlasGetFontLoaderForStbTruetype();
#endif
  return config;
}
}  // namespace

Fonts AddFonts(ImFontAtlas* atlas, const std::string& user_osd_font, const std::string& resources,
               bool replace)
{
  Fonts fonts;

  // User supplied font
  std::string file = user_osd_font;

  bool font_exists = !file.empty() && File::Exists(file);
  if (!font_exists)
  {
    // Default supplied font
    file = resources + "OSD_Font.ttf";
    font_exists = File::Exists(file);
  }

  if (font_exists)
  {
    if (replace)
      atlas->Clear();
    const ImFontConfig config = Config();
    fonts.osd = atlas->AddFontFromFileTTF(file.c_str(), 0.0f, &config);
  }
  else
  {
    // The first font is the default: keep ImGui's own ahead of the chat font.
    fonts.osd = atlas->AddFontDefault();
  }

  // The online chat's font: Noto Sans with Noto Sans JP merged in for kana and kanji. Glyphs are
  // rasterized when first drawn, and only these fonts' glyphs exist (others draw as the fallback).
  if (File::Exists(resources + "NotoSans-Regular.ttf"))
  {
    const ImFontConfig config = Config();
    fonts.chat =
        atlas->AddFontFromFileTTF((resources + "NotoSans-Regular.ttf").c_str(), 0.0f, &config);
    if (fonts.chat && File::Exists(resources + "NotoSansJP-Regular.ttf"))
    {
      ImFontConfig merge = Config();
      merge.MergeMode = true;
      atlas->AddFontFromFileTTF((resources + "NotoSansJP-Regular.ttf").c_str(), 0.0f, &merge);
    }
  }
  return fonts;
}

void SetStyle(ImGuiStyle& style, float scale)
{
  // ScaleAllSizes scales in-place, so calling it twice will double-apply the scale
  // Reset the style first so that the scale is applied to the base style, not an already-scaled one
  style = {};
  style.FontScaleMain = scale;
  style.WindowRounding = 7.0f;
  style.ScaleAllSizes(scale);
}
}  // namespace BrawlOnline::UI
