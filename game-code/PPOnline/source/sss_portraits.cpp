// Ranked's stage select shows who plays whom (the user's, 2026-10-16: game 1's strikes start the
// moment the queue pops, so the opponent's character was never seen before striking). Left and
// right of the stage preview, each player's CSS portrait stands in a panel of the colour of the
// port that player gets in the match (P1 red, P2 blue), the name under it; this player on the
// left. Games 2+ show the last game's characters. Ranked only.
//
// The players come with Dolphin's step (GameStep pv, by in-game port). The portrait is the one
// P+'s CSS shows: P+'s AsyncRSP plugin loads /menu/common/char_bust_tex/MenSelchrFaceB<CharNo>0.brres
// for a CSS panel, whose textures are MenSelchrFaceB.<CharNo * 10 + 1 + colour> (sel_char's
// muSelCharPlayerArea::setCharPic, text+0x14B68: CharNo = muMenu::getStockFrameID(
// exchangeMuSelchkind2MuStockchkind(selchkind))), CI8 128x160 with an RGB5A3 palette. Each file
// is read through the game's file system (gfFileIOHandle::readRequest, asynchronous: no stall),
// into the Network heap, which lives across scenes; the one texture and palette are copied out
// and the file freed at once. The copies are freed when the stage select ends.
//
// Drawn by match_hud.cpp after the frame (the HUD's 640x480 space, which the game's 16:9 picture
// stretches 4/3 wide: the launcher pins the Wii to 16:9, SYSCONF IPL.AR), with GX directly.
#include <OS/OSCache.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "sss_portraits.h"

extern "C" int sprintf(char* buf, const char* fmt, ...);

// Tests (harness, through PPOnlineMain.map).
extern "C" struct SssPortraitsDebug {
    u32 shown;           // stage selects the panels were shown on
    u32 loads;           // portrait files requested
    u32 loaded;          // portraits copied out
    u32 failed;          // files or textures not found
    u32 freed;           // portrait copies freed
    u32 drawn;           // frames drawn
    u16 texNo[2];        // per side: the texture shown (MenSelchrFaceB.<n>), 0 none
    u8 port[2];          // per side: the in-game port (its colour), 0xFF none
    u8 _pad[2];
} g_sssPortraits = {0, 0, 0, 0, 0, 0, {0, 0}, {0xFF, 0xFF}, {0, 0}};

namespace SssPortraits {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // ---- The players ------------------------------------------------------------------------
    static PPOM::GameStep::PreviewPlayer s_pv[2];   // by in-game port, kept for this stage select
    static bool s_havePreview = false;

    const PPOM::GameStep* previewStep()
    {
        if (OnlineMenu::roomCss()) return NULL;
        const PPOM::GameStep* st = OnlineMenu::rankedStep();
        return st && st->pvCount ? st : NULL;
    }

    // The other player's entry is that machine's bytes: only what this CSS could lock in.
    static bool validPlayer(const PPOM::GameStep::PreviewPlayer& pv)
    {
        if (pv.present != 1 || !OnlineMenu::selectableCharKind(pv.charKind)) return false;
        return (pv.costume & PPOM::COSTUME_ALTS) != PPOM::COSTUME_ALTS;
    }

    static const PPOM::GameStep::PreviewPlayer* sidePlayer(int side, int* port)
    {
        for (int p = 0; p < 2; p++) {
            if (validPlayer(s_pv[p]) && (s_pv[p].local ? 0 : 1) == side) {
                if (port) *port = p;
                return &s_pv[p];
            }
        }
        return NULL;
    }

    // ---- The portraits ----------------------------------------------------------------------
    typedef int (*ExchangeFn)(int);
    static const ExchangeFn GM_TO_SELCH = (ExchangeFn)0x800AF708;     // exchangeGmCharacterKind2MuSelchkind
    static const ExchangeFn SELCH_TO_STOCK = (ExchangeFn)0x800AF82C;  // exchangeMuSelchkind2MuStockchkind
    static const ExchangeFn STOCK_FRAME = (ExchangeFn)0x800AF600;     // getStockFrameID

