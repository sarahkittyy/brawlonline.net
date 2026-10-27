// The rules menu's L / R page hints (page_hints.h; docs/design/special-rules.md §5).
//
// The art: the pause screen's button atlas (info archive, TEX0 InfPauseButton00, C8 120x132 with
// its PLT0), which has GameCube L and R and a red arrow. The info archive is loaded in the
// InfoResource heap (heap 9) on the room CSS; its TEX0 and PLT0 are found there by name once per
// menu, and the three pictures converted to RGB5A3 (a Fighter1Resoruce block, freed with the menu).
//
// The drawing: a scene-object callback (nw4r IScnObjCallback; ScnObj +0xD4 the callback, +0xD8
// its timing, +0xDA the operations) on one of the menu's models, whose world matrix is identity:
// its view matrix (ScnObj +0x6C) is the menu camera's, so the quads are in the menu's own units
// (the MORE RULES rows are about x -29..29). As ms::Message's draw callback (0x8006B9BC) does: at
// timing C the textured 2D setup (gfDrawSetVtxPosColorTexPrimEnvironment: pos f32, colour RGBA8,
// texture f32 in VTXFMT1), the matrix (GXLoadPosMtxImm, GXSetCurrentMtx), the quads, and g3d's
// state invalidated (0x801A12AC(0x7FF)) so the models after it load theirs again. The screen's
// edges come from the camera's orthographic projection (GX's shadow copy, __GXData +0x52C): the
// buttons sit between the menu and the screen's edge at 16:9 and 4:3 alike.
#include "page_hints.h"

#include <OS/OSCache.h>
#include <string.h>

#include "special_box.h"

namespace PageHints {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    typedef void* (*AllocFn)(int heap, u32 size);
    typedef void (*FreeFn)(void* p);
    static const AllocFn HEAP_ALLOC = (AllocFn)0x800249E4;   // gfHeapManager::alloc
    static const FreeFn HEAP_FREE = (FreeFn)0x80024A4C;
    static const int HEAP_FIGHTER1 = 18;
    static const u32 HEAP_INFOS = 0x80494958;   // g_HeapInfos: {name, start, size, arena}, 0x10 each
    static const int HEAP_INFO_RESOURCE = 9;

    typedef void (*VoidFn)();
    typedef void (*LoadMtxFn)(const float* mtx, u32 id);
    typedef void (*U32Fn)(u32);
    typedef void (*InitTexFn)(void* obj, void* image, u16 w, u16 h, u32 fmt, u32 s, u32 t, u8 mip);
    typedef void (*InitLodFn)(void* obj, u32 minF, u32 magF, float minLod, float maxLod, float bias, u8 clamp,
                              u8 edge, u32 aniso);
    typedef void (*LoadTexFn)(void* obj, u32 map);
    typedef void (*BeginFn)(u32 prim, u32 fmt, u16 n);
    static const VoidFn SETUP_2D_TEX = (VoidFn)0x8001A7AC;   // gfDrawSetVtxPosColorTexPrimEnvironment
    static const LoadMtxFn LOAD_POS_MTX = (LoadMtxFn)0x801F51DC;   // GXLoadPosMtxImm
    static const U32Fn SET_CURRENT_MTX = (U32Fn)0x801F52E4;        // GXSetCurrentMtx
    static const U32Fn SET_CULL = (U32Fn)0x801F136C;               // GXSetCullMode
    static const InitTexFn INIT_TEX = (InitTexFn)0x801F28F0;       // GXInitTexObj
    static const InitLodFn INIT_LOD = (InitLodFn)0x801F2B44;       // GXInitTexObjLOD
    static const LoadTexFn LOAD_TEX = (LoadTexFn)0x801F2FB0;       // GXLoadTexObj
    static const BeginFn GX_BEGIN = (BeginFn)0x801F1088;           // GXBegin
    static const U32Fn G3D_INVALIDATE = (U32Fn)0x801A12AC;         // nw4r::g3d::G3DState::Invalidate

