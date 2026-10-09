// Legal stages only on P+'s stage select, with the SSS's existing unavailable state (the design's
// plan for 5.4 row 5). Slippi parity: Direct's loser picks freely, so nothing turns this on yet;
// kept, behind a debug flag, for Ranked's strike / counterpick steps.
//
// The existing state is P+ v3.2's own Stage Striking (Source/Project+/Random.asm, "Stage Striking
// & Page Switch"): a per-page bit table STAGE_STRIKE_TABLE (0x8042C822, 5 pages x 6 bytes:
// u16 positions 31.., u32 positions 0..30, bit = page position). P+'s buttonProc hook
// (sel_stage+0x4EE8 = 0x806B586C) draws a struck icon with the icon's second texture set (frame
// +400: greyed with a white X), redraws the whole page when PAGE_INDEX (0x8042C821) differs from
// CURRENT_PAGE (0x80496000), and every frame without A it drops the selection (task+0x248 = -1,
// no highlight, blank preview) while the cursor is on a struck icon. P+'s random
// (selectSequential, CODE @ 0x806B7638) draws from "random switch AND NOT struck".
//
// While the restriction is on (CFG_SSS_LEGAL; no online mode turns it on, see active()):
//  - H1 sel_stage+0x4EDC (`lwz r3,0x13C(r1)`, the pressed buttons, just before P+'s two hooks):
//    the strike table = NOT allowed on every page, the random switch (RSS_EXDATA pages) = allowed
//    (saved, put back once the stage select is gone), PAGE_INDEX = 0xFF when the table changed so
//    P+ redraws; B and X are taken out of r3 so P+'s hook neither clears the strikes (B) nor
//    strikes more (X, X on RANDOM).
//  - H2 sel_stage+0x525C (`lwz r26,0x248(r29)`, A/START on a stage): a stage that is not allowed
//    reads as "no selection" (-1). P+'s per-frame check alone misses the frame the cursor enters
//    a struck icon: setHover (+0x5C00, run before buttonProc) sets +0x248 and an A on that same
//    frame took the struck stage (verified live). With -1, A does nothing and START picks random.
//  - H3 sel_stage+0x54C0 (`lwz r3,0x24C(r29)`, A/START on the stage-builder / custom-list page,
//    where P+ draws no strikes): no selection.
//  - H4 sel_stage+0x4CF4 (`lis r3,0x805A`, B leaves the stage select): P+ (StageFiles.asm
//    0x806B5684) does not leave while anything is struck; clear the table first so B still goes
//    back to the CSS.
// The tick puts the player's random switch back and clears the strike table once the scene is no
// longer scSelStage (P+'s screenless random also reads the table).
#include <sy_core.h>
#include <string.h>
#include <mu/mu_msg.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "stage_legal.h"

extern "C" {
    extern u8 g_onlineCss;
    extern u8 g_onlinePickStage;
}

namespace StageLegal {

    static const u32 RSS_EXDATA = 0x8042C4E8;         // random switch: page p at +6p {u16 31.., u32 0..30}
    static const u32 STAGE_PAGES = 0x8042C524;        // page p at +0x28p: {u8 count, u8 slot[39]}
    static const u32 SLOT_KINDS = 0x8042C5EC;         // slot s at +2s: {u8 srStageKind, u8 cosmetic}
    static const u32 PAGE_INDEX = 0x8042C821;         // page P+'s strike art was last drawn for
    static const u32 STRIKE_TABLE = 0x8042C822;       // same layout as the random switch
    const u32 STRIKE_TABLE_ADDR = STRIKE_TABLE;
    const u32 CURRENT_PAGE = 0x80496000;
    static const int PAGES = 5;
    static const int PAGE_BYTES = PAGES * 6;

    // P+ v3.2's legal list (Switch00.rss "Default", docs/game-code.md section 11), srStageKind.
    static const u8 PPLUS_LEGAL[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0C,
                                     0x0D, 0x1C, 0x1F, 0x21, 0x23, 0x2D, 0x2E};
    static u8 s_kinds[64];
    static int s_count = -1;            // -1: P+'s legal list

    static u8 s_savedSwitch[PAGE_BYTES];
    static bool s_switchSaved = false;

    void setList(const u8* kinds, int n)
    {
        if (!kinds || n <= 0) { s_count = -1; return; }
        if (n > (int)sizeof(s_kinds)) n = sizeof(s_kinds);
        memcpy(s_kinds, kinds, n);
        s_count = n;
    }