    typedef bool (*ReadRequestFn)(void* handle, const char* path, int heap, int length, int offset);
    typedef bool (*HandleBoolFn)(void* handle);
    typedef void* (*HandlePtrFn)(void* handle);
    typedef int (*HandleIntFn)(void* handle);
    typedef void (*HandleVoidFn)(void* handle);
    typedef void* (*AllocFn)(int heap, u32 size);
    typedef void (*FreeFn)(void* p);
    static const ReadRequestFn READ_REQUEST = (ReadRequestFn)0x80021498;   // gfFileIOHandle::readRequest(path, HeapType, len, off)
    static const HandleBoolFn IS_READY = (HandleBoolFn)0x80021F38;         // gfFileIOHandle::isReady
    static const HandlePtrFn GET_BUFFER = (HandlePtrFn)0x80021F94;         // gfFileIOHandle::getBuffer
    static const HandleIntFn GET_SIZE = (HandleIntFn)0x80021F88;           // gfFileIOHandle::getSize
    static const HandleVoidFn RELEASE = (HandleVoidFn)0x80021FAC;          // gfFileIOHandle::release (the request only)
    static const AllocFn HEAP_ALLOC = (AllocFn)0x800249E4;                 // gfHeapManager::alloc(HeapType, size)
    static const FreeFn HEAP_FREE = (FreeFn)0x80024A4C;                    // gfHeapManager::free
    static const int HEAP_NETWORK = 6;

    static const int TEX_W = 128, TEX_H = 160;
    static const u32 IMAGE_SIZE = TEX_W * TEX_H;   // CI8
    static const u32 PALETTE_MAX = 256 * 2;        // RGB5A3

    struct Portrait {
        u32 handle;          // gfFileIOHandle: its gfFileIORequest*
        bool pending;        // a read is out
        u8 charNo;
        u16 texNo;           // wanted
        u16 shownNo;         // copied out (0: none)
        void* block;         // the copy's allocation
        u8* image;           // 32-aligned in block
        u8* palette;         // 32-aligned in block
        u16 paletteEntries;
    };
    static Portrait s_portrait[2];   // by side

    // A brres's entry `name` of `group`. nw4r's dictionaries: the root's at +0x18 ("root" at
    // +0x10), {u32 size, u32 n, then n + 1 entries of 0x10}, each entry's name and data offsets
    // (+8, +12) from its dictionary's start.
    static const u8* bresEntry(const u8* b, u32 size, const char* group, const char* name)
    {
        if (size < 0x40 || *(const u32*)b != 0x62726573 /* bres */ || *(const u32*)(b + 0x10) != 0x726F6F74) return NULL;
        const u8* root = b + 0x18;
        u32 n = *(const u32*)(root + 4);
        for (u32 i = 1; i <= n && i < 16; i++) {
            const u8* e = root + 8 + 0x10 * i;
            s32 no = *(const s32*)(e + 8), dof = *(const s32*)(e + 12);
            if (no <= 0 || dof <= 0 || (u32)(root - b) + (u32)dof + 8 > size) continue;
            if (strcmp((const char*)(root + no), group) != 0) continue;
            const u8* d = root + dof;
            u32 m = *(const u32*)(d + 4);
            for (u32 k = 1; k <= m && k < 512; k++) {
                const u8* f = d + 8 + 0x10 * k;
                s32 fn = *(const s32*)(f + 8), fd = *(const s32*)(f + 12);
                if (fn <= 0 || fd <= 0 || (u32)(d - b) + (u32)fd + 0x40 > size) continue;
                if (strcmp((const char*)(d + fn), name) == 0) return d + fd;
            }
        }
        return NULL;
    }