    static volatile u32* const WGPIPE = (volatile u32*)0xCC008000;
    static inline void putF(float f) { *(volatile float*)WGPIPE = f; }
    static inline void putU(u32 v) { *WGPIPE = v; }

    // The pictures in the atlas (texels): L, R, the arrow (pointing up).
    struct Pic {
        u16 x, y, w, h;   // its box in the atlas
        u16 tw, th;       // our texture's size (4-texel tiles)
    };
    static const Pic PICS[3] = {{70, 46, 47, 31, 48, 32}, {71, 80, 47, 30, 48, 32}, {1, 1, 14, 14, 16, 16}};
    enum { PIC_L = 0, PIC_R = 1, PIC_ARROW = 2 };

    // The models the callback goes on: every MuObject the rules task keeps (+0x40..+0x2B0: RULES'
    // and MORE RULES' rows, values and frames), those of its two pages (+0x268 RULES, +0x26C MORE RULES:
    // their first 0x200 bytes) and the Special Versus box's rows, looked for every frame (the pages
    // make theirs when they show); whichever of them is drawn draws the hints, once a frame.
    static const u32 HOSTS_FROM = 0x40, HOSTS_TO = 0x2B0;
    static const u32 RULES_PAGE1 = 0x268, RULES_PAGE2 = 0x26C, PAGE_SCAN = 0x200;
    static const int MAX_HOSTS = 200;

    struct Callback {
        void* const* vtable;
    };

    static struct {
        u8* block;           // the three RGB5A3 pictures
        u8* tex[3];
        bool looked;         // the atlas was looked for (this menu)
        u8* scn[MAX_HOSTS];  // the models the callback is on
        u8 oldFlags[MAX_HOSTS][4];
        int hosts;
        u32 scnVtable;

        bool left, right;
        u32 frame, drawnFrame;
        bool opened;
    } s;

    extern "C" {
        // Tests and tuning: drawn (count), the host model, where the buttons went.
        struct PageHintsDebug {
            u32 drawn;
            u32 scn;
            u32 found;       // 1 the atlas, 2 its palette too, 3 the pictures made
            float x[2], y;
            float edge[2];
            float size;
            u32 calls;       // the callback's (any operation)
            u8 left, right, hosts, projType;
            float proj[2];
        };
        PageHintsDebug g_pageHints;
        float g_pageHintsY = 3.0f;   // the buttons' centre height (menu units)
    }

    // ---- The pictures ---------------------------------------------------------------------------
    static u8* findInHeap(u32 magic, const char* name)
    {
        const u8* hi = (const u8*)(HEAP_INFOS + 0x10 * HEAP_INFO_RESOURCE);
        u8* base = *(u8**)(hi + 4);
        u32 size = *(const u32*)(hi + 8);
        if (!isPtr((u32)base) || size == 0 || size > 0x400000) return NULL;
        u32 n = (u32)strlen(name);
        for (u32 o = 0; o + 0x40 < size; o += 4) {
            u8* p = base + o;
            if (*(u32*)p != magic) continue;
            s32 noff = *(s32*)(p + 0x14);
            if (noff <= 0 || o + noff + n + 1 > size) continue;
            u32 k = 0;
            while (k <= n && p[noff + k] == (u8)name[k]) k++;   // (the DOL has no memcmp)
            if (k > n) return p;
        }
        return NULL;
    }

    static u16 toRgb5a3(u32 fmt, u16 v)
    {
        if (fmt == 2) return v;   // RGB5A3 already
        if (fmt == 1) {           // RGB565: opaque
            return (u16)(0x8000 | ((v >> 11) << 10) | (((v >> 6) & 31) << 5) | (v & 31));
        }
        u8 a = (u8)(v >> 8), i = (u8)v;   // IA8
        if (a >= 0xE0) return (u16)(0x8000 | ((i >> 3) << 10) | ((i >> 3) << 5) | (i >> 3));
        return (u16)(((a >> 5) << 12) | ((i >> 4) << 8) | ((i >> 4) << 4) | (i >> 4));
    }

