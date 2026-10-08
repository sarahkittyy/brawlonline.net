// "DISCONNECTED" in the match HUD (Slippi StartEngineLoop.asm:33-39: red FF0000FF, centred near
// the top, until the scene ends). docs/game-code.md "Online matches", the in-match disconnect.
//
// Drawn with the game's own text renderer, ms::CharWriter (the base of Brawl's Message / MuMsg
// text), and the game's resident system font (font kind 4, the one P+'s Code Menu prints with).
// MuMsg itself is not used: a MuMsg window only draws attached to a model node
// (MuMsg::attachScnMdlSimple / attachMuObject), and the match HUD has no free node at the top
// centre (the only in-match MuMsg, IfCenter+0x164, is the pause screen's song title).
//
// Where: gfApplication's frame loop at 0x8001792C, after the frame is drawn and before the copy
// to the XFB (P+'s Code Menu draws at 0x80017928, the instruction before, the same way). We set
// our own 2D projection (640x480 ortho, the HUD's space), an identity position matrix, no depth
// test, and print. Nothing is allocated: the CharWriter is a static in the plugin, so nothing
// needs freeing when scMelee is torn down. The text is drawn only while the current scene is
// scMelee and switches itself off as soon as the scene changes.
#include <memory.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"

namespace MatchHud {

    // ms::CharWriter (BrawlHeaders ms/ms_char_writer.h), 0x70 bytes.
    static u8 s_cw[0x80] __attribute__((aligned(8)));
    static bool s_constructed = false;
    static bool s_on = false;
    static const char* s_text = "DISCONNECTED";

    typedef void (*CwCtorFn)(void* cw);
    typedef void (*CwSetFontFn)(void* cw, int kind);
    typedef void (*CwSetupGXFn)(void* cw);
    typedef void (*CwPrintFn)(void* cw, u16 ch);
    typedef void (*GetGlyphFn)(void* font, void* out, u16 ch);
    static const CwCtorFn s_cwCtor = (CwCtorFn)0x8006EBCC;            // ms::CharWriter::CharWriter
    static const CwSetFontFn s_cwSetFont = (CwSetFontFn)0x8006EE5C;   // font by kind (+0x48, +0x68)
    static const CwSetupGXFn s_cwSetupGX = (CwSetupGXFn)0x8006EEEC;   // ms::CharWriter::SetupGX
    static const CwPrintFn s_cwPrint = (CwPrintFn)0x8006FE50;         // ms::CharWriter::Print

    // GX / MTX (DOL; their use in the camera setup 0x80018DE4 shows the arguments).
    typedef void (*ViewportFn)(float, float, float, float, float, float);
    typedef void (*ScissorFn)(u32, u32, u32, u32);
    typedef void (*OrthoFn)(float (*m)[4], float t, float b, float l, float r, float n, float f);
    typedef void (*SetProjectionFn)(float (*m)[4], int type);
    typedef void (*LoadPosMtxFn)(float (*m)[4], u32 id);
    typedef void (*SetCurrentMtxFn)(u32 id);
    typedef void (*SetZModeFn)(u8 enable, int func, u8 update);
    typedef void (*SetCullModeFn)(int mode);
    static const ViewportFn s_gxViewport = (ViewportFn)0x801F5484;
    static const ScissorFn s_gxScissor = (ScissorFn)0x801F5500;
    static const OrthoFn s_mtxOrtho = (OrthoFn)0x801ECF4C;          // C_MTXOrtho(m, t, b, l, r, n, f)
    static const SetProjectionFn s_gxProjection = (SetProjectionFn)0x801F50EC;   // 1 = orthographic
    static const LoadPosMtxFn s_gxLoadPosMtx = (LoadPosMtxFn)0x801F51DC;        // GXLoadPosMtxImm
    static const SetCurrentMtxFn s_gxCurrentMtx = (SetCurrentMtxFn)0x801F52E4;
    static const SetZModeFn s_gxZMode = (SetZModeFn)0x801F4774;
    static const SetCullModeFn s_gxCullMode = (SetCullModeFn)0x801F136C;

