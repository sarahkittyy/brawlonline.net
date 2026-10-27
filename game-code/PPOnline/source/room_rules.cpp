// The room's rules on the room CSS (docs/design/room-rules.md decisions 3-5, docs/game-code.md
// §14 "Room rules RE").
//
// P+'s rules menu is muRuleTask (sora_menu_rule.rel, module 18, loaded with every CSS). The CSS
// opens it over itself with sel_char text+0x105D8 openRules(muSelCharTask*, mode), mode 0 being
// the whole menu (RULES, MORE RULES, the item switch, the random stage switch), keeps it at
// muSelCharTask+0x62C and sets +0x630 while it is open; writing +0x288 = 1 into the task closes it
// on the next frame. Its pages read the set rule, the menu record and RSS_EXDATA when they are
// made and write their working copies back when they are left, so:
// - before opening, the room's rules (SESSION Rules, which Dolphin fills from room-state on the
//   room CSS) are put into the set rule and the menu record (OnlineMenu::applyRules);
// - when the host's menu closes, the set rule and the menu record hold the host's edits; those
//   that are room rules go to Dolphin (ROOM_RULES), which sends room-rules. The server takes back
//   everyone's ready and sends the new rules with room-state, and every game is played with them.
// - Everyone else gets the same screens read-only: every page reads its input through one
//   function (module 18 text+0x12E0, the page in r30, the result `mr r3,r31` at text+0x15B4),
//   whose result is filtered here.

#include "room_rules.h"

#include <string.h>

#include "online_menu.h"
#include "ppom.h"
#include "room_play.h"
#include "stage_switch.h"

namespace RoomRules {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // (the DOL has no memcmp: a call to it would jump to the unresolved stub)
    static bool same(const void* a, const void* b, u32 n)
    {
        const u8* x = (const u8*)a;
        const u8* y = (const u8*)b;
        for (u32 i = 0; i < n; i++) {
            if (x[i] != y[i]) return false;
        }
        return true;
    }