    static bool makePictures()
    {
        if (s.looked) return s.block != NULL;
        s.looked = true;
        u8* tex = findInHeap(0x54455830, "InfPauseButton00");   // 'TEX0'
        if (!tex) return false;
        g_pageHints.found = 1;
        u8* plt = findInHeap(0x504C5430, "InfPauseButton00");   // 'PLT0'
        if (!plt) return false;
        g_pageHints.found = 2;
        u16 w = *(u16*)(tex + 0x1C), h = *(u16*)(tex + 0x1E);
        if (*(u32*)(tex + 0x20) != 9 || w != 120 || h != 132) return false;   // C8, the size P+ ships
        const u8* img = tex + *(u32*)(tex + 0x10);
        u32 pfmt = *(u32*)(plt + 0x18);
        u16 pcount = *(u16*)(plt + 0x1C);
        const u16* pal = (const u16*)(plt + *(u32*)(plt + 0x10));
        u32 need = 0;
        for (int k = 0; k < 3; k++) need += (u32)PICS[k].tw * PICS[k].th * 2;
        s.block = (u8*)HEAP_ALLOC(HEAP_FIGHTER1, need + 32);
        if (!s.block) return false;
        u8* out = (u8*)(((u32)s.block + 31) & ~31u);
        const int wb = (w + 7) / 8;
        for (int k = 0; k < 3; k++) {
            const Pic& P = PICS[k];
            s.tex[k] = out;
            for (int y = 0; y < P.th; y++) {
                for (int x = 0; x < P.tw; x++) {
                    u16 c = 0;   // transparent
                    if (x < P.w && y < P.h) {
                        int sx = P.x + x, sy = P.y + y;
                        u8 idx = img[((sy / 4) * wb + sx / 8) * 32 + (sy % 4) * 8 + sx % 8];   // C8: 8x4 tiles
                        if (idx < pcount) c = toRgb5a3(pfmt, pal[idx]);
                    }
                    *(u16*)(out + ((y / 4) * (P.tw / 4) + x / 4) * 32 + ((y % 4) * 4 + x % 4) * 2) = c;   // 4x4 tiles
                }
            }
            out += (u32)P.tw * P.th * 2;
        }
        DCFlushRange(s.block, need + 32);
        g_pageHints.found = 3;
        return true;
    }

    // ---- The drawing ----------------------------------------------------------------------------
    // A quad at (x0..x1, y0..y1) in menu units, white with `alpha`; `turn` the picture's quarter
    // turns (0 as it is, 1 its top to the left, 3 its top to the right).
    static void quad(int k, float x0, float y0, float x1, float y1, u8 alpha, int turn)
    {
        u32 obj[8];
        INIT_TEX(obj, s.tex[k], PICS[k].tw, PICS[k].th, 5 /* RGB5A3 */, 0, 0, 0);
        INIT_LOD(obj, 1, 1, 0.0f, 0.0f, 0.0f, 0, 0, 0);
        LOAD_TEX(obj, 0);
        float u1 = (float)PICS[k].w / PICS[k].tw, v1 = (float)PICS[k].h / PICS[k].th;
        // corners: top left, top right, bottom right, bottom left (y up)
        float px[4] = {x0, x1, x1, x0}, py[4] = {y1, y1, y0, y0};
        float uu[4] = {0, u1, u1, 0}, vv[4] = {0, 0, v1, v1};
        u32 colour = 0xFFFFFF00u | alpha;
        GX_BEGIN(0x80 /* GX_QUADS */, 1 /* GX_VTXFMT1 */, 4);
        for (int i = 0; i < 4; i++) {
            int j = (i + turn) & 3;   // the texture's corner on this screen corner
            putF(px[i]);
            putF(py[i]);
            putF(0.0f);
            putU(colour);
            putF(uu[j]);
            putF(vv[j]);
        }
    }