    static void freeCopy(Portrait& p)
    {
        if (p.block) {
            HEAP_FREE(p.block);
            g_sssPortraits.freed++;
        }
        p.block = NULL;
        p.image = p.palette = NULL;
        p.shownNo = 0;
    }

    // The file is in: the wanted texture (else the character's first) and its palette copied out.
    static void takeFile(int side)
    {
        Portrait& p = s_portrait[side];
        const u8* b = (const u8*)GET_BUFFER(&p.handle);
        int size = GET_SIZE(&p.handle);
        RELEASE(&p.handle);
        p.handle = 0;
        p.pending = false;
        if (!isPtr((u32)b) || size < 0x40) {
            g_sssPortraits.failed++;
            return;
        }
        u16 tries[2] = {p.texNo, (u16)(p.charNo * 10 + 1)};
        for (int t = 0; t < 2 && !p.block; t++) {
            char name[32];
            sprintf(name, "MenSelchrFaceB.%03d", tries[t]);
            const u8* tex = bresEntry(b, (u32)size, "Textures(NW4R)", name);
            const u8* plt = bresEntry(b, (u32)size, "Palettes(NW4R)", name);
            if (!tex || !plt || *(const u32*)tex != 0x54455830 /* TEX0 */ || *(const u32*)plt != 0x504C5430) continue;
            if (*(const u16*)(tex + 0x1C) != TEX_W || *(const u16*)(tex + 0x1E) != TEX_H || *(const u32*)(tex + 0x20) != 9 /* CI8 */) continue;
            if (*(const u32*)(plt + 0x18) != 2 /* RGB5A3 */) continue;
            u16 entries = *(const u16*)(plt + 0x1C);
            const u8* img = tex + *(const s32*)(tex + 0x10);
            const u8* pal = plt + *(const s32*)(plt + 0x10);
            if (entries == 0 || entries > 256 || img < b || img + IMAGE_SIZE > b + size || pal + 2 * entries > b + size) continue;
            void* block = HEAP_ALLOC(HEAP_NETWORK, IMAGE_SIZE + PALETTE_MAX + 64);
            if (!block) break;
            u8* a = (u8*)(((u32)block + 31) & ~31u);
            memcpy(a, img, IMAGE_SIZE);
            memcpy(a + IMAGE_SIZE, pal, 2 * entries);
            DCFlushRange(a, IMAGE_SIZE + PALETTE_MAX);
            p.block = block;
            p.image = a;
            p.palette = a + IMAGE_SIZE;
            p.paletteEntries = entries;
            p.shownNo = tries[t];
            g_sssPortraits.loaded++;
        }
        if (!p.block) g_sssPortraits.failed++;
        HEAP_FREE((void*)b);
        g_sssPortraits.texNo[side] = p.shownNo;
    }

    static void request(int side, const PPOM::GameStep::PreviewPlayer& pv)
    {
        Portrait& p = s_portrait[side];
        int stock = SELCH_TO_STOCK(GM_TO_SELCH(pv.charKind));
        int charNo = STOCK_FRAME(stock);
        if (charNo < 0 || charNo > 99) {
            g_sssPortraits.failed++;
            return;
        }
        p.charNo = (u8)charNo;
        p.texNo = (u16)(charNo * 10 + 1 + (pv.costume & PPOM::COSTUME_COLOUR));
        char path[64];
        sprintf(path, "/menu/common/char_bust_tex/MenSelchrFaceB%02d0.brres", charNo);
        p.handle = 0;
        if (READ_REQUEST(&p.handle, path, HEAP_NETWORK, 0, 0)) {
            p.pending = true;
            g_sssPortraits.loads++;
        } else {
            g_sssPortraits.failed++;
        }
    }

