// "DISCONNECTED" in the match HUD (Slippi StartEngineLoop.asm:33-39: red FF0000FF, centred near
// the top, until the scene ends). docs/game-code.md "Online matches", the in-match disconnect.
// "DESYNC DETECTED" the same way when the session ended the match on a desync (Slippi
// StartEngineLoop.asm:42-48).
// And, in every online match, each player's account name under their damage, as Slippi shows the
// display name under the percent (Brawl's HUD has no name there; its name tags float over the
// fighters). The names come from SESSION, the same on both machines; drawing them changes nothing
// in the game's state.
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

extern "C" {
    extern u8 g_onlineMatchGame;
}

namespace MatchHud {

    // ms::CharWriter (BrawlHeaders ms/ms_char_writer.h), 0x70 bytes.
    static u8 s_cw[0x80] __attribute__((aligned(8)));
    static bool s_constructed = false;
    static bool s_on = false;
    static bool s_desync = false;

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

    void show(bool on, bool desync) { s_on = on; s_desync = desync; }
    bool shown() { return s_on; }

    // Width of `text` as ms::CharWriter::Print advances it (proportional mode):
    // advance = m_58 * scaleX * (glyph.charWidth * m_68); the glyph is {texture*, s8 left,
    // u8 glyphWidth, s8 charWidth, ...} (Print reads +4 and +6 of it).
    static float textWidth(const u16* text)
    {
        void* font = *(void**)(s_cw + 0x48);
        if (!font) return 0;
        GetGlyphFn getGlyph = (GetGlyphFn)(*(u32*)(*(u32*)font + 0x50));
        float w = 0;
        for (const u16* p = text; *p; p++) {
            u8 glyph[0x40];
            memset(glyph, 0, sizeof(glyph));
            getGlyph(font, glyph, *p);
            w += getF32(0x58) * getF32(0x24) * ((float)(s8)glyph[6] * getF32(0x68));
        }
        return w;
    }

    // `text` (UTF-16, NUL-ended) in `color` with a black edge, centred on x, its top at y.
    static void printCentred(const u16* text, float x, float y, float scale, u32 color)
    {
        setU32(0x08, color); setU32(0x0C, color); setU32(0x10, color); setU32(0x14, color);
        setU32(0x18, color);
        s_cw[0x42] = 0xFF;          // alpha
        s_cw[0x43] = 0;             // proportional
        setF32(0x24, scale);        // scale x
        setF32(0x28, scale);        // scale y
        setF32(0x50, 1.0f);
        setF32(0x5C, 1.0f);         // edge width
        setU32(0x60, 0x000000FF);   // edge colour
        setF32(0x2C, x - textWidth(text) * 0.5f);
        setF32(0x30, y);
        setF32(0x34, 0);
        s_cwSetupGX(s_cw);
        s_gxZMode(0, 7 /* GX_ALWAYS */, 0);
        s_gxCullMode(0 /* GX_CULL_NONE */);
        for (const u16* p = text; *p; p++) s_cwPrint(s_cw, *p);
    }

    // Brawl's damage panels are spread evenly about the centre of the 640-wide HUD, in port
    // order (two players: centres 243 and 397); the names go under them, as small as Melee's.
    static const float PANEL_GAP = 154.0f;
    static const float NAME_TOP = 449.0f;
    static const float NAME_SCALE = 0.55f;

    static void drawNames()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        int n = se.numPlayers;
        if (n < 1 || n > 4) return;
        for (int i = 0; i < n; i++) {
            const PPOM::SessionPlayer& pl = se.players[i];
            if (!pl.present) continue;
            u16 name[PPOM::NAME_LEN + 1];
            int len = 0;
            for (; len < PPOM::NAME_LEN && pl.name[len]; len++) {
                u16 c = pl.name[len];
                name[len] = c < 0x20 ? (u16)' ' : c;   // nothing the renderer reads as a command
            }
            name[len] = 0;
            if (!len) continue;
            float x = SCREEN_W * 0.5f + ((float)i - (float)(n - 1) * 0.5f) * PANEL_GAP;
            printCentred(name, x, NAME_TOP, NAME_SCALE, 0xFFFFFFFF);
        }
    }

    static void draw()
    {
        bool names = g_onlineMatchGame != 0;
        if (!s_on && !names) return;
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

        if (names) drawNames();
        if (!s_on) return;
        // Red (Slippi FF0000FF), opaque, with a thin black edge so it reads on any stage.
        const char* s = s_desync ? "DESYNC DETECTED" : "DISCONNECTED";
        u16 text[16];
        int n = 0;
        for (; s[n] && n < 15; n++) text[n] = (u8)s[n];
        text[n] = 0;
        printCentred(text, SCREEN_W * 0.5f, TOP, SCALE, 0xFF0000FF);
        // Tell Dolphin the game shows DISCONNECTED (LOCAL, not part of the rolled-back state):
        // its red OSD message is only the fallback for a game that cannot.
        if (!s_desync) PPOM::g_block.local.hudDisconnected = 1;
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
