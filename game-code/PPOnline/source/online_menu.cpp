// Online menu flow (design 5.4), built only from Brawl/P+ screens and the game's message system.
//
//   main menu PLAY ONLINE  -> the ONLINE page at once (Brawl's connect dialogs are skipped,
//                             netmenu.cpp skipConnectWindow)
//   ONLINE page            WITH FRIENDS = DIRECT -> CSS
//                          WITH ANYONE  -> BASIC VERSUS = UNRANKED, TEAM BATTLE = TEAMS -> CSS
//   CSS (sqNetAnyOkiraku)  P+'s competitive rules; status in the CSS's own rule-line window;
//                          START = search (Unranked) or connect-code entry (Direct, Teams);
//                          Z = cancel / clear error, hold Z = disconnect (Slippi);
//                          hold B (Brawl's own) = back to the ONLINE page, which sends
//                          CLEANUP_CONNECTION when the menu loads (Slippi OnMenuLoad.asm)
#include <mu/mu_msg.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "netmenu.h"

extern "C" {
    extern u8 g_onlineCss;
    extern u8 g_onlineReturnPage;
    extern u8 g_onlineFriendsCursor;
    int sprintf(char* buf, const char* fmt, ...);
}

extern "C" {
    // 1 while the player is locked in on the online CSS (searching, connecting, connected or an
    // error not yet cleared): the CSS hooks below then ignore A/B on the character.
    u8 g_onlineCssLock = 0;
    extern u8 g_onlineMatchGame;
    extern u8 g_onlinePickStage;
}

namespace OnlineMenu {

    enum Phase { PH_IDLE = 0, PH_SEARCHING = 1, PH_CONNECTING = 2, PH_CONNECTED = 3, PH_ERROR = 4 };

    struct State {
        int mode;               // PPOM::Mode, or -1 when not in the online flow
        int phase;
        char code[PPOM::CODE_LEN + 1];      // Direct/Teams: the code searched for
        char peerName[PPOM::NAME_LEN + 1];
        char peerCode[PPOM::CODE_LEN + 1];
        char error[PPOM::ERROR_LEN + 1];
        char status[128];
        bool statusRed;
        bool statusDirty;
        u32 lastButtons;
        int zHeld;
        u32 searchSeq;          // seq of the current FIND_OPPONENT (0 = no search)
        bool pollPending;       // a GET_MATCH_STATE (or the FIND_OPPONENT) awaits its answer
        u32 pollFrames;         // frames it has been waiting
        char lastScene[24];
        // the CSS rule-line message window, captured from its MuMsg::printIndex call
        MuMsg* cssMsg;
        u32 cssWindow;
        // the player's own Vs rules, put back when the online flow ends
        bool haveSavedRules;
        u8 savedRule[0x88];
        u8 savedItemFrequency;
        // Connected (the gameplay session's lobby, SESSION/LOCAL): the game this player is
        // locked in for (0 = not locked in; game 1 is locked in by the search itself).
        int lockedGame;
        // The last character and costume locked in. Brawl's Wi-Fi CSS comes back from a match or
        // the stage select with the coin in the hand (it restores no selection in Wi-Fi mode);
        // while connected, START locks in with these unless another character is picked.
        int lastCss;
        int lastCostume;
        // The name tag on the player's panel when the CSS was left (save tag index, -1 none).
        int lastTag;
    };
    static State s;

    static const u32 BTN_START = 0x1000;
    static const u32 BTN_Z = 0x0010;
    static const int DISCONNECT_HOLD_DELAY = 0x30;   // Slippi HandleInputsOnCSS.asm:14 (48 frames)
    static const int SE_BACK = 2, SE_ERROR = 3;      // Brawl menu sounds (cancel, buzzer)
    static const int LOCAL_PORT = 0;                 // the online CSS has one local player area

    // P+ resident module bases (see netmenu.cpp).
    static const u32 SEL_CHAR_TEXT = 0x806828C4;
    static const u32 SEL_CHAR_TEXT_END = 0x806A07F0;

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // gfPadSystem (g_gfPadSystem 0x805A0040, BrawlHeaders gf_pad_system.h): gfPadStatus arrays
    // of 8 pads at +0x244 (debug = "sys", what getSysPadStatus returns), +0x444 (game), +0x644
    // (menu), and the merged pads at +0x844/+0x884/+0x8C4/+0x904. Button fields are the first
    // six words (current, current2, held, pressed, released, pressed2), START = 0x1000.
    static u8* padSystem()
    {
        u32 p = *(u32*)0x805A0040;
        return isPtr(p) ? (u8*)p : NULL;
    }

