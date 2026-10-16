// Ranked's stage select shows who plays whom (the user's, 2026-10-16: game 1's strikes start the
// moment the queue pops, so the opponent's character was never seen before striking). Left and
// right of the stage preview stands each player's card as the CSS shows it (the user's: "it
// should ideally use the same in-game elements"), in the colour of the port that player gets in
// the match, the account name on its plate; this player on the left. Games 2+ show the last
// game's characters. Ranked only.
//
// The players come with Dolphin's step (GameStep pv, by in-game port).
//
// The card is the CSS's own: the MuObjects muSelCharPlayerArea makes (area +0xB0..+0xD0, measured
// on a live CSS) from the main brres of P+'s /menu2/sc_selcharacter.pac, each given its
// "<model>__0" animations at the frames the CSS leaves them on:
//   Cbase4 (the striped base: VIS0 / CHR0 15, the end of its intro; CLR0 the port), Cplynm4 (the
//   name plate: PAT0 the port), Cmark4 (the series symbol: PAT0 fn_800AF6F0(stock) + 1, as
//   dispMarkKind; CLR0 the port), Cchar4 (the portrait: CLR0 0), Cchareff4 (CLR0 20), Cname4 (the
//   character's name: PAT0 CharNo * 10 + 1, as setCharPic), Centry4 (CLR0 0), Chuman4 (VIS0 2,
//   CHR0 1, PAT0 0, CLR0 0).
// The portrait goes onto Cchar4's materials Card06 and Card006_sdw as setCharPic puts it
// (sel_char text+0x14B68: MuObject fn_800B7224(obj, the material's index, "MenSelchrFaceB.%03d",
// the portrait file)), from P+'s /menu/common/char_bust_tex/MenSelchrFaceB<CharNo>0.brres (P+'s
// AsyncRSP plugin's files; CharNo = muMenu::getStockFrameID(exchangeMuSelchkind2MuStockchkind(
// selchkind)), the texture CharNo * 10 + 1 + the costume).
//
// The files are read through the game's file system (gfFileIOHandle::readRequest, asynchronous,
// so P+'s SD redirection applies and nothing stalls): the CSS file into Fighter4Resoruce, the
// portraits into Fighter3Resoruce (the two fighter heaps a 1v1 never uses; nothing of a match is
// loaded during its stage select), and the CSS file is decompressed into Fighter3Resoruce a
// slice per frame (CXReadUncompLZ, P+'s LZ). The cards are MuObjects in the stage select's scene
// group (as its own, heap MenuInstance), deleted in the stage select task's destructor before
// Brawl deletes its own, and every file freed then.
#include <OS/OSCache.h>
#include <string.h>

#include <mu/mu_msg.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "sss_portraits.h"

extern "C" int sprintf(char* buf, const char* fmt, ...);