    // SESSION's stage comes from the host's machine (another player's): only a stage this stage
    // select offers reaches the match setup. P+'s page table lists the slots of each page, the
    // slot table their srStageKind.
    bool selectableKind(int kind)
    {
        if (kind <= 0 || kind > 0xFF) return false;
        for (int p = 0; p < PAGES; p++) {
            const u8* page = (const u8*)(STAGE_PAGES + 0x28 * p);
            int count = page[0];
            if (count > 39) count = 39;
            for (int i = 0; i < count; i++) {
                const u8 slot = page[1 + i];
                if (slot >= 0x80) continue;
                if (((const u8*)SLOT_KINDS)[2 * slot] == kind) return true;
            }
        }
        return false;
    }

    bool allowedKind(int kind)
    {
        const u8* k = s_count < 0 ? PPLUS_LEGAL : s_kinds;
        int n = s_count < 0 ? (int)sizeof(PPLUS_LEGAL) : s_count;
        for (int i = 0; i < n; i++) if (k[i] == kind) return true;
        return false;
    }

    // Not used by any online mode today. Slippi restricts no stage select: Direct's loser picks
    // any stage, Unranked (and Teams game 1) draw from the server's list without a stage select,
    // and only Ranked has its own strike screen (design 5.4 rows 5-6, coordinator's correction
    // 2026-10-07). This is the mechanism a Ranked strike / counterpick step can drive later
    // (setList with the server's list); for now only the debug flag turns it on.
    // Ranked's stage steps (OnlineMenu::rankedStep, Dolphin's GameSetup): the stage select shows
    // the stages still selectable and greys the rest with P+'s own strike art, only the player
    // whose turn it is strikes (X) or picks (A), and the rule line says whose turn it is.
    static const PPOM::GameStep* ranked()
    {
        return g_onlinePickStage ? OnlineMenu::rankedStep() : NULL;
    }

    bool active()
    {
        return (PPOM::g_block.debug.cfg & PPOM::CFG_SSS_LEGAL) != 0 || ranked() != NULL;
    }

    // The stage select's message object: created by sel_stage+0xC38 (43 windows); window 0 is its
    // rule line ("4 VERSUS"), printed by sel_stage+0x635C.
    static MuMsg* s_sssMsg = NULL;
    static bool s_sssDirty = false;
    static char s_sssText[64];

    void onMsgCreate(MuMsg* m, u32 caller)
    {
        if (caller == 0x806B15BC) s_sssMsg = m;
    }

    void onSssPrint(u32 caller)
    {
        if (caller == 0x806B6CE0) s_sssDirty = true;
    }

    // Every frame of a ranked stage select: whose turn it is, in the header strip on the
    // "STAGE SELECT" line (window 0 of the stage select's message object, moved there: it is
    // otherwise a line left of the stage icons that P+'s layout hides behind them).
    static void showTurn(const char* text)
    {
        if (!s_sssMsg || !text[0]) return;
        if (!s_sssDirty && strcmp(text, s_sssText) == 0) return;
        strncpy(s_sssText, text, sizeof(s_sssText) - 1);
        s_sssDirty = false;
        float* ws = (float*)*(u32*)((u8*)s_sssMsg + 0xC);   // WindowSetting 0: x1 y1 x2 y2 at +4
        ws[1] = 237.0f;
        ws[2] = -347.0f;
        ws[3] = 537.0f;
        ws[4] = -387.0f;
        ws[12] = ws[13] = 0.8f;                             // scaleX, scaleY (+0x30)
        s_sssMsg->printf(0, "%s", text);
    }

    int kindAt(int page, int pos)
    {
        if (page < 0 || page >= PAGES || pos < 0) return -1;
        const u8* p = (const u8*)(STAGE_PAGES + page * 0x28);
        if (pos >= p[0] || pos >= 39) return -1;
        return *(const u8*)(SLOT_KINDS + 2 * p[1 + pos]);
    }

    // The allowed bits of one page: {u16 positions 31.., u32 positions 0..30}.
    static void pageMask(int page, u16* ext, u32* main)
    {
        *ext = 0;
        *main = 0;
        const u8* p = (const u8*)(STAGE_PAGES + page * 0x28);
        int count = p[0] > 39 ? 39 : p[0];
        for (int pos = 0; pos < count; pos++) {
            if (!allowedKind(kindAt(page, pos))) continue;
            if (pos < 31) *main |= 1u << pos;
            else *ext |= (u16)(1u << (pos - 31));
        }
    }

    static bool put16(u32 addr, u16 v)
    {
        if (*(u16*)addr == v) return false;
        *(u16*)addr = v;
        return true;
    }