    static void draw(u8* scn)
    {
        if (!s.opened || !s.block || s.drawnFrame == s.frame || (!s.left && !s.right)) return;
        s.drawnFrame = s.frame;
        // The screen's edges (the camera's orthographic projection, in its view space).
        u8* gx;
        asm volatile("lwz %0, -0x5b70(2)" : "=r"(gx));
        float left = -38.0f, right = 38.0f;
        const float* view = (const float*)(scn + 0x6C);
        if (isPtr((u32)gx)) {
            g_pageHints.projType = (u8)*(u32*)(gx + 0x528);
            g_pageHints.proj[0] = *(float*)(gx + 0x52C);
            g_pageHints.proj[1] = *(float*)(gx + 0x530);
        }
        // The menu's camera is a perspective one (GX_PERSPECTIVE, 0): at the models' depth (the view
        // matrix's z) the screen's half width is that depth / proj[0] (about 41 at 16:9, 31 at 4:3).
        if (isPtr((u32)gx)) {
            const float* proj = (const float*)(gx + 0x52C);
            float depth = -view[11];
            if (*(u32*)(gx + 0x528) == 0 && proj[0] > 0.001f && depth > 1.0f) {
                float half = depth / proj[0];
                left = -half - view[3];
                right = half - view[3];
            } else if (*(u32*)(gx + 0x528) == 1 && proj[0] > 0.001f) {
                left = (-1.0f - proj[1]) / proj[0] - view[3];
                right = (1.0f - proj[1]) / proj[0] - view[3];
            }
        }
        g_pageHints.edge[0] = left;
        g_pageHints.edge[1] = right;
        // Between the menu (about +-29.5) and the edge, as big as fits (P+'s L: 47x31 texels).
        const float MENU = 29.5f;
        float room = right - MENU;
        float size = room * 0.8f;
        if (size > 7.0f) size = 7.0f;
        if (size < 3.5f) size = 3.5f;
        float cx = MENU + room * 0.5f;
        if (cx + size * 0.5f > right - 0.3f) cx = right - 0.3f - size * 0.5f;
        // The pulse: up and back over a second.
        u32 t = s.frame % 60;
        float tri = (t < 30 ? (float)t : (float)(60 - t)) / 30.0f;
        float ease = tri * tri * (3.0f - 2.0f * tri);
        float sc = size * (1.0f + 0.08f * ease);
        u8 alpha = (u8)(175 + 80 * ease);
        float bw = sc, bh = sc * 31.0f / 47.0f, aw = sc * 0.42f;
        float y = g_pageHintsY;
        g_pageHints.size = sc;
        g_pageHints.y = y;

        SETUP_2D_TEX();
        SET_CULL(0);
        LOAD_POS_MTX(view, 0);
        SET_CURRENT_MTX(0);
        for (int side = 0; side < 2; side++) {
            if (side == 0 ? !s.left : !s.right) continue;
            float x = side == 0 ? -cx : cx;
            g_pageHints.x[side] = x;
            quad(side == 0 ? PIC_L : PIC_R, x - bw * 0.5f, y - bh * 0.5f, x + bw * 0.5f, y + bh * 0.5f, alpha, 0);
            float ay = y - bh * 0.5f - aw * 0.75f;
            quad(PIC_ARROW, x - aw * 0.5f, ay - aw * 0.5f, x + aw * 0.5f, ay + aw * 0.5f, alpha, side == 0 ? 1 : 3);
        }
        G3D_INVALIDATE(0x7FF);
        g_pageHints.drawn++;
    }