// Tests and tuning (harness, through PPOnlineMain.map). Side 0 is this player (left).
extern "C" struct SssPortraitsDebug {
    u32 shown;           // stage selects the cards were made on
    u32 loads;           // files requested
    u32 failed;          // files, textures or objects missing
    u32 made;            // cards made
    u32 deleted;         // cards deleted with the stage select
    u32 freed;           // files freed
    u16 texNo[2];        // per side: the portrait shown (MenSelchrFaceB.<n>), 0 none
    u8 port[2];          // per side: the in-game port (its colour), 0xFF none
    u8 state;            // 0 idle, 1 reading, 2 decompressing, 3 ready, 4 shown
    u8 remake;           // nonzero: delete the cards and make them again (cleared)
    float pos[2][3];     // per side: where the card stands (the stage select's units)
    float scale;
    s32 plateFrame;      // Cplynm4's PAT0 frame (-1: the port's)
    u32 objs;            // the card objects alive
    u32 obj[2][8];       // per side: the card's MuObjects (PARTS order)
} g_sssPortraits = {0, 0, 0, 0, 0, 0, {0, 0}, {0xFF, 0xFF}, 0, 0,
                    {{-29.4f, 0.0f, 0.0f}, {30.0f, 0.0f, 0.0f}}, 0.92f,
                    -1, 0, {{0}, {0}}};

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

    // ---- The game's functions ---------------------------------------------------------------
    typedef int (*ExchangeFn)(int);
    static const ExchangeFn GM_TO_SELCH = (ExchangeFn)0x800AF708;     // exchangeGmCharacterKind2MuSelchkind
    static const ExchangeFn SELCH_TO_STOCK = (ExchangeFn)0x800AF82C;  // exchangeMuSelchkind2MuStockchkind
    static const ExchangeFn STOCK_FRAME = (ExchangeFn)0x800AF600;     // getStockFrameID
    static const ExchangeFn STOCK_MARK = (ExchangeFn)0x800AF6F0;      // the series symbol (dispMarkKind)

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
    static const int HEAP_FIGHTER3 = 20, HEAP_FIGHTER4 = 21, MENU_HEAP = 0x2A;

    typedef u32 (*UncompSizeFn)(const void* src);
    typedef void (*UncompInitFn)(void* ctx, void* dst);
    typedef s32 (*UncompReadFn)(void* ctx, const void* src, u32 len);
    static const UncompSizeFn CX_SIZE = (UncompSizeFn)0x80205FD8;          // CXGetUncompressedSize
    static const UncompInitFn CX_INIT_LZ = (UncompInitFn)0x80205554;       // CXInitUncompContextLZ
    static const UncompReadFn CX_READ_LZ = (UncompReadFn)0x80205860;       // CXReadUncompLZ: 0 done, <0 error

    typedef u8* (*CreateFn)(void* modelRes, const char* name, int priority, void* texRes, int heap);
    typedef void (*NameFn)(u8* obj, const char* name);
    typedef void (*FrameFn)(u8* obj, float frame);
    typedef void (*MtxIdentFn)(float* m);
    typedef void (*MtxSrtFn)(float* m, const float* scale, const float* rot, const float* trans);
    typedef void (*SetMtxFn)(void* scnObj, int type, const float* m);
    typedef void (*ResInitFn)(void* resFile);
    typedef u32 (*GetResMatFn)(void* resMdl, const char* name);
    typedef void (*SetTexFn)(u8* obj, u32 matIndex, const char* texName, void* texRes);
    typedef void (*SetVisFn)(void* scnObj, bool vis);
    typedef void* (*DtorFn)(u8* task, int flags);
    static const CreateFn MU_CREATE = (CreateFn)0x800B2394;            // MuObject::create(ResFile*, name, ...)
    static const NameFn CHANGE_ANIM = (NameFn)0x800B534C;              // MuObject::changeAnimN
    static const NameFn CHANGE_CLR_ANIM = (NameFn)0x800B50CC;          // MuObject::changeClrAnimN
    static const FrameFn SET_FRAME_VIS = (FrameFn)0x800B784C;          // MuObject::setFrameVisible (anim +0x08)
    static const FrameFn SET_FRAME_NODE = (FrameFn)0x800B7798;         // (anim +0x0C, CHR0)
    static const FrameFn SET_FRAME_TEX = (FrameFn)0x800B7900;          // MuObject::setFrameTex (anim +0x10, PAT0)
    static const FrameFn SET_FRAME_COL = (FrameFn)0x800B7A18;          // MuObject::setFrameMatCol (anim +0x18, CLR0)
    static const MtxIdentFn MTX_IDENTITY = (MtxIdentFn)0x8003E35C;
    static const MtxSrtFn MTX_SRT = (MtxSrtFn)0x8003F0FC;              // as MuObject::setPos builds its matrix
    static const SetMtxFn SET_MTX = (SetMtxFn)0x801AAF04;              // ScnObj::SetMtx(MTX_LOCAL)
    static const ResInitFn RES_INIT = (ResInitFn)0x8018EA08;           // nw4r::g3d::ResFile::Init
    static const GetResMatFn GET_RES_MAT = (GetResMatFn)0x8018F258;    // nw4r::g3d::ResMdl::GetResMat(name)
    static const SetTexFn SET_TEX = (SetTexFn)0x800B7224;              // the material's texture, as setCharPic
    static const SetVisFn SET_VIS = (SetVisFn)0x80043D20;              // nwSMSetVisibility

    // The names, as the CSS writes a name tag on a card's plate: muSelCharPlayerArea's message
    // (sel_char text+0x11524: MuMsg::create(3, MenuInstance, 0x2B), its window 0 attached to
    // Chuman4's ScnMdl, text node 0, size 0.0625, face 1), the window's own settings otherwise.
    // One window per card. An attach chains the message into the model's draw callback
    // (ScnMdlSimple+0xD4): put back before the message is deleted.
    typedef MuMsg* (*MsgCreateFn)(u32, u32, u32);
    typedef void (*MsgAllocFn)(MuMsg*, u32 size, u32 count);
    typedef void (*MsgAttachFn)(MuMsg*, u32 window, void* scnMdl, u32 textNode, float size);
    typedef void (*MsgSetFaceFn)(MuMsg*, u32 window, u32 face);
    typedef void (*MsgDeleteFn)(MuMsg*, int);
    static const MsgCreateFn MSG_CREATE = (MsgCreateFn)0x800B8930;
    static const MsgAllocFn MSG_ALLOC = (MsgAllocFn)0x800B8B08;
    static const MsgAttachFn MSG_ATTACH = (MsgAttachFn)0x800B8C90;      // attachScnMdlSimple
    static const MsgSetFaceFn MSG_SET_FACE = (MsgSetFaceFn)0x800B9488;
    static const MsgDeleteFn MSG_DELETE = (MsgDeleteFn)0x800B8A0C;
    static MuMsg* s_msg = NULL;
    static u8* s_msgMdl[2];
    static u32 s_msgOldCallback[2], s_msgCallback[2];

    // ---- Files ------------------------------------------------------------------------------
    struct Read {
        u32 handle;          // gfFileIOHandle: its gfFileIORequest*
        bool pending;
        u8* buf;             // the file once read (freed by us)
        u32 size;
    };
    static Read s_cssRead, s_picRead[2];
    static u8* s_css = NULL;         // the decompressed CSS archive
    static u32 s_cssSize = 0, s_cssDone = 0;
    static u8 s_ctx[0x40] __attribute__((aligned(8)));   // CXUncompContextLZ
    static u32 s_cssRes = 0;         // its main brres (a ResFile value)
    static u32 s_picRes[2];          // the portrait files (ResFile values)
    static u16 s_texNo[2];

    static void request(Read& r, const char* path, int heap)
    {
        r.handle = 0;
        r.buf = NULL;
        r.size = 0;
        r.pending = READ_REQUEST(&r.handle, path, heap, 0, 0);
        if (r.pending) g_sssPortraits.loads++;
        else g_sssPortraits.failed++;
    }

    static bool poll(Read& r)
    {
        if (!r.pending || !IS_READY(&r.handle)) return false;
        r.buf = (u8*)GET_BUFFER(&r.handle);
        r.size = (u32)GET_SIZE(&r.handle);
        RELEASE(&r.handle);
        r.handle = 0;
        r.pending = false;
        if (!isPtr((u32)r.buf) || r.size < 0x40) {
            r.buf = NULL;
            g_sssPortraits.failed++;
        }
        return true;
    }

    static void freeBuf(u8*& p)
    {
        if (!p) return;
        HEAP_FREE(p);
        p = NULL;
        g_sssPortraits.freed++;
    }

    // An ARC's entry (Brawl's archive: "ARC\0", u16 count at +6, entries from +0x40, a 0x20
    // header {u16 type, u16 index, u32 size} each, 0x20-aligned) that is a brres holding `model`.
    static bool bresHas(const u8* b, u32 size, const char* group, const char* name);
    static u32 arcBres(const u8* arc, u32 size, const char* model)
    {
        if (size < 0x40 || *(const u32*)arc != 0x41524300) return 0;
        u16 n = *(const u16*)(arc + 6);
        u32 off = 0x40;
        for (u16 i = 0; i < n && off + 0x20 <= size; i++) {
            u32 sz = *(const u32*)(arc + off + 4);
            const u8* e = arc + off + 0x20;
            if (sz > size - off - 0x20) return 0;
            if (*(const u32*)e == 0x62726573 /* bres */ && bresHas(e, sz, "3DModels(NW4R)", model)) return (u32)e;
            off += 0x20 + ((sz + 0x1F) & ~0x1Fu);
        }
        return 0;
    }

    // nw4r's dictionaries: the root's at +0x18 ("root" at +0x10), {u32 size, u32 n, then n + 1
    // entries of 0x10}, each entry's name and data offsets (+8, +12) from its dictionary's start.
    static bool bresHas(const u8* b, u32 size, const char* group, const char* name)
    {
        if (size < 0x40 || *(const u32*)(b + 0x10) != 0x726F6F74 /* root */) return false;
        const u8* root = b + 0x18;
        u32 n = *(const u32*)(root + 4);
        for (u32 i = 1; i <= n && i < 16; i++) {
            const u8* e = root + 8 + 0x10 * i;
            s32 no = *(const s32*)(e + 8), dof = *(const s32*)(e + 12);
            if (no <= 0 || dof <= 0 || (u32)(root - b) + (u32)dof + 8 > size) continue;
            if (strcmp((const char*)(root + no), group) != 0) continue;
            const u8* d = root + dof;
            u32 m = *(const u32*)(d + 4);
            for (u32 k = 1; k <= m && k < 1024; k++) {
                const u8* f = d + 8 + 0x10 * k;
                s32 fn = *(const s32*)(f + 8);
                if (fn > 0 && strcmp((const char*)(d + fn), name) == 0) return true;
            }
        }
        return false;
    }

    // ---- The cards --------------------------------------------------------------------------
    enum { F_NONE = -1, F_PORT = -2, F_MARK = -3, F_NAME = -4, F_PLATE = -5 };
    // portClr: the part's colour animation is "<model>__<port>" (its CLR0s __0..__3+), not __0.
    struct Part { const char* model; s16 vis, node, pat, clr; bool portClr; };
    static const Part PARTS[] = {
        {"MenSelchrCbase4_TopN", 15, 15, F_NONE, F_PORT, false},
        {"MenSelchrCplynm4_TopN", F_NONE, F_NONE, F_PLATE, F_NONE, false},
        {"MenSelchrCmark4_TopN", F_NONE, F_NONE, F_MARK, F_PORT, false},
        {"MenSelchrCchar4_TopN", F_NONE, F_NONE, F_NONE, 0, false},
        {"MenSelchrCchareff4_TopN", F_NONE, F_NONE, F_NONE, 20, false},
        {"MenSelchrCname4_TopN", F_NONE, F_NONE, F_NAME, F_NONE, false},
        {"MenSelchrCentry4_TopN", F_NONE, F_NONE, F_NONE, 0, true},
        {"MenSelchrChuman4_TopN", 2, 1, 0, 0, true},
    };
    static const int N_PARTS = sizeof(PARTS) / sizeof(PARTS[0]);
    static const int PORTRAIT_PART = 3;
    static u8* s_obj[2][N_PARTS];
    static u8* s_task = NULL;
    static DtorFn s_origDtor = NULL;

    static u8* stageSelectTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        u32 scene = isPtr(mgr) ? *(u32*)(mgr + 4) : 0;
        u32 task = isPtr(scene) ? *(u32*)(scene + 0x3AC) : 0;
        return isPtr(task) ? (u8*)task : NULL;
    }

    // A frame on one of the object's animations (anim +slot), held there: rate 0 (FrameCtrl at
    // +0x18 of nw4r's AnmObj: frame, rate, start, end).
    static void setFrame(u8* obj, u32 slot, FrameFn set, float frame)
    {
        u8* anim = *(u8**)(obj + 0x14);
        if (!isPtr((u32)anim)) return;
        u8* a = *(u8**)(anim + slot);
        if (!isPtr((u32)a)) return;
        set(obj, frame);
        *(float*)(a + 0x1C) = 0.0f;
    }

    static void place(int side)
    {
        float m[12];
        float k = g_sssPortraits.scale;
        float sc[3] = {k, k, k};
        float rot[3] = {0, 0, 0};
        MTX_IDENTITY(m);
        MTX_SRT(m, sc, rot, g_sssPortraits.pos[side]);
        for (int i = 0; i < N_PARTS; i++) {
            u8* obj = s_obj[side][i];
            if (!obj) continue;
            SET_MTX(*(void**)(obj + 0xC), 0, m);
            u8* scn = *(u8**)(obj + 0x10);
            if (!isPtr((u32)scn)) continue;
            typedef void (*ProcFn)(u8*, int, int);
            ((ProcFn)(*(u32*)(*(u32*)scn + 0x20)))(scn, 2, 0);
            ((ProcFn)(*(u32*)(*(u32*)scn + 0x20)))(scn, 5, 0);
        }
    }

    static const int PLATE_PART = 7;   // Chuman4

    static void writeName(int side, const PPOM::GameStep::PreviewPlayer& pv)
    {
        u8* plate = s_obj[side][PLATE_PART];
        u8* mdl = plate ? *(u8**)(plate + 0x10) : NULL;
        if (!isPtr((u32)mdl)) return;
        if (!s_msg) {
            s_msg = MSG_CREATE(3, MENU_HEAP, 0x2B);
            if (!s_msg) return;
            MSG_ALLOC(s_msg, 0x100, 2);
        }
        s_msgMdl[side] = mdl;
        s_msgOldCallback[side] = *(u32*)(mdl + 0xD4);
        MSG_ATTACH(s_msg, side, mdl, 0, 0.0625f);
        MSG_SET_FACE(s_msg, side, 1);
        s_msgCallback[side] = *(u32*)(mdl + 0xD4);
        char name[PPOM::NAME_LEN + 1];
        PPOM::u16ToAscii(name, pv.name, sizeof(name));
        if (!name[0]) strcpy(name, pv.local ? "You" : "Opponent");
        s_msg->printf(side, "%s", name);
    }

    static void deleteNames()
    {
        if (!s_msg) return;
        for (int side = 0; side < 2; side++) {
            u8* mdl = s_msgMdl[side];
            if (mdl && *(u32*)(mdl + 0xD4) == s_msgCallback[side]) *(u32*)(mdl + 0xD4) = s_msgOldCallback[side];
            s_msgMdl[side] = NULL;
        }
        MSG_DELETE(s_msg, 1);
        s_msg = NULL;
    }

    static void makeCard(u8* task, int side)
    {
        int port = -1;
        const PPOM::GameStep::PreviewPlayer* pv = sidePlayer(side, &port);
        if (!pv || !s_cssRes) return;
        int stock = SELCH_TO_STOCK(GM_TO_SELCH(pv->charKind));
        int charNo = STOCK_FRAME(stock);
        float portF = (float)(port + 1);
        float markF = (float)(STOCK_MARK(stock) + 1);
        float nameF = (float)(charNo * 10 + 1);
        float plateF = g_sssPortraits.plateFrame >= 0 ? (float)g_sssPortraits.plateFrame : portF;
        u8* group = *(u8**)(task + 0x50);
        for (int i = 0; i < N_PARTS; i++) {
            const Part& p = PARTS[i];
            u8* obj = MU_CREATE(&s_cssRes, p.model, 0x10, &s_cssRes, MENU_HEAP);
            if (!isPtr((u32)obj)) {
                g_sssPortraits.failed++;
                continue;
            }
            s_obj[side][i] = obj;
            g_sssPortraits.obj[side][i] = (u32)obj;
            g_sssPortraits.objs++;
            char anim[48];
            sprintf(anim, "%s__0", p.model);
            CHANGE_ANIM(obj, anim);
            if (p.portClr && port > 0) {
                sprintf(anim, "%s__%d", p.model, port);
                CHANGE_CLR_ANIM(obj, anim);
            }
            const s16 want[4] = {p.vis, p.node, p.pat, p.clr};
            const u32 slots[4] = {0x08, 0x0C, 0x10, 0x18};
            const FrameFn sets[4] = {SET_FRAME_VIS, SET_FRAME_NODE, SET_FRAME_TEX, SET_FRAME_COL};
            for (int k = 0; k < 4; k++) {
                if (want[k] == F_NONE) continue;
                float f = want[k] == F_PORT ? portF : want[k] == F_MARK ? markF : want[k] == F_NAME ? nameF
                        : want[k] == F_PLATE ? plateF : (float)want[k];
                setFrame(obj, slots[k], sets[k], f);
            }
            if (i == PORTRAIT_PART && s_picRes[side]) {
                char tex[24];
                sprintf(tex, "MenSelchrFaceB.%03d", s_texNo[side]);
                static const char* const MATS[2] = {"Card06", "Card006_sdw"};
                for (int m = 0; m < 2; m++) {
                    u32 mat = GET_RES_MAT(obj + 8, MATS[m]);
                    if (isPtr(mat)) SET_TEX(obj, *(u32*)(mat + 0xC), tex, &s_picRes[side]);
                }
            }
            u8* scn = *(u8**)(obj + 0x10);
            if (isPtr((u32)group) && isPtr((u32)scn)) {
                typedef bool (*InsertFn)(u8*, u32, u8*);
                ((InsertFn)(*(u32*)(*(u32*)group + 0x34)))(group, *(u32*)(group + 0xE4), scn);
            }
        }
        writeName(side, *pv);
        g_sssPortraits.made++;
        g_sssPortraits.port[side] = (u8)port;
        g_sssPortraits.texNo[side] = s_texNo[side];
        place(side);
    }

    static void deleteCards()
    {
        deleteNames();
        for (int side = 0; side < 2; side++) {
            bool any = false;
            for (int i = 0; i < N_PARTS; i++) {
                u8* obj = s_obj[side][i];
                if (!obj) continue;
                typedef void (*DelFn)(u8*, int);
                ((DelFn)(*(u32*)(*(u32*)(obj + 0x5C) + 8)))(obj, 1);
                s_obj[side][i] = NULL;
                g_sssPortraits.obj[side][i] = 0;
                g_sssPortraits.objs--;
                any = true;
            }
            if (any) g_sssPortraits.deleted++;
            g_sssPortraits.texNo[side] = 0;
            g_sssPortraits.port[side] = 0xFF;
        }
    }

    // Everything of this stage select's: the files (a read still out is freed when it lands).
    static void freeAll()
    {
        freeBuf(s_cssRead.buf);
        freeBuf(s_css);
        s_cssRes = 0;
        for (int side = 0; side < 2; side++) {
            freeBuf(s_picRead[side].buf);
            s_picRes[side] = 0;
        }
        g_sssPortraits.state = 0;
    }

    static void start()
    {
        g_sssPortraits.shown++;
        g_sssPortraits.state = 1;
        request(s_cssRead, "/menu2/sc_selcharacter.pac", HEAP_FIGHTER4);
        for (int side = 0; side < 2; side++) {
            int port = -1;
            const PPOM::GameStep::PreviewPlayer* pv = sidePlayer(side, &port);
            s_texNo[side] = 0;
            if (!pv) continue;
            int charNo = STOCK_FRAME(SELCH_TO_STOCK(GM_TO_SELCH(pv->charKind)));
            if (charNo < 0 || charNo > 99) continue;
            s_texNo[side] = (u16)(charNo * 10 + 1 + (pv->costume & PPOM::COSTUME_COLOUR));
            char path[64];
            sprintf(path, "/menu/common/char_bust_tex/MenSelchrFaceB%02d0.brres", charNo);
            request(s_picRead[side], path, HEAP_FIGHTER3);
        }
    }

    static const u32 SLICE = 0x10000;   // compressed bytes decompressed per frame

    void tick()
    {
        // Reads out land even after the stage select: freed at once then.
        bool inSss = Online::inScene("scSelStage");
        if (poll(s_cssRead) && (!inSss || !s_havePreview)) freeBuf(s_cssRead.buf);
        for (int side = 0; side < 2; side++) {
            if (poll(s_picRead[side]) && (!inSss || !s_havePreview)) freeBuf(s_picRead[side].buf);
        }
        if (!inSss) {
            if (s_havePreview) {
                s_havePreview = false;
                deleteCards();   // the destructor has run (nothing left), or never made
                freeAll();
            }
            return;
        }
        if (!s_havePreview) {
            const PPOM::GameStep* st = previewStep();
            if (!st) return;
            memcpy(s_pv, st->pv, sizeof(s_pv));
            s_havePreview = true;
            s_task = NULL;
            start();
        }
        u8* task = stageSelectTask();
        if (!task) return;
        if (g_sssPortraits.remake) {
            g_sssPortraits.remake = 0;
            deleteCards();
            if (g_sssPortraits.state == 4) g_sssPortraits.state = 3;
        }
        switch (g_sssPortraits.state) {
        case 1:   // the CSS file
            if (s_cssRead.pending) return;
            if (!s_cssRead.buf) {
                g_sssPortraits.state = 0;
                return;
            }
            if (*(u32*)s_cssRead.buf == 0x41524300 /* ARC: not compressed */) {
                s_css = s_cssRead.buf;
                s_cssRead.buf = NULL;
                s_cssSize = s_cssRead.size;
                s_cssDone = s_cssSize;
            } else {
                s_cssSize = CX_SIZE(s_cssRead.buf);
                s_css = s_cssSize > 0x40 && s_cssSize < 0x800000 ? (u8*)HEAP_ALLOC(HEAP_FIGHTER3, s_cssSize + 0x20) : NULL;
                if (!s_css) {
                    g_sssPortraits.failed++;
                    freeBuf(s_cssRead.buf);
                    g_sssPortraits.state = 0;
                    return;
                }
                CX_INIT_LZ(s_ctx, s_css);
                s_cssDone = 0;
            }
            g_sssPortraits.state = 2;
            return;
        case 2:   // decompressing, a slice a frame
            if (s_cssRead.buf) {
                u32 n = s_cssRead.size - s_cssDone < SLICE ? s_cssRead.size - s_cssDone : SLICE;
                s32 r = CX_READ_LZ(s_ctx, s_cssRead.buf + s_cssDone, n);
                s_cssDone += n;
                if (r > 0 && s_cssDone < s_cssRead.size) return;
                freeBuf(s_cssRead.buf);
                if (r < 0) {
                    g_sssPortraits.failed++;
                    freeAll();
                    return;
                }
                DCFlushRange(s_css, s_cssSize);
            }
            s_cssRes = arcBres(s_css, s_cssSize, "MenSelchrCbase4_TopN");
            if (!s_cssRes) {
                g_sssPortraits.failed++;
                freeAll();
                return;
            }
            RES_INIT(&s_cssRes);
            g_sssPortraits.state = 3;
            return;
        case 3:   // the portraits, then the cards
            for (int side = 0; side < 2; side++) {
                if (s_picRead[side].pending) return;
            }
            for (int side = 0; side < 2; side++) {
                s_picRes[side] = (u32)s_picRead[side].buf;
            }
            s_task = task;
            makeCard(task, 0);
            makeCard(task, 1);
            g_sssPortraits.state = 4;
            return;
        case 4:
            place(0);
            place(1);
            return;
        default:
            return;
        }
    }

    // The stage select task's destructor (sel_stage text+0x368): our cards first, as the task
    // deletes its own (each MuObject: vtable at +0x5C, entry +8, with 1 = delete), while its scene
    // group (+0x50) is still there; then the files.
    static void* hkTaskDtor(u8* task, int flags)
    {
        if (task && task == s_task) {
            deleteCards();
            freeAll();
            s_task = NULL;
        }
        return s_origDtor(task, flags);
    }

    void install(CoreApi* api)
    {
        api->syReplaceFuncRel(0x368, reinterpret_cast<void*>(hkTaskDtor), (void**)&s_origDtor, 11 /* sora_menu_sel_stage */);
    }
}