    // Buttons of the four GameCube ports, read before maskStart() below: `field` 0x8 = held,
    // 0xC = pressed this frame (the pad system's own trigger, as the menus use it, so a short
    // press is not lost when a game frame spans several pad reads).
    static u32 padButtons(u32 field)
    {
        u8* ps = padSystem();
        if (!ps) return 0;
        u32 b = 0;
        for (int p = 0; p < 4; p++) {
            b |= *(volatile u32*)(ps + 0x244 + 0x40 * p + field);
        }
        return b;
    }

    // On the online CSS, START belongs to us (Slippi: lock in / search / enter code, and
    // Confirm in the code keypad). Brawl's own CSS would also act on it ("READY TO FIGHT" ->
    // leave for the stage select), so it is removed from every pad status the game reads this
    // frame. Called right after gfPadSystem::updateSystem, before any scene code runs.
    static void maskStart()
    {
        u8* ps = padSystem();
        if (!ps) return;
        for (u32 off = 0x244; off < 0x944; off += 0x40) {
            u32* f = (u32*)(ps + off);
            for (int i = 0; i < 6; i++) f[i] &= ~BTN_START;
        }
    }

    static void playSE(int id)
    {
        typedef void (*PlaySEFn)(void*, int, int, int, int, int);
        void* snd = *(void**)0x805A01D0;   // g_sndSystem
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }

    // ----------------------------------------------------------------------------------------
    // CSS objects