    // The callback (nw4r g3d IScnObjCallback): its draws at timing C (after the model's own).
    static void cbNop(Callback*, u32, u8*, u32, void*) {}
    static void cbDtor(Callback*, int) {}
    static void cbDraw(Callback*, u32 timing, u8* scn, u32, void*)
    {
        g_pageHints.calls++;
        if (timing == 4) draw(scn);
    }
    static void* const VTABLE[8] = {NULL, NULL, (void*)cbDtor, (void*)cbNop, (void*)cbNop, (void*)cbNop,
                                    (void*)cbDraw, (void*)cbDraw};
    static Callback s_callback = {VTABLE};

    // The callback onto `obj`'s model when it is a MuObject's (a ScnMdl, as the menu's models are)
    // with none yet (one with a MuMsg on it is not ours) and not ours already.
    static void attachTo(u8* obj, u8* rt)
    {
        if (!isPtr((u32)obj) || obj == rt || s.hosts >= MAX_HOSTS) return;
        u8* scn = *(u8**)(obj + 0x10);
        if (!isPtr((u32)scn) || scn == rt || *(u32*)scn != s.scnVtable || *(u32*)(scn + 0xD4) != 0) return;
        int i = s.hosts++;
        memcpy(s.oldFlags[i], scn + 0xD8, 4);
        *(Callback**)(scn + 0xD4) = &s_callback;
        scn[0xD8] = 0x04;                 // timing C
        scn[0xD9] = 0;                    // not deleted with the model
        *(u16*)(scn + 0xDA) = 0x003F;     // every operation (the draws come to cbDraw, the rest do nothing)
        s.scn[i] = scn;
        g_pageHints.scn = (u32)scn;
    }

    static void attach(u8* rt)
    {
        // (the class of the menu's models: the first row's)
        if (!s.scnVtable) {
            u8* row = *(u8**)(rt + 0x90);   // (MORE RULES' TIME LIMIT row)
            u8* scn = isPtr((u32)row) ? *(u8**)(row + 0x10) : NULL;
            if (!isPtr((u32)scn)) return;
            s.scnVtable = *(u32*)scn;
        }
        for (u32 off = HOSTS_FROM; off < HOSTS_TO; off += 4) attachTo(*(u8**)(rt + off), rt);
        const u32 pages[2] = {RULES_PAGE1, RULES_PAGE2};
        for (int p = 0; p < 2; p++) {
            u8* page = *(u8**)(rt + pages[p]);
            if (!isPtr((u32)page)) continue;
            for (u32 off = 0; off < PAGE_SCAN; off += 4) {
                u8* obj = *(u8**)(page + off);
                // (a MuObject: its ResMdl +8 an MDL0)
                if (isPtr((u32)obj) && isPtr(*(u32*)(obj + 8)) && **(u32**)(obj + 8) == 0x4D444C30) attachTo(obj, rt);
            }
        }
        for (int i = 1; i < 10; i++) attachTo(SpecialBox::object(i), rt);
    }

    static void detach(bool menuThere)
    {
        for (int i = 0; i < s.hosts; i++) {
            u8* scn = s.scn[i];
            if (scn && menuThere && *(Callback**)(scn + 0xD4) == &s_callback) {
                *(u32*)(scn + 0xD4) = 0;
                memcpy(scn + 0xD8, s.oldFlags[i], 4);
            }
            s.scn[i] = NULL;
        }
    }

    void open()
    {
        memset(&s, 0, sizeof(s));
        s.opened = true;
    }

    void tick(u8* rt, bool left, bool right)
    {
        if (!s.opened || !rt) return;
        s.frame++;
        s.left = left;
        s.right = right;
        g_pageHints.left = left;
        g_pageHints.right = right;
        if (!makePictures()) return;
        attach(rt);
        g_pageHints.hosts = (u8)s.hosts;
    }

    void close(bool menuThere)
    {
        if (!s.opened) return;
        detach(menuThere);
        if (s.block) HEAP_FREE(s.block);
        memset(&s, 0, sizeof(s));
    }
}