    void tick()
    {
        // A read out finishes even after the stage select (its file is freed then).
        for (int side = 0; side < 2; side++) {
            Portrait& p = s_portrait[side];
            if (p.pending && IS_READY(&p.handle)) takeFile(side);
        }
        if (!Online::inScene("scSelStage")) {
            if (s_havePreview) {
                s_havePreview = false;
                for (int side = 0; side < 2; side++) {
                    freeCopy(s_portrait[side]);
                    g_sssPortraits.texNo[side] = 0;
                    g_sssPortraits.port[side] = 0xFF;
                }
            }
            for (int side = 0; side < 2; side++) {
                if (!s_portrait[side].pending) freeCopy(s_portrait[side]);   // a late one's copy
            }
            return;
        }
        if (s_havePreview) return;
        const PPOM::GameStep* st = previewStep();
        if (!st) return;
        memcpy(s_pv, st->pv, sizeof(s_pv));
        s_havePreview = true;
        g_sssPortraits.shown++;
        for (int side = 0; side < 2; side++) {
            int port = -1;
            const PPOM::GameStep::PreviewPlayer* pv = sidePlayer(side, &port);
            g_sssPortraits.port[side] = pv ? (u8)port : 0xFF;
            if (pv && !s_portrait[side].pending) request(side, *pv);
        }
    }

    // ---- Drawing ----------------------------------------------------------------------------
    // GX (DOL; the unnamed ones identified by ms::CharWriter::SetupGX's calls, 0x8006EEEC, and by
    // the SDK's order around GXInitTexCacheRegion).
    typedef void (*GX1)(u32);
    typedef void (*GX2)(u32, u32);
    typedef void (*GX3)(u32, u32, u32);
    typedef void (*GX4)(u32, u32, u32, u32);
    typedef void (*GX5)(u32, u32, u32, u32, u32);
    typedef void (*GX6)(u32, u32, u32, u32, u32, u32);
    typedef void (*GX7)(u32, u32, u32, u32, u32, u32, u32);
    typedef void (*GXVoid)();
    typedef void (*InitTexCIFn)(void* obj, void* image, u16 w, u16 h, u32 fmt, u32 wrapS, u32 wrapT, u8 mip, u32 tlut);
    typedef void (*InitTlutFn)(void* obj, void* lut, u32 fmt, u16 entries);
    typedef void (*LoadObjFn)(void* obj, u32 id);
    static const GXVoid GX_CLEAR_VTX_DESC = (GXVoid)0x801EFB10;
    static const GX2 GX_SET_VTX_DESC = (GX2)0x801EF280;            // (attr, type)
    static const GX5 GX_SET_VTX_ATTR_FMT = (GX5)0x801EFB44;        // (fmt, attr, cnt, type, frac)
    static const GX1 GX_SET_NUM_CHANS = (GX1)0x801F2644;
    static const GX7 GX_SET_CHAN_CTRL = (GX7)0x801F2668;
    static const GX1 GX_SET_NUM_TEX_GENS = (GX1)0x801F0480;
    static const GX6 GX_SET_TEX_COORD_GEN2 = (GX6)0x801F0258;
    static const GX1 GX_SET_NUM_TEV_STAGES = (GX1)0x801F41F8;
    static const GX4 GX_SET_TEV_ORDER = (GX4)0x801F409C;           // (stage, coord, map, colour)
    static const GX2 GX_SET_TEV_OP = (GX2)0x801F3B9C;              // (stage, mode)
    static const GX4 GX_SET_BLEND_MODE = (GX4)0x801F46CC;
    static const GX5 GX_SET_ALPHA_COMPARE = (GX5)0x801F3FD8;
    static const GX3 GX_SET_Z_MODE = (GX3)0x801F4774;
    static const GX1 GX_SET_CULL_MODE = (GX1)0x801F136C;
    static const GX3 GX_BEGIN = (GX3)0x801F1088;                    // (primitive, vtxfmt, count)
    static const InitTexCIFn GX_INIT_TEX_OBJ_CI = (InitTexCIFn)0x801F2AFC;
    static const InitTlutFn GX_INIT_TLUT_OBJ = (InitTlutFn)0x801F3004;
    static const LoadObjFn GX_LOAD_TLUT = (LoadObjFn)0x801F302C;
    static const LoadObjFn GX_LOAD_TEX_OBJ = (LoadObjFn)0x801F2FB0;
    static volatile u32* const FIFO = (volatile u32*)0xCC008000;

