// The other players' cursors on an online stage select (Sarah's board, 2026-10-29: "sync the
// cursors up on the map screen ... multiple cursors on the one screen ... they say p1 p2 p3 p4 on
// them"): a room where anyone picks (every player on the stage select at once, the first pick
// plays) and Ranked's strikes (and a room set's steps), where the opponent is on it too.
//
// Each machine writes its own cursor into LOCAL (ownCursor: sel_stage's cursor MuObject, task+0x200,
// its position +0x3C / +0x40, and P+'s page 0x80496000) every frame of an online stage select;
// Dolphin sends it with the session's state message (20 a second) and writes the other players'
// into LOCAL.cursors by in-game port. LOCAL is out of the rolled-back state: each machine has the
// others' cursors and not its own.
//
// The cursor is P+'s crosshair, model MenSelmapCursorA of the stage select's own brres (the
// cursor's MuObject +4, its ResFile), with its animations of the same name: CHR0 and CLR0 loop
// (the pulse), PAT0 frame 0 no label, 1-4 the textures MenSelmapCursorPly.0-3, "P1".."P4" on the
// crosshair (read live, 2026-10-29). Another player's cursor is a MuObject made the same way
// (MuObject::create, its scene group task+0x50 as sss_portraits.cpp's cards), labelled with their
// port, moved with MuObject::setPos (0x800B6838, as sel_stage moves its own) towards the last
// position heard (a fraction of the way each frame, 20 updates a second), its pulse kept in step
// with this player's cursor, and hidden while it is on another page or gone. This player's own
// cursor gets its port's label too where others' are shown. The cursors are deleted in the stage
// select task's destructor (sss_portraits.cpp's hook) before Brawl deletes its own.
#include <OS/OSCache.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "sss_cursors.h"

extern "C" {
    extern u8 g_onlineCss;
    extern u8 g_onlinePickStage;

    // Tests (harness, through PPOnlineMain.map).
    struct SssCursorsDebug {
        u32 written;         // LOCAL.ownCursor changes
        u32 made;            // cursors made
        u32 deleted;         // cursors deleted with the stage select
        u32 refused;         // peers' cursors refused (page or position out of range)
        u8 shown;            // bit p: port p's cursor shown this frame
        u8 ownLabel;         // the PAT0 frame given to this player's own cursor (0 none)
        u8 alive;            // cursors alive
        u8 pad;
        float pos[4][2];     // by port: where its cursor is drawn
        u32 obj[4];          // by port: its MuObject
    } g_sssCursors __attribute__((used)) = {0, 0, 0, 0, 0, 0, 0, 0, {{0}}, {0}};
}

namespace SssCursors {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    typedef u8* (*CreateFn)(void* modelRes, const char* name, int priority, void* texRes, int heap);
    typedef void (*NameFn)(u8* obj, const char* name);
    typedef void (*FrameFn)(u8* obj, float frame);
    typedef void (*SetPosFn)(u8* obj, const float* pos);
    typedef void (*SetVisFn)(void* scnObj, bool vis);
    static const CreateFn MU_CREATE = (CreateFn)0x800B2394;        // MuObject::create(ResFile*, name, ...)
    static const NameFn CHANGE_ANIM = (NameFn)0x800B534C;          // MuObject::changeAnimN
    static const FrameFn SET_FRAME_TEX = (FrameFn)0x800B7900;      // MuObject::setFrameTex (PAT0)
    static const SetPosFn SET_POS = (SetPosFn)0x800B6838;          // MuObject::setPos
    static const SetVisFn SET_VIS = (SetVisFn)0x80043D20;          // nwSMSetVisibility
    static const u32 MU_OBJECT_VTABLE = 0x80455D70;                // MuObject's (its +0x5C)
    static const int MENU_HEAP = 0x2A;
    static const char* const MODEL = "MenSelmapCursorA";
    static const u32 CURRENT_PAGE = 0x80496000;
    // The animations' slots in a MuObject's anim block (+0x14) and an AnmObj's frame (+0x18) and
    // rate (+0x1C), as sss_portraits.cpp sets them.
    static const u32 ANIM_VIS = 0x08, ANIM_CHR = 0x0C, ANIM_PAT = 0x10, ANIM_CLR = 0x18;
    // How far towards the last heard position a cursor moves each frame, and the distance from
    // which it jumps there instead (another page, or a long gap).
    static const float FOLLOW = 0.35f, JUMP = 24.0f;