    static u8* cssTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        return isPtr(task) ? (u8*)task : NULL;
    }

    static u8* setRule()
    {
        u32 gg = *(u32*)0x805A00E0;    // g_GameGlobal
        if (!isPtr(gg)) return NULL;
        u32 r = *(u32*)(gg + 0x1C);
        return isPtr(r) ? (u8*)r : NULL;
    }

    static u8* menuRecord()
    {
        u32 gg = *(u32*)0x805A00E0;
        if (!isPtr(gg)) return NULL;
        u32 rec = *(u32*)(gg + 0x24);
        return isPtr(rec) ? (u8*)(rec + 0x810) : NULL;
    }

    static void playSE(int id)
    {
        typedef void (*PlaySEFn)(void*, int, int, int, int, int);
        void* snd = *(void**)0x805A01D0;   // g_sndSystem
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }

    static const u32 TASK_RULES = 0x62C, TASK_RULES_OPEN = 0x630;   // muSelCharTask
    static const u32 RULES_CLOSE = 0x288;                            // muRuleTask: u8, 1 = close
    static const u32 RULES_PAGE1 = 0x268, RULES_PAGE2 = 0x26C;       // RULES, MORE RULES
    static const u32 RULES_ITEMS = 0x270;                            // the item switch
    static const u32 RULES_SWITCH = 0x274;                           // the stage switch
    static const u32 OPEN_RULES = 0x80692E9C;                        // sel_char text+0x105D8
    static const int STATUS_FRAMES = 180;

    static struct {
        bool opened;        // we opened the menu that is open now
        bool asHost;        // ... as the host (its edits are the room's)
        bool readOnly;      // ... for viewing: the input filter is on
        bool setOnly;       // ... between a set's games: read-only but the host's SET line (End set)
        bool haveRules;
        bool changedNow;
        PPOM::Rules last;   // SESSION's rules last frame (a change: "Host changed the rules")
        PPOM::RoomStages lastStages;   // ... and the room's stages (the STAGE SWITCH)
        int statusFrames;
        bool statusRed;
        const char* status;
    } s;

    static u8* rulesTask()
    {
        u8* task = cssTask();
        u32 rt = task ? *(u32*)(task + TASK_RULES) : 0;
        return isPtr(rt) ? (u8*)rt : NULL;
    }

    bool changedNow() { return s.changedNow; }

    bool isOpen()
    {
        u8* task = cssTask();
        return task && *(u32*)(task + TASK_RULES_OPEN) != 0;
    }

    static void forceClose()
    {
        // The pages write nothing back when closed this way (game-code.md §14): the room's rules
        // stay as they are.
        u8* rt = rulesTask();
        // A byte: isEnd (module 18 text+0xF78) reads it with `lbz r0,0x288(r3)` (a word write
        // set +0x28B and left the menu open).
        if (rt) *(u8*)(rt + RULES_CLOSE) = 1;
    }

    static void say(const char* text, bool red)
    {
        s.status = text;
        s.statusRed = red;
        s.statusFrames = STATUS_FRAMES;
    }

    const char* statusText(bool* red)
    {
        if (s.statusFrames <= 0) return NULL;
        if (red) *red = s.statusRed;
        return s.status;
    }

    void reset()
    {
        StageSwitch::close();   // (P+'s switch put back)
        s.opened = s.asHost = s.readOnly = s.setOnly = false;
        s.haveRules = false;
        s.statusFrames = 0;
    }

    // P+'s openRules (sel_char text+0x105D8) called with every non-volatile register (r14-r31,
    // CR, LR) saved and put back around it: called as an ordinary function, the caller's values in
    // them did not survive (staging feedback 2026-10-12: a viewer's portraits swapped places while
    // RULES was open and swapped back, "as if re-entering the room"; seen live: right after the call
    // tickRoom's own local slot read 0 instead of the viewer's 2, so its panels were placed for
    // slot 1 until the menu closed and the next tick placed them again; the host, in the first
    // slot, already had 0).
    extern "C" __attribute__((naked)) void pponline_callPreserving(u32 fn, u32 a0, u32 a1)
    {
        asm volatile(
            "stwu 1, -0x60(1)\n\t"
            "mflr 0\n\t"
            "stw 0, 0x64(1)\n\t"
            "mfcr 0\n\t"
            "stw 0, 0x08(1)\n\t"
            "stmw 14, 0x10(1)\n\t"
            "mtctr 3\n\t"
            "mr 3, 4\n\t"
            "mr 4, 5\n\t"
            "bctrl\n\t"
            "lmw 14, 0x10(1)\n\t"
            "lwz 0, 0x08(1)\n\t"
            "mtcrf 0xFF, 0\n\t"
            "lwz 0, 0x64(1)\n\t"
            "mtlr 0\n\t"
            "addi 1, 1, 0x60\n\t"
            "blr\n\t");
    }

    void open(bool setOnly)
    {
        u8* task = cssTask();
        const PPOM::Session& se = PPOM::g_block.session;
        if (!task || isOpen() || !se.rules.valid) return;
        int me = PPOM::g_block.local.localPort;
        s.asHost = me < PPOM::SESSION_PLAYERS && se.roomHost == me;
        s.readOnly = !s.asHost || setOnly;
        s.setOnly = setOnly && s.asHost;
        // The menu shows what is in the set rule and the menu record: the room's rules; its stage
        // switch the room's stages.
        OnlineMenu::applyRules();
        StageSwitch::open(!s.readOnly);
        pponline_callPreserving(OPEN_RULES, (u32)task, 0);
        s.opened = isOpen();
        if (s.opened) playSE(1);
    }


    // The host's edits as SESSION's Rules: what the rules menu wrote back into the set rule and
    // the menu record (game-code.md §14's table), and the STAGE SWITCH's hazards. The rest of the
    // record (stock control) stays the room's. Lines that are not room rules here (P+'s score to win, stage choice; stock
    // control, whose row is the SET line, is set in a ruleset) are not taken: applyRules writes
    // the match's values there for every match.
    static void sendEdits()
    {
        const PPOM::Rules& cur = PPOM::g_block.session.rules;
        u8* r = setRule();
        u8* m = menuRecord();
        if (!r || !m || !cur.valid) return;
        PPOM::RoomRulesRequest req;
        memset(&req, 0, sizeof(req));
        req.op = PPOM::ROOM_RULES;
        req.rules = cur;
        PPOM::Rules& ru = req.rules;
        ru.style = r[2] & 7;
        ru.stocks = r[4];
        ru.damageRatio = r[6];
        ru.timeMinutes = r[8];
        ru.teamAttack = r[9] ? 1 : 0;
        ru.pause = r[0xA] ? 1 : 0;
        ru.autoLCancel = r[0xB] ? 1 : 0;
        ru.inputAssist = r[0xC] ? 1 : 0;
        ru.itemFrequency = m[0];
        memcpy(ru.itemSwitch, m + 8, sizeof(ru.itemSwitch));
        StageSwitch::hazards(ru);   // the STAGE SWITCH's Z (every stage, or one)
        if (same(&ru, &cur, sizeof(ru))) return;
        if (ru.style != PPOM::STYLE_STOCK && ru.timeMinutes == 0) {
            say("A time match needs a time limit.", true);
            playSE(3);
            return;
        }
        if (ru.itemFrequency > 3) {
            say("That item frequency isn't online yet.", true);
            playSE(3);
            return;
        }
        PPOM::post(PPOM::CMD_ROOM, &req, sizeof(req));
    }

    bool tick(bool inRoom, bool host, bool waiting, bool setBetween)
    {
        const PPOM::Session& se = PPOM::g_block.session;
        if (s.statusFrames > 0) s.statusFrames--;
        // The room's rules changed: the host's own edit comes back from the server; for the others
        // it is news (and what they were looking at is out of date).
        bool changed = false;
        if (inRoom && se.rules.valid) {
            changed = s.haveRules && (!same(&s.last, &se.rules, sizeof(s.last)) ||
                                      !same(&s.lastStages, &se.stages, sizeof(s.lastStages)));
            memcpy(&s.last, &se.rules, sizeof(s.last));
            memcpy(&s.lastStages, &se.stages, sizeof(s.lastStages));
            s.haveRules = true;
            if (changed && !host) say("Host changed the rules", false);
            s.changedNow = changed;
        } else {
            s.haveRules = false;
            s.changedNow = false;
        }

        bool menuOpen = isOpen();
        if (s.opened && menuOpen) {
            u8* rt = rulesTask();
            if (rt) StageSwitch::tick(rt);
            // Close it for a game, for a host who no longer is one, and for a viewer whose screens
            // are out of date (they can open it again).
            // (Between a set's games it stays open while that lasts, or until the set's session
            // ends and the room is waiting again: End set.)
            bool allowed = s.setOnly ? (waiting || setBetween) : waiting;
            if (!inRoom || !allowed || (s.asHost && !host) || (changed && !s.asHost)) forceClose();
        }
        if (s.opened && !menuOpen) {
            bool edits = s.asHost && !s.setOnly && host && inRoom && waiting;
            if (edits) sendEdits();                  // (with the STAGE SWITCH's hazards)
            bool stages = StageSwitch::close();
            s.opened = false;
            s.readOnly = false;
            s.setOnly = false;
            if (edits && stages) StageSwitch::send();
        }
        if (PPOM::g_block.debug.cfg & PPOM::CFG_TEST_OPEN_RULES) {
            PPOM::g_block.debug.cfg &= ~PPOM::CFG_TEST_OPEN_RULES;
            if (inRoom && (waiting || setBetween)) open(!waiting);
        }
        return menuOpen || isOpen();
    }

    // The page's input (module 18 text+0x12E0's result): directions 1 up, 2 down, 4 left, 8 right,
    // 0x10 A or START (decide), 0x20 B, 0x40 L, 0x80 R, 0x100 START, 0x200 A, 0x400 X, 0x800 Y.
    // Each page's cursor line is its +0x42 (found live, test_room_rules.py's exploration):
    //   RULES (task+0x268): 0 style, 1 stocks (score to win in time / coin), 2 stock control,
    //     3 damage ratio, 4 stage choice, 5 ITEM SWITCH, 6 MORE RULES (the bottom row, left / right
    //     between the two). Left / right change a value line; so does A (A on style cycled it).
    //   MORE RULES (+0x26C): 0 time limit, 1 team attack, 2 pause, 3 auto L-cancel, 4 input assist,
    //     5 RANDOM STAGE SWITCH.
    //   The item switch (+0x270) and the random stage switch (+0x274): A toggles, L / R presets.
    // A viewer only moves, opens the pages and goes back. The host edits every line that is a room
    // rule; the others (stock control: handicap is left out, decision 19; stage choice: the session
    // picks; score to win; the stage switch: the stage list is not a room rule yet) are read-only
    // for the host too, so nothing they change is quietly dropped.
    static const u32 IN_MOVE = 0x1 | 0x2, IN_SIDE = 0x4 | 0x8, IN_DIRS = 0xF, IN_A = 0x10 | 0x200, IN_B = 0x20;
    static const u32 RULES_STYLE = 0x66C;   // RULES' working copy: style

    extern "C" u32 pponline_rulesInput(u32 page, u32 bits)
    {
        if (!s.opened) return bits;
        u8* rt = rulesTask();
        if (!rt) return bits;
        u32 line = *(u16*)(page + 0x42);
        if (page == *(u32*)(rt + RULES_PAGE1)) {
            if (line >= 5) return bits & (IN_MOVE | IN_SIDE | IN_A | IN_B);   // the page buttons
            // STOCK CONTROL and STAGE CHOICE are the room's SET and RULESET lines (room_play.cpp)
            // (between a set's games the host keeps the SET line alone: End set)
            if (line == 2 || line == 4)
                return RoomPlay::rulesInput(line, bits, s.asHost && (!s.readOnly || (line == 2 && s.setOnly)));
            if (s.readOnly) return bits & (IN_MOVE | IN_B);
            bool stock = *(u8*)(page + RULES_STYLE) == PPOM::STYLE_STOCK;
            if (line == 2 || line == 4 || (line == 1 && !stock)) return bits & (IN_MOVE | IN_B);
            return bits;
        }
        if (page == *(u32*)(rt + RULES_PAGE2)) {
            if (line >= 5) return bits & (IN_MOVE | IN_A | IN_B);              // RANDOM STAGE SWITCH
            return s.readOnly ? bits & (IN_MOVE | IN_B) : bits;
        }
        if (page == *(u32*)(rt + RULES_ITEMS)) return s.readOnly ? bits & (IN_DIRS | IN_B) : bits;
        if (page == *(u32*)(rt + RULES_SWITCH)) return StageSwitch::input((u8*)page, bits);   // stage_switch.cpp
        return bits & (IN_DIRS | IN_B);
    }

    // module 18 text+0x15B4 `mr r3,r31` (0x806A50D8): r3 = the filtered input, the page in r30.
    // Back at text+0x15B8.
    __attribute__((naked)) void rulesInputHook()
    {
        asm volatile(
            "mr 3, 30\n\t"
            "mr 4, 31\n\t"
            "lis 12, pponline_rulesInput@ha\n\t"
            "addi 12, 12, pponline_rulesInput@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x806A\n\t"
            "ori 12, 12, 0x50DC\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    void install(CoreApi* api)
    {
        api->sySimpleHookRel(0x15B4, reinterpret_cast<void*>(rulesInputHook), 18 /* sora_menu_rule */);
    }
}
