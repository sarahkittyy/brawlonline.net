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
#include "training_room.h"

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
    static const float HINT_SCALE = 0.7f;

    // Online every match is played in 16:9 (Dolphin pins Brawl's screen setting: GameplaySession
    // PinScreenSetting), and the game then draws into the 640-wide picture squeezed by 3/4, which
    // the screen stretches back out. Our text was drawn unsqueezed, so it came out 4/3 too wide
    // (bug report #198: "the player name tags under the character portraits are stretched
    // horizontally at all times"). The screen setting: gmGlobalRecord (GameGlobal 0x805A00E0
    // +0x24) +0x838, bits 0xC0 (0xC0 16:9, 0 4:3).
    static float s_aspectX = 1.0f;   // the text's width factor: 0.75 in 16:9

    static float aspectX()
    {
        u32 gg = *(u32*)0x805A00E0;
        if (gg < 0x80000000 || gg >= 0x81800000) return 1.0f;
        u32 rec = *(u32*)(gg + 0x24);
        if (rec < 0x80000000 || rec >= 0x81800000) return 1.0f;
        return (*(u8*)(rec + 0x838) & 0xC0) == 0xC0 ? 0.75f : 1.0f;
    }

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

    // `text` (UTF-16, NUL-ended) in `color` with a black edge, centred on x, its top at y; scaleX
    // as it shows on screen (s_aspectX applied here).
    static void printCentredXY(const u16* text, float x, float y, float scaleX, float scaleY, u32 color);
    static void printCentred(const u16* text, float x, float y, float scale, u32 color)
    {
        printCentredXY(text, x, y, scale, scale, color);
    }
    static void printCentredXY(const u16* text, float x, float y, float scaleX, float scaleY, u32 color)
    {
        setU32(0x08, color); setU32(0x0C, color); setU32(0x10, color); setU32(0x14, color);
        setU32(0x18, color);
        s_cw[0x42] = 0xFF;          // alpha
        s_cw[0x43] = 0;             // proportional
        setF32(0x24, scaleX * s_aspectX);   // scale x
        setF32(0x28, scaleY);       // scale y
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

    // Each name goes under its player's damage panel, read from the HUD itself (staging feedback
    // 2026-10-10: the names were placed by port, as if the ports between were there; Brawl packs
    // the panels of the players present, and their spacing depends on how many there are: 18
    // HUD units apart for three, found live). g_IfMngr (0x805A02D0) +0x4C: IfPlayer per port;
    // its +0x30 is the panel's MuObject, whose ScnMdl's top node (world matrix +0xEC, x at +0xC)
    // is the panel's place in HUD units. Mapped to the 640-wide space: x * 7.688 + 279.7 (fitted
    // live from the damage digits, the name centred under the panel as before). Without a HUD
    // object (never seen) the panels are taken as packed and evenly spread, 154 apart.
    // A name wider than its panel is made narrower to fit.
    // That node is the damage digits, and they move when the player is KO'd: they fly off (x
    // drifts for about 15 frames) and snap back at the respawn (staging feedback 2026-10-10: the
    // name slid with them). So each port's place is read once per match, the first frame it is
    // the same as the frame before (at rest: from when the HUD first has it until the first KO;
    // before that the matrix is not set up and fails the checks), and kept for the match.
    // But a salty runback loads the same match again, and its panels' matrices come back at the
    // same addresses still holding the last match's values until the HUD sets them again: a
    // player KO'd just before the runback had their digits off to the side, which passed the
    // checks, stayed the same frame after frame and was kept, so their name sat at the left of
    // the screen the whole match (bug report #198, 3-player rooms with salty runbacks; seen in
    // the harness: the old value until game frame 92, when the fighters come in). So nothing is
    // read or drawn until the match itself has run PANEL_FROM_FRAME frames (the frame the
    // rollback session starts at; nobody can be KO'd in the countdown): g_GameFrame's counter
    // holds the last match's count until the match's first frame and starts again from 1 there.
    static const float PANEL_GAP = 154.0f;
    static const float HUD_UNIT = 7.688f, HUD_CENTRE = 279.7f;
    static const u32 GAME_FRAME = 0x901812A4;   // g_GameFrame's frame counter
    static const u32 PANEL_FROM_FRAME = 120;
    static float s_restX[4], s_lastX[4];
    static u8 s_seen = 0, s_rest = 0;   // bit per port: s_lastX read, s_restX kept
    static u8 s_sim = 0;                // this scene: 0 not drawn yet, 1 before its first frame, 2 running
    static u32 s_sceneFrame;            // the counter when the scene was first drawn

    static void forgetPanels() { s_seen = 0; s_rest = 0; s_sim = 0; }

    static bool panelsReady()
    {
        const u32 gf = *(u32*)GAME_FRAME;
        if (s_sim == 0) {
            s_sim = 1;
            s_sceneFrame = gf;
        }
        if (s_sim == 1 && gf != s_sceneFrame) s_sim = 2;
        return s_sim == 2 && gf >= PANEL_FROM_FRAME;
    }

    static bool livePanelX(int port, float* out);
    static bool panelX(int port, float* out)
    {
        if (port < 0 || port > 3) return false;
        u8 bit = (u8)(1 << port);
        if (s_rest & bit) {
            *out = s_restX[port];
            return true;
        }
        if (!livePanelX(port, out)) return false;
        if ((s_seen & bit) && s_lastX[port] == *out) {
            s_restX[port] = *out;
            s_rest |= bit;
        }
        s_lastX[port] = *out;
        s_seen |= bit;
        return true;
    }

    static bool livePanelX(int port, float* out)
    {
        u32 mgr = *(u32*)0x805A02D0;   // g_IfMngr
        if (mgr < 0x80000000 || mgr >= 0x81800000 || port < 0 || port > 3) return false;
        u32 ifp = *(u32*)(mgr + 0x4C + 4 * port);
        if (ifp < 0x80000000 || ifp >= 0x81800000) return false;
        u32 obj = *(u32*)(ifp + 0x30);
        if (obj < 0x80000000 || obj >= 0x81800000) return false;
        u32 scn = *(u32*)(obj + 0xC);
        if (scn < 0x80000000 || scn >= 0x81800000 || *(u32*)scn != 0x804663C8) return false;
        u32 mtx = *(u32*)(scn + 0xEC);
        if (mtx < 0x80000000 || mtx >= 0x81800000) return false;
        float sx = *(float*)mtx, x = *(float*)(mtx + 0xC);
        if (!(sx > 0.05f && sx < 5.0f) || !(x > -80.0f && x < 80.0f)) return false;
        *out = HUD_CENTRE + x * HUD_UNIT;
        return true;
    }
    static const float NAME_MAX_W = 130.0f;   // four panels are about 147 apart: a gap between names
    static const float NAME_TOP = 449.0f;
    // 0.45 of the system font (0.55 until 2026-10-23; Sarah: "is there a smaller font we can use
    // for the usernames below the players in the vod, that's still readable even at 480? like
    // slippi's looks a lot cleaner"). The game's other resident fonts (kinds 2 and 7) are bolder or
    // miss glyphs; the system font at 0.45 still reads in a 480p render (1x internal resolution).
    static const float NAME_SCALE = 0.45f;

    static void drawNames()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        int n = se.numPlayers;
        if (n < 1 || n > 4) return;
        if (!panelsReady()) return;
        int count = 0;
        for (int i = 0; i < n; i++) {
            if (se.players[i].present) count++;
        }
        int k = -1;   // the player's panel: its place among the players present
        for (int i = 0; i < n; i++) {
            const PPOM::SessionPlayer& pl = se.players[i];
            if (!pl.present) continue;
            k++;
            u16 name[PPOM::NAME_LEN + 1];
            int len = 0;
            for (; len < PPOM::NAME_LEN && pl.name[len]; len++) {
                u16 c = pl.name[len];
                name[len] = c < 0x20 ? (u16)' ' : c;   // nothing the renderer reads as a command
            }
            name[len] = 0;
            if (!len) continue;
            float x;
            if (!panelX(i, &x)) x = SCREEN_W * 0.5f + ((float)k - (float)(count - 1) * 0.5f) * PANEL_GAP;
            // the width at NAME_SCALE (textWidth reads the CharWriter's scale: set it first)
            setF32(0x24, NAME_SCALE * s_aspectX);
            float w = textWidth(name);
            float scale = NAME_SCALE;
            if (w > NAME_MAX_W) scale = NAME_SCALE * NAME_MAX_W / w;
            // same height for a narrower name: only x shrinks, y keeps NAME_SCALE
            printCentredXY(name, x, NAME_TOP, scale, NAME_SCALE, 0xFFFFFFFF);
        }
    }

    // The 2D state the text is drawn in: the HUD's 640x480 space, y down, no depth test, no culling.
    static void begin2D()
    {
        if (!s_constructed) {
            s_cwCtor(s_cw);
            s_constructed = true;
        }
        s_cwSetFont(s_cw, FONT_SYSTEM);
        float proj[4][4];
        float pos[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        s_gxViewport(0, 0, SCREEN_W, SCREEN_H, 0, 1);
        s_gxScissor(0, 0, (u32)SCREEN_W, (u32)SCREEN_H);
        s_mtxOrtho(proj, 0, SCREEN_H, 0, SCREEN_W, -1.0f, 1.0f);
        s_gxProjection(proj, 1);
        s_gxLoadPosMtx(pos, 0);
        s_gxCurrentMtx(0);
    }

    static void draw()
    {
        bool names = g_onlineMatchGame != 0;
        bool hint = OnlineMatch::quitHintShown();
        if (!s_on && !names && !hint) {
            forgetPanels();
            return;
        }
        if (!Online::inScene("scMelee")) {
            s_on = false;   // the text lives as long as the match scene
            forgetPanels();
            return;
        }
        begin2D();
        s_aspectX = aspectX();

        if (names) drawNames();
        if (!s_on) {
            // Unranked: START shows how to quit (Slippi's client pause screen, which shows only
            // L+R+A+START; here the player's own controller's buttons), white where DISCONNECTED
            // would be; START again hides it.
            if (hint) {
                const char* s = OnlineMatch::quitHintText();
                u16 text[24];
                int n = 0;
                for (; s[n] && n < 23; n++) text[n] = (u8)s[n];
                text[n] = 0;
                printCentred(text, SCREEN_W * 0.5f, TOP, HINT_SCALE, 0xFFFFFFFF);
                PPOM::g_block.debug.scratch[10] |= 0x10000000;   // tests: the hint was drawn
            }
            return;
        }
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

    // The waiting room's fade (training_room.cpp, TrainingRoom::fadeAlpha): a black quad over the
    // whole 640x480 space, in any scene, on top of everything else. Vertex colour only: the
    // CharWriter's own 2D state first (fog off, swap tables, ...), then one TEV stage passing the
    // vertex colour, alpha blending, and a quad of position + colour written to the FIFO.
    // The GX functions are the DOL's (named in brawl-decomp's symbols, the others found in
    // ms::CharWriter::SetupGX, 0x8006EEEC, which calls them with the same enums).
    typedef void (*GxVoidFn)();
    typedef void (*GxIntFn)(int);
    typedef void (*GxSetVtxDescFn)(int attr, int type);
    typedef void (*GxSetVtxAttrFmtFn)(int fmt, int attr, int cnt, int type, int frac);
    typedef void (*GxChanCtrlFn)(int chan, int enable, int ambSrc, int matSrc, u32 lightMask, int diffFn, int attnFn);
    typedef void (*GxTevOrderFn)(int stage, int coord, int map, int color);
    typedef void (*GxTevOpFn)(int stage, int mode);
    typedef void (*GxBlendModeFn)(int type, int src, int dst, int logicOp);
    typedef void (*GxAlphaCompareFn)(int comp0, u8 ref0, int op, int comp1, u8 ref1);
    typedef void (*GxBeginFn)(int prim, int fmt, u16 verts);
    static const GxVoidFn s_gxClearVtxDesc = (GxVoidFn)0x801EFB10;
    static const GxSetVtxDescFn s_gxSetVtxDesc = (GxSetVtxDescFn)0x801EF280;
    static const GxSetVtxAttrFmtFn s_gxSetVtxAttrFmt = (GxSetVtxAttrFmtFn)0x801EFB44;
    static const GxIntFn s_gxSetNumChans = (GxIntFn)0x801F2644;
    static const GxChanCtrlFn s_gxSetChanCtrl = (GxChanCtrlFn)0x801F2668;
    static const GxIntFn s_gxSetNumTexGens = (GxIntFn)0x801F0480;
    static const GxIntFn s_gxSetNumIndStages = (GxIntFn)0x801F3ADC;
    static const GxIntFn s_gxSetTevDirect = (GxIntFn)0x801F3AFC;
    static const GxIntFn s_gxSetNumTevStages = (GxIntFn)0x801F41F8;
    static const GxTevOrderFn s_gxSetTevOrder = (GxTevOrderFn)0x801F409C;
    static const GxTevOpFn s_gxSetTevOp = (GxTevOpFn)0x801F3B9C;
    static const GxBlendModeFn s_gxSetBlendMode = (GxBlendModeFn)0x801F46CC;
    static const GxAlphaCompareFn s_gxSetAlphaCompare = (GxAlphaCompareFn)0x801F3FD8;
    static const GxBeginFn s_gxBegin = (GxBeginFn)0x801F1088;

    static void drawFade()
    {
        const u8 alpha = TrainingRoom::fadeAlpha();
        if (!alpha) return;
        begin2D();
        s_cwSetupGX(s_cw);
        s_gxClearVtxDesc();
        s_gxSetVtxDesc(9 /* GX_VA_POS */, 1 /* GX_DIRECT */);
        s_gxSetVtxDesc(11 /* GX_VA_CLR0 */, 1);
        s_gxSetVtxAttrFmt(0, 9, 1 /* GX_POS_XYZ */, 4 /* GX_F32 */, 0);
        s_gxSetVtxAttrFmt(0, 11, 1 /* GX_CLR_RGBA */, 5 /* GX_RGBA8 */, 0);
        s_gxSetNumChans(1);
        s_gxSetChanCtrl(4 /* GX_COLOR0A0 */, 0, 0 /* GX_SRC_REG */, 1 /* GX_SRC_VTX */, 0, 0 /* GX_DF_NONE */, 2 /* GX_AF_NONE */);
        s_gxSetNumTexGens(0);
        s_gxSetNumIndStages(0);
        s_gxSetTevDirect(0);
        s_gxSetNumTevStages(1);
        s_gxSetTevOrder(0, 0xFF /* GX_TEXCOORD_NULL */, 0xFF /* GX_TEXMAP_NULL */, 4 /* GX_COLOR0A0 */);
        s_gxSetTevOp(0, 4 /* GX_PASSCLR */);
        s_gxSetBlendMode(1 /* GX_BM_BLEND */, 4 /* GX_BL_SRCALPHA */, 5 /* GX_BL_INVSRCALPHA */, 0xF);
        s_gxSetAlphaCompare(7 /* GX_ALWAYS */, 0, 0, 7, 0);
        s_gxZMode(0, 7 /* GX_ALWAYS */, 0);
        s_gxCullMode(0 /* GX_CULL_NONE */);
        static const float X0 = -8.0f, X1 = SCREEN_W + 8.0f, Y0 = -8.0f, Y1 = SCREEN_H + 8.0f;
        const float xs[4] = {X0, X1, X1, X0};
        const float ys[4] = {Y0, Y0, Y1, Y1};
        volatile float* fifoF = (volatile float*)0xCC008000;   // the GX write-gather pipe
        volatile u32* fifoU = (volatile u32*)0xCC008000;
        s_gxBegin(0x80 /* GX_QUADS */, 0, 4);
        for (int i = 0; i < 4; i++) {
            *fifoF = xs[i];
            *fifoF = ys[i];
            *fifoF = 0.0f;
            *fifoU = alpha;   // RGBA: black, this alpha
        }
    }

    // Brawl's name tags over the fighters are drawn once, when the HUD is made: fn_800E10B0
    // prints the tag on a box into the EFB and copies it into a texture (fn_8007270C), which the
    // tag model shows for the whole match. Dolphin's default
    // shader mode leaves out a draw whose shader is still compiling ("Skip Drawing"), so with
    // those shaders not cached yet (a first run, a new Dolphin, every harness instance) the tag
    // came out as an empty grey box for the whole match, offline too (Sarah: "oftentimes the
    // 'skip drawing' thing makes the tags not show up in the first game. it's a known bug").
    // Around the call the plugin sets a BP register no GX code uses (0x7F, BPMEM_NO_SKIP_DRAWS in
    // Dolphin's BPMemory.h) through the FIFO, so it reaches Dolphin's GPU thread in order with
    // the tag's own draws: while it holds NO_SKIP_DRAWS_ON, Dolphin compiles a missing shader
    // and waits for it instead of leaving the draw out. Only these few draws, as the HUD loads.
    // A Dolphin without it ignores the register (an unknown BP register is logged and kept).
    typedef void (*TagRenderFn)(void* tex, const u16* name, void* col0, void* col1, int flag,
                                int w, int h, int r10, int stackArg);
    static TagRenderFn s_origTagRender = NULL;
    static const u32 BP_NO_SKIP_DRAWS = 0x7F;
    static const u32 NO_SKIP_DRAWS_ON = 0x50504E;   // "PPN"

    static void writeBP(u32 reg, u32 value)
    {
        volatile u8* fifo8 = (volatile u8*)0xCC008000;   // the GX write-gather pipe
        volatile u32* fifo32 = (volatile u32*)0xCC008000;
        *fifo8 = 0x61;   // load BP register
        *fifo32 = (reg << 24) | (value & 0xFFFFFF);
    }

    static void hkTagRender(void* tex, const u16* name, void* col0, void* col1, int flag, int w,
                            int h, int r10, int stackArg)
    {
        writeBP(BP_NO_SKIP_DRAWS, NO_SKIP_DRAWS_ON);
        s_origTagRender(tex, name, col0, col1, flag, w, h, r10, stackArg);
        writeBP(BP_NO_SKIP_DRAWS, 0);
    }

    // Inline hook at 0x8001792C (gfApplication frame loop, `addi r3, r30, 0x118`).
    static void onFrameDrawn()
    {
        OnlineMatch::onFrameDrawn();
        draw();
        drawFade();
    }

    void install(CoreApi* api)
    {
        api->syInlineHook(0x8001792C, reinterpret_cast<void*>(onFrameDrawn));
        api->syReplaceFunc(0x800E10B0, reinterpret_cast<void*>(hkTagRender), (void**)&s_origTagRender);
    }
}