    struct Cursor {
        u8* obj;
        float x, y;
        bool placed;
    };
    static Cursor s_cur[PPOM::SESSION_PLAYERS_LOCAL];
    static u8* s_task = NULL;
    static u32 s_res = 0;   // the stage select's brres (a ResFile value)

    static u8* stageSelectTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        u32 scene = isPtr(mgr) ? *(u32*)(mgr + 4) : 0;
        u32 task = isPtr(scene) ? *(u32*)(scene + 0x3AC) : 0;
        return isPtr(task) ? (u8*)task : NULL;
    }

    // sel_stage's own cursor (task+0x200), once it is made.
    static u8* ownCursor(u8* task)
    {
        u32 c = *(u32*)(task + 0x200);
        if (!isPtr(c) || *(u32*)(c + 0x5C) != MU_OBJECT_VTABLE) return NULL;
        return isPtr(*(u32*)(c + 4)) && isPtr(*(u32*)(c + 0x14)) ? (u8*)c : NULL;
    }

    static u8* anim(u8* obj, u32 slot)
    {
        u8* block = *(u8**)(obj + 0x14);
        if (!isPtr((u32)block)) return NULL;
        u8* a = *(u8**)(block + slot);
        return isPtr((u32)a) ? a : NULL;
    }

    static void setLabel(u8* obj, int frame)
    {
        u8* pat = anim(obj, ANIM_PAT);
        if (!pat) return;
        if (*(float*)(pat + 0x18) == (float)frame && *(float*)(pat + 0x1C) == 0.0f) return;
        SET_FRAME_TEX(obj, (float)frame);
        *(float*)(pat + 0x1C) = 0.0f;
    }

    // Another player's cursor looks as this player's does: the same VIS0 frame (sel_stage holds
    // its cursor's at 0; played on, it shows P+'s coin-battle parts, "1 COIN") and it pulses with
    // it (the same CHR0 / CLR0 frames).
    static void followPulse(u8* obj, u8* own)
    {
        static const u32 SLOTS[3] = {ANIM_VIS, ANIM_CHR, ANIM_CLR};
        for (int i = 0; i < 3; i++) {
            u8* from = anim(own, SLOTS[i]);
            u8* to = anim(obj, SLOTS[i]);
            if (!from || !to) continue;
            *(float*)(to + 0x18) = *(float*)(from + 0x18);
            *(float*)(to + 0x1C) = 0.0f;
        }
    }

    static u8* make(u8* task, u8* own, int port)
    {
        s_res = *(u32*)(own + 4);
        u8* scnOwn = *(u8**)(own + 0x10);
        // Under this player's own (a higher priority draws later, on top: sss_portraits.cpp's parts).
        int priority = isPtr((u32)scnOwn) ? *(scnOwn + 0xD0) : 0x20;
        if (priority > 0) priority--;
        u8* obj = MU_CREATE(&s_res, MODEL, priority, &s_res, MENU_HEAP);
        if (!isPtr((u32)obj)) return NULL;
        CHANGE_ANIM(obj, MODEL);
        setLabel(obj, port + 1);
        followPulse(obj, own);
        u8* group = *(u8**)(task + 0x50);
        u8* scn = *(u8**)(obj + 0x10);
        if (isPtr((u32)group) && isPtr((u32)scn)) {
            typedef bool (*InsertFn)(u8*, u32, u8*);
            ((InsertFn)(*(u32*)(*(u32*)group + 0x34)))(group, *(u32*)(group + 0xE4), scn);
        }
        g_sssCursors.made++;
        g_sssCursors.alive++;
        g_sssCursors.obj[port] = (u32)obj;
        return obj;
    }

    static void show(u8* obj, bool on)
    {
        u32 mdl = *(u32*)(obj + 0xC);
        if (isPtr(mdl)) SET_VIS((void*)mdl, on);
    }

    static void deleteAll()
    {
        for (int p = 0; p < PPOM::SESSION_PLAYERS_LOCAL; p++) {
            Cursor& c = s_cur[p];
            if (c.obj) {
                typedef void (*DelFn)(u8*, int);
                ((DelFn)(*(u32*)(*(u32*)(c.obj + 0x5C) + 8)))(c.obj, 1);
                g_sssCursors.deleted++;
                g_sssCursors.alive--;
            }
            memset(&c, 0, sizeof(c));
            g_sssCursors.obj[p] = 0;
        }
        g_sssCursors.shown = 0;
        g_sssCursors.ownLabel = 0;
    }

    static void writeOwn(const PPOM::SssCursor& c)
    {
        PPOM::SssCursor& own = PPOM::g_block.local.ownCursor;
        if (own.state == c.state && own.page == c.page && own.x == c.x && own.y == c.y) return;   // (no memcmp in the DOL)
        own = c;
        DCFlushRange(&own, sizeof(own));
        g_sssCursors.written++;
    }

    static s16 fixed(float v)
    {
        float f = v * 64.0f;
        if (f > PPOM::SSS_CURSOR_RANGE) f = PPOM::SSS_CURSOR_RANGE;
        if (f < -PPOM::SSS_CURSOR_RANGE) f = -PPOM::SSS_CURSOR_RANGE;
        return (s16)(f < 0 ? f - 0.5f : f + 0.5f);
    }

    static bool validPeer(const PPOM::SssCursor& c)
    {
        if (c.state != PPOM::CURSOR_ON) return false;
        if (c.page >= PPOM::SSS_PAGES || c.x > PPOM::SSS_CURSOR_RANGE || c.x < -PPOM::SSS_CURSOR_RANGE ||
            c.y > PPOM::SSS_CURSOR_RANGE || c.y < -PPOM::SSS_CURSOR_RANGE) {
            g_sssCursors.refused++;
            return false;
        }
        return true;
    }

    void tick()
    {
        PPOM::SssCursor mine;
        memset(&mine, 0, sizeof(mine));
        u8* task = g_onlineCss && g_onlinePickStage && Online::inScene("scSelStage") ? stageSelectTask() : NULL;
        if (task != s_task) {
            // (a stage select left without its destructor running: nothing of ours is in a scene)
            if (s_task) memset(s_cur, 0, sizeof(s_cur));
            s_task = task;
        }
        u8* own = task ? ownCursor(task) : NULL;
        if (!own) {
            writeOwn(mine);
            return;
        }
        const u8 page = *(u8*)CURRENT_PAGE;
        mine.state = PPOM::CURSOR_ON;
        mine.page = page;
        mine.x = fixed(*(float*)(own + 0x3C));
        mine.y = fixed(*(float*)(own + 0x40));
        writeOwn(mine);

        const PPOM::Local& lo = PPOM::g_block.local;
        u8 shown = 0;
        bool any = false;
        for (int p = 0; p < PPOM::SESSION_PLAYERS_LOCAL; p++) {
            Cursor& c = s_cur[p];
            const PPOM::SssCursor& in = lo.cursors[p];
            bool on = p != lo.localPort && validPeer(in);
            any = any || on;
            on = on && in.page == page;
            if (on && !c.obj) {
                c.obj = make(task, own, p);
                c.placed = false;
            }
            if (!c.obj) continue;
            if (!on) {
                show(c.obj, false);
                c.placed = false;
                continue;
            }
            float tx = in.x / 64.0f, ty = in.y / 64.0f;
            float dx = tx - c.x, dy = ty - c.y;
            if (!c.placed || dx > JUMP || dx < -JUMP || dy > JUMP || dy < -JUMP) {
                c.x = tx;
                c.y = ty;
                c.placed = true;
            } else {
                c.x += dx * FOLLOW;
                c.y += dy * FOLLOW;
            }
            float pos[3] = {c.x, c.y, *(float*)(own + 0x44)};
            SET_POS(c.obj, pos);
            setLabel(c.obj, p + 1);
            followPulse(c.obj, own);
            show(c.obj, true);
            shown |= (u8)(1 << p);
            g_sssCursors.pos[p][0] = c.x;
            g_sssCursors.pos[p][1] = c.y;
        }
        g_sssCursors.shown = shown;
        // This player's own cursor says who they are too, once anyone else's is there.
        if ((any || g_sssCursors.ownLabel) && lo.localPort < PPOM::SESSION_PLAYERS_LOCAL) {
            setLabel(own, lo.localPort + 1);
            g_sssCursors.ownLabel = (u8)(lo.localPort + 1);
        }
    }

    void taskGone(u8* task)
    {
        if (task && task == s_task) {
            deleteAll();
            s_task = NULL;
        }
    }
}