    static void put(float f) { *(volatile float*)FIFO = f; }

    // Vertex format 0: position xyz f32, colour rgba8, texture st f32.
    static void setupGX()
    {
        GX_CLEAR_VTX_DESC();
        GX_SET_VTX_DESC(9 /* POS */, 1 /* DIRECT */);
        GX_SET_VTX_DESC(11 /* CLR0 */, 1);
        GX_SET_VTX_DESC(13 /* TEX0 */, 1);
        GX_SET_VTX_ATTR_FMT(0, 9, 1 /* XYZ */, 4 /* F32 */, 0);
        GX_SET_VTX_ATTR_FMT(0, 11, 1 /* RGBA */, 5 /* RGBA8 */, 0);
        GX_SET_VTX_ATTR_FMT(0, 13, 1 /* ST */, 4 /* F32 */, 0);
        GX_SET_NUM_CHANS(1);
        GX_SET_CHAN_CTRL(4 /* COLOR0A0 */, 0, 0 /* REG */, 1 /* VTX */, 0, 0 /* DF_NONE */, 2 /* AF_NONE */);
        GX_SET_NUM_TEX_GENS(1);
        GX_SET_TEX_COORD_GEN2(0, 1 /* MTX2x4 */, 4 /* TEX0 */, 60 /* IDENTITY */, 0, 125 /* PTIDENTITY */);
        GX_SET_NUM_TEV_STAGES(1);
        GX_SET_BLEND_MODE(1 /* BLEND */, 4 /* SRCALPHA */, 5 /* INVSRCALPHA */, 15);
        GX_SET_ALPHA_COMPARE(7 /* ALWAYS */, 0, 0, 7, 0);
        GX_SET_Z_MODE(0, 7, 0);
        GX_SET_CULL_MODE(0);
    }

    static void quad(float x0, float y0, float x1, float y1, u32 rgba, float s0, float t0, float s1, float t1)
    {
        GX_BEGIN(0x80 /* QUADS */, 0, 4);
        put(x0); put(y0); put(0); *FIFO = rgba; put(s0); put(t0);
        put(x1); put(y0); put(0); *FIFO = rgba; put(s1); put(t0);
        put(x1); put(y1); put(0); *FIFO = rgba; put(s1); put(t1);
        put(x0); put(y1); put(0); *FIFO = rgba; put(s0); put(t1);
    }

    static void solid(float x0, float y0, float x1, float y1, u32 rgba)
    {
        GX_SET_TEV_ORDER(0, 0xFF /* TEXCOORD_NULL */, 0xFF /* TEXMAP_NULL */, 4 /* COLOR0A0 */);
        GX_SET_TEV_OP(0, 4 /* PASSCLR */);
        quad(x0, y0, x1, y1, rgba, 0, 0, 0, 0);
    }

    // Port colours, as the CSS's panels and the HUD (P1 red, P2 blue, P3 yellow, P4 green):
    // edge, panel, name plate.
    static const u32 COLOURS[4][3] = {
        {0xF04848FF, 0x5C1616E6, 0x9C2424FF},
        {0x4878F0FF, 0x16245CE6, 0x24409CFF},
        {0xF0C848FF, 0x5C4A16E6, 0x9C7A24FF},
        {0x48C858FF, 0x165C20E6, 0x249C34FF},
    };

