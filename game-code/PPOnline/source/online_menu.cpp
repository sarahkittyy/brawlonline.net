// Online menu flow (design 5.4), built only from Brawl/P+ screens and the game's message system.
//
//   main menu PLAY ONLINE  -> the ONLINE page at once (Brawl's connect dialogs are skipped,
//                             netmenu.cpp skipConnectWindow)
//   ONLINE page            WITH FRIENDS = DIRECT -> CSS
//                          WITH ANYONE  -> BASIC VERSUS = UNRANKED, TEAM BATTLE = TEAMS -> CSS
//   CSS (sqNetAnyOkiraku)  P+'s competitive rules; status in the CSS's own rule-line window;
//                          START = search (Unranked) or connect-code entry (Direct, Teams);
//                          Z or B = cancel / clear error, hold Z = disconnect (Slippi);
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
        char ownCode[PPOM::CODE_LEN + 1];   // this player's connect code (GET_ONLINE_STATUS)
        int appState;                       // GET_ONLINE_STATUS state (-1 = no answer yet)
        u32 statusFrames;                   // frames since the menus last asked for it
        bool ownShown;                      // the own-code window is attached on this CSS
        char ownPrinted[24];                // what that window shows
        char rankText[24];                  // Ranked: the rating (GET_RANK), "" until known
        u32 rankFrames;                     // Ranked: frames until GET_RANK is asked again
        PPOM::GameStep step;                // Ranked: the game setup step (GP_FETCH_STEP)
        char stepText[64];
        bool stepPending;
        u32 stepFrames;
        u32 stepSeq;                        // Ranked: seq of the last GP_FETCH_STEP sent
        u32 stepAnswered;                   // seq the step in `step` answers
        u32 stepFloor;                      // stepSeq the last time the game was off the CSS
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
        // the window's own vertical extent and line spacing (restored for one-line texts)
        bool haveWindow;
        float windowY1, windowY2, windowLineSpace;
        // the player's own settings, put back when the online flow ends (saveRules)
        bool haveSavedRules;
        u8 savedRule[0x88];
        u8 savedItemFrequency;
        u8 savedItemSwitch[8];
        u8 savedHazard;
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
        // The controller port (0-3) that last pressed START on the CSS: the lock-in carries it,
        // and the match plays the local player from that controller.
        u8 startPad;
    };
    static State s;

    static const u32 BTN_START = 0x1000;
    static const u32 BTN_Z = 0x0010;
    static const u32 BTN_B = 0x0200;
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

    // The lowest port whose pressed field (0xC) has `mask` this frame, -1 none.
    static int pressedPort(u32 mask)
    {
        u8* ps = padSystem();
        if (!ps) return -1;
        for (int p = 0; p < 4; p++) {
            if (*(volatile u32*)(ps + 0x244 + 0x40 * p + 0xC) & mask) return p;
        }
        return -1;
    }

    // On the online CSS, START belongs to us (Slippi: lock in / search / enter code, and
    // Confirm in the code keypad). Brawl's own CSS would also act on it ("READY TO FIGHT" ->
    // leave for the stage select), so it is removed from every pad status the game reads this
    // frame. Called right after gfPadSystem::updateSystem, before any scene code runs.
    static void maskButtons(u32 mask)
    {
        u8* ps = padSystem();
        if (!ps) return;
        for (u32 off = 0x244; off < 0x944; off += 0x40) {
            u32* f = (u32*)(ps + off);
            for (int i = 0; i < 6; i++) f[i] &= ~mask;
        }
    }
    static void maskStart() { maskButtons(BTN_START); }

    // Takes `mask` out of this frame's presses only (pressed, pressed2), so a press the plugin
    // has used does nothing else, while the held bits (Brawl's hold-B-to-leave counter) stay.
    static void maskPressed(u32 mask)
    {
        u8* ps = padSystem();
        if (!ps) return;
        for (u32 off = 0x244; off < 0x944; off += 0x40) {
            u32* f = (u32*)(ps + off);
            f[3] &= ~mask;
            f[5] &= ~mask;
        }
    }

    // P+'s Code Menu opens with L + R + D-pad Down on the CSS, the stage select and in a match.
    // It holds settings that change the match (Special Modes, per-player codes incl. a character
    // switch to Giga Bowser / Wario-Man, Debug Mode...), none of which is the player's choice
    // online. Its control code (P+ "Control Code Menu", a hook at 0x80029574, the end of
    // gfPadSystem::updateLow on the pad thread) reads the pads itself and opens the menu in the
    // same pass: state word 0x804E0034 = 4, the menus' freeze flag 0x805B8A08 = 1 (its old value
    // kept at 0x804E006C), 0x805B6DF8 kept at 0x804E0074. Online its activation is OFF
    // (codeMenuOff), so it does not open. Should one open anyway, anywhere in the online flow
    // (CSS, stage select, match), it is closed again on the main loop's next tick, the way its B
    // closes it (found live: the freeze flag and 0x805B6DF8 put back, 0x804E0074 cleared,
    // state 0). (A Code Menu opened offline keeps its settings; docs/game-code.md §6.)
    static void blockCodeMenu()
    {
        volatile u32* state = (volatile u32*)0x804E0034;
        if (*state != 4) return;
        *(volatile u32*)0x805B8A08 = *(volatile u32*)0x804E006C;
        u32 kept = *(volatile u32*)0x804E0074;
        if (kept) *(volatile u32*)0x805B6DF8 = kept;
        *(volatile u32*)0x804E0074 = 0;
        *state = 0;
        PPOM::g_block.debug.scratch[15] += 0x1000000u;   // tests: Code Menu opens refused
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

    // P+'s hold-shield slots (docs/brawl-memory-map.md): 0x36 Wario-Man and 0x38 Giga Bowser are
    // not played online in any mode; they lock in as Wario and Bowser. 0x37 (solo Popo) stays.
    static int plainCss(int css)
    {
        if (css == 0x36) return 0x15;
        if (css == 0x38) return 0x0C;
        return css;
    }

    // SESSION's characters come from the other player's machine. Only what this CSS could lock in
    // reaches the match setup: the roster, plus solo Popo (P+'s hold-shield slot 0x37).
    bool selectableCharKind(int kind)
    {
        if (kind < 0 || kind > 0xFF) return false;
        for (u32 i = 0; i < sizeof(PPLUS_ROSTER); i++) {
            if (charKindOf(PPLUS_ROSTER[i]) == kind) return true;
        }
        return charKindOf(0x37) == kind;
    }

    // The character to lock in: the one on the coin, else (connected, back from a match or
    // the stage select) the last one locked in.
    static int lockChar()
    {
        int c = selectedChar();
        if (c >= 0) return plainCss(c);
        return s.phase == PH_CONNECTED ? plainCss(s.lastCss) : -1;
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
        PPOM::writeLockIn(true, (u8)css, charKindOf(css), (u8)costume, stagePick, asl, (u8)game, &pv,
                          s.startPad);
        s.lockedGame = game;
        s.lastCss = picked >= 0 ? picked : s.lastCss;
        s.lastCostume = costume;
        s.lastTag = tag;
    }
    static void unlock()
    {
        PPOM::writeLockIn(false, 0xFF, 0xFF, 0, 0xFFFF, 0, 0, NULL, s.startPad);
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

    // Direct (and the code-based rooms): the loser of the last game picks the stage, Slippi's
    // HandleInputsOnCSS ISWINNER_LOST. Dolphin decides who (SESSION picksStage): 1v1 the loser
    // (a draw: both), teams the losing team's lower port, a free-for-all the last place (a tie:
    // the lower port); docs/nplayer/setup.md.
    static bool picksStage()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        u8 me = PPOM::g_block.local.localPort;
        return (s.mode == PPOM::MODE_DIRECT || s.mode == PPOM::MODE_TEAMS) &&
               se.state != PPOM::SS_NONE && me < 4 && se.players[me].picksStage;
    }

    static bool lockedForNext()
    {
        int g = sessionGame();
        return g && s.lockedGame == g;
    }

    // The CSS header models that only make sense for Brawl's Wi-Fi mode: the mode title
    // texture (MenSelchrTitleW, "HOME-RUN CONTEST" in P+'s files) and the rule numeral
    // (MenSelchrRnum1/2, the "2" of "2-minute"), whose rule text line carries our status.
    // Both are images with no text slot, so they are hidden (nwSMSetVisibility). So are the
    // ITEM and STAGE buttons at the right end of the bar (MenSelchrState0005 / 0006, found live
    // by hiding the CSS's models one at a time): they open P+'s item and stage switches, which
    // are locked online (cssSwitchButtons below), and the status line takes their place.
    static void hideHeaderArt()
    {
        static const u32 OBJS[] = {0x418 /* TitleW */, 0x158 /* Rnum1 */, 0x15C /* Rnum2 */,
                                   0x420 /* State0005: ITEM */, 0x424 /* State0006: STAGE */,
                                   0x3C4 /* Ready: the READY TO FIGHT banner (cssReadyBanner) */};
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

    // The stage select's data (GameGlobal+0x14): +0x25 is P+'s hazard switch for the picked stage
    // (0 = hazards on, P+'s stage select default; Z on a stage, or the hazard switch for a random
    // pick, sets 1 = off; P+ Net-StageFiles.asm muSelectStageTask::pointPointer and dispPreview).
    // sqVsMelee's match setup copies it into the match (gmGlobalModeMelee+0x29 bit 0x20, P+
    // Random.asm hook at 0x806DCEE8), outside the setup the session compares at its barrier.
    static u8* stageSelData()
    {
        u32 gg = *(u32*)0x805A00E0;
        if (!isPtr(gg)) return NULL;
        u32 st = *(u32*)(gg + 0x14);
        return isPtr(st) ? (u8*)st : NULL;
    }

    static const int HAZARD = 0x25;
    static const int ITEM_SWITCH = 8;   // menu record +0x818: the ITEM SWITCH screen's switches
    // P+ v3.2's item switch after a fresh boot (its save's default; frequency 0 = items off).
    static const u8 PPLUS_ITEM_SWITCH[8] = {0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    // P+'s Code Menu (pf/menu3/dnet.cmnu, loaded at boot to 0x804E0000, 0x2520 bytes) keeps its
    // settings in its own lines, which P+'s codes read directly: the Special Modes (Random
    // Angle, War, Big Head, flight, the gameplay modifiers: hitstun, hitlag, SDI, shields,
    // staling, jumpsquat...), each player's codes (character select incl. transformations,
    // infinite shield, percent select, input buffer, automatic L-cancelling), Debug Mode and
    // its displays, Alternate Stages, Tag-Based Costumes, Endless Friendlies and more. None of
    // it is in the match setup the gameplay session compares, so a value set offline would
    // reach an online match on one machine only. Online every line holds its default (the
    // file's value, which is P+'s competitive setup): each value line keeps its value at +8 and
    // its default at +0x10 (fudgepop's Code Menu layout: +0 size u16, +2 type 0 selection /
    // 1 integer / 2 float, +6 the text's offset; a selection line has its options' base, a
    // pointer into the menu, at +0x18; integer and float lines have their text at +0x20). The
    // lines are found by that layout (all 85 value lines of P+ v3.2's menu, checked against a
    // dump; docs/game-code.md §6); their values are saved on the way in and put back when the
    // menus load, as the set rule.
    static const u32 CODE_MENU = 0x804E0000, CODE_MENU_SIZE = 0x2520;
    static const int CODE_MENU_LINES = 96;   // P+ v3.2 has 85; the Syringe heap has no room for more
    static u32 s_codeMenuSaved[CODE_MENU_LINES];
    static int s_codeMenuSavedCount = -1;
    // The defaults as the menu file has them, read once at boot: a line's +0x10 is not always
    // fixed (the per-player Character Select lines keep the current character there during a
    // match, found live), so the online values come from this copy.
    static u32 s_codeMenuDefault[CODE_MENU_LINES];
    static int s_codeMenuDefaults = 0;

    static bool codeMenuLine(u32 p)
    {
        u8* l = (u8*)p;
        u32 size = *(u16*)l, type = l[2], text = l[6];
        if ((size & 3) || size < 0x20 || size > 0x400 || p + size > CODE_MENU + CODE_MENU_SIZE) return false;
        if (type > 2 || text < 0x1C || text >= size) return false;
        u32 options = *(u32*)(l + 0x18);
        if (type == 0 ? (options < CODE_MENU || options >= CODE_MENU + CODE_MENU_SIZE) : text != 0x20) return false;
        const char* t = (const char*)l + text;
        if (!((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z'))) return false;
        for (u32 i = 0; text + i < size && t[i]; i++) {
            if (t[i] == '%') return true;
            if ((u8)t[i] < 0x20 || (u8)t[i] >= 0x7F) return false;
        }
        return false;
    }

    // mode 0: save the values, 1: put the defaults in, 2: put the saved values back,
    // 3: read the defaults (at boot).
    static int codeMenuLines(int mode)
    {
        int n = 0;
        for (u32 p = CODE_MENU; p + 0x20 <= CODE_MENU + CODE_MENU_SIZE && n < CODE_MENU_LINES; p += 4) {
            if (!codeMenuLine(p)) continue;
            u32* value = (u32*)(p + 8);
            if (mode == 0) s_codeMenuSaved[n] = *value;
            else if (mode == 1) *value = n < s_codeMenuDefaults ? s_codeMenuDefault[n] : *(u32*)(p + 0x10);
            else if (mode == 2) { if (n < s_codeMenuSavedCount) *value = s_codeMenuSaved[n]; }
            else s_codeMenuDefault[n] = *(u32*)(p + 0x10);
            n++;
        }
        return n;
    }

    // P+'s "Code Menu Activation" line (0 Default, 1 PM 3.6: not in a match, 2 OFF) is the only
    // thing that stops the Code Menu from opening. Its control code runs at the end of
    // gfPadSystem::updateLow (0x80029574), on Brawl's pad thread, and reads this machine's own
    // controller: when it opens it writes the menus' freeze flag (0x805B8A08), its state and
    // the held buttons at a time that is not tied to the game frame. blockCodeMenu closes it on
    // the next tick of the main loop, but a game frame or a savestate of the rollback session
    // that falls in between sees it on this machine only: spamming L + R + D-pad Down desynced
    // online matches under rollback (test_code_menu_combo_spam_keeps_the_match_in_sync). With
    // the line OFF its control code never opens it (Net-CodeMenu.asm, loc_0x145). Online the
    // line is OFF; the player's own value comes back with the other lines (restoreRules).
    static void codeMenuOff()
    {
        static const char ACTIVATION[] = "Code Menu Activation";
        for (u32 p = CODE_MENU; p + 0x20 <= CODE_MENU + CODE_MENU_SIZE; p += 4) {
            if (!codeMenuLine(p) || ((u8*)p)[2] != 0) continue;
            if (strncmp((const char*)p + ((u8*)p)[6], ACTIVATION, sizeof(ACTIVATION) - 1) != 0) continue;
            *(u32*)(p + 8) = 2;
            return;
        }
    }

    // Every frame until the Code Menu is loaded (at boot, before any match): its defaults.
    static void readCodeMenuDefaults()
    {
        if (s_codeMenuDefaults <= 0) s_codeMenuDefaults = codeMenuLines(3);
    }

    static void saveRules()
    {
        u8* r = setRule();
        u8* m = menuRecord();
        u8* st = stageSelData();
        if (!r || !m || !st || s.haveSavedRules) return;
        memcpy(s.savedRule, r, sizeof(s.savedRule));
        s.savedItemFrequency = m[0];
        memcpy(s.savedItemSwitch, m + ITEM_SWITCH, sizeof(s.savedItemSwitch));
        s.savedHazard = st[HAZARD];
        s_codeMenuSavedCount = codeMenuLines(0);
        s.haveSavedRules = true;
    }

    static void restoreRules()
    {
        u8* r = setRule();
        u8* m = menuRecord();
        u8* st = stageSelData();
        if (!r || !m || !st || !s.haveSavedRules) return;
        memcpy(r, s.savedRule, sizeof(s.savedRule));
        m[0] = s.savedItemFrequency;
        memcpy(m + ITEM_SWITCH, s.savedItemSwitch, sizeof(s.savedItemSwitch));
        st[HAZARD] = s.savedHazard;
        codeMenuLines(2);
        s.haveSavedRules = false;
    }

    // The online ruleset, the same on every machine whatever was set offline: the set rule's 16
    // bytes P+'s code writes (P+'s competitive defaults above), pause only in Direct, item
    // frequency 0 with P+'s default item switch, hazards on (also for Direct's loser's pick: its
    // hazard toggle is not part of the setup the other machine gets), every Code Menu line at
    // its default but its activation, which is OFF (codeMenuOff). Written when the Wi-Fi
    // sequence starts (in place of Brawl's Wi-Fi rules) and again by every online match setup,
    // right before sqVsMelee's setup reads them (OnlineMatch::setupMatch), so a setting changed
    // in between (or left over from offline play) never reaches an online match.
    void applyRules()
    {
        static const u8 PPLUS_RULE[16] = {0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x0A, 0x00,
                                          0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        u8* r = setRule();
        u8* m = menuRecord();
        if (!r || !m) return;
        u8 time = r[3], stocks = r[4], stockTime = r[8];
        // (only the 16 bytes P+ writes: the set rule's size is not known, what follows may be other data)
        memcpy(r, PPLUS_RULE, sizeof(PPLUS_RULE));
        if (PPOM::g_block.debug.cfg & PPOM::CFG_TEST_RULES) {
            r[3] = time;            // tests: shorter games (harness ppom.test_rules)
            r[4] = stocks;
            r[8] = stockTime;
        }
        r[0xA] = (s.mode == PPOM::MODE_DIRECT) ? 1 : 0;   // m_allowPause
        m[0] = 0;                                          // item frequency: none
        memcpy(m + ITEM_SWITCH, PPLUS_ITEM_SWITCH, sizeof(PPLUS_ITEM_SWITCH));
        u8* st = stageSelData();
        if (st) st[HAZARD] = 0;                            // hazards on
        int lines = codeMenuLines(1);                      // every Code Menu line at its default
        codeMenuOff();                                     // except its activation: OFF
        PPOM::g_block.debug.scratch[3] = ((u32)lines << 16) | ((PPOM::g_block.debug.scratch[3] + 1) & 0xFFFF);
    }

    // ----------------------------------------------------------------------------------------
    // Status text: Slippi's CSS strings (LoadCSSText.asm:95-157) in the one text line the
    // Brawl CSS has (its rule line). Slippi shows three lines plus a hint; we show the line
    // that carries the state: "Select your character" -> "Press START to search/enter code"
    // -> "Searching for ..." -> "Connecting to ..." -> "Playing: <name>", errors in red.

    static void setStatus(const char* text, bool red)
    {
        // Texts from Dolphin, the server and the opponent's name: printable ASCII only (the
        // message system reads control bytes as commands; u16ToAscii already made the rest '?').
        char clean[sizeof(s.status)];
        int n = 0;
        for (; text[n] && n < (int)sizeof(clean) - 1; n++) {
            u8 c = (u8)text[n];
            clean[n] = (c < 0x20 || c >= 0x7F) ? ' ' : (char)c;
        }
        clean[n] = 0;
        text = clean;
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
            // Ranked's game setup has its own line (whose turn it is, the opponent's character).
            if (rankedStep() && s.step.active && s.stepText[0] && !lockedForNext()) {
                setStatus(s.stepText, false);
                return;
            }
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

    // The status line: the rule line's own MuMsg window (MenSelchrRule's textN0), left-aligned,
    // whose font narrows to fit the window (MuMsg width mode 0). Long texts used to be squeezed
    // unreadably narrow (a 68-character server error at about a third of the font's width), so
    // the line is laid out here and never relies on the narrowing:
    //   - the window is widened over the whole bar: to the left over the hidden "2" rule numeral
    //     and mode title, to the right over the hidden ITEM / STAGE buttons (up to the screen's
    //     safe area). It then fits about 675 font units (about 38 average characters; 21 "W"s)
    //     at the font's normal size, against 352 for the rule line's own window;
    //   - a text that fits is printed as one line at the normal size;
    //   - a longer one is wrapped at a space onto two lines at 0.7 of the size (both lines inside
    //     the bar, the font's proportions kept), and if even that is too long, the second line
    //     is cut at a space and ends with "...".
    // The width is measured with the window's own font (ut::Font::GetGlyph through the Message,
    // the advance ms::CharWriter::Print uses; match_hud.cpp), in font units.
    //
    // MuMsg+0xC is its window settings (0x48 bytes each): +0x00 flags, +0x04..+0x10 the window
    // rect (x1, y1, x2, y2; y up), +0x2C line spacing (-1 = the font's), +0x30/+0x34 the font
    // scale x/y (MuMsg::beginPrint applies them with Message::setScale).
    struct WindowSetting {
        u32 flags;
        float x1, y1, x2, y2;
        u8 _14[0x2C - 0x14];
        float lineSpace;
        float scaleX, scaleY;
        u8 _38[0x48 - 0x38];
    };

    static const float STATUS_X1 = -318.0f;     // the rule window's own: -208 .. 144
    static const float STATUS_X2 = 330.0f;
    static const float WRAP_SCALE = 0.7f;
    static const float WRAP_LINE_SPACE = 22.0f; // two lines at 0.7 inside the bar
    static const float WRAP_Y = 27.0f;          // the rule window's own is +-18.4
    // Font units that fit in the widened window at scale 1: at scale 1 a font unit is a window
    // unit (measured live, docs/game-code.md §6); a few units of margin.
    static const int STATUS_FIT = 640;

    static WindowSetting* cssWindowSetting()
    {
        if (!s.cssMsg) return NULL;
        u32 ws = *(u32*)((u8*)s.cssMsg + 0xC);
        if (!isPtr(ws) || s.cssWindow >= *(u32*)((u8*)s.cssMsg + 0x10)) return NULL;
        return (WindowSetting*)(ws + 0x48 * s.cssWindow);
    }

    // Width of `text` in the status font's units, and the advance of each character into `adv`.
    static int statusWidths(const char* text, int len, int* adv)
    {
        void* message = s.cssMsg ? s.cssMsg->m_message : NULL;
        void* font = message ? *(void**)((u8*)message + 0x48) : NULL;
        if (!isPtr((u32)font) || !isPtr(*(u32*)font)) font = NULL;
        int w = 0;
        for (int i = 0; i < len; i++) {
            int a = 30;   // no font yet: a typical glyph
            if (font) {
                typedef void (*GetGlyphFn)(void* font, void* out, u16 ch);
                GetGlyphFn getGlyph = (GetGlyphFn)(*(u32*)(*(u32*)font + 0x50));
                u8 glyph[0x40];
                memset(glyph, 0, sizeof(glyph));
                getGlyph(font, glyph, (u16)(u8)text[i]);
                a = (s8)glyph[6];
            }
            if (adv) adv[i] = a;
            w += a;
        }
        return w;
    }

    // Layouts of the status line (layoutStatus).
    enum { LAYOUT_ONE = 1, LAYOUT_TWO = 2, LAYOUT_ONE_SMALL = 3 };

    // Lay the status out into `out` (room for 164 bytes): one line at the normal size if it
    // fits; else two lines at WRAP_SCALE, split at the space that makes the longer line
    // shortest; a single word too long for the normal size: one small line; a text too long
    // even for two small lines: the first line as much as fits (cut at a space), the second
    // cut at a space and ended with "...".
    static int layoutStatus(const char* text, char* out, int fit)
    {
        static const int MAX = 159;
        int adv[MAX];
        int len = (int)strlen(text);
        if (len > MAX) len = MAX;
        int total = statusWidths(text, len, adv);
        memcpy(out, text, len);
        out[len] = 0;
        if (total <= fit) return LAYOUT_ONE;
        int wide = (int)(fit / WRAP_SCALE);
        // Balanced split: line 1 = [0, i), line 2 = (i, len) for a space at i.
        int best = -1, bestMax = 0x7FFFFFFF, left = 0;
        for (int i = 0; i < len; i++) {
            if (text[i] == ' ') {
                int right = total - left - adv[i];
                int m = left > right ? left : right;
                if (left <= wide && right <= wide && m < bestMax) {
                    best = i;
                    bestMax = m;
                }
            }
            left += adv[i];
        }
        if (best >= 0) {
            out[best] = '\n';
            return LAYOUT_TWO;
        }
        if (total <= wide) return LAYOUT_ONE_SMALL;
        // Too long: fill line 1 (cut at a space), then line 2 with "...".
        int w = 0, a = 0, cut = -1;
        while (a < len && w + adv[a] <= wide) {
            if (text[a] == ' ') cut = a;
            w += adv[a++];
        }
        if (cut > 0 && a < len) a = cut;
        int o = a;
        out[o++] = '\n';
        int r = a;
        while (r < len && text[r] == ' ') r++;
        int room = wide - statusWidths("...", 3, NULL);
        int b = r;
        w = 0;
        cut = -1;
        while (b < len && w + adv[b] <= room) {
            if (text[b] == ' ') cut = b;
            w += adv[b++];
        }
        if (b < len && cut > r) b = cut;
        while (b > r && (text[b - 1] == ' ' || text[b - 1] == '.' || text[b - 1] == ',')) b--;
        memcpy(out + o, text + r, b - r);
        o += b - r;
        memcpy(out + o, "...", 4);
        return LAYOUT_TWO;
    }

    // The player's own connect code (Slippi's CSS shows it in Direct: LoadCSSText.asm "Set to 2
    // to display connect code (for direct only)"; ours in both code-based modes, Direct and
    // Teams), so that it can be told to a friend: at the top between LEAVE and the status line,
    // in the rule line's font. It is window 1 of the rule line's MuMsg (muSelCharTask+0x5C4,
    // 8 windows, only window 0 used), attached to the same node (MenSelchrRule textN0, with
    // MuMsg::attachScnMdlSimple as the CSS attaches window 0, at the same size) and placed left
    // of the bar, right-aligned against it. The code comes from GET_ONLINE_STATUS (Dolphin
    // reads it from user.json), which the menus ask for.
    // In Ranked the same window shows the player's rating instead (Slippi's CSS shows the rank
    // there): the Elo number and, after a set, its change, e.g. "1523 (+14)".
    static const u32 OWN_WINDOW = 1;
    static const float OWN_X1 = -640.0f, OWN_X2 = -395.0f;
    static const char* ownText()
    {
        if (usesCode()) return s.ownCode;
        if (s.mode == PPOM::MODE_RANKED) return s.rankText;
        return "";
    }
    static void showOwnCode()
    {
        const char* text = ownText();
        if (!s.cssMsg || !text[0]) return;
        if (s.ownShown) {
            if (strcmp(text, s.ownPrinted) != 0) {
                s.cssMsg->printf(OWN_WINDOW, "%s", text);
                strncpy(s.ownPrinted, text, sizeof(s.ownPrinted) - 1);
            }
            return;
        }
        MuMsg* m = s.cssMsg;
        u8* task = cssTask();
        u32 obj = task ? *(u32*)(task + 0x150) : 0;   // MenSelchrRule, the rule line's model
        u32 mdl = isPtr(obj) ? *(u32*)(obj + 0x10) : 0;
        u8* message = (u8*)m->m_message;
        u32 bufs = isPtr((u32)message) ? *(u32*)(message + 0x1D8) : 0;
        if (!isPtr(mdl) || !isPtr(bufs) || *(u32*)((u8*)m + 0x10) <= OWN_WINDOW) return;
        float size = *(float*)(*(u32*)bufs + 0x28);   // window 0's (Message::attachMsgBuf)
        typedef void (*AttachFn)(MuMsg*, u32, u32 scnMdl, u32 node, float size);
        ((AttachFn)0x800B8C90)(m, OWN_WINDOW, mdl, 0, size);   // MuMsg::attachScnMdlSimple
        WindowSetting* ws = (WindowSetting*)(*(u32*)((u8*)m + 0xC) + 0x48 * OWN_WINDOW);
        ws->x1 = OWN_X1;
        ws->x2 = OWN_X2;
        if (s.haveWindow) {
            ws->y1 = s.windowY1;
            ws->y2 = s.windowY2;
        }
        m->setAlignMode(OWN_WINDOW, MuMsg::Align_Right);
        m->setFontColor(OWN_WINDOW, 0xFF, 0xFF, 0xFF, 0xFF);
        m->printf(OWN_WINDOW, "%s", text);
        strncpy(s.ownPrinted, text, sizeof(s.ownPrinted) - 1);
        s.ownShown = true;
        PPOM::g_block.debug.scratch[12] |= 0x80000000u;   // tests: the own code is shown
    }

    // MuMsg::printf does not apply the msbin's style tags, so set the colour (black as the
    // original line, red for errors).
    static void printCssStatus()
    {
        if (s.statusRed) {
            s.cssMsg->setFontColor(s.cssWindow, 255, 0, 0, 255);   // Slippi's error red FF0000FF
        } else {
            s.cssMsg->setFontColor(s.cssWindow, 0, 0, 0, 255);
        }
        WindowSetting* ws = cssWindowSetting();
        char text[164];
        int layout = LAYOUT_ONE;
        if (ws) {
            if (!s.haveWindow) {
                s.haveWindow = true;
                s.windowY1 = ws->y1;
                s.windowY2 = ws->y2;
                s.windowLineSpace = ws->lineSpace;
            }
            ws->x1 = STATUS_X1;
            ws->x2 = STATUS_X2;
            layout = layoutStatus(s.status, text, STATUS_FIT);
            bool two = layout == LAYOUT_TWO;
            ws->scaleX = ws->scaleY = layout == LAYOUT_ONE ? 1.0f : WRAP_SCALE;
            ws->lineSpace = two ? WRAP_LINE_SPACE : s.windowLineSpace;
            ws->y1 = two ? WRAP_Y : s.windowY1;
            ws->y2 = two ? -WRAP_Y : s.windowY2;
        } else {
            strncpy(text, s.status, sizeof(text) - 1);
            text[sizeof(text) - 1] = 0;
        }
        // tests: the layout << 24 | the text's width in font units
        PPOM::g_block.debug.scratch[14] = ((u32)layout << 24) |
                                          ((u32)statusWidths(s.status, (int)strlen(s.status), NULL) & 0xFFFFFF);
        s.cssMsg->printf(s.cssWindow, "%s", text);
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

    // The CSS's ITEM and STAGE buttons (hand buttons 0x19 and 0x1A) open P+'s item switch
    // (frequency and item switches) and its random stage switch with the hazard switch, all of
    // which change the match on the machine where they are set. Online nothing in the rules is
    // the player's choice (Slippi), so on the online CSS they are hidden (hideHeaderArt) and A
    // where they were does nothing.
    // muSelCharTask::buttonProcInAllArea, sel_char+0x7CCC `cmpwi r29,0x19` (r29 = the button):
    // 0x19 -> open the item switch (+0x105D8 with 1), 0x1A -> the stage switch (with 2), else on
    // at +0x7D10. Ours: the two buttons go to the function's exit (+0x8334) instead.
    extern "C" void pponline_cssLockedButton()
    {
        PPOM::g_block.debug.scratch[15] = (PPOM::g_block.debug.scratch[15] & ~0xFFu) |
                                          ((PPOM::g_block.debug.scratch[15] + 1) & 0xFF);
    }
    __attribute__((naked)) void cssSwitchButtons()
    {
        asm volatile(
            "lis 12, g_onlineCss@ha\n\t"
            "lbz 12, g_onlineCss@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "beq 1f\n\t"
            "cmpwi 29, 0x19\n\t"
            "beq 2f\n\t"
            "cmpwi 29, 0x1a\n\t"
            "beq 2f\n\t"
            "1:\n\t"
            "cmpwi 29, 0x19\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0xA594\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "2:\n\t"
            "lis 12, pponline_cssLockedButton@ha\n\t"
            "addi 12, 12, pponline_cssLockedButton@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0xABF8\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // The READY TO FIGHT banner. Once every player on the CSS has a character, Brawl shows it
    // (muSelCharTask+0x510 bit 0x80, model MenSelchrReady at +0x3C4) and the hand over it is
    // hand target 5: A there (as START anywhere) starts the CSS's own countdown (state 1),
    // setToGlobal and the scene's exit, and sqNetAnyOkiraku then opens Brawl's network stage
    // vote, which hangs the game (the user's crash, 2026-10-08). Online START is ours (maskStart)
    // and the match starts from SESSION; the banner is hidden (hideHeaderArt) and never hit:
    // the hand's hit test, sel_char+0xDDB0 `cmpwi r0,1` (the banner shown?), goes to its "not
    // over the banner" branch (+0xDE3C). OnlineMatch::onSelStage turns any other way out of the
    // CSS back to the CSS.
    __attribute__((naked)) void cssReadyBanner()
    {
        asm volatile(
            "lis 12, g_onlineCss@ha\n\t"
            "lbz 12, g_onlineCss@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "beq 1f\n\t"
            "lis 12, 0x8069\n\t"
            "ori 12, 12, 0x0700\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "1:\n\t"
            "cmpwi 0, 1\n\t"
            "lis 12, 0x8069\n\t"
            "ori 12, 12, 0x0678\n\t"
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
        api->sySimpleHookRel(0x7CCC, reinterpret_cast<void*>(cssSwitchButtons), 10);
        api->sySimpleHookRel(0xDDB0, reinterpret_cast<void*>(cssReadyBanner), 10);
    }

    void enter(int mode)
    {
        saveRules();
        codeMenuOff();
        s.mode = mode;
        s.phase = PH_IDLE;
        s.rankText[0] = 0;
        s.rankFrames = 0;   // Ranked: ask for the rating at once
        s.cssMsg = NULL;
        s.zHeld = 0;
        s.code[0] = 0;
        s.startPad = 0;
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
        req.lockedChar = c < 0 ? 0xFF : (u8)plainCss(c);
        req.costume = (u8)selectedCostume();
        PPOM::asciiToU16(req.code, s.code, PPOM::CODE_LEN);
        // The search locks the player in for game 1 (Slippi FN_LOCK_IN_AND_SEARCH); the session
        // gets the lock-in from LOCAL as soon as it exists.
        lockIn(1, 0xFFFF, 0);
        s.searchSeq = PPOM::post(PPOM::CMD_FIND_OPPONENT, &req, sizeof(req));
        s.pollPending = true;   // Dolphin answers FIND_OPPONENT with the match state
        s.pollFrames = 0;
        // A stage select left after the last connection ended (Direct's pick) leaves its stage
        // behind; it is not this connection's.
        OnlineMatch::clearPickedStage();
        s.phase = PH_SEARCHING;
    }

    int currentMode() { return s.mode; }

    // Slippi's HandleOnlineLockedOptions.asm: logged out (0) or with an update required (2),
    // Ranked, Unranked, Direct and Teams are locked. Not before Dolphin has answered.
    bool modesLocked() { return s.appState >= 0 && s.appState != 1; }

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

    // Ranked: the step in `s.step` was asked for on this CSS. One asked for before (on the stage
    // select, before a match) can be out of date: a pick just made, a game just played.
    static bool stepFresh()
    {
        return s.stepAnswered > s.stepFloor;
    }

    // Connected on the CSS (Slippi HandleInputsOnCSS.asm HANDLE_CONNECTED): lock in with START
    // (Direct's loser first picks a stage on the stage select), and once SESSION has the setup
    // of the game this player is locked in for, leave the CSS for that match.
    static void tickConnected(u32 pressed)
    {
        if (s.phase != PH_CONNECTED || g_onlineMatchGame) return;
        int game = sessionGame();
        if (!game) return;
        const bool ranked = s.mode == PPOM::MODE_RANKED;
        if (ranked && !stepFresh()) return;
        if (ranked && s.step.active && s.step.toSss) {
            // Ranked: both players are on the stage select for the strikes / ban / pick, also
            // when they come back to the CSS before it is done.
            g_onlinePickStage = 1;
            leaveCss(1);
            return;
        }
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
        if (ranked && s.step.active && s.step.mayLock && s.step.timeUp && lockChar() >= 0) {
            // Ranked: the character step's time is up (Slippi takes the character selected), or
            // after a draw the characters stay.
            lockIn(game, 0xFFFF, 0);
            return;
        }
        if (!(pressed & BTN_START) || lockChar() < 0) return;
        if (ranked && !(s.step.active && s.step.mayLock)) return;
        if (picksStage()) {
            g_onlinePickStage = 1;
            leaveCss(1);
        } else {
            lockIn(game, 0xFFFF, 0);
        }
    }

    // Ranked: the rating, rounded ("1523"), and after a set its change ("1523 (+14)").
    static void onRank(const PPOM::RankInfo& r)
    {
        if (r.state == PPOM::RANK_UNKNOWN || !(r.rating > -100000.0f && r.rating < 100000.0f)) {
            s.rankText[0] = 0;
            return;
        }
        int rating = (int)(r.rating + (r.rating < 0 ? -0.5f : 0.5f));
        if (r.hasChange && r.change > -100000.0f && r.change < 100000.0f) {
            int change = (int)(r.change + (r.change < 0 ? -0.5f : 0.5f));
            sprintf(s.rankText, "%d (%c%d)", rating, change < 0 ? '-' : '+', change < 0 ? -change : change);
        } else {
            sprintf(s.rankText, "%d", rating);
        }
    }

    // Ranked, connected: Slippi's GP_FETCH_STEP every frame (one at a time), on the CSS and the
    // stage select, so turns and strikes show at once.
    static void pollStep()
    {
        if (s.mode != PPOM::MODE_RANKED || s.phase != PH_CONNECTED) {
            s.step.active = 0;
            s.stepPending = false;
            return;
        }
        if (s.stepPending && ++s.stepFrames < 60) return;
        s.stepSeq = PPOM::post(PPOM::CMD_GP_FETCH_STEP, NULL, 0);
        s.stepPending = true;
        s.stepFrames = 0;
    }

    // Ranked: the last step (active or not: an inactive step means the stage is decided, or the
    // connection is gone, so a ranked stage select leaves). NULL in every other mode.
    const PPOM::GameStep* rankedStep()
    {
        return s.mode == PPOM::MODE_RANKED ? &s.step : NULL;
    }

    const char* rankedText() { return s.stepText; }

    // Ranked: GET_RANK once a second on the CSS, so the rating appears as soon as Dolphin has it
    // and the change shows up once the server has rated a finished set.
    static void pollRank()
    {
        if (s.mode != PPOM::MODE_RANKED) return;
        if (s.rankFrames > 0) {
            s.rankFrames--;
            return;
        }
        PPOM::post(PPOM::CMD_GET_RANK, NULL, 0);
        s.rankFrames = 60;
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
        if (r->cmd == PPOM::CMD_GET_RANK) {
            onRank(*(const PPOM::RankInfo*)r->payload);
            return;
        }
        if (r->cmd == PPOM::CMD_GP_FETCH_STEP) {
            memcpy(&s.step, r->payload, sizeof(s.step));
            if (s.step.nKinds > sizeof(s.step.kinds)) s.step.nKinds = sizeof(s.step.kinds);
            PPOM::u16ToAscii(s.stepText, s.step.text, sizeof(s.stepText));
            s.stepPending = false;
            s.stepAnswered = r->seq;
            // Not connected (any more): no step, whatever the answer was.
            if (s.phase != PH_CONNECTED) s.step.active = 0;
            return;
        }
        if (r->cmd == PPOM::CMD_GET_ONLINE_STATUS) {
            // The account (Dolphin reads user.json): keep the connect code for the CSS header.
            const PPOM::OnlineStatus& st = *(const PPOM::OnlineStatus*)r->payload;
            char code[PPOM::CODE_LEN + 1];
            PPOM::u16ToAscii(code, st.code, sizeof(code));
            int n = 0;
            if (st.state == 1) {
                for (; code[n]; n++) {
                    char ch = code[n];
                    bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                              ch == '#';
                    if (!ok) {
                        n = 0;   // not a connect code: show none
                        break;
                    }
                }
            }
            memcpy(s.ownCode, code, n);
            s.ownCode[n] = 0;
            s.appState = st.state;
            u32& lockDebug = PPOM::g_block.debug.scratch[7];
            lockDebug = (lockDebug & ~0xFFu) | (u8)(st.state + 1);   // tests: the state seen
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
            s.stepFloor = s.stepSeq;
            // The Code Menu's activation is OFF online (codeMenuOff), so it does not open; this
            // closes one that opened anyway (a menu file without the line).
            if (g_onlineCss && s.mode >= 0) blockCodeMenu();
            OnlineMatch::tickMatch();
            return;
        }
        OnlineMatch::offMatch();
        readCodeMenuDefaults();
        bool sceneChanged = strcmp(scene, s.lastScene) != 0;
        if (sceneChanged) {
            strncpy(s.lastScene, scene, sizeof(s.lastScene) - 1);
            // The status line's window belongs to the CSS that just ended (its MuMsg is freed
            // with the scene): forget it until the next CSS prints its rule line (cssLine).
            // Using it later read the freed MuMsg's font and hung the game after a Direct set's
            // second game (found with test_direct_set_under_the_gameplay_session).
            if (strcmp(scene, "scSelctCharacter") != 0) {
                s.cssMsg = NULL;
                s.haveWindow = false;
                s.ownShown = false;
            }
            if (strcmp(scene, "muMenuMain") == 0) {
                // Slippi sends CLEANUP_CONNECTION whenever the menus load (OnMenuLoad.asm:73-92):
                // backing out of the CSS cancels a search or disconnects. The menu also asks
                // for the account (GET_ONLINE_STATUS, Slippi's lock state).
                if (s.mode >= 0) leave();
                PPOM::post(PPOM::CMD_CLEANUP_CONNECTION, NULL, 0);
                PPOM::post(PPOM::CMD_GET_ONLINE_STATUS, NULL, 0);
                s.statusFrames = 0;
            }
        }
        // Dolphin logs in by itself once the launcher writes user.json (Slippi's watcher), so
        // the menus ask again every second: the online modes unlock without leaving the page.
        if (strcmp(scene, "muMenuMain") == 0 && ++s.statusFrames >= 60) {
            PPOM::post(PPOM::CMD_GET_ONLINE_STATUS, NULL, 0);
            s.statusFrames = 0;
        }
        pollMailbox();
        pollMatchState();
        pollStep();

        bool onCss = g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0;
        if (!onCss) s.stepFloor = s.stepSeq;   // only a step asked for on the CSS counts there
        g_onlineCssLock = (onCss && lockedIn()) ? 1 : 0;
        if (g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelStage") == 0) {
            // Direct's loser picks the stage on P+'s stage select. The pick is the stage and its
            // alternate (L/R); P+'s hazard toggle (Z on a stage or on Random) is a rule, which the
            // match setup forces on both machines, so Z is taken out here; so is the Code Menu.
            // Ranked's stage select is Slippi's game setup screen, which nothing leaves before
            // the steps are done: START (P+'s random pick) and B (back to the CSS) are taken out
            // too. Taking them out of buttonProc's buttons (stage_legal.cpp) is not enough: the
            // stage select reads them elsewhere as well, and left for the CSS mid-strike.
            u32 mask = BTN_Z;
            if (s.mode == PPOM::MODE_RANKED && g_onlinePickStage) mask |= BTN_START | BTN_B;
            maskButtons(mask);
            blockCodeMenu();
        }
        if (onCss) blockCodeMenu();
        if (strcmp(scene, "scSelctCharacter") != 0 && !CodeEntry::active()) CodeEntry::forgetLabels();
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
        if (pressed & BTN_START) {
            int p = pressedPort(BTN_START);
            if (p >= 0) s.startPad = (u8)p;
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
            // error, hold 48 frames to disconnect. B cancels and clears as well (Slippi's B does
            // nothing while locked in); that press is used up, so the coin stays where it is and
            // the next B takes it back. Holding on still leaves the CSS.
            if (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING || s.phase == PH_ERROR) {
                if (pressed & (BTN_Z | BTN_B)) {
                    if (pressed & BTN_B) maskPressed(BTN_B);
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
        pollRank();
        showOwnCode();
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
        if (msg != s.cssMsg || window != s.cssWindow) s.haveWindow = s.ownShown = false;   // a new CSS
        s.cssMsg = msg;
        s.cssWindow = window;
        s.statusDirty = true;   // re-printed by the next tick with our colour
        // (returning " " here froze the display on the CSS: presents stopped; keep real text)
        updateStatus();
        return s.status[0] ? s.status : "Select your character";
    }

    struct Init {
        Init() { s.mode = -1; s.lastScene[0] = 0; s.appState = -1; }
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