    static const int FONT_SYSTEM = 4;   // resident system font (0x80497E44)
    static const float SCREEN_W = 640.0f, SCREEN_H = 480.0f;
    // Slippi's text: FF0000FF, scale 0.7 of Melee's HUD canvas, centred near the top. Here: the
    // system font at 0.9 (about 250 x 37 of the 640x480 HUD space), just below the match timer.
    static const float SCALE = 0.9f;
    static const float TOP = 70.0f;

    static void setU32(u32 off, u32 v) { *(u32*)(s_cw + off) = v; }
    static void setF32(u32 off, float v) { *(float*)(s_cw + off) = v; }
    static float getF32(u32 off) { return *(float*)(s_cw + off); }

    void show(bool on) { s_on = on; }
    bool shown() { return s_on; }

    // Width of `text` as ms::CharWriter::Print advances it (proportional mode):
    // advance = m_58 * scaleX * (glyph.charWidth * m_68); the glyph is {texture*, s8 left,
    // u8 glyphWidth, s8 charWidth, ...} (Print reads +4 and +6 of it).
    static float textWidth(const char* text)
    {
        void* font = *(void**)(s_cw + 0x48);
        if (!font) return 0;
        GetGlyphFn getGlyph = (GetGlyphFn)(*(u32*)(*(u32*)font + 0x50));
        float w = 0;
        for (const char* p = text; *p; p++) {
            u8 glyph[0x40];
            memset(glyph, 0, sizeof(glyph));
            getGlyph(font, glyph, (u16)(u8)*p);
            w += getF32(0x58) * getF32(0x24) * ((float)(s8)glyph[6] * getF32(0x68));
        }
        return w;
    }

    static void draw()
    {
        if (!s_on) return;
        if (!Online::inScene("scMelee")) {
            s_on = false;   // the text lives as long as the match scene
            return;
        }
        if (!s_constructed) {
            s_cwCtor(s_cw);
            s_constructed = true;
        }
        s_cwSetFont(s_cw, FONT_SYSTEM);

        // 2D: the HUD's 640x480 space, y down, no depth test, no culling.
        float proj[4][4];
        float pos[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        s_gxViewport(0, 0, SCREEN_W, SCREEN_H, 0, 1);
        s_gxScissor(0, 0, (u32)SCREEN_W, (u32)SCREEN_H);
        s_mtxOrtho(proj, 0, SCREEN_H, 0, SCREEN_W, -1.0f, 1.0f);
        s_gxProjection(proj, 1);
        s_gxLoadPosMtx(pos, 0);
        s_gxCurrentMtx(0);

        // Red (Slippi FF0000FF), opaque, with a thin black edge so it reads on any stage.
        const u32 RED = 0xFF0000FF;
        setU32(0x08, RED); setU32(0x0C, RED); setU32(0x10, RED); setU32(0x14, RED);
        setU32(0x18, RED);
        s_cw[0x42] = 0xFF;          // alpha
        s_cw[0x43] = 0;             // proportional
        setF32(0x24, SCALE);        // scale x
        setF32(0x28, SCALE);        // scale y
        setF32(0x50, 1.0f);
        setF32(0x5C, 1.0f);         // edge width
        setU32(0x60, 0x000000FF);   // edge colour

        float w = textWidth(s_text);
        setF32(0x2C, (SCREEN_W - w) * 0.5f);
        setF32(0x30, TOP);
        setF32(0x34, 0);
        s_cwSetupGX(s_cw);
        s_gxZMode(0, 7 /* GX_ALWAYS */, 0);
        s_gxCullMode(0 /* GX_CULL_NONE */);
        for (const char* p = s_text; *p; p++) s_cwPrint(s_cw, (u16)(u8)*p);
        // Tell Dolphin the game shows it (LOCAL, not part of the rolled-back state): its red OSD
        // message is only the fallback for a game that cannot.
        PPOM::g_block.local.hudDisconnected = 1;
    }

    // Inline hook at 0x8001792C (gfApplication frame loop, `addi r3, r30, 0x118`).
    static void onFrameDrawn()
    {
        OnlineMatch::onFrameDrawn();
        draw();
    }

    void install(CoreApi* api)
    {
        api->syInlineHook(0x8001792C, reinterpret_cast<void*>(onFrameDrawn));
    }
}
