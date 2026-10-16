// Ranked's stage select shows both players' fighters, each standing in its idle animation (Wait1)
// and facing the other, so a player sees the opponent's character before striking (the user's,
// 2026-10-15).
//
// The fighters cost no memory of their own. Between the CSS and the stage select Brawl already
// loads every player record's fighter (gmSelCharData) for the match into that slot's resource
// heap (Fighter<n>Resoruce, 5.3 MB each, empty on the menus): its module, Fit<Name>.pac,
// Fit<Name>MotionEtc.pac and the costume's Fit<Name><NN>.pac (measured on the ranked stage
// select: Fighter1Resoruce 94 % full, the others empty). online_match.cpp puts both players of
// Dolphin's preview (GameStep pv, by in-game port) into their slots first, so the opponent's
// fighter is loaded too.
//
// From those raw files (found by their format in the slot's heap):
//   - the costume's ARC: entry 0 the model's brres (3DModels "Fit<Name><NN>"), entry 1 its
//     textures' brres;
//   - the motion's brres (AnmChr and AnmVis "Wait1": the idle's bones and which meshes show,
//     so the model's spare hands and faces stay hidden as in a match).
// Each fighter is a MuObject, made as the stage select makes its own models (sel_stage
// text+0x1220: MuObject::create(ResFile*, name, priority, texture ResFile*, heap 0x2A)), whose
// ScnMdl goes into the stage select's scene group (task+0x50; its vtable +0x34 insert, as
// text+0x12C4 does). MuObject::changeAnimN looks animations up by name in the object's own file
// (+4): pointed at the motion's brres for that call, it binds Wait1's node, visibility and other
// animations to the model, then the model's file is put back. Placed with ScnObj::SetMtx (as
// MuObject::setPos, plus a scale). The objects live in the menu's heap (0x2A, MenuInstance) and
// are deleted in the stage select task's destructor (sel_stage text+0x368) before Brawl deletes
// its own: a block left in MenuInstance hangs the next scene change.
//
// Nothing here reaches a match: the stage select is not rolled back, and the session sets every
// machine's random seeds and frame counters from the host's at the match's start.
#include <OS/OSCache.h>
#include <string.h>

#include <mu/mu_msg.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "sss_fighters.h"

// Tests and tuning (harness, through PPOnlineMain.map). Side 0 is this player (left), 1 the
// opponent (right).
extern "C" struct SssFightersDebug {
    u32 preloads;        // stage selects whose preview went into gmSelCharData
    u32 made;            // fighters made
    u32 deleted;         // fighters deleted with the stage select
    u32 scans;           // heap scans
    u32 state[2];        // per side: 1 model found, 2 motion found, 4 made, 8 animated, 0x10 failed
    u32 model[2];        // per side: the model's brres
    u32 tex[2];          // per side: the textures' brres
    u32 motion[2];       // per side: the motion's brres
    u32 heap[2];         // per side: the resource heap (g_HeapInfos index) its files are in
    u32 obj[2];          // per side: the MuObject
    float pos[2][3];     // per side: where it stands (the stage select's units)
    float rot[2][3];     // per side: rotation (degrees)
    float scale[2];      // per side
    int priority;        // MuObject draw priority (read when made)
    u32 enabled;         // 0: make nothing (tests)
    u32 anim;            // 0 no animation, 1 the bones' only, 2 all of Wait1's
    u32 remake;          // nonzero: delete the fighters and make them again (cleared)
    u32 init;            // ResFile::Init the model and texture files first
    float label[2][4];   // per side: the name line's window (x1, y1, x2, y2; the stage select's
                         // rule-line node's units, as stage_legal.cpp moves that line)
    float labelScale;
    u32 labels;          // label windows made
    float fit[2];        // the largest a fighter may stand (width, height; the stage select's
                         // units, measured between its bones): a bigger one is scaled down
    float fitted[2];     // per side: the factor its size was scaled by (0: not measured yet)
    float bones[2][2];   // per side: its bones' extent (width, height) when measured
} g_sssFighters = {0, 0, 0, 0, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
                   {{-29.0f, -16.0f, 0.0f}, {29.0f, -16.0f, 0.0f}},
                   {{0.0f, 120.0f, 0.0f}, {0.0f, -120.0f, 0.0f}},
                   {0.8f, 0.8f}, 0x10, 1, 2, 0, 1,
                   {{-250.0f, 129.0f, 10.0f, 89.0f}, {744.0f, 158.0f, 1004.0f, 118.0f}}, 0.8f, 0,
                   {16.0f, 14.0f}, {0.0f, 0.0f}, {{0.0f, 0.0f}, {0.0f, 0.0f}}};

