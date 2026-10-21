#pragma once
#include <types.h>

namespace Labels {
    // Draw `text` (one or two lines, split at '\n') with the game's bold font into the TEX0's
    // own image, fitted into the box (texture pixels). RGB5A3: black letters, a white rim of
    // `rim` pixels; IA4: white letters, a black rim; I4: white letters. False if the texture or
    // the font is not there.
    bool render(u8* tex0, const char* text, int x0, int y0, int x1, int y1, int rim);
    // CMPR only: the label inside the box (4-texel aligned), the texture's own art around it
    // kept; white letters with a black outline (Brawl's button labels, e.g. the CSS's ITEM).
    bool renderInBox(u8* tex0, const char* text, int x0, int y0, int x1, int y1, int rim);
    // The TEX0 a model's materials use under `name` (bound by the game at load), or NULL.
    u8* boundTexture(u8* mdl0, const char* name);
    // Bind `name` in a model to another TEX0 (the texture links' texture infos): for a model
    // copied out of its archive, whose materials then get the image too (see netmenu.cpp).
    int rebindTexture(u8* mdl0, const char* name, u8* tex0);
    // An I4 icon made from an I4 half-icon and its mirror image (Brawl draws some icons as one
    // half twice), fitted and centred in `dst`'s own size.
    bool mirroredIcon(u8* dst, const u8* srcHalf);
    // A TEX0's image word (its physical address >> 5), as the GXTexObjs and display lists hold it.
    u32 imageWord(const u8* tex0);
    // A copy of an I4 or IA4 TEX0 (header and image, 32-byte aligned, a copyBlock), or NULL.
    u8* copyTexture(const u8* tex0);
    // A 32-byte aligned block of `size` bytes in the Network heap for a copy the menus keep, or
    // NULL. Kept until releaseCopies, which frees every one (OnlineMenu::tick, before any scene
    // that loads fighters: Brawl decompresses costumes in the Network heap, bug #75); the
    // modules forget theirs first (forgetCopies) and make them again on the next menu.
    u8* copyBlock(u32 size);
    bool holdsCopies();
    void releaseCopies();
    // Point a MuObject's model `name` at `want` (the MDL0's texture links and lists), and its
    // ScnMdl's own copies of the texture objects (ScnMdl and its material buffers +0x14C / +0x140)
    // from any of `from` to it. Call every frame: the ScnMdl fills its copies after it is made.
    void bindTexture(u8* obj, const char* name, u8* want, const u32* from, int nFrom);
    // The MDL0 part of bindTexture alone (a model whose MuObject is gone). False: not bound.
    bool bindModel(u8* mdl0, const char* name, u8* want);
}