    // The panels (640x480 units). The picture is 16:9: a unit across is 4/3 of one down, so the
    // portrait's 128x160 is drawn 0.75 as wide in units. Centred left and right of the stage
    // preview (the user's outline, 2026-10-16).
    static const float CENTRE_X[2] = {81.0f, 559.0f};
    static const float TOP = 132.0f;
    static const float PORTRAIT_H = 150.0f;
    static const float PORTRAIT_W = PORTRAIT_H * 0.8f * 0.75f;
    static const float EDGE = 2.0f, PAD = 5.0f, PLATE_H = 24.0f;

    static void panelRect(int side, float* x0, float* x1, float* plateTop, float* bottom)
    {
        float halfW = PORTRAIT_W * 0.5f + PAD * 0.75f;
        *x0 = CENTRE_X[side] - halfW;
        *x1 = CENTRE_X[side] + halfW;
        *plateTop = TOP + PAD + PORTRAIT_H + PAD;
        *bottom = *plateTop + PLATE_H;
    }

    bool drawPanels()
    {
        if (!s_havePreview || !Online::inScene("scSelStage")) return false;
        bool any = false;
        for (int side = 0; side < 2; side++) {
            int port = -1;
            if (!sidePlayer(side, &port) || port < 0 || port > 3) continue;
            if (!any) setupGX();
            any = true;
            const u32* c = COLOURS[port];
            float x0, x1, plateTop, bottom;
            panelRect(side, &x0, &x1, &plateTop, &bottom);
            float ex = EDGE * 0.75f;
            solid(x0 - ex, TOP - EDGE, x1 + ex, bottom + EDGE, c[0]);
            solid(x0, TOP, x1, plateTop, c[1]);
            solid(x0, plateTop, x1, bottom, c[2]);
            Portrait& p = s_portrait[side];
            if (p.image) {
                static u8 texObj[0x20] __attribute__((aligned(32)));
                static u8 tlutObj[0xC] __attribute__((aligned(4)));
                GX_INIT_TLUT_OBJ(tlutObj, p.palette, 2 /* RGB5A3 */, p.paletteEntries);
                GX_LOAD_TLUT(tlutObj, 0 /* TLUT0 */);
                GX_INIT_TEX_OBJ_CI(texObj, p.image, TEX_W, TEX_H, 9 /* C8 */, 0 /* CLAMP */, 0, 0, 0 /* TLUT0 */);
                GX_LOAD_TEX_OBJ(texObj, 0 /* TEXMAP0 */);
                GX_SET_TEV_ORDER(0, 0 /* TEXCOORD0 */, 0 /* TEXMAP0 */, 4);
                GX_SET_TEV_OP(0, 0 /* MODULATE */);
                float px0 = CENTRE_X[side] - PORTRAIT_W * 0.5f;
                quad(px0, TOP + PAD, px0 + PORTRAIT_W, TOP + PAD + PORTRAIT_H, 0xFFFFFFFF, 0, 0, 1, 1);
            }
        }
        if (any) g_sssPortraits.drawn++;
        return any;
    }

    bool nameLine(int side, u16* out, int max, float* x, float* y, float* maxW)
    {
        if (!s_havePreview || side < 0 || side > 1) return false;
        const PPOM::GameStep::PreviewPlayer* pv = sidePlayer(side, NULL);
        if (!pv) return false;
        char name[PPOM::NAME_LEN + 1];
        PPOM::u16ToAscii(name, pv->name, sizeof(name));
        if (!name[0]) strcpy(name, pv->local ? "You" : "Opponent");
        int n = 0;
        for (; name[n] && n < max - 1; n++) {
            u8 ch = (u8)name[n];
            out[n] = ch < 0x20 ? (u16)' ' : ch;   // nothing the renderer reads as a command
        }
        out[n] = 0;
        float x0, x1, plateTop, bottom;
        panelRect(side, &x0, &x1, &plateTop, &bottom);
        *x = CENTRE_X[side];
        *y = plateTop + 3.0f;
        *maxW = x1 - x0 - 6.0f;
        return true;
    }
}
