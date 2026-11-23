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
// And over the online CSS, Ranked's ping check popup (drawPingPopup): "High ping: N ms / Play
// anyway?" with PLAY and CANCEL (bug report #226).
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
#include "local_aspect.h"
#include "ppom.h"
#include "training_room.h"

extern "C" {
    extern u8 g_onlineMatchGame;
    int sprintf(char* buf, const char* fmt, ...);
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
    // On a screen of another shape (LocalAspect) that space is the 16:9 picture in the middle of
    // the screen, where the HUD is; `wholeScreen` is for what covers the screen instead (the fade):
    // the 640x480 space stretched over all of it, as the game draws its own fades.
    static void begin2D(bool wholeScreen = false)
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
        if (!wholeScreen) LocalAspect::fit();
        s_gxLoadPosMtx(pos, 0);
        s_gxCurrentMtx(0);
    }

    // ---- Unranked's quit hint, drawn as the pause screen's top -----------------------------------
    // Sarah's board, 2026-11-22 (card 748; first asked on bug #290's card, 2026-11-14: "get the full
    // start-menu UI to show up on the screen instead of the placeholder text ... it wouldn't actually
    // affect the camera or allow controls ... it would basically be drawing only the pause menu hud
    // overlay"; then "we can hide all the camera controls and just have it be the START + LRASTART
    // thing at the top", and "make sure the camera controls graphic is gone no matter what
    // controller they're using, so it's only the L+R+A+START equivalent and START button
    // equivalent for that controller"). The hint is the pause screen's own top for the player's
    // controller, from the pictures the pause screen draws it with, where it draws them: "Quit:"
    // and that controller's quit buttons at the top left, "Back:" and its START (PLUS on a Wii
    // Remote or Classic Controller) at the top right. GameCube: L + R + A + START; Classic
    // Controller: L + R + a + PLUS; Wii Remote alone: B + 1 + 2 + PLUS; with a Nunchuk: Z + B + 1 +
    // PLUS. The camera controls, "P1 PAUSE" and the song title are left out. Every place was
    // measured on P+'s real pause in a local Versus match at 16:9, one for each controller (each
    // picture template-matched against the screenshot; run/scratch/pause-art).
    // The pictures are the info archive's, in the InfoResource heap for the whole match (TEXES).
    // They are drawn from there, as they are (the C4 / C8 ones through their palettes in TLUT 0):
    // nothing is allocated or copied, and nothing of the game's state is touched. Found by name once
    // a match (one pass over the heap); with a picture of this controller's not as P+ ships it, the
    // white text stays.
    typedef void (*InitTexFn)(void* obj, void* image, u16 w, u16 h, u32 fmt, u32 s, u32 t, u8 mip);
    typedef void (*InitTexCIFn)(void* obj, void* image, u16 w, u16 h, u32 fmt, u32 s, u32 t, u8 mip, u32 tlut);
    typedef void (*InitLodFn)(void* obj, u32 minF, u32 magF, float minLod, float maxLod, float bias, u8 clamp,
                              u8 edge, u32 aniso);
    typedef void (*LoadTexFn)(void* obj, u32 map);
    typedef void (*InitTlutFn)(void* obj, void* lut, u32 fmt, u16 n);
    typedef void (*LoadTlutFn)(void* obj, u32 name);
    typedef void (*VoidFn)();
    static const InitTexFn s_gxInitTex = (InitTexFn)0x801F28F0;         // GXInitTexObj
    static const InitTexCIFn s_gxInitTexCI = (InitTexCIFn)0x801F2AFC;   // GXInitTexObjCI
    static const InitLodFn s_gxInitLod = (InitLodFn)0x801F2B44;         // GXInitTexObjLOD
    static const LoadTexFn s_gxLoadTex = (LoadTexFn)0x801F2FB0;         // GXLoadTexObj
    static const InitTlutFn s_gxInitTlut = (InitTlutFn)0x801F3004;      // GXInitTlutObj
    static const LoadTlutFn s_gxLoadTlut = (LoadTlutFn)0x801F302C;      // GXLoadTlut
    static const VoidFn s_setup2DTex = (VoidFn)0x8001A7AC;   // gfDrawSetVtxPosColorTexPrimEnvironment
    typedef void (*BeginFn)(int prim, int fmt, u16 verts);
    static const BeginFn s_gxBeginTex = (BeginFn)0x801F1088;      // GXBegin
    typedef void (*InvalidateFn)(u32 flags);
    static const InvalidateFn s_g3dInvalidate = (InvalidateFn)0x801A12AC;   // nw4r::g3d::G3DState::Invalidate

    static const u32 HEAP_INFOS = 0x80494958;   // g_HeapInfos: {name, start, size, arena}, 0x10 each
    static const int HEAP_INFO_RESOURCE = 9;
    static const u32 MAGIC_TEX0 = 0x54455830, MAGIC_PLT0 = 0x504C5430;

    // The pictures (GX texture formats: 2 IA4, 8 C4, 9 C8; C4 / C8 with a PLT0 of the same name).
    struct TexInfo {
        const char* name;
        u8 fmt;
        u16 w, h;
    };
    enum { TX_WORDS, TX_GC, TX_CLASSIC, TX_PLUS, TX_WII, TX_COUNT };
    static const TexInfo TEXES[TX_COUNT] = {
        {"InfPause03Info3", 2, 128, 20},        // "Back:", "Quit:", "+"
        {"InfPauseButton00", 9, 120, 132},      // GameCube buttons, START/PAUSE
        {"InfButton04ClBtn01", 9, 64, 168},     // Classic Controller buttons
        {"InfButton04ClBtn01St", 8, 24, 24},    // PLUS
        {"InfButton04NnBtn01", 9, 112, 96},     // Wii Remote and Nunchuk buttons
    };

    static struct {
        u8* tex[TX_COUNT];
        u8* plt[TX_COUNT];
        u32 lookedAt;  // the game frame of the last search + 1 (0: not this match)
    } s_art;

    static bool isMem(u32 p) { return p >= 0x80000000 && p < 0x81800000; }

    // The section at `p` is `magic` named `name` (a BRRES sub-file: +0x14 the name's offset).
    static bool sectionIs(const u8* p, u32 magic, const char* name)
    {
        if (!isMem((u32)p) || *(const u32*)p != magic) return false;
        s32 noff = *(const s32*)(p + 0x14);
        if (noff <= 0 || noff > 0x200000) return false;
        const char* s = (const char*)p + noff;
        for (int k = 0;; k++) {
            if (s[k] != name[k]) return false;
            if (!name[k]) return true;
        }
    }

    // Picture `i` is there as P+ ships it, its texels (and palette) 32-byte aligned (GX reads them there).
    static bool texOk(int i)
    {
        const TexInfo& T = TEXES[i];
        const u8* t = s_art.tex[i];
        if (!sectionIs(t, MAGIC_TEX0, T.name) || *(const u32*)(t + 0x20) != T.fmt ||
            *(const u16*)(t + 0x1C) != T.w || *(const u16*)(t + 0x1E) != T.h ||
            ((u32)t + *(const u32*)(t + 0x10)) % 32 != 0)
            return false;
        if (T.fmt != 8 && T.fmt != 9) return true;
        const u8* p = s_art.plt[i];
        return sectionIs(p, MAGIC_PLT0, T.name) && ((u32)p + *(const u32*)(p + 0x10)) % 32 == 0;
    }

    static void findArt()
    {
        // At most one search a second (a match whose archive is not there keeps the text).
        const u32 gf = *(u32*)GAME_FRAME;
        if (s_art.lookedAt && gf + 1 - s_art.lookedAt < 60) return;
        s_art.lookedAt = gf + 1;
        memset(s_art.tex, 0, sizeof(s_art.tex));
        memset(s_art.plt, 0, sizeof(s_art.plt));
        const u8* hi = (const u8*)(HEAP_INFOS + 0x10 * HEAP_INFO_RESOURCE);
        u8* base = *(u8**)(hi + 4);
        u32 size = *(const u32*)(hi + 8);
        if (!isMem((u32)base) || size == 0 || size > 0x400000) return;
        for (u32 o = 0; o + 0x40 < size; o += 4) {
            u8* p = base + o;
            const u32 m = *(u32*)p;
            if (m != MAGIC_TEX0 && m != MAGIC_PLT0) continue;
            for (int i = 0; i < TX_COUNT; i++) {
                if (sectionIs(p, m, TEXES[i].name)) (m == MAGIC_TEX0 ? s_art.tex : s_art.plt)[i] = p;
            }
        }
    }

    // A picture: its texture, its box there (texels), its quarter turns to the left (1; 3 to the
    // right: the START/PAUSE label, the Classic Controller's L and R and the Nunchuk's Z lie on their
    // side in their atlases), and where it goes in the 640x480 space (left, top, width, height).
    struct Piece {
        u8 tex, turn, x, y, w, h;
        float left, top, width, height;
    };
    // Each controller's pictures: "Quit:", its quit buttons, "Back:" and its START.
    static const Piece PIECES_GC[] = {
        {TX_WORDS, 0, 62, 0, 45, 20, 120.32f, 50.00f, 29.37f, 14.67f},    // Quit:
        {TX_GC, 0, 70, 46, 47, 31, 152.53f, 49.67f, 21.38f, 18.33f},      // L
        {TX_WORDS, 0, 113, 4, 14, 14, 172.11f, 53.33f, 7.73f, 10.00f},   // +
        {TX_GC, 0, 70, 80, 48, 30, 178.55f, 50.00f, 21.64f, 17.67f},      // R
        {TX_WORDS, 0, 113, 4, 14, 14, 197.36f, 53.00f, 7.99f, 10.33f},   // +
        {TX_GC, 0, 2, 94, 36, 36, 206.63f, 49.67f, 13.66f, 18.00f},       // A
        {TX_WORDS, 0, 113, 4, 14, 14, 220.29f, 53.00f, 7.99f, 10.33f},   // +
        {TX_GC, 0, 40, 10, 16, 16, 232.66f, 51.67f, 10.56f, 13.00f},      // START
        {TX_GC, 1, 57, 0, 8, 37, 226.22f, 44.33f, 23.96f, 6.67f},         // START/PAUSE
        {TX_WORDS, 0, 0, 0, 49, 20, 456.55f, 79.67f, 39.68f, 19.33f},     // Back:
        {TX_GC, 0, 40, 10, 16, 16, 503.70f, 79.00f, 18.55f, 24.00f},      // START
        {TX_GC, 1, 57, 0, 8, 37, 492.11f, 66.00f, 42.77f, 12.00f},        // START/PAUSE
    };
    static const Piece PIECES_CLASSIC[] = {
        {TX_WORDS, 0, 62, 0, 45, 20, 120.32f, 50.00f, 29.37f, 14.67f},    // Quit:
        {TX_CLASSIC, 3, 1, 116, 23, 52, 153.04f, 49.67f, 24.22f, 13.67f}, // L
        {TX_WORDS, 0, 113, 4, 14, 14, 176.75f, 53.00f, 7.73f, 10.33f},   // +
        {TX_CLASSIC, 3, 1, 63, 23, 52, 185.76f, 49.67f, 23.70f, 14.00f},  // R
        {TX_WORDS, 0, 113, 4, 14, 14, 209.73f, 53.00f, 7.73f, 10.33f},   // +
        {TX_CLASSIC, 0, 1, 1, 30, 30, 218.23f, 47.67f, 14.17f, 18.00f},   // a
        {TX_WORDS, 0, 113, 4, 14, 14, 231.37f, 53.00f, 7.47f, 10.33f},   // +
        {TX_PLUS, 0, 1, 1, 22, 22, 239.61f, 49.67f, 11.85f, 15.67f},      // PLUS
        {TX_WORDS, 0, 0, 0, 49, 20, 456.55f, 70.00f, 39.68f, 19.00f},     // Back: (higher: no label above)
        {TX_PLUS, 0, 1, 1, 22, 22, 504.99f, 68.33f, 16.75f, 22.00f},      // PLUS
    };
    static const Piece PIECES_WII[] = {
        {TX_WORDS, 0, 62, 0, 45, 20, 120.32f, 50.00f, 29.37f, 14.67f},    // Quit:
        {TX_WII, 0, 44, 1, 33, 47, 156.39f, 43.33f, 15.20f, 28.00f},      // B
        {TX_WORDS, 0, 113, 4, 14, 14, 173.66f, 53.00f, 7.73f, 10.33f},   // +
        {TX_WII, 0, 83, 36, 28, 28, 182.67f, 48.33f, 12.88f, 17.00f},     // 1
        {TX_WORDS, 0, 113, 4, 14, 14, 195.81f, 53.00f, 7.73f, 10.33f},   // +
        {TX_WII, 0, 83, 65, 28, 28, 204.57f, 48.67f, 12.88f, 16.67f},     // 2
        {TX_WORDS, 0, 113, 4, 14, 14, 217.20f, 53.00f, 7.99f, 10.33f},   // +
        {TX_PLUS, 0, 1, 1, 22, 22, 226.47f, 49.33f, 11.59f, 15.67f},      // PLUS
        {TX_WORDS, 0, 0, 0, 49, 20, 456.55f, 70.00f, 39.68f, 19.00f},     // Back: (higher: no label above)
        {TX_PLUS, 0, 1, 1, 22, 22, 504.99f, 68.33f, 16.75f, 22.00f},      // PLUS
    };
    static const Piece PIECES_NUNCHUK[] = {
        {TX_WORDS, 0, 62, 0, 45, 20, 120.32f, 50.00f, 29.37f, 14.67f},    // Quit:
        {TX_WII, 3, 45, 49, 31, 42, 155.88f, 48.00f, 19.58f, 18.67f},     // Z
        {TX_WORDS, 0, 113, 4, 14, 14, 176.75f, 53.00f, 7.73f, 10.33f},   // +
        {TX_WII, 0, 44, 1, 33, 47, 186.28f, 43.33f, 15.20f, 28.00f},      // B
        {TX_WORDS, 0, 113, 4, 14, 14, 202.00f, 53.00f, 7.73f, 10.33f},   // +
        {TX_WII, 0, 83, 36, 28, 28, 211.27f, 48.67f, 12.88f, 16.67f},     // 1
        {TX_WORDS, 0, 113, 4, 14, 14, 224.15f, 53.00f, 7.99f, 10.33f},   // +
        {TX_PLUS, 0, 1, 1, 22, 22, 233.17f, 49.33f, 12.11f, 15.33f},      // PLUS
        {TX_WORDS, 0, 0, 0, 49, 20, 456.55f, 70.00f, 39.68f, 19.00f},     // Back: (higher: no label above)
        {TX_PLUS, 0, 1, 1, 22, 22, 504.99f, 68.33f, 16.75f, 22.00f},      // PLUS
    };
    struct Layout {
        const Piece* pieces;
        u32 n;
    };
    #define LAYOUT(a) {a, sizeof(a) / sizeof(a[0])}
    static const Layout LAYOUTS[4] = {LAYOUT(PIECES_GC), LAYOUT(PIECES_CLASSIC), LAYOUT(PIECES_WII),
                                      LAYOUT(PIECES_NUNCHUK)};   // by gfPadType
    #undef LAYOUT

    static void loadArt(int i)
    {
        const TexInfo& T = TEXES[i];
        u32 obj[8];
        u8* t = s_art.tex[i];
        void* texels = t + *(u32*)(t + 0x10);
        if (T.fmt == 8 || T.fmt == 9) {
            u32 tlut[3];
            u8* p = s_art.plt[i];
            s_gxInitTlut(tlut, p + *(u32*)(p + 0x10), *(u32*)(p + 0x18), *(u16*)(p + 0x1C));
            s_gxLoadTlut(tlut, 0 /* GX_TLUT0 */);
            s_gxInitTexCI(obj, texels, T.w, T.h, T.fmt, 0, 0, 0, 0);
        } else {
            s_gxInitTex(obj, texels, T.w, T.h, T.fmt, 0, 0, 0);
        }
        s_gxInitLod(obj, 1, 1, 0.0f, 0.0f, 0.0f, 0, 0, 0);
        s_gxLoadTex(obj, 0);
    }

    extern "C" {
        u32 g_pauseArt;       // tests: frames the hint was drawn as the pause screen's pictures
        u32 g_pauseArtPad;    // tests: the controller type it was last drawn for (gfPadType)
    }

    static bool drawPauseArt()
    {
        const u32 pad = OnlineMatch::quitHintPad();
        if (pad > 3) return false;
        const Layout& L = LAYOUTS[pad];
        bool ok = true;
        for (int pass = 0; pass < 2; pass++) {
            ok = true;
            for (u32 i = 0; i < L.n && ok; i++) ok = texOk(L.pieces[i].tex);
            if (ok || pass) break;
            findArt();
        }
        if (!ok) return false;
        s_setup2DTex();
        s_gxZMode(0, 7 /* GX_ALWAYS */, 0);
        s_gxCullMode(0 /* GX_CULL_NONE */);
        int loaded = -1;
        volatile float* fifoF = (volatile float*)0xCC008000;   // the GX write-gather pipe
        volatile u32* fifoU = (volatile u32*)0xCC008000;
        for (u32 i = 0; i < L.n; i++) {
            const Piece& P = L.pieces[i];
            if (P.tex != loaded) {
                loadArt(P.tex);
                loaded = P.tex;
            }
            const float tw = TEXES[P.tex].w, th = TEXES[P.tex].h;
            const float u0 = P.x / tw, u1 = (P.x + P.w) / tw, v0 = P.y / th, v1 = (P.y + P.h) / th;
            // screen corners: top left, top right, bottom right, bottom left (y down); the
            // texture's corner on each, `turn` corners on
            const float xs[4] = {P.left, P.left + P.width, P.left + P.width, P.left};
            const float ys[4] = {P.top, P.top, P.top + P.height, P.top + P.height};
            const float us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
            s_gxBeginTex(0x80 /* GX_QUADS */, 1 /* GX_VTXFMT1 */, 4);
            for (int k = 0; k < 4; k++) {
                const int j = (k + P.turn) & 3;
                *fifoF = xs[k];
                *fifoF = ys[k];
                *fifoF = 0.0f;
                *fifoU = 0xFFFFFFFF;
                *fifoF = us[j];
                *fifoF = vs[j];
            }
        }
        s_g3dInvalidate(0x7FF);   // g3d loads its own textures and TLUTs again
        g_pauseArt++;
        g_pauseArtPad = pad;
        return true;
    }

    static void drawPairBanner(const u16* text);
    static void draw()
    {
        bool names = g_onlineMatchGame != 0;
        bool hint = OnlineMatch::quitHintShown();
        u16 pairLine[96];
        bool pair = OnlineMatch::pairRoomLine(pairLine, 96);
        if (!s_on && !names && !hint && !pair) {
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

        // The names go with the damage panels, at the screen's bottom edge; the rest is under the
        // timer, at its top (LocalAspect: on a narrower screen those are not the picture's own).
        LocalAspect::edge(-1);
        if (names) drawNames();
        LocalAspect::edge(1);
        if (!s_on && pair) {
            // A 2v2 party's pair room: the waiting room's banner, with what it is and how to leave.
            drawPairBanner(pairLine);
            PPOM::g_block.debug.scratch[10] |= 0x40000000;   // tests: the pair room's banner was drawn
            return;
        }
        if (!s_on) {
            // Unranked: START shows how to quit (Slippi's client pause screen, which shows only
            // L+R+A+START; here the player's own controller's buttons): the pause screen's top
            // (drawPauseArt), or white text where DISCONNECTED would be; START again hides it.
            if (hint && drawPauseArt()) {
                PPOM::g_block.debug.scratch[10] |= 0x10000000;   // tests: the hint was drawn
            } else if (hint) {
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

    // The GX state of a flat quad (vertex position + colour, alpha blended) in begin2D's space.
    static void quadSetup()
    {
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
    }

    static void quad(float x0, float y0, float x1, float y1, u32 rgba)
    {
        const float xs[4] = {x0, x1, x1, x0};
        const float ys[4] = {y0, y0, y1, y1};
        volatile float* fifoF = (volatile float*)0xCC008000;   // the GX write-gather pipe
        volatile u32* fifoU = (volatile u32*)0xCC008000;
        s_gxBegin(0x80 /* GX_QUADS */, 0, 4);
        for (int i = 0; i < 4; i++) {
            *fifoF = xs[i];
            *fifoF = ys[i];
            *fifoF = 0.0f;
            *fifoU = rgba;
        }
    }

    static void drawFade()
    {
        const u8 alpha = TrainingRoom::fadeAlpha();
        if (!alpha) return;
        begin2D(true);
        quadSetup();
        quad(-8.0f, -8.0f, SCREEN_W + 8.0f, SCREEN_H + 8.0f, alpha);   // black, this alpha
    }

    // A 2v2 party's pair room's banner (OnlineMatch::pairRoomLine; Sarah's, 2026-11-03: "definitely
    // will also want the banner back"), as the waiting room's own (Brawl's banner there, which
    // only the room's operator has): a dark band across the screen, the line moving through it
    // from right to left and coming round again. Drawn only, by the frame hook: nothing of it is
    // in the game's state. Its place follows the match's game frame, so a rollback's frames run
    // again move it as they did the first time.
    static const float BANNER_Y0 = 84.0f, BANNER_Y1 = 128.0f;
    static const float BANNER_SCALE = 0.62f;
    static const float BANNER_TEXT_TOP = 94.0f;
    static const u32 BANNER_RGBA = 0x141414C8;
    static const u32 BANNER_SPEED = 2;     // 640-space units a frame
    static const u32 BANNER_GAP = 160;     // between the line's end and its next start
    static void drawPairBanner(const u16* text)
    {
        quadSetup();
        // Across the screen, a wider one too (LocalAspect): the 640 space is the 16:9 picture in
        // the middle, the screen goes on `side` past each of its side edges, and the line comes in
        // at the screen's right edge and leaves at its left.
        const float side = (LocalAspect::wider() - 1.0f) * SCREEN_W * 0.5f;
        quad(-side - 8.0f, BANNER_Y0, SCREEN_W + side + 8.0f, BANNER_Y1, BANNER_RGBA);
        setF32(0x24, BANNER_SCALE * s_aspectX);
        const float w = textWidth(text);
        const u32 period = (u32)(SCREEN_W + 2.0f * side) + (u32)w + BANNER_GAP;
        const float left = SCREEN_W + side - (float)((*(u32*)GAME_FRAME * BANNER_SPEED) % period);
        printCentredXY(text, left + w * 0.5f, BANNER_TEXT_TOP, BANNER_SCALE, BANNER_SCALE, 0xFFFFFFFF);
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

    // Ranked's ping check popup over the CSS (OnlineMenu::pingPopup; bug report #226, Sarah's
    // board 2026-11-05: "a popup would be nice ... just 'Play anyway?'"): the screen dimmed, a box
    // with the round trip in a dark red band (P+'s red), "Play anyway?" with the seconds left and
    // PLAY / CANCEL, the cursor's button red; once this player has answered, the line instead
    // (waiting for the other player, or how it ended). Drawn only: online_menu.cpp takes the
    // controllers while it is up.
    static const float PU_X0 = 196.0f, PU_X1 = 444.0f, PU_Y0 = 156.0f, PU_Y1 = 318.0f;
    static const float PU_BAND = 46.0f;
    static const float PU_BTN_Y0 = 262.0f, PU_BTN_Y1 = 298.0f, PU_BTN_W = 100.0f, PU_BTN_GAP = 16.0f;
    static const float PU_TITLE_SCALE = 0.8f, PU_TEXT_SCALE = 0.66f, PU_BTN_SCALE = 0.66f;

    // UTF-8 into `out` (n UTF-16 units with its NUL), anything under a space as a space (nothing the
    // renderer reads as a command).
    static void toU16(u16* out, int n, const char* in)
    {
        int k = 0;
        for (int i = 0; in[i] && k < n - 1;) {
            u16 c = PPOM::utf8Next(in, &i);
            out[k++] = c < 0x20 ? (u16)' ' : c;
        }
        out[k] = 0;
    }

    // `text` centred on x at y, narrowed to `maxW` if it is wider (its height kept).
    static void printFitted(const char* text, float x, float y, float scale, float maxW, u32 color)
    {
        u16 t[96];
        toU16(t, 96, text);
        setF32(0x24, scale * s_aspectX);
        const float w = textWidth(t);
        printCentredXY(t, x, y, w > maxW ? scale * maxW / w : scale, scale, color);
    }

    static void drawPingPopup()
    {
        OnlineMenu::PingPopup p;
        if (!OnlineMenu::pingPopup(&p)) return;
        // The dim covers the screen, whatever its shape (as the fade); the popup is in the 16:9
        // picture with the CSS.
        begin2D(true);
        quadSetup();
        quad(-8.0f, -8.0f, SCREEN_W + 8.0f, SCREEN_H + 8.0f, 0x00000090);   // the CSS dimmed
        begin2D();
        s_aspectX = aspectX();
        const float cx = (PU_X0 + PU_X1) * 0.5f, maxW = PU_X1 - PU_X0 - 24.0f;
        quadSetup();
        quad(PU_X0 - 3.0f, PU_Y0 - 3.0f, PU_X1 + 3.0f, PU_Y1 + 3.0f, 0xE6E6E6FF);   // rim
        quad(PU_X0, PU_Y0, PU_X1, PU_Y1, 0x1A1A22F6);
        quad(PU_X0, PU_Y0, PU_X1, PU_Y0 + PU_BAND, 0x8C1010FF);
        char buf[48];
        sprintf(buf, "High ping: %u ms", (unsigned)p.ms);
        printFitted(buf, cx, PU_Y0 + 8.0f, PU_TITLE_SCALE, maxW, 0xFFFFFFFF);
        if (p.state == OnlineMenu::PINGUP_ASK) {
            sprintf(buf, "Play anyway? (%u)", (unsigned)p.seconds);
            printFitted(buf, cx, PU_Y0 + 64.0f, PU_TEXT_SCALE, maxW, 0xFFFFFFFF);
            static const char* const LABELS[2] = {"PLAY", "CANCEL"};
            for (int i = 0; i < 2; i++) {
                const bool on = p.cursor == i;
                const float x0 = cx + (i == 0 ? -PU_BTN_GAP * 0.5f - PU_BTN_W : PU_BTN_GAP * 0.5f);
                quadSetup();
                if (on) quad(x0 - 2.0f, PU_BTN_Y0 - 2.0f, x0 + PU_BTN_W + 2.0f, PU_BTN_Y1 + 2.0f, 0xFFFFFFFF);
                quad(x0, PU_BTN_Y0, x0 + PU_BTN_W, PU_BTN_Y1, on ? 0xD82020FF : 0x44444CFF);
                printFitted(LABELS[i], x0 + PU_BTN_W * 0.5f, PU_BTN_Y0 + 7.0f, PU_BTN_SCALE, PU_BTN_W - 8.0f,
                            on ? 0xFFFFFFFF : 0xB0B0B0FF);
            }
        } else {
            printFitted("Play anyway?", cx, PU_Y0 + 64.0f, PU_TEXT_SCALE, maxW, 0x9A9AA2FF);
            printFitted(p.line, cx, PU_BTN_Y0 + 7.0f, PU_TEXT_SCALE, maxW,
                        p.state == OnlineMenu::PINGUP_END ? 0xFF7070FF : 0xFFFFFFFF);
        }
        OnlineMenu::pingPopupDrawn();
    }

    // Inline hook at 0x8001792C (gfApplication frame loop, `addi r3, r30, 0x118`).
    static void onFrameDrawn()
    {
        OnlineMatch::onFrameDrawn();
        draw();
        drawPingPopup();
        drawFade();
        LocalAspect::frame();
    }

    void install(CoreApi* api)
    {
        api->syInlineHook(0x8001792C, reinterpret_cast<void*>(onFrameDrawn));
        api->syReplaceFunc(0x800E10B0, reinterpret_cast<void*>(hkTagRender), (void**)&s_origTagRender);
    }
}