    // Every frame of a restricted stage select (from H1). Idempotent.
    static void apply()
    {
        if (!s_switchSaved) {
            memcpy(s_savedSwitch, (const void*)RSS_EXDATA, PAGE_BYTES);
            s_switchSaved = true;
        }
        bool changed = false;
        for (int page = 0; page < PAGES; page++) {
            u16 ext;
            u32 main;
            pageMask(page, &ext, &main);
            // Halfword stores: the tables are only 2-aligned.
            u32 sw = RSS_EXDATA + page * 6, st = STRIKE_TABLE + page * 6;
            put16(sw, ext);
            put16(sw + 2, (u16)(main >> 16));
            put16(sw + 4, (u16)main);
            changed |= put16(st, (u16)~ext);
            changed |= put16(st + 2, (u16)(~main >> 16));
            changed |= put16(st + 4, (u16)~main);
        }
        if (changed) *(u8*)PAGE_INDEX = 0xFF;   // P+ redraws the page's strike art
        PPOM::g_block.debug.scratch[12]++;
    }

    void tick()
    {
        if (!Online::inScene("scSelStage")) {
            s_sssMsg = NULL;
            s_sssText[0] = 0;
        }
        // The random switch is the player's own preset: put it back once the stage select (and
        // its random roulette) is gone.
        if (s_switchSaved && !Online::inScene("scSelStage")) {
            memcpy((void*)RSS_EXDATA, s_savedSwitch, PAGE_BYTES);
            memset((void*)STRIKE_TABLE, 0, PAGE_BYTES);   // P+'s random without a screen reads it too
            s_switchSaved = false;
        }
    }
}

extern "C" {
    // Ranked: leave the stage select at once (A on any stage: the steps decide the stage).
    static bool s_autoLeave = false;

    // H1: returns the pressed buttons the rest of buttonProc (P+'s hooks) sees in r3.
    __attribute__((used)) u32 pponline_sssButtons(u8* task, u32 pressed)
    {
        if (!StageLegal::active()) return pressed;
        const PPOM::GameStep* st = StageLegal::ranked();
        if (st) StageLegal::setList(st->kinds, st->nKinds ? st->nKinds : -1);
        StageLegal::apply();
        u32 out = pressed & ~(0x200u | 0x400u | 0x80000u);   // B, X (GameCube), X (Classic)
        if (!st) return out;
        out &= ~(0x1000u | 0x10u);   // START (random), Z (hazards: the online rules set them)
        // P+'s striking (Random.asm, the buttonProc hook) runs only while gfSceneManager+0x284
        // is 1, as after the Versus CSS; the online sequence opens the stage select with 0.
        u32 mgr = *(u32*)0x805A0060;
        if (mgr >= 0x80000000 && mgr < 0x81800000) *(int*)(mgr + 0x284) = 1;
        StageLegal::showTurn(OnlineMenu::rankedText());
        s_autoLeave = !st->active || (st->type != PPOM::STEP_STRIKE && st->type != PPOM::STEP_PICK);
        if (s_autoLeave) return out | 0x100u;
        if (!(st->myTurn && st->type == PPOM::STEP_PICK)) out &= ~0x100u;   // A only to pick
        if (st->myTurn && st->type == PPOM::STEP_STRIKE && (pressed & (0x400u | 0x80000u))) {
            int kind = StageLegal::kindAt(*(u8*)StageLegal::CURRENT_PAGE, *(int*)(task + 0x248));
            if (StageLegal::allowedKind(kind)) {
                u8 k = (u8)kind;
                PPOM::post(PPOM::CMD_GP_COMPLETE_STEP, &k, 1);
            }
        }
        return out;
    }

    // H2: the selection A/START would take on a normal page, or -1.
    __attribute__((used)) int pponline_sssFilterPos(u8* task, int pos)
    {
        if (!StageLegal::active()) return pos;
        int page = *(u8*)StageLegal::CURRENT_PAGE;
        if (StageLegal::ranked() && s_autoLeave) {
            // Any stage P+ still lets A take (not struck); the steps decided the real one.
            for (int p = 0; p < 39; p++) {
                if (StageLegal::allowedKind(StageLegal::kindAt(page, p))) return p;
            }
            for (int p = 0; p < 39; p++) {
                if (StageLegal::kindAt(page, p) > 0) return p;
            }
            return pos;
        }
        if (pos < 0) return pos;
        int kind = StageLegal::kindAt(page, pos);
        if (StageLegal::allowedKind(kind)) {
            if (StageLegal::ranked()) {
                u8 k = (u8)kind;
                PPOM::post(PPOM::CMD_GP_COMPLETE_STEP, &k, 1);   // the loser's pick
            }
            return pos;
        }
        PPOM::g_block.debug.scratch[13]++;   // refused picks (tests)
        return -1;
    }
}

