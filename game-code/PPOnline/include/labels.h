#pragma once
#include <types.h>

namespace Labels {
    // Draw `text` (one or two lines, split at '\n') with the game's bold font into the TEX0's
    // own image, fitted into the box (texture pixels). RGB5A3: black letters, a white rim of
    // `rim` pixels; I4: white letters. False if the texture or the font is not there.
    bool render(u8* tex0, const char* text, int x0, int y0, int x1, int y1, int rim);
    // The TEX0 a model's materials use under `name` (bound by the game at load), or NULL.
    u8* boundTexture(u8* mdl0, const char* name);
    // Bind `name` in a model to another TEX0 (the texture links' texture infos): for a model
    // copied out of its archive, whose materials then get the image too (see netmenu.cpp).
    int rebindTexture(u8* mdl0, const char* name, u8* tex0);
    // An I4 icon made from an I4 half-icon and its mirror image (Brawl draws some icons as one
    // half twice), fitted and centred in `dst`'s own size.
    bool mirroredIcon(u8* dst, const u8* srcHalf);
}