    static u8* cssTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        return isPtr(task) ? (u8*)task : NULL;
    }

    // muSelCharPlayerArea+0x1B8: the character on the player's coin, 0x28 = none; +0x1BC its
    // colour number. 0x29 is Random.
    static u8* playerArea()
    {
        u8* task = cssTask();
        if (!task) return NULL;
        u32 area = *(u32*)(task + 0x44 + 4 * LOCAL_PORT);
        return isPtr(area) ? (u8*)area : NULL;
    }

    static int selectedChar()
    {
        u8* area = playerArea();
        if (!area) return -1;
        int c = *(int*)(area + 0x1B8);
        return (c >= 0 && c < 0x40 && c != 0x28) ? c : -1;
    }

    static int selectedCostume()
    {
        u8* area = playerArea();
        if (!area) return 0;
        int c = *(int*)(area + 0x1BC);
        return (c >= 0 && c < 0x20) ? c : 0;
    }

    // The gmCharacterKind of a CSS id (P+'s table, through the game's own conversion); Random is
    // drawn here, so that the lock-in Dolphin sends is a real character, as Slippi resolves
    // random before it sends its selections.
    static const u8 PPLUS_ROSTER[] = {
        0x00, 0x09, 0x0D, 0x15, 0x05, 0x0C, 0x01, 0x1A, 0x0A, 0x07, 0x13, 0x25, 0x02, 0x24, 0x0E,
        0x0F, 0x14, 0x08, 0x23, 0x2E, 0x2B, 0x2C, 0x2A, 0x20, 0x03, 0x04, 0x0B, 0x19, 0x06, 0x16,
        0x1F, 0x11, 0x2D, 0x21, 0x12, 0x22, 0x10, 0x17, 0x18, 0x26, 0x27, 0x30};
    static int resolveCss(int css)
    {
        if (css != 0x29) return css;
        typedef int (*RandiFn)(int);
        int i = ((RandiFn)0x8003FC7C)((int)sizeof(PPLUS_ROSTER));
        return PPLUS_ROSTER[i % sizeof(PPLUS_ROSTER)];
    }
    static u8 charKindOf(int css)
    {
        typedef int (*ExchangeFn)(int);
        return (u8)((ExchangeFn)0x800AF80C)(css);   // muMenu::exchangeMuSelchkind2GmCharacterKind
    }

    // The character to lock in: the one on the coin, else (connected, back from a match or
    // the stage select) the last one locked in.
    static int lockChar()
    {
        int c = selectedChar();
        if (c >= 0) return c;
        return s.phase == PH_CONNECTED ? s.lastCss : -1;
    }

    // ----------------------------------------------------------------------------------------
    // Name tags and their controls (the design's port values, 5.1). The player picks a tag on the
    // CSS's own name button (muSelCharPlayerArea+0x1C8: the save's tag index, -1 none); its
    // controls go with the lock-in, and the match applies them to this player's port on both
    // machines (online_match.cpp).

    static const int TAG_SLOTS = 120;     // the save's tag records (0x78 = "no tag")
    static const u32 TAG_SIZE = 0x124;

    static u8* tagRecord(int index)
    {
        if (index < 0 || index >= TAG_SLOTS) return NULL;
        u32 gg = *(u32*)0x805A00E0;    // g_GameGlobal
        if (!isPtr(gg)) return NULL;
        u32 rec = *(u32*)(gg + 0x28);  // the save's name records
        if (!isPtr(rec)) return NULL;
        u8* t = (u8*)(rec + 0xE0 + index * TAG_SIZE);
        return *(u16*)t ? t : NULL;    // an empty name is an unused slot
    }

    static int selectedTag()
    {
        u8* area = playerArea();
        if (!area) return -1;
        int t = *(int*)(area + 0x1C8);
        return tagRecord(t) ? t : -1;
    }

    static void portValuesOf(int tag, PPOM::PortValues* pv)
    {
        memset(pv, 0, sizeof(*pv));
        u8* t = tagRecord(tag);
        if (!t) return;
        pv->flags = PPOM::PV_TAG;
        pv->rumble = t[0x0C];
        for (int i = 0; i < PPOM::TAG_CHARS; i++) pv->tag[i] = ((u16*)t)[i];
        memcpy(pv->layout, t + 0x14, PPOM::LAYOUT_SIZE);
    }

    // Slippi's lock-in (MSRB_IS_LOCAL_PLAYER_READY + SET_MATCH_SELECTIONS) into LOCAL.
    static void lockIn(int game, u16 stagePick, u8 asl)
    {
        int picked = selectedChar();
        int css = resolveCss(lockChar());
        if (css < 0) return;
        int costume = picked >= 0 ? selectedCostume() : s.lastCostume;
        int tag = picked >= 0 ? selectedTag() : s.lastTag;
        PPOM::PortValues pv;
        portValuesOf(tag, &pv);
        PPOM::writeLockIn(true, (u8)css, charKindOf(css), (u8)costume, stagePick, asl, (u8)game, &pv);
        s.lockedGame = game;
        s.lastCss = picked >= 0 ? picked : s.lastCss;
        s.lastCostume = costume;
        s.lastTag = tag;
    }
    static void unlock()
    {
        PPOM::writeLockIn(false, 0xFF, 0xFF, 0, 0xFFFF, 0, 0, NULL);
        s.lockedGame = 0;
        OnlineMatch::clearPickedStage();
    }

    // The CSS remembers the placed character (Slippi: after a match or the stage select the
    // character is still selected, coin and costume included). Brawl's Wi-Fi CSS writes no
    // selection into gmSelCharData when it leaves, and it rebuilds the player's panel from
    // gmSelCharData when it starts: the character from its per-port byte (+0x0A + 4 * port), the
    // costume, state and name tag from the player's record (+0xB8: +0x01 state, +0x05 colour,
    // +0x18 tag). So the coin is noted here when the CSS is left, and written back before the CSS
    // starts again (OnlineMatch's way back to the CSS).
    static void rememberCss()
    {
        int c = selectedChar();
        if (c < 0) return;   // coin in the hand: keep the last lock-in's
        s.lastCss = c;
        s.lastCostume = selectedCostume();
        s.lastTag = selectedTag();
    }

    void restoreCss()
    {
        if (s.mode < 0 || s.lastCss < 0) return;
        u32 gg = *(u32*)0x805A00E0;
        if (!isPtr(gg)) return;
        u32 selp = *(u32*)(gg + 0x10);   // gmSelCharData
        if (!isPtr(selp)) return;
        u8* sel = (u8*)selp;
        u8* rec = sel + 0xB8 + LOCAL_PORT * 0x5C;
        sel[0x0A + 4 * LOCAL_PORT] = (u8)s.lastCss;
        if (s.lastCss != 0x29) rec[0x00] = charKindOf(s.lastCss);
        rec[0x01] = 0;                                   // human
        rec[0x05] = (u8)s.lastCostume;                   // colour
        rec[0x18] = tagRecord(s.lastTag) ? (u8)s.lastTag : 0x78;
        PPOM::g_block.debug.scratch[12] = 0x10000 | ((u32)s.lastCss << 8) | (u8)s.lastCostume;
    }

    // Leave the CSS scene with an exit code (the scene manager's own mechanism, as every scene
    // does): 1 = go on, which sqNetAnyOkiraku turns into the stage select (state 3), where
    // online_match.cpp takes over.
    static void leaveCss(int code)
    {
        rememberCss();
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return;
        *(int*)(mgr + 0x284) = code;
        *(int*)(mgr + 0x288) = 2;
    }

    static int sessionGame()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        return se.state != PPOM::SS_NONE ? se.game : 0;
    }

    // Direct: the loser of the last game picks the stage (a draw: both pick), Slippi's
    // HandleInputsOnCSS ISWINNER_LOST.
    static bool picksStage()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        u8 me = PPOM::g_block.local.localPort;
        return s.mode == PPOM::MODE_DIRECT && se.state != PPOM::SS_NONE && se.lastWinner != 0xFF &&
               me < 4 && se.lastWinner != me;
    }

    static bool lockedForNext()
    {
        int g = sessionGame();
        return g && s.lockedGame == g;
    }

    // The CSS header models that only make sense for Brawl's Wi-Fi mode: the mode title
    // texture (MenSelchrTitleW, "HOME-RUN CONTEST" in P+'s files) and the rule numeral
    // (MenSelchrRnum1/2, the "2" of "2-minute"), whose rule text line carries our status.
    // Both are images with no text slot, so they are hidden (nwSMSetVisibility).
    static void hideHeaderArt()
    {
        static const u32 OBJS[] = {0x418 /* TitleW */, 0x158 /* Rnum1 */, 0x15C /* Rnum2 */};
        typedef void (*SetVisFn)(void* scnMdl, bool vis);
        u8* task = cssTask();
        if (!task) return;
        for (u32 i = 0; i < sizeof(OBJS) / sizeof(OBJS[0]); i++) {
            u32 obj = *(u32*)(task + OBJS[i]);
            if (!isPtr(obj)) continue;
            u32 mdl = *(u32*)(obj + 0xC);   // MuObject::m_scnMdl
            if (!isPtr(mdl)) continue;
            ((SetVisFn)0x80043D20)((void*)mdl, false);   // nwSMSetVisibility
        }
    }

    // ----------------------------------------------------------------------------------------
    // Rules (Slippi: every online mode uses the same fixed ruleset, EXI_DeviceSlippi.cpp
    // 2114-2133: 4 stocks, 8 minutes, items off, real pause only in Direct). P+'s own
    // competitive defaults are its "Default Settings Modifier" code (RSBE01.txt/NETPLAY.txt):
    // set rule 0x9017F360 = 00 00 01 00 | 04 00 0A 00 | 08 01 01 00 | 00 00 00 00 (stock, 4 stocks,
    // damage 1.0, 8-minute stock time limit, team attack on, pause on); items off is item
    // frequency 0 in the menu record (getGlobalRecordMenuDatap()[0], what the ITEM screen edits).

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

    static void saveRules()
    {
        u8* r = setRule();
        u8* m = menuRecord();
        if (!r || !m || s.haveSavedRules) return;
        memcpy(s.savedRule, r, sizeof(s.savedRule));
        s.savedItemFrequency = m[0];
        s.haveSavedRules = true;
    }

    static void restoreRules()
    {
        u8* r = setRule();
        u8* m = menuRecord();
        if (!r || !m || !s.haveSavedRules) return;
        memcpy(r, s.savedRule, sizeof(s.savedRule));
        m[0] = s.savedItemFrequency;
        s.haveSavedRules = false;
    }

    void applyRules()
    {
        static const u8 PPLUS_RULE[16] = {0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x0A, 0x00,
                                          0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        u8* r = setRule();
        u8* m = menuRecord();
        if (!r || !m) return;
        memcpy(r, PPLUS_RULE, sizeof(PPLUS_RULE));
        r[0xA] = (s.mode == PPOM::MODE_DIRECT) ? 1 : 0;   // m_allowPause
        m[0] = 0;                                          // item frequency: none
        PPOM::g_block.debug.scratch[3]++;
    }

    // ----------------------------------------------------------------------------------------
    // Status text: Slippi's CSS strings (LoadCSSText.asm:95-157) in the one text line the
    // Brawl CSS has (its rule line). Slippi shows three lines plus a hint; we show the line
    // that carries the state: "Select your character" -> "Press START to search/enter code"
    // -> "Searching for ..." -> "Connecting to ..." -> "Playing: <name>", errors in red.

    static void setStatus(const char* text, bool red)
    {
        if (strcmp(text, s.status) == 0 && red == s.statusRed) return;
        strncpy(s.status, text, sizeof(s.status) - 1);
        s.status[sizeof(s.status) - 1] = 0;
        s.statusRed = red;
        s.statusDirty = true;
    }

    static bool usesCode() { return s.mode == PPOM::MODE_DIRECT || s.mode == PPOM::MODE_TEAMS; }

    static void updateStatus()
    {
        char buf[128];
        switch (s.phase) {
        case PH_IDLE:
            if (selectedChar() < 0) {
                setStatus("Select your character", false);
            } else {
                setStatus(usesCode() ? "Press START to enter code" : "Press START to search", false);
            }
            return;
        case PH_SEARCHING:
            sprintf(buf, "Searching for %s", usesCode() && s.code[0] ? s.code : "opponent");
            setStatus(buf, false);
            return;
        case PH_CONNECTING:
            sprintf(buf, "Connecting to %s", usesCode() && s.code[0] ? s.code : "opponent");
            setStatus(buf, false);
            return;
        case PH_CONNECTED:
            // Slippi's lines when connected (LoadCSSText.asm): "Press START to lock in" /
            // "select stage", "Locked in" + "Waiting on opponent", and "Playing: <name>".
            if (lockedForNext()) {
                if (PPOM::g_block.local.remoteReady || PPOM::g_block.session.state == PPOM::SS_MATCH_READY) {
                    sprintf(buf, "Playing: %s", s.peerName);
                    setStatus(buf, false);
                } else {
                    setStatus("Waiting on opponent", false);
                }
            } else if (lockChar() < 0) {
                setStatus("Select your character", false);
            } else if (picksStage() && OnlineMatch::pickedStage() == 0xFFFF) {
                setStatus("Press START to select stage", false);
            } else {
                setStatus("Press START to lock in", false);
            }
            return;
        case PH_ERROR:
        default:
            setStatus(s.error[0] ? s.error : "Error", true);
            return;
        }
    }

    // The rule line is a fixed-width, right-aligned window: MuMsg::printf does not apply the
    // msbin's style tags, so set the colour (black as the original line, red for errors).
    static void printCssStatus()
    {
        if (s.statusRed) {
            s.cssMsg->setFontColor(s.cssWindow, 255, 0, 0, 255);   // Slippi's error red FF0000FF
        } else {
            s.cssMsg->setFontColor(s.cssWindow, 0, 0, 0, 255);
        }
        if (PPOM::g_block.debug.cfg & PPOM::CFG_CSS_AUTOWIDTH) s.cssMsg->setFontWidthModeAuto(s.cssWindow);
        s.cssMsg->printf(s.cssWindow, "%s", s.status);
    }

    // ----------------------------------------------------------------------------------------

    // ----------------------------------------------------------------------------------------
    // Lock the character while locked in (Slippi PreventAPressCharUnselect.asm and
    // PreventBPressCharUnselect.asm: on the online CSS, while MSRB_IS_LOCAL_PLAYER_READY, i.e.
    // from the lock-in that starts a search until CLEANUP_CONNECTION clears it, A and B on the
    // character do nothing, nor does a costume change (PreventColorChange.asm); holding B to
    // leave still works). Brawl's CSS handles one player's
    // pad in sel_char+0x6FFC (pad = getSysPadStatus copy at sp+0x48):
    //   +0x726C  r29 = B pressed this frame; +0x74F4.. uses it to take the coin back to the hand
    //   +0x75B4  hand over the character grid: +0x8500 handles A (drop the held coin on a
    //            character, or pick up a placed coin)
    // Holding B to leave is the hand's own counter (sel_char+0x1B794, held B from the sys pad),
    // which is not touched, and A on LEAVE and the other buttons goes through +0x7A94/+0x883C.

    // +0x726C `rlwinm r29,r0,23,31,31`: B pressed, unless locked. Back to +0x7270.
    __attribute__((naked)) void cssBPressed()
    {
        asm volatile(
            "rlwinm 29, 0, 23, 31, 31\n\t"
            "lis 12, g_onlineCssLock@ha\n\t"
            "lbz 12, g_onlineCssLock@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "beq 1f\n\t"
            "li 29, 0\n\t"
            "1:\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x9B34\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // +0x7280 `cmpwi r27,0`: r27/r3 are the two costume buttons (X/Y on the GameCube pad,
    // sel_char+0x19508/+0x19604); +0x7290..+0x74F0 changes the costume. Slippi blocks that too
    // while locked in (PreventColorChange.asm): go straight to the B part at +0x74F4.
    __attribute__((naked)) void cssCostume()
    {
        asm volatile(
            "lis 12, g_onlineCssLock@ha\n\t"
            "lbz 12, g_onlineCssLock@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "bne 1f\n\t"
            "cmpwi 27, 0\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x9B48\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "1:\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x9DB8\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // +0x75B4 `mr r3,r24`, the start of `coinAPress(task, port, pad)`'s call: skip the call
    // (to +0x75E8, the function's exit) when locked, else carry on at +0x75B8.
    __attribute__((naked)) void cssCoinAPress()
    {
        asm volatile(
            "lis 12, g_onlineCssLock@ha\n\t"
            "lbz 12, g_onlineCssLock@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "bne 1f\n\t"
            "mr 3, 24\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x9E7C\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "1:\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x9EAC\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    static bool lockedIn()
    {
        // Connected: locked while locked in for the next game (between games the character can
        // change until START locks it in again, as on Slippi).
        if (s.phase == PH_CONNECTED) return lockedForNext() || g_onlineMatchGame != 0;
        return s.phase == PH_SEARCHING || s.phase == PH_CONNECTING || s.phase == PH_ERROR;
    }

    void install(CoreApi* api)
    {
        CodeEntry::install(api);
        OnlineMatch::install(api);
        api->sySimpleHookRel(0x726C, reinterpret_cast<void*>(cssBPressed), 10 /* sora_menu_sel_char */);
        api->sySimpleHookRel(0x7280, reinterpret_cast<void*>(cssCostume), 10);
        api->sySimpleHookRel(0x75B4, reinterpret_cast<void*>(cssCoinAPress), 10);
    }

    void enter(int mode)
    {
        saveRules();
        s.mode = mode;
        s.phase = PH_IDLE;
        s.cssMsg = NULL;
        s.zHeld = 0;
        s.code[0] = 0;
        s.lastButtons = 0xFFFFFFFF;   // ignore buttons held from the menu
        s.searchSeq = 0;
        s.pollPending = false;
        s.status[0] = 0;
        unlock();
        s.lastCss = -1;
        s.lastCostume = 0;
        s.lastTag = -1;
        g_onlineMatchGame = 0;
        g_onlinePickStage = 0;
        g_onlineCss = 1;
        // Leaving the CSS goes back to the page the mode was picked on (netmenu.cpp
        // exitToOnlinePage): Direct to the ONLINE page, Unranked/Teams to WITH ANYONE.
        // Direct (BASIC VERSUS) goes back to WITH FRIENDS' page (sqMenuMain entry 0x1C reopens
        // muProcWifiAnybody with BASIC VERSUS highlighted; Teams' sqNetAnyTeamMelee returns
        // with 0x1D, TEAM BATTLE highlighted), and from there B lands on WITH FRIENDS; Unranked
        // and Ranked go back to the ONLINE page (0x1F) with WITH ANYONE highlighted.
        bool friends = mode == PPOM::MODE_DIRECT || mode == PPOM::MODE_TEAMS;
        g_onlineReturnPage = friends ? 0 : 0x1F;
        g_onlineFriendsCursor = friends ? 1 : 0;
        PPOM::g_block.debug.menuState = (u32)(mode + 1);
    }

    static void leave()
    {
        s.mode = -1;
        s.phase = PH_IDLE;
        s.searchSeq = 0;
        s.pollPending = false;
        s.cssMsg = NULL;
        unlock();
        g_onlineMatchGame = 0;
        g_onlinePickStage = 0;
        g_onlineCss = 0;
        restoreRules();
        PPOM::g_block.debug.menuState = 0;
    }

    static void startSearch()
    {
        PPOM::FindOpponent req;
        memset(&req, 0, sizeof(req));
        req.mode = (u8)s.mode;
        int c = selectedChar();
        req.lockedChar = c < 0 ? 0xFF : (u8)c;
        req.costume = (u8)selectedCostume();
        PPOM::asciiToU16(req.code, s.code, PPOM::CODE_LEN);
        // The search locks the player in for game 1 (Slippi FN_LOCK_IN_AND_SEARCH); the session
        // gets the lock-in from LOCAL as soon as it exists.
        lockIn(1, 0xFFFF, 0);
        s.searchSeq = PPOM::post(PPOM::CMD_FIND_OPPONENT, &req, sizeof(req));
        s.pollPending = true;   // Dolphin answers FIND_OPPONENT with the match state
        s.pollFrames = 0;
        s.phase = PH_SEARCHING;
    }

    int currentMode() { return s.mode; }

    void codeEntered(const char* code)
    {
        strncpy(s.code, code, PPOM::CODE_LEN);
        s.code[PPOM::CODE_LEN] = 0;
        if (usesCode() && s.phase == PH_IDLE && s.code[0]) startSearch();
    }

    static void cleanup()
    {
        PPOM::post(PPOM::CMD_CLEANUP_CONNECTION, NULL, 0);
        unlock();
        s.phase = PH_IDLE;
        s.searchSeq = 0;
        s.pollPending = false;
        s.zHeld = 0;
    }

    static void onMatchState(const PPOM::MatchState& m)
    {
        int before = s.phase;
        PPOM::u16ToAscii(s.peerName, m.peerName, sizeof(s.peerName));
        PPOM::u16ToAscii(s.peerCode, m.peerCode, sizeof(s.peerCode));
        switch (m.mmState) {
        case PPOM::MM_IDLE:
            // The search or connection ended without an error (cleanup, peer gone): back to
            // the idle prompt, as Slippi's CSS does (design 5.6).
            s.phase = PH_IDLE;
            s.searchSeq = 0;
            unlock();
            break;
        case PPOM::MM_INITIALIZING:
        case PPOM::MM_MATCHMAKING:
            s.phase = PH_SEARCHING;
            break;
        case PPOM::MM_OPPONENT_CONNECTING:
            s.phase = PH_CONNECTING;
            break;
        case PPOM::MM_CONNECTION_SUCCESS:
            s.phase = PH_CONNECTED;
            break;
        case PPOM::MM_ERROR:
        default:
            s.phase = PH_ERROR;
            PPOM::u16ToAscii(s.error, m.errorText, sizeof(s.error));
            break;
        }
        // Slippi's CSS sounds (HandleInputsOnCSS.asm:39-78): error on entering the error
        // state, back when a connection goes away.
        if (s.phase == PH_ERROR && before != PH_ERROR) playSE(SE_ERROR);
        if (before == PH_CONNECTED && s.phase != PH_CONNECTED && s.phase != PH_ERROR) playSE(SE_BACK);
    }

    // Connected on the CSS (Slippi HandleInputsOnCSS.asm HANDLE_CONNECTED): lock in with START
    // (Direct's loser first picks a stage on the stage select), and once SESSION has the setup
    // of the game this player is locked in for, leave the CSS for that match.
    static void tickConnected(u32 pressed)
    {
        if (s.phase != PH_CONNECTED || g_onlineMatchGame) return;
        int game = sessionGame();
        if (!game) return;
        if (s.lockedGame == game) {
            const PPOM::Session& se = PPOM::g_block.session;
            if (se.state == PPOM::SS_MATCH_READY && se.game == game) {
                g_onlineMatchGame = (u8)game;
                PPOM::g_block.debug.scratch[10] = 0x100 * game;   // tests: the match started
                leaveCss(1);
            }
            return;
        }
        if (OnlineMatch::pickedStage() != 0xFFFF && lockChar() >= 0) {
            // Back from the stage select: locked in with the stage (ExitSSSUponStageSelect).
            lockIn(game, OnlineMatch::pickedStage(), OnlineMatch::pickedAsl());
            return;
        }
        if (!(pressed & BTN_START) || lockChar() < 0) return;
        if (picksStage()) {
            g_onlinePickStage = 1;
            leaveCss(1);
        } else {
            lockIn(game, 0xFFFF, 0);
        }
    }

    static void pollMailbox()
    {
        const PPOM::Response* r = PPOM::pollResponse();
        if (!r) return;
        // Responses to different commands share the one slot: route them by command.
        if (r->cmd == PPOM::CMD_FETCH_CODE_SUGGESTION) {
            CodeEntry::onSuggestion(*r);
            return;
        }
        if (r->cmd == PPOM::CMD_GET_MATCH_STATE) {
            // The answer to FIND_OPPONENT or to one of the polls that follow it.
            if (s.mode >= 0 && s.searchSeq && r->seq >= s.searchSeq) {
                s.pollPending = false;
                onMatchState(*(const PPOM::MatchState*)r->payload);
            }
        }
    }

    // Slippi's CSS asks for the match state every frame while it searches or is connected
    // (GET_MATCH_STATE). One poll at a time: the next goes out once Dolphin has answered.
    static void pollMatchState()
    {
        if (s.mode < 0 || !s.searchSeq) return;
        if (s.phase != PH_SEARCHING && s.phase != PH_CONNECTING && s.phase != PH_CONNECTED) return;
        if (s.pollPending && ++s.pollFrames < 60) return;   // re-ask if an answer got lost
        PPOM::post(PPOM::CMD_GET_MATCH_STATE, NULL, 0);
        s.pollPending = true;
        s.pollFrames = 0;
    }

    void tick()
    {
        AnyoneMenu::tick();
        const char* scene = Online::currentSceneName();
        // In a match: nothing that differs between the machines may change the game while the
        // match runs under rollback, so no mailbox here (OnlineMatch::tickMatch only reacts to
        // a disconnect, which Dolphin reports after the rollback session has ended).
        if (strcmp(scene, "scMelee") == 0) {
            OnlineMatch::tickMatch();
            return;
        }
        bool sceneChanged = strcmp(scene, s.lastScene) != 0;
        if (sceneChanged) {
            strncpy(s.lastScene, scene, sizeof(s.lastScene) - 1);
            if (strcmp(scene, "muMenuMain") == 0) {
                // Slippi sends CLEANUP_CONNECTION whenever the menus load (OnMenuLoad.asm:73-92):
                // backing out of the CSS cancels a search or disconnects. The menu also asks
                // for the account (GET_ONLINE_STATUS, Slippi's lock state).
                if (s.mode >= 0) leave();
                PPOM::post(PPOM::CMD_CLEANUP_CONNECTION, NULL, 0);
                PPOM::post(PPOM::CMD_GET_ONLINE_STATUS, NULL, 0);
            }
        }
        pollMailbox();
        pollMatchState();

        bool onCss = g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0;
        g_onlineCssLock = (onCss && lockedIn()) ? 1 : 0;
        if (!onCss) {
            PPOM::g_block.debug.scratch[1] = 0;
            return;
        }

        hideHeaderArt();
        u32 b = padButtons(0x8);
        u32 pressed = padButtons(0xC);
        if (s.lastButtons == 0xFFFFFFFF) {
            // ignore buttons still held from the menu until they are released
            pressed = 0;
            if (!b) s.lastButtons = 0;
        }
        maskStart();
        if (CodeEntry::active()) {
            CodeEntry::tick(pressed & BTN_START);
        } else {
            // START: lock in and search (Unranked) or enter the code (Direct, Teams), once a
            // character is selected (Slippi shows the START prompt only then).
            if ((pressed & BTN_START) && s.phase == PH_IDLE && selectedChar() >= 0) {
                if (usesCode()) {
                    CodeEntry::open(LOCAL_PORT);
                } else {
                    startSearch();
                }
            }
            // Z (HandleInputsOnCSS.asm:172-205, 539-561): press to cancel a search or clear an
            // error, hold 48 frames to disconnect.
            if (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING || s.phase == PH_ERROR) {
                if (pressed & BTN_Z) {
                    cleanup();
                    playSE(SE_BACK);
                }
            } else if (s.phase == PH_CONNECTED) {
                if (b & BTN_Z) {
                    if (++s.zHeld > DISCONNECT_HOLD_DELAY) {
                        cleanup();
                        playSE(SE_BACK);
                    }
                } else {
                    s.zHeld = 0;
                }
                tickConnected(pressed);
            }
        }
        g_onlineCssLock = lockedIn() ? 1 : 0;
        PPOM::g_block.debug.scratch[1] = (u32)g_onlineCssLock | ((u32)s.phase << 4);   // harness/tests
        updateStatus();
        if (s.statusDirty && s.cssMsg) {
            printCssStatus();
            s.statusDirty = false;
        }
    }

    // Called from the printIndex hook for every message line; returns a replacement or NULL.
    const char* cssLine(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller)
    {
        if (s.mode < 0 || !g_onlineCss) return NULL;
        if (caller < SEL_CHAR_TEXT || caller >= SEL_CHAR_TEXT_END) return NULL;
        // The rule line: "-minute KO fest!" / "-stock Smash!" / ... / "-score KO fest!"
        if (!(Online::msbinLineContains(msbin, (int)line, "fest!") ||
              Online::msbinLineContains(msbin, (int)line, "Smash!") ||
              Online::msbinLineContains(msbin, (int)line, "'til you drop") ||
              Online::msbinLineContains(msbin, (int)line, "beat 'em up"))) {
            return NULL;
        }
        s.cssMsg = msg;
        s.cssWindow = window;
        s.statusDirty = true;   // re-printed by the next tick with our colour
        // (returning " " here froze the display on the CSS: presents stopped; keep real text)
        updateStatus();
        return s.status[0] ? s.status : "Select your character";
    }

    struct Init {
        Init() { s.mode = -1; s.lastScene[0] = 0; }
    };
    static Init s_init;
}

namespace NetMenu {
    void onlineMenuEntered(int mode) { OnlineMenu::enter(mode); }
}

namespace Text {
    struct Relabel {
        const char* scene;   // only in this scene
        const char* match;   // substring of the original message line
        const char* text;    // replacement
    };

    // Slippi's own descriptions (SdMenu.usd patch, OnMenuPrep.asm:511-520): "Online Play"
    // 0x644, Ranked 0x645, Unranked 0x646, Direct 0x647, Teams 0x64B. Brawl groups the modes in
    // two pages: WITH FRIENDS = the code-based ones (Direct, Teams), WITH ANYONE = matchmaking
    // (Unranked, Ranked: anyone_menu.cpp, which prints those two pages' lines).
    static const Relabel RELABELS[] = {
        // main menu PLAY ONLINE = Slippi's 1P-menu "Online Play"
        {"muMenuMain", "Play different modes online", "Compete against online opponents."},
        // ONLINE page (button art stays WITH FRIENDS / WITH ANYONE)
        {"muMenuMain", "registered as Friends", "Play a specific person."},
        {"muMenuMain", "Play against random people", "Compete against online opponents."},
        // WITH FRIENDS page (Brawl's WITH ANYONE page; button art stays BASIC VERSUS / TEAM BATTLE)
        {"muMenuMain", "quick fight with someone", "Play a specific person."},
        {"muMenuMain", "Form a team with someone", "Play teams games."},
    };

    const char* overrideFor(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller)
    {
        const char* css = OnlineMenu::cssLine(msg, window, line, msbin, caller);
        if (css) return css;
        const char* anyone = AnyoneMenu::line(msg, window, line, msbin);
        if (anyone) return anyone;
        const char* scene = Online::currentSceneName();
        for (u32 i = 0; i < sizeof(RELABELS) / sizeof(RELABELS[0]); i++) {
            const Relabel& r = RELABELS[i];
            if (strcmp(scene, r.scene) != 0) continue;
            if (Online::msbinLineContains(msbin, (int)line, r.match)) return r.text;
        }
        return NULL;
    }
}
