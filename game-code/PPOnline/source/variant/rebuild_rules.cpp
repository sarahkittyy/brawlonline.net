// The room's rules menu made again (rules_rebuild.h; variant.h PPON_REBUILD_RULES: in the
// plugins built for a build that does not open Brawl's from its character select, empty in the
// others).
#include "variant.h"
#if PPON_REBUILD_RULES
#include <gf/gf_memory_pool.h>
#include <string.h>
#include <types.h>
#include "build.h"
#include "ppom.h"
#include "room_rules.h"
#include "rules_rebuild.h"

namespace RulesRebuild {
    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }
    static const u32 TASK_RULES = 0x62C, TASK_RULES_OPEN = 0x630;   // muSelCharTask
    static const u32 OPEN_RULES = 0x80692E9C;                        // sel_char text+0x105D8

    // Brawl's openRules (sel_char text+0x105D8) as P+ has it, for a build whose codeset replaces it:
    // REX's (Remix/Rules.asm, "CSS Rules goes to Main Menu Rules") makes it a scene change to the
    // main menu's rules screen and back, which would take the room's character select away. The
    // rules menu itself (sora_menu_rule, module 18, text at 0x806A3B24) is loaded with REX's
    // character select as with P+'s, at the same place, and so is everything openRules calls (the
    // first words below, compared with P+'s): the room's menu is made as P+ makes it, the same
    // calls in the same order (disassembled from P+'s, 0x80692E9C..0x80693018). Online only: the
    // build's own button keeps the build's own way.
    static bool canOpenRulesOurselves()
    {
        static const u32 FIRST_WORDS[][2] = {
            {0x806A3B24, 0x9421FF60},   // muRuleTask's constructor (module 18)
            {0x8069EECC, 0x9421FFE0}, {0x806850A4, 0x3C80805A}, {0x806A4B54, 0x9421FFE0},
            {0x806862A0, 0x5480103A}, {0x8069C7B4, 0x9421FFF0}, {0x8068628C, 0x5480103A},
            {0x8069EE40, 0x9421FFF0}, {0x800B5854, 0x9421FF40}, {0x806A0D18, 0x00030506},
        };
        for (u32 i = 0; i < sizeof(FIRST_WORDS) / sizeof(FIRST_WORDS[0]); i++) {
            if (*(const u32*)FIRST_WORDS[i][0] != FIRST_WORDS[i][1]) return false;
        }
        return true;
    }
    typedef void* (*NewFn)(u32 size, u32 heap);
    typedef void (*VirtFn)(u8*);
    static void callSlot5C(u8* obj)   // obj's vtable (at +0x3C), slot 0x5C
    {
        u8* vt = *(u8**)(obj + 0x3C);
        ((VirtFn) * (u32*)(vt + 0x5C))(obj);
    }
    // The archive the menu is made from when it is not the character select's own (task+0x624):
    // set by prepare() for the call that follows.
    static u8* s_rulesArchive = NULL;

    extern "C" void pponline_openRulesVanilla(u8* task, int mode)
    {
        const NewFn alloc = (NewFn)0x8000C8B8;                       // new(size, heap)
        const u32 MENU_INSTANCE = 0x2A;
        u8* menu = *(u8**)(task + 0x6C);
        u8* bg = Build::cssModel(task, Build::CSS_RULE_BASE);
        if (!bg) return;
        typedef void (*Slot34Fn)(u8*, u32, u32);
        ((Slot34Fn) * (u32*)(*(u8**)menu + 0x34))(menu, *(u32*)(menu + 0xE4), *(u32*)(bg + 0x10));
        ((void (*)(u8*, int, int, int, int))0x800B5854)(bg, 0, 0x11, 1, 0);
        u8* back = (u8*)alloc(0x54, MENU_INSTANCE);
        if (!back) return;
        back = ((u8 * (*)(u8*, u8*, int))0x8069EECC)(back, bg, 0);
        callSlot5C(back);
        *(u8**)(task + 0x628) = back;
        ((void (*)(u8*))0x806850A4)(task);
        u8* a628 = *(u8**)(task + 0x628);
        u8* a624 = s_rulesArchive ? s_rulesArchive : *(u8**)(task + 0x624);   // (menuArchive below)
        u8 m = ((const u8*)0x806A0D18)[mode];
        u8* menu2 = *(u8**)(task + 0x6C);
        u8* rt = (u8*)alloc(0x2B0, MENU_INSTANCE);
        if (!rt) return;
        rt = ((u8 * (*)(u8*, u8*, u8*, u8*, u32, u32))0x806A3B24)(rt, menu2, a624, a628, m, 0x6A);
        callSlot5C(rt);
        u8 b = *(u8*)(task + 0x634);
        *(u8**)(task + TASK_RULES) = rt;
        *(u32*)(task + TASK_RULES_OPEN) = 1;
        ((void (*)(u8*, u32))0x806A4B54)(rt, b ? 0xFF : 0);
        for (int i = 0; i < *(int*)(task + 0x648); i++) {
            ((void (*)(void*, int))0x8069C7B4)(((void* (*)(u8*, int))0x806862A0)(task, i), 1);
            ((void (*)(void*, int))0x8069EE40)(((void* (*)(u8*, int))0x8068628C)(task, i), 1);
        }
    }
    // ---- A character select without a rules menu (REX) ------------------------------------------
    // muRuleTask's constructor takes its pages from the archive at task+0x624, the character
    // select's sc_selcharacter2.pac (a gfArchive: getData(type 2, index 0) its models, then its
    // messages, type 1). P+'s file is a copy of its main menu's rules archive (mu_menumain.pac's
    // ARC named MenuRule_en); REX's is an empty ARC (its rules are reached from the main menu only,
    // and its character select keeps its own files in InfoExtraResource, where P+ keeps that one),
    // so the menu was made from nothing and the game crashed (an invalid write to 0xD0 at
    // 0x801AB00C, then a jump to 0). Such a build's room asks Dolphin for its own main menu's rules
    // archive (CMD_MENU_ARCHIVE: Online/SpecialAssets), keeps it in Fighter1Resoruce (free on the
    // character select, as the SPECIAL box's buffer is) with a gfArchive the game makes of it
    // (setFileImage), and opens the menu from that. Asked for as the room's CSS comes up, so the
    // first RULES opens at once; freed with the character select (taskGone), before any fighter
    // is loaded there.
    typedef void* (*ArcGetDataFn)(void* arc, int type, int index, u32 kind);
    typedef void (*ArcCtFn)(void* arc);
    typedef void (*ArcDtFn)(void* arc, int del);
    typedef void (*ArcImageFn)(void* arc, void* image, u32 size, int heap);
    typedef void* (*HeapAllocFn)(int heap, u32 size);
    typedef void (*HeapFreeFn)(void* p);
    static const ArcGetDataFn ARC_GET_DATA = (ArcGetDataFn)0x80015DDC;   // gfArchive::getData
    static const ArcCtFn ARC_CT = (ArcCtFn)0x8001370C;
    static const ArcDtFn ARC_DT = (ArcDtFn)0x80013B1C;
    static const ArcImageFn ARC_SET_IMAGE = (ArcImageFn)0x80014890;      // setFileImage
    static const HeapAllocFn HEAP_ALLOC = (HeapAllocFn)0x800249E4;
    static const HeapFreeFn HEAP_FREE = (HeapFreeFn)0x80024A4C;
    static const int HEAP_FIGHTER1 = 18;
    static const u32 ARC_SIZE = 0x80;              // (the constructor's last field is +0x7C)
    static const u32 MENU_MAGIC = 0x4D415243;      // "MARC" (SpecialAssets::MENU_MAGIC)
    static const u32 MENU_MAX = 0x400000;
    static const u32 ARC_MAGIC = 0x41524300;       // "ARC\0": the archive written over it

    static struct {
        u8* task;          // the character select it is for (0: none wanted)
        u8* raw;           // the buffer and its 32-aligned start
        u8* buf;
        u32 capacity;      // what to allocate (Dolphin's size; 0 until told)
        u32 nonce;
        u8 state;          // PPOM::SA_*
        bool asked;
        int askIn;
        u8* arc;           // the gfArchive on the buffer, once written
    } m;

    static bool hasRulesModels(void* arc) { return isPtr((u32)arc) && ARC_GET_DATA(arc, 2, 0, 0xFFFE) != NULL; }

    static void menuRelease()
    {
        if (m.arc) {
            ARC_DT(m.arc, 0);   // (frees the index setFileImage made; the image is ours)
            HEAP_FREE(m.arc);
        }
        if (m.raw) HEAP_FREE(m.raw);
        memset(&m, 0, sizeof(m));
    }

    static void menuAsk()
    {
        if (m.capacity && !m.raw) {
            m.raw = (u8*)HEAP_ALLOC(HEAP_FIGHTER1, m.capacity + 32);
            if (!m.raw) {
                m.state = PPOM::SA_UNAVAILABLE;
                return;
            }
            m.buf = (u8*)(((u32)m.raw + 31) & ~31u);
        }
        m.nonce = PPOM::g_block.debug.frames ^ 0x6D41C3E5;
        if (m.buf) {
            *(u32*)m.buf = MENU_MAGIC;
            *(u32*)(m.buf + 4) = m.nonce;
        }
        // (no buffer yet: Dolphin answers with the archive's size)
        PPOM::SpecialAssetsRequest req = {(u32)m.buf, m.buf ? m.capacity : 0, m.nonce};
        PPOM::post(PPOM::CMD_MENU_ARCHIVE, &req, sizeof(req));
        m.asked = true;
    }

    void onMenuArchive(const PPOM::Response& r)
    {
        m.asked = false;
        if (!m.task) return;
        const u8 st = r.status == 0 && r.payload[0] >= PPOM::SA_LOADING &&
                              r.payload[0] <= PPOM::SA_UNAVAILABLE ? r.payload[0] : PPOM::SA_UNAVAILABLE;
        const u32 size = *(const u32*)(r.payload + 4);
        if (st == PPOM::SA_READY && r.payload[1] && m.buf && *(u32*)m.buf == ARC_MAGIC && size <= m.capacity) {
            u8* arc = (u8*)HEAP_ALLOC(HEAP_FIGHTER1, ARC_SIZE);
            if (arc) {
                memset(arc, 0, ARC_SIZE);
                ARC_CT(arc);
                ARC_SET_IMAGE(arc, m.buf, size, HEAP_FIGHTER1);
                m.arc = arc;
            }
            m.state = m.arc && hasRulesModels(m.arc) ? PPOM::SA_READY : PPOM::SA_UNAVAILABLE;
            return;
        }
        if (st == PPOM::SA_READY && !m.raw && size >= 0x40 && size <= MENU_MAX) {
            m.capacity = (size + 31) & ~31u;   // now with a buffer of its size
            m.askIn = 0;
            m.state = PPOM::SA_LOADING;
            return;
        }
        m.state = st == PPOM::SA_READY ? PPOM::SA_UNAVAILABLE : st;
        m.askIn = st == PPOM::SA_LOADING ? 20 : 0;
    }

    void taskGone(u8* task)
    {
        if (task && task == m.task) menuRelease();
    }

    // ---- Room for the menu --------------------------------------------------------------------
    // The menu makes its task and models in MenuInstance (heap 0x2A: muRuleTask's constructor
    // passes it to every MuObject::create), about 1.4 MB of it (P+ v3.2's room CSS: 0x1DAB40 free
    // before, 0x88F80 with the menu open). REX's character select leaves 0xA3AC0 there, so its
    // models were made with no memory (a null scene object: the crash above). Where the heap's
    // largest free block is smaller than that, heap 0x2A is Fighter1Resoruce's pool while the menu
    // is made (g_HeapInfos[0x2A]'s pool, put back right after): a block remembers its own pool
    // (gfMemoryPool block +0), so everything is freed where it was taken, the menu's models when it
    // closes, before any fighter is loaded there. P+'s room CSS has the room and is as before.
    typedef u32 (*HeapMaxFreeFn)(int heap);
    static const HeapMaxFreeFn HEAP_MAX_FREE = (HeapMaxFreeFn)0x80024A34;   // gfHeapManager::getMaxFreeSize
    static const int MENU_INSTANCE = 0x2A;
    static const u32 RULES_MENU_NEED = 0x180000;
    // The pool to put in for the menu's making (NULL: MenuInstance has the room, or nothing has).
    static gfMemoryPool* roomForMenu()
    {
        if (HEAP_MAX_FREE(MENU_INSTANCE) >= RULES_MENU_NEED) return NULL;
        gfMemoryPool* f1 = g_HeapInfos[HEAP_FIGHTER1].m_memoryPool;
        if (!isPtr((u32)f1) || HEAP_MAX_FREE(HEAP_FIGHTER1) < RULES_MENU_NEED) return NULL;
        return f1;
    }

    // The character select's own archive has the menu's models (P+, its mods): nothing to do.
    // Else the archive for it: ours once ready (NULL until then; asked for here).
    static bool cssHasRulesMenu(u8* task) { return hasRulesModels(*(void**)(task + 0x624)); }
    static u8* menuArchive(u8* task)
    {
        if (m.task != task) {
            menuRelease();
            m.task = task;
            m.state = PPOM::SA_LOADING;
        }
        if (m.state == PPOM::SA_READY) return m.arc;
        if (m.state == PPOM::SA_LOADING && !m.asked && --m.askIn <= 0) menuAsk();
        return NULL;
    }


    // ---- What room_rules.cpp calls (rules_rebuild.h) -------------------------------------------
    static bool s_pending;          // RULES pressed while the menu's archive loads
    static bool s_pendingSetOnly;

    u32 openFn() { return canOpenRulesOurselves() ? (u32)&pponline_openRulesVanilla : 0; }

    Menu prepare(u8* task, bool setOnly)
    {
        s_rulesArchive = NULL;
        if (cssHasRulesMenu(task)) return MENU_OWN;
        if (!canOpenRulesOurselves()) return MENU_NONE;
        u8* archive = menuArchive(task);
        if (archive) {
            s_rulesArchive = archive;   // (for the call that follows)
            return MENU_REBUILT;
        }
        if (m.state == PPOM::SA_UNAVAILABLE) return MENU_MISSING;
        s_pending = true;
        s_pendingSetOnly = setOnly;
        return MENU_LOADING;
    }

    void call(u32 fn, u8* task)
    {
        gfMemoryPool* borrowed = roomForMenu();
        gfMemoryPool* menuPool = g_HeapInfos[MENU_INSTANCE].m_memoryPool;
        if (borrowed) g_HeapInfos[MENU_INSTANCE].m_memoryPool = borrowed;
        pponline_callPreserving(fn, (u32)task, 0);
        if (borrowed) g_HeapInfos[MENU_INSTANCE].m_memoryPool = menuPool;
        s_rulesArchive = NULL;
    }

    void tick(u8* task, bool inRoom, bool mayOpen)
    {
        if (inRoom && task && (m.task == task || (!cssHasRulesMenu(task) && canOpenRulesOurselves()))) {
            menuArchive(task);
            if (s_pending && m.state != PPOM::SA_LOADING) {
                s_pending = false;
                if (mayOpen) RoomRules::open(s_pendingSetOnly);
            }
        } else {
            s_pending = false;
        }
    }

    void forget() { s_pending = false; }
}
#endif