namespace StageLegal {

    // sel_stage+0x4EDC (0x806B5860): `lwz r3,0x13C(r1)`. Volatile registers are dead here (the
    // instruction before is a `bl`, and the branches that land here reload what they use); LR
    // is saved in buttonProc's frame. Back to +0x4EE0 (P+ MusicSelect's hook on the A test).
    __attribute__((naked)) void hookButtons()
    {
        asm volatile(
            "mr 3, 29\n\t"
            "lwz 4, 0x13C(1)\n\t"
            "lis 12, pponline_sssButtons@ha\n\t"
            "addi 12, 12, pponline_sssButtons@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x806B\n\t"
            "ori 12, 12, 0x5864\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // sel_stage+0x525C (0x806B5BE0): `lwz r26,0x248(r29)`; r3 (the page, 0x228) is live after.
    // Back to +0x5260 `cmpwi r26,0`.
    __attribute__((naked)) void hookPickPos()
    {
        asm volatile(
            "stwu 1, -0x20(1)\n\t"
            "stw 3, 0x8(1)\n\t"
            "mr 3, 29\n\t"
            "lwz 4, 0x248(29)\n\t"
            "lis 12, pponline_sssFilterPos@ha\n\t"
            "addi 12, 12, pponline_sssFilterPos@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "mr 26, 3\n\t"
            "lwz 3, 0x8(1)\n\t"
            "addi 1, 1, 0x20\n\t"
            "lis 12, 0x806B\n\t"
            "ori 12, 12, 0x5BE4\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // sel_stage+0x54C0 (0x806B5E44): `lwz r3,0x24C(r29)` (stage-builder / custom-list page).
    // Back to +0x54C4 `cmpwi r3,0`.
    __attribute__((naked)) void hookCustomPos()
    {
        asm volatile(
            "lwz 3, 0x24C(29)\n\t"
            "stwu 1, -0x20(1)\n\t"
            "stw 3, 0x8(1)\n\t"
            "lis 12, pponline_sssCustomAllowed@ha\n\t"
            "addi 12, 12, pponline_sssCustomAllowed@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "cmpwi 3, 0\n\t"
            "lwz 3, 0x8(1)\n\t"
            "addi 1, 1, 0x20\n\t"
            "bne 1f\n\t"
            "li 3, -1\n\t"
            "1:\n\t"
            "lis 12, 0x806B\n\t"
            "ori 12, 12, 0x5E48\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // sel_stage+0x4CF4 (0x806B5678): `lis r3,0x805A`, B pressed on the stage select (task+0x6150
    // set): Brawl backs out to the CSS. P+'s hook two instructions later (StageFiles.asm
    // 0x806B5684) cancels that while any stage is struck (B then clears the strikes instead), so
    // with our strikes B could never leave. Restricted: clear the table first; B leaves as it
    // does without strikes (and on P+'s custom-list page it still goes back a page; H1 then
    // strikes again). Volatile registers are dead here (r3 is set next, r4-r8 before the playSE).
    __attribute__((naked)) void hookBack()
    {
        asm volatile(
            "lis 12, pponline_sssBack@ha\n\t"
            "addi 12, 12, pponline_sssBack@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 3, 0x805A\n\t"
            "lis 12, 0x806B\n\t"
            "ori 12, 12, 0x567C\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    void install(CoreApi* api)
    {
        api->sySimpleHookRel(0x4CF4, reinterpret_cast<void*>(hookBack), 11 /* sora_menu_sel_stage */);
        api->sySimpleHookRel(0x4EDC, reinterpret_cast<void*>(hookButtons), 11 /* sora_menu_sel_stage */);
        api->sySimpleHookRel(0x525C, reinterpret_cast<void*>(hookPickPos), 11);
        api->sySimpleHookRel(0x54C0, reinterpret_cast<void*>(hookCustomPos), 11);
    }
}

extern "C" {
    // B on a restricted stage select: let Brawl's back-out happen (see hookBack).
    __attribute__((used)) void pponline_sssBack()
    {
        if (StageLegal::active()) memset((void*)StageLegal::STRIKE_TABLE_ADDR, 0, 30);
    }

    // H3: 1 if a stage-builder / custom-list stage may be taken.
    __attribute__((used)) int pponline_sssCustomAllowed()
    {
        if (!StageLegal::active()) return 1;
        PPOM::g_block.debug.scratch[13]++;
        return 0;
    }
}