namespace SssFighters {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }
    static bool memcmp4(const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

    static const u32 HEAP_INFOS = 0x80494958;   // g_HeapInfos {name*, gfMemoryPool*, size, arena}
    static const int HEAP_FIGHTER1_RESOURCE = 18;
    static const int MENU_HEAP = 0x2A;          // MenuInstance
    static const char* const IDLE = "Wait1";

    typedef u8* (*CreateFn)(void* modelRes, const char* name, int priority, void* texRes, int heap);
    typedef void (*NameFn)(u8* obj, const char* name);
    typedef void (*LoopFn)(void* anim, bool loop);
    typedef void (*MtxIdentFn)(float* m);
    typedef void (*MtxSrtFn)(float* m, const float* scale, const float* rot, const float* trans);
    typedef void (*SetMtxFn)(void* scnObj, int type, const float* m);
    typedef void* (*DtorFn)(u8* task, int flags);
    static const CreateFn MU_CREATE = (CreateFn)0x800B2394;      // MuObject::create(ResFile*, const char*, ...)
    static const NameFn CHANGE_ANIM = (NameFn)0x800B534C;        // MuObject::changeAnimN
    static const NameFn CHANGE_NODE_ANIM = (NameFn)0x800B45A8;   // MuObject::changeNodeAnimN
    static const LoopFn SET_LOOP = (LoopFn)0x80028278;           // gfModelAnimation::setLoop
    static const MtxIdentFn MTX_IDENTITY = (MtxIdentFn)0x8003E35C;
    static const MtxSrtFn MTX_SRT = (MtxSrtFn)0x8003F0FC;        // as MuObject::setPos builds its matrix
    static const SetMtxFn SET_MTX = (SetMtxFn)0x801AAF04;        // ScnObj::SetMtx(MTX_LOCAL)
    typedef void (*ResInitFn)(void* resFile);
    static const ResInitFn RES_INIT = (ResInitFn)0x8018EA08;     // nw4r::g3d::ResFile::Init

    static PPOM::GameStep::PreviewPlayer s_pv[2];   // by in-game port, kept for this stage select
    static bool s_havePreview = false;
    static u8* s_task = NULL;       // the stage select the fighters belong to
    static u8* s_obj[2];            // by side
    static u32 s_modelRes[2], s_texRes[2];   // ResFile values (a brres pointer each), by side
    static int s_tries[2];
    static int s_wait[2];           // frames to the next try
    static float s_fit[2];          // by side: the size factor from measure (0: not measured)
    static int s_age[2];            // by side: frames since it was made
    static DtorFn s_origDtor = NULL;

    // The name lines: a message object of our own (as the stage select makes its own: MuMsg::create
    // (1, 0x2A, 0x2B), sel_stage text+0xC28), one window per side, attached as the stage select's
    // rule line (window 0) is: MuMsg::attachScnMdlSimple(window, the ScnMdl of task+0x80, text
    // node 0, 0.0625). The attach chains the message into that model's draw callback
    // (ScnMdlSimple+0xD4); the callback from before is put back before the message is deleted.
    typedef MuMsg* (*MsgCreateFn)(u32, u32, u32);
    typedef void (*MsgAllocFn)(MuMsg*, u32 size, u32 count);
    typedef void (*MsgAttachFn)(MuMsg*, u32 window, void* scnMdl, u32 textNode, float size);
    typedef void (*MsgInitWsFn)(MuMsg*, void* ws);
    typedef void (*MsgSetFaceFn)(MuMsg*, u32 window, u32 face);
    typedef void (*MsgDeleteFn)(MuMsg*, int);
    static MuMsg* s_msg = NULL;
    static u8* s_msgMdl = NULL;        // the model the windows are on
    static u32 s_msgOldCallback = 0, s_msgCallback = 0;
    static bool s_labelled[2];
    static float s_labelRect[2][4];    // the rects last set

    const PPOM::GameStep* previewStep()
    {
        if (OnlineMenu::roomCss()) return NULL;
        const PPOM::GameStep* st = OnlineMenu::rankedStep();
        return st && st->pvCount ? st : NULL;
    }

    bool validPreview(const PPOM::GameStep::PreviewPlayer& pv)
    {
        if (pv.present != 1 || !OnlineMenu::selectableCharKind(pv.charKind)) return false;
        return (pv.costume & PPOM::COSTUME_ALTS) != PPOM::COSTUME_ALTS;
    }

    void notePreload() { g_sssFighters.preloads++; }

    // ---- The fighters' files --------------------------------------------------------------------
    // Brawl's fighter slots (sora_melee .bss+0x2E90 g_ftSlotManager -> {ftSlot* slots, count},
    // 0x4D4 each; measured on the ranked stage select): +0x160 the gmCharacterKind (0xCCCCCCCC
    // unused), +4 1 while its loads run, +0x188 0x12 loading / 0x100 loaded. Slot order is not the
    // port order. The fighters are made only once every used slot is loaded: before that the files
    // are still arriving.
    static const u32 MOD_MELEE = 27;
    static const u32 SLOT_MANAGER = 0x2E90;
    static const u32 SLOT_SIZE = 0x4D4;

    static u32 moduleBss(u32 id)
    {
        u32 m = *(u32*)0x800030C8;
        for (int n = 0; isPtr(m) && n < 64; n++) {
            if (*(u32*)m == id) {
                u32 sec = *(u32*)(m + 0x10);
                return isPtr(sec) ? *(u32*)(sec + 6 * 8) & ~1u : 0;   // section 6 = .bss
            }
            m = *(u32*)(m + 4);
        }
        return 0;
    }

    // 1: every used slot is loaded and `kinds` are among them; 0: not yet; -1: no slot manager.
    static int slotsLoaded(const u8* kinds, int n)
    {
        u32 bss = moduleBss(MOD_MELEE);
        if (!isPtr(bss)) return -1;
        u32 mgr = *(u32*)(bss + SLOT_MANAGER);
        if (!isPtr(mgr)) return -1;
        u32 slots = *(u32*)mgr, count = *(u32*)(mgr + 4);
        if (!isPtr(slots) || count == 0 || count > 16) return -1;
        u32 seen = 0;
        for (u32 i = 0; i < count; i++) {
            const u8* s = (const u8*)(slots + SLOT_SIZE * i);
            u32 kind = *(const u32*)(s + 0x160);
            if (kind >= 0x40) continue;
            if (*(const u32*)(s + 4) != 0 || *(const u32*)(s + 0x188) != 0x100) return 0;
            for (int k = 0; k < n; k++) if (kinds[k] == kind) seen |= 1u << k;
        }
        return seen == (1u << n) - 1 ? 1 : 0;
    }

    // gmCharacterKind -> the name in its files (fighter/<name>/Fit<Name>...: P+ v3.2's roster, as
    // Dolphin's GameSetup CharName lists it). Ice Climbers: Popo's model (Nana is not shown).
    static const char* fileName(u8 kind)
    {
        static const char* const NAMES[0x36] = {
            "Mario", "Donkey", "Link", "Samus", "SZerosuit", "Yoshi", "Kirby", "Fox",
            "Pikachu", "Luigi", "Captain", "Ness", "Koopa", "Peach", "Zelda", "Sheik",
            "Popo", "Popo", NULL, "Marth", "GameWatch", "Falco", "Ganon", "Wario",
            "Metaknight", "Pit", "Pikmin", "Lucas", "Diddy", NULL, "PokeLizardon", NULL,
            "PokeZenigame", NULL, "PokeFushigisou", "Dedede", "Lucario", "Ike", "Robot", "Purin",
            "ToonLink", "Wolf", "Snake", "Sonic", "GKoopa", "WarioMan", NULL, NULL,
            NULL, NULL, "Roy", "Mewtwo", NULL, "Knuckles"};
        return kind < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[kind] : NULL;
    }

    // A brres (nw4r ResFile): its root dictionary at +0x10 + 8; a dictionary is {u32 size, u32 n,
    // then n + 1 entries of 0x10: u16 ref, u16 flag, u16 left, u16 right, s32 name, s32 data}, the
    // name and data offsets from the dictionary's start. Everything is checked to stay inside
    // [lo, hi): the heap holds whatever was loaded there before too.
    struct Range { u32 lo, hi; };

    static bool inRange(const Range& r, u32 p, u32 n) { return p >= r.lo && p + n <= r.hi && p + n >= p; }

    static const u8* dictEntry(const Range& r, const u8* dict, int i, const char** name)
    {
        u32 d = (u32)dict;
        if (!inRange(r, d, 8)) return NULL;
        u32 n = *(const u32*)(dict + 4);
        if (n > 4096 || i < 1 || (u32)i > n || !inRange(r, d + 8 + 0x10 * (u32)i, 0x10)) return NULL;
        const u8* e = dict + 8 + 0x10 * i;
        s32 nameOff = *(const s32*)(e + 8), dataOff = *(const s32*)(e + 12);
        if (!nameOff || !dataOff || !inRange(r, d + nameOff, 1) || !inRange(r, d + dataOff, 4)) return NULL;
        if (name) *name = (const char*)(dict + nameOff);
        return dict + dataOff;
    }

    static int dictCount(const Range& r, const u8* dict)
    {
        if (!inRange(r, (u32)dict, 8)) return 0;
        u32 n = *(const u32*)(dict + 4);
        return n > 4096 ? 0 : (int)n;
    }

    static bool sameName(const Range& r, const char* a, const char* b)
    {
        for (int i = 0; i < 64; i++) {
            if (!inRange(r, (u32)a + i, 1)) return false;
            if (a[i] != b[i]) return false;
            if (!b[i]) return true;
        }
        return false;
    }

    // The brres's dictionary of `group` (e.g. "3DModels(NW4R)"), or NULL.
    static const u8* bresGroup(const Range& r, const u8* bres, const char* group)
    {
        if (!inRange(r, (u32)bres, 0x20) || *(const u32*)bres != 0x62726573 /* bres */) return NULL;
        const u8* root = bres + 0x10;
        if (*(const u32*)root != 0x726F6F74 /* root */) return NULL;
        const u8* dict = root + 8;
        int n = dictCount(r, dict);
        for (int i = 1; i <= n; i++) {
            const char* nm;
            const u8* d = dictEntry(r, dict, i, &nm);
            if (d && sameName(r, nm, group)) return d;
        }
        return NULL;
    }

    static bool groupHas(const Range& r, const u8* group, const char* name)
    {
        int n = dictCount(r, group);
        for (int i = 1; i <= n; i++) {
            const char* nm;
            if (dictEntry(r, group, i, &nm) && sameName(r, nm, name)) return true;
        }
        return false;
    }

    // An ARC (Brawl's archive): "ARC\0", u16 version, u16 count, entries from +0x40, each a 0x20
    // header {u16 type, u16 index, u32 size, ...} and its data, 0x20-aligned.
    static const u8* arcEntry(const Range& r, const u8* arc, int i, u16* type)
    {
        if (!inRange(r, (u32)arc, 0x40) || *(const u32*)arc != 0x41524300 /* ARC\0 */) return NULL;
        u16 n = *(const u16*)(arc + 6);
        if (i >= n) return NULL;
        u32 off = 0x40;
        for (int k = 0; k <= i; k++) {
            if (!inRange(r, (u32)arc + off, 0x20)) return NULL;
            u32 size = *(const u32*)(arc + off + 4);
            if (size > r.hi - r.lo) return NULL;
            if (k == i) {
                if (type) *type = *(const u16*)(arc + off);
                return arc + off + 0x20;
            }
            off += 0x20 + ((size + 0x1F) & ~0x1Fu);
        }
        return NULL;
    }

    // A file the heap holds now, not one freed before: gfMemoryPool::alloc (0x80025C58) puts
    // 0xCCCCCCCC in the word before every block it hands out, free (0x8002632C) 0xDDDDDDDD.
    static bool liveBlock(const Range& r, u32 a) { return inRange(r, a - 4, 4) && *(const u32*)(a - 4) == 0xCCCCCCCC; }

    struct Found { u32 model, tex, motion; const char* name; };

    // A costume ARC: entry 0 a model brres whose first model is "Fit...", then its textures' brres
    // (the next entry; the Ice Climbers' second is Nana's model, which is not shown).
    static bool costumeArc(const Range& r, const u8* arc, Found* f)
    {
        u16 t0, t1 = 0;
        const u8* m = arcEntry(r, arc, 0, &t0);
        if (!m || t0 != 2) return false;
        const u8* t = NULL;
        for (int i = 1; i < 4 && !t; i++) {
            const u8* e = arcEntry(r, arc, i, &t1);
            if (!e) return false;
            if (t1 == 3) t = e;
        }
        if (!t) return false;
        const u8* models = bresGroup(r, m, "3DModels(NW4R)");
        if (!models || dictCount(r, models) < 1 || !bresGroup(r, t, "Textures(NW4R)")) return false;
        const char* nm;
        if (!dictEntry(r, models, 1, &nm) || !inRange(r, (u32)nm, 4) ||
            nm[0] != 'F' || nm[1] != 'i' || nm[2] != 't') return false;
        f->model = (u32)m;
        f->tex = (u32)t;
        f->name = nm;
        return true;
    }

    static bool motionBres(const Range& r, const u8* b)
    {
        const u8* chr = bresGroup(r, b, "AnmChr(NW4R)");
        return chr && dictCount(r, chr) > 20 && groupHas(r, chr, IDLE);
    }

    // The motions' brres: Fit<Name>MotionEtc.pac is an ARC of ARCs, one of which holds it;
    // Fit<Name>Motion.pac (Bowser's, with a FitKoopaEtc.pac of its own) holds it directly.
    static u32 motionArc(const Range& r, const u8* arc)
    {
        for (int i = 0; i < 4; i++) {
            u16 t;
            const u8* sub = arcEntry(r, arc, i, &t);
            if (!sub) return 0;
            if (motionBres(r, sub)) return (u32)sub;
            if (*(const u32*)sub != 0x41524300) continue;
            for (int k = 0; k < 4; k++) {
                const u8* b = arcEntry(r, sub, k, &t);
                if (!b) break;
                if (motionBres(r, b)) return (u32)b;
            }
        }
        return 0;
    }

    static bool startsWith(const char* model, const char* name)
    {
        for (int i = 0; name[i]; i++) if (model[3 + i] != name[i]) return false;
        return true;
    }

    // "Fit<name>" then anything (the costume's number, P+'s AltZ / AltR, Roy's "Standard"), but
    // not a longer roster name's (Wario's files are not WarioMan's).
    static bool modelOf(const char* model, const char* name)
    {
        if (!name || !startsWith(model, name)) return false;
        int len = 0;
        while (name[len]) len++;
        for (u8 k = 0; k < 0x40; k++) {
            const char* other = fileName(k);
            if (!other || other == name) continue;
            int ol = 0;
            while (other[ol]) ol++;
            if (ol > len && startsWith(model, other)) return false;
        }
        return true;
    }

    // One fighter's live costume and motion in a resource heap (a slot's costume and motion load
    // into the same Fighter<n>Resoruce; measured), its model named for `name`.
    static bool scanHeap(int heap, const char* name, Found* f)
    {
        g_sssFighters.scans++;
        const u8* info = (const u8*)(HEAP_INFOS + 16 * heap);
        u32 pool = *(const u32*)(info + 4), size = *(const u32*)(info + 8);
        if (!isPtr(pool) || size < 0x10000 || size > 0x1000000) return false;
        Range r = {pool, pool + size};
        f->model = f->tex = f->motion = 0;
        for (u32 a = (pool + 0x3F) & ~0x1Fu; a + 0x40 <= r.hi; a += 0x20) {
            if (*(const u32*)a != 0x41524300 /* ARC\0 */ || !liveBlock(r, a)) continue;
            Found c;
            if (!f->model && costumeArc(r, (const u8*)a, &c)) {
                if (!modelOf(c.name, name)) return false;   // another fighter's heap
                f->model = c.model;
                f->tex = c.tex;
                f->name = c.name;
            } else if (!f->motion) {
                f->motion = motionArc(r, (const u8*)a);
            }
            if (f->model && f->motion) return true;
        }
        return false;
    }

    // ---- The models ---------------------------------------------------------------------------
    static u8* stageSelectTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        u32 scene = isPtr(mgr) ? *(u32*)(mgr + 4) : 0;
        u32 task = isPtr(scene) ? *(u32*)(scene + 0x3AC) : 0;
        return isPtr(task) ? (u8*)task : NULL;
    }

    static void place(int side)
    {
        u8* obj = s_obj[side];
        float m[12];
        float k = g_sssFighters.scale[side] * (s_fit[side] > 0.0f ? s_fit[side] : 1.0f);
        float sc[3] = {k, k, k};
        MTX_IDENTITY(m);
        MTX_SRT(m, sc, g_sssFighters.rot[side], g_sssFighters.pos[side]);
        SET_MTX(*(void**)(obj + 0xC), 0, m);
        // As MuObject::setPos: the model's G3dProc 2 and 5 (its world and view matrices again).
        u8* scn = *(u8**)(obj + 0x10);
        if (!isPtr((u32)scn)) return;
        typedef void (*ProcFn)(u8*, int, int);
        ((ProcFn)(*(u32*)(*(u32*)scn + 0x20)))(scn, 2, 0);
        ((ProcFn)(*(u32*)(*(u32*)scn + 0x20)))(scn, 5, 0);
    }

    static int s_heap[2] = {-1, -1};   // by side: the resource heap its files are in (-1 none yet)

    // Fighters keep their sizes in a match (Jigglypuff is small, Bowser big), and a big one would
    // cover the stage icons: a few frames after it is made (its bones posed by Wait1), the extent of
    // its bones' world positions (ScnMdl+0xEC, one 3x4 matrix per node, in the stage select's
    // units with our placement) is measured, and one bigger than `fit` is scaled down to it.
    typedef void (*SetVisFn)(void* scnMdl, bool vis);
    static const SetVisFn SET_VIS = (SetVisFn)0x80043D20;            // nwSMSetVisibility
    typedef u32 (*NodeCountFn)(void* resMdl);
    static const NodeCountFn NODE_COUNT = (NodeCountFn)0x8018EE4C;   // ResMdl::GetResNodeNumEntries
    static void measure(int side)
    {
        u8* scn = *(u8**)(s_obj[side] + 0x10);
        if (!isPtr((u32)scn)) return;
        const float* mtx = *(const float**)(scn + 0xEC);
        u32 n = NODE_COUNT(scn + 0xE8);
        SET_VIS(*(void**)(s_obj[side] + 0xC), true);   // hidden until measured (make)
        if (!isPtr((u32)mtx) || n < 2 || n > 1024) {
            s_fit[side] = 1.0f;
            return;
        }
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (u32 i = 0; i < n; i++) {
            float x = mtx[12 * i + 3], y = mtx[12 * i + 7];
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        float w = x1 - x0, h = y1 - y0, f = 1.0f;
        if (w > g_sssFighters.fit[0]) f = g_sssFighters.fit[0] / w;
        if (h * f > g_sssFighters.fit[1]) f = g_sssFighters.fit[1] / h;
        s_fit[side] = f;
        g_sssFighters.fitted[side] = f;
        g_sssFighters.bones[side][0] = w;
        g_sssFighters.bones[side][1] = h;
    }

    static void make(u8* task, int side, u8 kind)
    {
        const char* name = fileName(kind);
        Found f;
        int heap = -1;
        for (int h = HEAP_FIGHTER1_RESOURCE; h < HEAP_FIGHTER1_RESOURCE + 4 && heap < 0; h++) {
            if (h == s_heap[1 - side]) continue;   // a mirror match: the other one's
            if (scanHeap(h, name, &f)) heap = h;
        }
        if (heap < 0) return;
        s_heap[side] = heap;
        s_fit[side] = 0.0f;
        s_age[side] = 0;
        g_sssFighters.state[side] |= 3;
        g_sssFighters.model[side] = f.model;
        g_sssFighters.tex[side] = f.tex;
        g_sssFighters.motion[side] = f.motion;
        g_sssFighters.heap[side] = (u32)heap;
        s_modelRes[side] = f.model;
        s_texRes[side] = f.tex;
        // As a fighter's load does before its model is made: the shapes' display lists get the
        // vertex arrays' addresses (the preload only read the file). The same addresses again
        // when the match makes the fighter from this file.
        if (g_sssFighters.init) {
            RES_INIT(&s_texRes[side]);
            RES_INIT(&s_modelRes[side]);
        }
        u8* obj = MU_CREATE(&s_modelRes[side], f.name, g_sssFighters.priority, &s_texRes[side], MENU_HEAP);
        if (!isPtr((u32)obj)) {
            g_sssFighters.state[side] |= 0x10;
            return;
        }
        s_obj[side] = obj;
        g_sssFighters.obj[side] = (u32)obj;
        g_sssFighters.state[side] |= 4;
        g_sssFighters.made++;
        // The idle from the motion's brres: MuObject looks it up in its own file (+4).
        u32 own = *(u32*)(obj + 4);
        *(u32*)(obj + 4) = f.motion;
        if (g_sssFighters.anim == 1) CHANGE_NODE_ANIM(obj, IDLE);
        else if (g_sssFighters.anim == 2) CHANGE_ANIM(obj, IDLE);
        *(u32*)(obj + 4) = own;
        void* anim = *(void**)(obj + 0x14);
        if (isPtr((u32)anim)) {
            SET_LOOP(anim, true);
            g_sssFighters.state[side] |= 8;
        }
        // Into the stage select's scene group (task+0x50), at its end, as the task adds its own.
        u8* group = *(u8**)(task + 0x50);
        u8* scn = *(u8**)(obj + 0x10);
        if (isPtr((u32)group) && isPtr((u32)scn)) {
            typedef bool (*InsertFn)(u8*, u32, u8*);
            ((InsertFn)(*(u32*)(*(u32*)group + 0x34)))(group, *(u32*)(group + 0xE4), scn);
        }
        SET_VIS(*(void**)(obj + 0xC), false);   // until measure has its size
        place(side);
    }

    static void deleteLabels()
    {
        if (!s_msg) return;
        if (s_msgMdl && *(u32*)(s_msgMdl + 0xD4) == s_msgCallback) *(u32*)(s_msgMdl + 0xD4) = s_msgOldCallback;
        ((MsgDeleteFn)0x800B8A0C)(s_msg, 1);
        s_msg = NULL;
        s_msgMdl = NULL;
        s_labelled[0] = s_labelled[1] = false;
    }

    static void placeLabel(int side)
    {
        u8* ws = *(u8**)((u8*)s_msg + 0xC) + 0x48 * side;
        float* rect = (float*)(ws + 4);
        const float* want = g_sssFighters.label[side];
        if (s_labelled[side] && memcmp4(s_labelRect[side], want)) return;
        for (int k = 0; k < 4; k++) rect[k] = s_labelRect[side][k] = want[k];
        float* sc = (float*)(ws + 0x30);
        sc[0] = sc[1] = g_sssFighters.labelScale;
    }

    static void label(u8* task, int side, const PPOM::GameStep::PreviewPlayer& pv)
    {
        if (!s_msg) {
            u8* host = *(u8**)(task + 0x80);
            u8* mdl = isPtr((u32)host) ? *(u8**)(host + 0x10) : NULL;
            if (!isPtr((u32)mdl)) return;
            MuMsg* m = ((MsgCreateFn)0x800B8930)(1, MENU_HEAP, 0x2B);
            if (!m) return;
            ((MsgAllocFn)0x800B8B08)(m, 0x80, 2);
            s_msgMdl = mdl;
            s_msgOldCallback = *(u32*)(mdl + 0xD4);
            for (u32 w = 0; w < 2; w++) {
                ((MsgAttachFn)0x800B8C90)(m, w, mdl, 0, 0.0625f);
                ((MsgInitWsFn)0x800B8BE0)(m, *(u8**)((u8*)m + 0xC) + 0x48 * w);
                ((MsgSetFaceFn)0x800B9488)(m, w, 1);
                m->setAlignMode(w, MuMsg::Align_Center);
                m->setFontColor(w, 0xFF, 0xFF, 0xFF, 0xFF);
            }
            s_msgCallback = *(u32*)(mdl + 0xD4);
            s_msg = m;
        }
        placeLabel(side);
        if (s_labelled[side]) return;
        char name[PPOM::NAME_LEN + 1];
        PPOM::u16ToAscii(name, pv.name, sizeof(name));
        if (!name[0]) strcpy(name, pv.local ? "You" : "Opponent");
        // The menu font has no ASCII parentheses: the full-width ones (as code_entry.cpp's '#').
        if (pv.rating >= 0.0f && pv.rating < 100000.0f)
            s_msg->printf(side, "%s \xEF\xBC\x88%d\xEF\xBC\x89", name, (int)(pv.rating + 0.5f));
        else s_msg->printf(side, "%s", name);
        s_labelled[side] = true;
        g_sssFighters.labels++;
    }

    static void deleteFighters()
    {
        for (int side = 0; side < 2; side++) {
            u8* obj = s_obj[side];
            if (!obj) continue;
            typedef void (*DelFn)(u8*, int);
            ((DelFn)(*(u32*)(*(u32*)(obj + 0x5C) + 8)))(obj, 1);
            s_obj[side] = NULL;
            g_sssFighters.obj[side] = 0;
            g_sssFighters.deleted++;
        }
    }

    static void forget()
    {
        s_msg = NULL;   // (went with the stage select: hkTaskDtor)
        s_msgMdl = NULL;
        s_labelled[0] = s_labelled[1] = false;
        s_task = NULL;
        s_obj[0] = s_obj[1] = NULL;
        s_tries[0] = s_tries[1] = 0;
        s_wait[0] = s_wait[1] = 0;
        s_heap[0] = s_heap[1] = -1;
        s_havePreview = false;
    }

    void tick()
    {
        if (!Online::inScene("scSelStage")) {
            if (s_task || s_havePreview) forget();
            return;
        }
        if (!s_havePreview) {
            const PPOM::GameStep* st = previewStep();
            if (!st) return;
            memcpy(s_pv, st->pv, sizeof(s_pv));
            s_havePreview = true;
            g_sssFighters.state[0] = g_sssFighters.state[1] = 0;
        }
        u8* task = stageSelectTask();
        if (!task) return;
        if (task != s_task) {
            s_task = task;
            s_obj[0] = s_obj[1] = NULL;
        }
        if (g_sssFighters.remake) {
            g_sssFighters.remake = 0;
            deleteFighters();
            deleteLabels();
            s_heap[0] = s_heap[1] = -1;
            s_tries[0] = s_tries[1] = 0;
            s_wait[0] = s_wait[1] = 0;
        }
        // Brawl loads the fighters while the stage select runs (online_match.cpp preloadPreview):
        // nothing is made until every slot is loaded. Then a few tries, half a second apart.
        u8 kinds[2];
        int nKinds = 0;
        for (int port = 0; port < 2; port++) {
            if (validPreview(s_pv[port])) kinds[nKinds++] = s_pv[port].charKind;
        }
        bool loaded = nKinds && slotsLoaded(kinds, nKinds) == 1;
        for (int port = 0; port < 2; port++) {
            const PPOM::GameStep::PreviewPlayer& pv = s_pv[port];
            if (!validPreview(pv)) continue;
            int side = pv.local ? 0 : 1;
            label(task, side, pv);
            if (!s_obj[side]) {
                if (!g_sssFighters.enabled || !loaded || s_tries[side] >= 5) continue;
                if (s_wait[side] > 0) {
                    s_wait[side]--;
                    continue;
                }
                s_tries[side]++;
                s_wait[side] = 30;
                make(task, side, pv.charKind);
                continue;
            }
            if (s_fit[side] == 0.0f && ++s_age[side] >= 3) measure(side);
            place(side);
        }
    }

    // The stage select task's destructor (sel_stage text+0x368): ours first, as the task deletes
    // its own (each MuObject: vtable at +0x5C, entry +8, with 1 = delete), while its scene group
    // (+0x50) is still there.
    static void* hkTaskDtor(u8* task, int flags)
    {
        if (task && task == s_task) {
            deleteFighters();
            deleteLabels();
            s_task = NULL;
        }
        return s_origDtor(task, flags);
    }

    void install(CoreApi* api)
    {
        api->syReplaceFuncRel(0x368, reinterpret_cast<void*>(hkTaskDtor), (void**)&s_origDtor, 11 /* sora_menu_sel_stage */);
    }
}
