// Online menu flow (design 5.4), built only from Brawl/P+ screens and the game's message system.
//
//   main menu PLAY ONLINE  -> the ONLINE page at once (Brawl's connect dialogs are skipped,
//                             netmenu.cpp skipConnectWindow)
//   ONLINE page            WITH FRIENDS = DIRECT -> CSS
//                          WITH ANYONE  -> BASIC VERSUS = UNRANKED, TEAM BATTLE = TEAMS -> CSS
//   CSS (sqNetAnyOkiraku)  P+'s competitive rules; status in the CSS's own rule-line window;
//                          START = search (Unranked) or connect-code entry (Direct, Teams);
//                          Z or B = cancel / clear error; hold B (Brawl's own) = back to the
//                          ONLINE page, which sends CLEANUP_CONNECTION when the menu loads
//                          (Slippi OnMenuLoad.asm): the only way to disconnect (no hold Z)
#include <mu/mu_msg.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "training_room.h"
#include "ppom.h"
#include "netmenu.h"
#include "room_play.h"
#include "stage_switch.h"
#include "room_rules.h"
#include "rules_button.h"
#include "labels.h"

extern "C" {
    extern u8 g_onlineCss;
    extern u8 g_onlineReturnPage;
    extern u8 g_onlineFriendsCursor;
    extern u8 g_cssPicHold;   // room_css.cpp showTeamAt
    int sprintf(char* buf, const char* fmt, ...);
}

extern "C" {
    // 1 while the player is locked in on the online CSS (searching, connecting, connected or an
    // error not yet cleared): the CSS hooks below then ignore A/B on the character.
    u8 g_onlineCssLock = 0;
    // A team room's CSS (tickRoom): A on the local panel picks the next team (cssPanelClick
    // notes it in g_cssPanelTeam, tickRoom sends it).
    u8 g_cssTeamRoom = 0;
    u8 g_cssPanelTeam = 0;
    extern u8 g_onlineMatchGame;
    extern u8 g_onlinePickStage;
}

namespace OnlineMenu {

    enum Phase { PH_IDLE = 0, PH_SEARCHING = 1, PH_CONNECTING = 2, PH_CONNECTED = 3, PH_ERROR = 4 };

    struct State {
        int mode;               // PPOM::Mode, or -1 when not in the online flow
        int phase;
        char code[PPOM::CODE_LEN + 1];      // Direct/Teams: the code searched for
        bool codePreset;                    // Direct: `code` is the launcher's invite (LOCAL
                                            // directJoin): the first START searches it, no keypad
        char peerName[PPOM::NAME_LEN + 1];
        char peerCode[PPOM::CODE_LEN + 1];
        char error[PPOM::ERROR_LEN + 1];
        char ownCode[PPOM::CODE_LEN + 1];   // this player's connect code (GET_ONLINE_STATUS)
        char ownName[RoomCss::NAME_CHARS + 1];   // and display name (printable ASCII)
        int appState;                       // GET_ONLINE_STATUS state (-1 = no answer yet)
        u32 statusFrames;                   // frames since the menus last asked for it
        bool ownShown;                      // the own-code window is attached on this CSS
        bool roomBtnsShown;                 // the room's settings windows are attached
        char roomBtnPrinted[3][40];         // what they show
        s8 roomBtnHover;                    // the one the hand is on (-1 none)
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
        char quitNotice[PPOM::NAME_LEN + 8];   // "<name> quit": the last game ended by a quit
        u32 quitNoticeFrames;               // frames left to show it on the CSS
        u8 quitNoticePort;                  // whose quit it is
        bool quitHold;                      // Ranked / a room's set: on the CSS after a quit until
                                            // every player pressed START (quitHolding)
        u32 lastButtons;
        u32 startGuard;         // frames START presses are dropped after the keypad (see codeEntered)
        bool cssSeen;           // the online CSS was up last frame (holdBGuard)
        bool bGuard;            // B still held from before this CSS: taken out until released
        bool startOnLanding;    // START pressed while the coin flew to its place (startWithCoinUp)
        bool startDropHeld;     // the START that put the coin down is still held
        u32 roomLeaveFrames;    // frames on the CSS since the search started for the room
        u32 roomHoldStart;      // the waiting room: frames START has been held (hold START cancels)
        bool roomStartUp;       // START was up once in the room (the CSS's search press is not a hold)
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
        bool roomStepSss;   // a room's set: on the stage select for GameSetup's steps
        u8 savedRule[0x88];
        u8 savedItemFrequency;
        u8 savedItemSwitch[8];
        u8 savedHazard;
        // Connected (the gameplay session's lobby, SESSION/LOCAL): the game this player is
        // locked in for (0 = not locked in; game 1 is locked in by the search itself).
        int lockedGame;
        // The stage of that lock-in: 0xFFFF none, PPOM::STAGE_PENDING the loser's (the stage
        // select comes once every player is locked in), else the stage picked there.
        u16 lockStage;
        u8 lockedStocks;   // the stock count the lock-in carries (stock control), 0 none
        // The last character and costume locked in. Brawl's Wi-Fi CSS comes back from a match or
        // the stage select with the coin in the hand (it restores no selection in Wi-Fi mode);
        // while connected, START locks in with these unless another character is picked.
        int lastCss;
        int lastCostume;
        // The name tag on the player's panel (save tag index, -1 none), kept from one online
        // CSS to the next (every mode, every room) and given to each new one (prepareCss). Until
        // the player has been on an online CSS this boot it is the tag of the last lock-in of an
        // earlier boot, which Dolphin keeps (OnlineStatus lastTag, found by its name).
        int lastTag;
        bool tagChosen;     // lastTag is from this boot's CSS, not Dolphin's
        // The controller port (0-3) that last pressed START on the CSS: the lock-in carries it,
        // and the match plays the local player from that controller.
        u8 startPad;
        // P+'s alt costume (PPOM::COSTUME_ALT_Z / _R, 0 none) held on that START, the CSS's
        // own (not the keypad's); every lock-in carries it (altHeld).
        u8 startAlt;
        // Rooms (RoomEntry; MODE_TEAMS): how the room CSS was entered, and what Dolphin says.
        int roomEntry;
        bool roomCreateSent;
        bool roomPollPending;
        u32 roomPollFrames;
        u8 roomPhase;                       // RoomStatus.phase of the last answer
        bool roomError;
        char roomText[96];                  // the status line Dolphin gave (ASCII)
        u8 roomTaken;                       // SLOT_TAKEN bits seen (sounds)
        bool roomSeen;                      // roomTaken is from this room's view
        bool roomReload;                    // a launcher join on an online CSS: build it again
        int roomKickPort;                   // the host holds A on this occupied slot (-1 none)
        int roomKickHeld;                   // for this many frames
        bool roomRHeld;                     // the host's R is down: TEAMS on its release (tickRoom)
        // A replay (PPOM v8, the playback Dolphin): this CSS plays the replays Dolphin puts in
        // SESSION, each one's match once (playbackPending), and takes no input.
        bool playback;
        bool playbackPending;
    };
    static State s;
    bool roomCss();
    static bool roomStockControl();
    static u8 s_headerPress = 0;   // the game's press on ITEM (0x19) / STAGE (0x1A) on a room's CSS
    // A press that sat the player out or in (START, A) does nothing more until it is let go: the
    // pad's pressed field can still have it on the next frame, when the room may already say
    // otherwise (with Dolphin answering at once, START played again and then readied).
    static u32 s_sitPressHeld = 0;
    void u16Name(char* out, const u16* in);   // a name from SESSION / Dolphin: printable ASCII

    static const u32 BTN_START = 0x1000;
    static const u32 BTN_Z = 0x0010;
    static const u32 BTN_B = 0x0200;
    static const int KICK_HOLD_DELAY = 0x30;         // a room host's hold A to remove a player (48 frames)
    static const u32 START_GUARD_FRAMES = 45;        // START ignored after a room code (codeEntered)
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

    // Holding B on the CSS is Brawl's own way out (the hand's counter, see cssBPressed): back to
    // the ONLINE page, which leaves a set or a room. A B still held from the scene before must not
    // count: a ranked stage select takes B out (it never leaves before the steps are done), and
    // a player who held it there while the opponent picked was on the CSS with B held when the
    // stage select left by itself, and the counter took them out of the set (harness, 2026-10-12).
    // So a B down when the online CSS comes up is taken out of every pad until it is released;
    // a new hold leaves as before. The current field (+0x0) is the fresh read, before any mask.
    static void holdBGuard()
    {
        const bool down = (padButtons(0x0) & BTN_B) != 0;
        if (!s.cssSeen) {
            s.cssSeen = true;
            s.bGuard = down;
        } else if (!down) {
            s.bGuard = false;
        }
        if (s.bGuard) maskButtons(BTN_B);
    }

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

    // Adds `mask` to this frame's presses (pressed, pressed2) of one controller port, in each of
    // its pad statuses (sys, game, menu), as if it had been pressed there.
    static void addPressed(int port, u32 mask)
    {
        u8* ps = padSystem();
        if (!ps || port < 0 || port > 3) return;
        for (u32 off = 0x244; off < 0x844; off += 0x200) {
            u32* f = (u32*)(ps + off + 0x40 * port);
            f[3] |= mask;
            f[5] |= mask;
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

    // The character only once the coin is put down: with the coin in the hand (+0x1F8) or flying
    // to its place (+0x1F9), +0x1B8 is the portrait the hand is over, and START readied the player
    // with whatever the hand was passing (the user's, 2026-10-18).
    static int selectedChar()
    {
        u8* area = playerArea();
        if (!area) return -1;
        if (area[0x1F8] || area[0x1F9]) return -1;
        int c = *(int*)(area + 0x1B8);
        return (c >= 0 && c < 0x40 && c != 0x28) ? c : -1;
    }

    // P+'s "Hold Z for AltZ Characters, R/C for AltR Characters" (LegacyTE/AltCostume.asm, hook
    // 0x8068478C) reads the buttons of the player's controller when its CSS is left: Z AltZ, else R
    // AltR; a Nunchuk Z / C; a Wii Remote alone none. The online CSS never leaves that way (P+'s
    // flags are written by the match setup from SESSION, online_match.cpp), so the buttons are read
    // here, on the START that locks in, from that controller (gfPadStatus +0x0, the fresh read P+
    // uses), with the controller type P+ takes from the player's area (+0x1E0: 2 Wii Remote,
    // 3 Nunchuk, else GameCube / Classic).
    static u8 altHeld(int port)
    {
        u8* ps = padSystem();
        if (!ps || port < 0 || port > 3) return 0;
        u32 b = *(volatile u32*)(ps + 0x244 + 0x40 * port);
        u8* area = playerArea();
        int type = area ? *(volatile int*)(area + 0x1E0) : 0;
        if (type == 2) return 0;
        if (b & 0x10) return PPOM::COSTUME_ALT_Z;
        if (b & (type == 3 ? 0x400u : 0x20u)) return PPOM::COSTUME_ALT_R;
        return 0;
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
    // the stage select) the last one locked in. A room is never PH_CONNECTED: there it is the
    // room's lock-in the loser's stage pick completes (staging, 2026-10-10: back from the stage
    // select the CSS has no coin placed yet, the lock-in with the pick found no character, the
    // pick was dropped and the stage select opened again, over and over).
    static bool roomLockedForNext();
    static bool resumeStep();
    static int lockChar()
    {
        // A desynced game played again: the character of that game, whatever the CSS shows.
        if (resumeStep() && s.lastCss >= 0) return plainCss(s.lastCss);
        int c = selectedChar();
        if (c >= 0) return plainCss(c);
        return s.phase == PH_CONNECTED || (roomCss() && roomLockedForNext()) ? plainCss(s.lastCss) : -1;
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

    // The save's tag with this name (the first, as the tag list shows them), -1 none.
    static int findTag(const u16* name)
    {
        if (!name[0]) return -1;
        for (int i = 0; i < TAG_SLOTS; i++) {
            const u16* t = (const u16*)tagRecord(i);
            if (!t) continue;
            int k = 0;
            while (k < PPOM::TAG_CHARS && t[k] == name[k] && name[k]) k++;
            if (k == PPOM::TAG_CHARS || (!t[k] && !name[k])) return i;
        }
        return -1;
    }

    // The tag's name as the plate prints it (printable ASCII, as u16Name), "" for none.
    static void tagName(int tag, char* out)
    {
        u16 name[PPOM::TAG_CHARS + 1];
        const u16* t = (const u16*)tagRecord(tag);
        for (int i = 0; i < PPOM::TAG_CHARS; i++) name[i] = t ? t[i] : 0;
        name[PPOM::TAG_CHARS] = 0;
        u16Name(out, name);
    }

    static void noteOwnSlotColour();   // P+'s slot colours (below, with the Code Menu)
    static u8 lockSlotColour();
    static void cssSlotColours();

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
        int coin = lockChar();
        int css = resolveCss(coin);
        if (css < 0) return;
        int costume = picked >= 0 ? selectedCostume() : s.lastCostume;
        int tag = picked >= 0 ? selectedTag() : s.lastTag;
        PPOM::PortValues pv;
        portValuesOf(tag, &pv);
        // Stock control: the count picked on the panel's STOCKS box (room_css.cpp), which the match
        // setup gives this player (online_match.cpp).
        pv.stocks = roomCss() && roomStockControl() ? RoomCss::myStocks() : 0;
        s.lockedStocks = pv.stocks;
        // P+'s slot colour, P1's line on this CSS (cssSlotColours), which the match gives this
        // player's port on every machine (online_match.cpp).
        noteOwnSlotColour();
        pv.slotColour = lockSlotColour();
        // The coin's CSS id (RANDOM's when on it, which Dolphin shows the others instead of the
        // character drawn: COSTUME_RANDOM), the character drawn.
        PPOM::writeLockIn(true, (u8)coin, charKindOf(css), (u8)(costume | s.startAlt), stagePick, asl,
                          (u8)game, &pv, s.startPad);
        s.lockedGame = game;
        s.lockStage = stagePick;
        s.lastCss = picked >= 0 ? picked : s.lastCss;
        s.lastCostume = costume;
        s.lastTag = tag;
    }
    static void unlock()
    {
        PPOM::writeLockIn(false, 0xFF, 0xFF, 0, 0xFFFF, 0, 0, NULL, s.startPad);
        s.lockedGame = 0;
        s.lockStage = 0xFFFF;
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

    // Every online CSS (built from gmSelCharData, see above) starts with the player's tag.
    static void restoreTag()
    {
        u32 gg = *(u32*)0x805A00E0;
        if (!isPtr(gg)) return;
        u32 selp = *(u32*)(gg + 0x10);   // gmSelCharData
        if (!isPtr(selp)) return;
        u8* rec = (u8*)selp + 0xB8 + LOCAL_PORT * 0x5C;
        rec[0x18] = tagRecord(s.lastTag) ? (u8)s.lastTag : 0x78;
    }

    // The tag of the last lock-in (the search's START), as a record's +0x18: 0x78 for none.
    u8 lockedTagRecord()
    {
        return tagRecord(s.lastTag) ? (u8)s.lastTag : 0x78;
    }

    void prepareCss()
    {
        if (roomCss()) RoomCss::prepareRecords();
        if (s.mode >= 0) restoreTag();
    }

    void restoreCss()
    {
        prepareCss();
        if (s.mode >= 0) cssSlotColours();   // the match's colours out of the lines
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
    // the lower port); docs/nplayer/setup.md. One player picks: of several (a 1v1 draw), the
    // lower port, since the stage select comes after every player has locked in. (Between games
    // SESSION's `present` is 0: Dolphin fills the players in with the next game's setup.)
    static int stagePicker()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        if ((s.mode != PPOM::MODE_DIRECT && s.mode != PPOM::MODE_TEAMS) || se.state == PPOM::SS_NONE)
            return -1;
        for (int i = 0; i < PPOM::SESSION_PLAYERS; i++) {
            if (se.players[i].picksStage) return i;
        }
        return -1;
    }

    static bool picksStage()
    {
        int p = stagePicker();
        return p >= 0 && p == PPOM::g_block.local.localPort;
    }

    // Another player picks the next game's stage and is on its stage select now (Direct and the
    // rooms): connected, this player locked in for the game, every other player too (the
    // picker's stage select opens on its LOCAL.remoteReady, which is the same "all locked in"),
    // and the setup not decided yet (the host waits for the pick). The picker's port, else -1;
    // the others' line reads "P<n> choosing stage...". Of several pickers (a 1v1 draw) the
    // lowest port picks, as stagePicker.
    static int otherPickerChoosing(bool room)
    {
        const PPOM::Session& se = PPOM::g_block.session;
        const PPOM::Local& lo = PPOM::g_block.local;
        if (!room && s.mode != PPOM::MODE_DIRECT && s.mode != PPOM::MODE_TEAMS) return -1;
        if (lo.state != PPOM::LS_CONNECTED || !lo.remoteReady || se.state != PPOM::SS_LOBBY ||
            !se.game || s.lockedGame != se.game)
            return -1;
        for (int i = 0; i < PPOM::SESSION_PLAYERS; i++) {
            if (se.players[i].picksStage) return i == lo.localPort ? -1 : i;
        }
        return -1;
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
    // The READY TO FIGHT banner (MenSelchrReady) shows while START would ready the player
    // (startReadies) and is hidden otherwise; it stays inert either way (cssReadyBanner).
    static bool roomIn();
    static bool startReadies();
    static void hideHeaderArt()
    {
        static const u32 OBJS[] = {0x418 /* TitleW */, 0x158 /* Rnum1 */, 0x15C /* Rnum2 */,
                                   0x420 /* State0005: ITEM */, 0x424 /* State0006: STAGE */,
                                   0x3C4 /* Ready: the READY TO FIGHT banner (cssReadyBanner) */};
        typedef void (*SetVisFn)(void* scnMdl, bool vis);
        u8* task = cssTask();
        if (!task) return;
        bool roomButtons = roomCss() && roomIn();   // ITEM / STAGE are the room's buttons
        for (u32 i = 0; i < sizeof(OBJS) / sizeof(OBJS[0]); i++) {
            if (roomButtons && (OBJS[i] == 0x420 || OBJS[i] == 0x424)) continue;
            u32 obj = *(u32*)(task + OBJS[i]);
            if (!isPtr(obj)) continue;
            u32 mdl = *(u32*)(obj + 0xC);   // MuObject::m_scnMdl
            if (!isPtr(mdl)) continue;
            bool vis = OBJS[i] == 0x3C4 && startReadies();
            ((SetVisFn)0x80043D20)((void*)mdl, vis);   // nwSMSetVisibility
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
    //
    // P+'s "Autosave Replays" line (0 OFF, 1 ON; ON in the netplay menu dnet.cmnu, OFF in
    // data.cmnu) is off online too (bug reports #48 and #49, 2026-10-18). With it ON, P+'s
    // Control Code Menu sets 0x804E003C to 2 in a match, and its End Match code then writes the
    // game's replay to the SD card (/Project+/rp/rp_*.bin) before the match scene ends: ~7 s
    // after every full-length game, and on a full card the write fails and P+ tries it again
    // every 8-16 s for good, so the game never leaves the match. Online every game is in
    // Dolphin's own replay (.brep) already.
    static void codeMenuSelect(const char* name, u32 value)
    {
        u32 len = strlen(name);
        for (u32 p = CODE_MENU; p + 0x20 <= CODE_MENU + CODE_MENU_SIZE; p += 4) {
            if (!codeMenuLine(p) || ((u8*)p)[2] != 0) continue;
            if (strncmp((const char*)p + ((u8*)p)[6], name, len) != 0) continue;
            *(u32*)(p + 8) = value;
            return;
        }
    }

    // P+'s slot colours (QuickLava's PlayerSlotColorChangers, Net-CodeMenu.asm): each port's
    // colour is a Code Menu value line, found through the pointer at 0x804E02F4 + port * 4 (its
    // value at +8, 0-12; its default at +0x10, the port's own: 0 red, 1 blue, 2 yellow, 3 green).
    // L / R on a CSS panel's player box steps that panel's line, Z resets it (the hook at
    // 0x8068B168, also on the online CSS, where the local panel is area 0: P1's line); the CSS
    // panels and the match HUD (name tags, stock icons, damage, magnifier) are drawn from them,
    // a team battle from the defaults. Online every Code Menu line goes to its default
    // (applyRules), and the match shows each port's line: everyone was red, blue, yellow, green.
    // Now the player's colour is P1's line on the online CSS (what they had as P1 offline when
    // they went online, then their L / R there); it travels with the lock-in's port values and
    // the match setup writes all four lines from SESSION on every machine (OnlineMatch). Purely
    // visual: only the materials' colour animation reads them.
    static const u32 SLOT_COLOUR_LINES = 0x804E02F4;
    static u32 s_ownSlotColour = 0;   // the value P1's line has on the online CSS

    static u32* slotColourLine(int port)
    {
        u32 l = *(u32*)(SLOT_COLOUR_LINES + 4 * port);
        if (l < CODE_MENU || l + 0x14 > CODE_MENU + CODE_MENU_SIZE || (l & 3)) return NULL;
        return (u32*)l;
    }

    static void setSlotColour(int port, int colour)
    {
        u32* l = slotColourLine(port);
        if (l) l[2] = colour >= 0 && colour < PPOM::SLOT_COLOURS ? (u32)colour : l[4];
    }

    // The online CSS's lines: P1's (the local panel's) is the player's colour, the others their
    // defaults (a match may have left the players' colours in them).
    static void cssSlotColours()
    {
        setSlotColour(0, (int)s_ownSlotColour);
        for (int i = 1; i < 4; i++) setSlotColour(i, -1);
    }

    static void noteOwnSlotColour()
    {
        u32* l = slotColourLine(0);
        if (l && l[2] < (u32)PPOM::SLOT_COLOURS) s_ownSlotColour = l[2];
    }

    // The lock-in's PortValues.slotColour: 0 for the line's default (P1's red, which the player
    // did not pick: that player gets their port's colour), else the colour + 1.
    static u8 lockSlotColour()
    {
        u32* l = slotColourLine(0);
        return l && s_ownSlotColour != l[4] ? (u8)(s_ownSlotColour + 1) : 0;
    }

    void setSlotColours(const s8* colour)
    {
        for (int i = 0; i < 4; i++) setSlotColour(i, colour[i]);
    }

    static void codeMenuOff()
    {
        codeMenuSelect("Code Menu Activation", 2);
        codeMenuSelect("Autosave Replays", 0);
    }

    // P+'s salty runback and results skip: its "End Match" control code (Net-CodeMenu.asm, hook at
    // 0x806D4850) reads this machine's own pads when the match scene ends and, with the header's
    // buttons held (u16 0x804E000A, L+R+Y: the pause screen's quit with Y; 0x804E000E, L+R+X),
    // sets the sequence's next state to 1 or 13 instead of 10. In sqNetAnyOkiraku that skips
    // OnlineMatch's state 10 (back to the online CSS) on that machine only. The header bytes
    // 0x804E0008 / 0x804E000C switch each one off (non-zero; nothing else writes them). Off
    // online, as Slippi has no runback online; the player's own values come back with the
    // other lines (restoreRules).
    static const u32 CM_RUNBACK_OFF = 0x804E0008, CM_SKIP_RESULTS_OFF = 0x804E000C;
    static u8 s_runbackSaved[2];
    static void runbackOff()
    {
        *(volatile u8*)CM_RUNBACK_OFF = 1;
        *(volatile u8*)CM_SKIP_RESULTS_OFF = 1;
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
        noteOwnSlotColour();   // the player's P1 colour offline
        s_runbackSaved[0] = *(u8*)CM_RUNBACK_OFF;
        s_runbackSaved[1] = *(u8*)CM_SKIP_RESULTS_OFF;
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
        *(u8*)CM_RUNBACK_OFF = s_runbackSaved[0];
        *(u8*)CM_SKIP_RESULTS_OFF = s_runbackSaved[1];
        s.haveSavedRules = false;
    }

    // The online ruleset, the same on every machine whatever was set offline. With the session's
    // rules (SESSION Rules, from the server through Dolphin; docs/design/room-rules.md section 3),
    // the set rule as P+'s rules menu keeps it (docs/game-code.md section 14, Room rules RE):
    // style (+2, low 3 bits), stocks (+4), damage ratio (+6), the time limit (+8, every style; the
    // time / coin target +3 stays infinite), team attack (+9), pause (+0xA, 1 normal / 0 hold),
    // auto L-cancel (+0xB), input assist (+0xC) and stock control (+5); the item frequency and
    // switches in the menu record, and the hazard switch. Without them (an older server), P+'s competitive set: the set rule's 16 bytes P+'s code writes, pause
    // only in Direct, item frequency 0 with P+'s default item switch, hazards off (the default
    // since 2026-10-12, the user's). Either way hazards
    // also hold for Direct's loser's pick (its hazard toggle is not part of the setup the other
    // machine gets), and every other Code Menu line is at its default but its activation and
    // Autosave Replays, which are OFF (codeMenuOff). Written when the Wi-Fi sequence starts (in
    // place of Brawl's Wi-Fi rules) and
    // again by every online match setup, right before sqVsMelee's setup reads them
    // (OnlineMatch::setupMatch), so a setting changed in between (or left over from offline play)
    // never reaches an online match. Dolphin compares the result on every machine before the match
    // (GameplaySession SetupKey).
    void applyRules()
    {
        static const u8 PPLUS_RULE[16] = {0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x0A, 0x00,
                                          0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        u8* r = setRule();
        u8* m = menuRecord();
        if (!r || !m) return;
        u8 time = r[3], stocks = r[4], stockTime = r[8];
        const PPOM::Rules& ru = PPOM::g_block.session.rules;
        // (only the 16 bytes P+ writes: the set rule's size is not known, what follows may be other data)
        memcpy(r, PPLUS_RULE, sizeof(PPLUS_RULE));
        u8* st = stageSelData();
        if (ru.valid) {
            r[2] = (u8)((r[2] & ~7) | (ru.style & 7));     // m_rule
            r[4] = ru.stocks;
            r[6] = ru.damageRatio;
            r[8] = ru.timeMinutes;                         // time limit, every style (P+)
            r[9] = ru.teamAttack ? 1 : 0;
            r[0xA] = ru.pause ? 1 : 0;                     // 1 normal, 0 hold START
            r[0xB] = ru.autoLCancel ? 1 : 0;
            r[0xC] = ru.inputAssist ? 1 : 0;
            // P+'s STOCK CONTROL (+5, `m_handicap`: 2 on), stock matches only as its menu has it;
            // each player's count is their match player's +0x26 (OnlineMatch::setupMatch).
            r[5] = ru.stockControl && ru.style == PPOM::STYLE_STOCK ? 2 : 0;
            m[0] = ru.itemFrequency;
            memcpy(m + ITEM_SWITCH, ru.itemSwitch, sizeof(ru.itemSwitch));
            if (st) st[HAZARD] = ru.hazards ? 0 : 1;
        } else {
            r[0xA] = (s.mode == PPOM::MODE_DIRECT) ? 1 : 0;   // m_allowPause
            m[0] = 0;                                          // item frequency: none
            memcpy(m + ITEM_SWITCH, PPLUS_ITEM_SWITCH, sizeof(PPLUS_ITEM_SWITCH));
            if (st) st[HAZARD] = 1;                            // hazards off
        }
        if (PPOM::g_block.debug.cfg & PPOM::CFG_TEST_RULES) {
            r[3] = time;            // tests: shorter games (harness ppom.test_rules)
            r[4] = stocks;
            r[8] = stockTime;
        }
        // (On the CSS, as when a room's rules menu opens, P1's line has the player's colour.)
        if (strcmp(Online::currentSceneName(), "scSelctCharacter") == 0) noteOwnSlotColour();
        int lines = codeMenuLines(1);                      // every Code Menu line at its default
        codeMenuOff();                                     // except activation and autosave: OFF
        runbackOff();                                      // and P+'s salty runback
        cssSlotColours();                                  // and the player's slot colour on P1
        PPOM::g_block.debug.scratch[3] = ((u32)lines << 16) | ((PPOM::g_block.debug.scratch[3] + 1) & 0xFFFF);
    }

    // ----------------------------------------------------------------------------------------
    // Status text: Slippi's CSS strings (LoadCSSText.asm:95-157) in the one text line the
    // Brawl CSS has (its rule line). Slippi shows three lines plus a hint; we show the line
    // that carries the state: "Select your character" -> "Press START to search/enter code"
    // -> "Searching for ..." -> "Connecting to ..." -> "Playing: <name>", errors in red.

    static const u32 QUIT_NOTICE_FRAMES = 300;   // 5 s

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
    // Unranked and Ranked search from the waiting room (training_room.cpp); a replay never does.
    static bool usesWaitingRoom()
    {
        return (s.mode == PPOM::MODE_UNRANKED || s.mode == PPOM::MODE_RANKED) && !roomCss() && !s.playback;
    }

    static void updateRoomStatus();
    // Who quit the last game (noteQuit), shown over every line but an error for its first frames
    // back on the CSS, on every machine of the match.
    static bool quitNoticeShown()
    {
        if ((!s.quitNoticeFrames && !s.quitHold) || s.phase == PH_ERROR || s.roomError) return false;
        if (s.quitHold) {
            char buf[sizeof(s.quitNotice) + 32];
            const bool locked = roomCss() ? roomLockedForNext() : lockedForNext();
            sprintf(buf, "%s - %s", s.quitNotice, locked ? "Waiting on opponent" : "Press START");
            setStatus(buf, false);
        } else {
            setStatus(s.quitNotice, false);
        }
        // tests: the notice is on the status line (0x2000000) and whose (bits 26-27)
        PPOM::g_block.debug.scratch[10] = (PPOM::g_block.debug.scratch[10] & ~0xE000000u) | 0x2000000 |
                                          ((u32)s.quitNoticePort << 26);
        return true;
    }

    static void updateStatus()
    {
        char buf[128];
        if (quitNoticeShown()) return;
        if (roomCss()) {
            updateRoomStatus();
            return;
        }
        switch (s.phase) {
        case PH_IDLE:
            if (s.codePreset && s.code[0]) {
                // The launcher's invite: the other player's code where Direct shows it.
                // (one line: "Select your character to play ABCD#123" wrapped and was cut off)
                sprintf(buf, selectedChar() < 0 ? "Select character vs %s" : "Press START to play %s", s.code);
                setStatus(buf, false);
            } else if (selectedChar() < 0) {
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
            // Ranked's game setup has its own line (whose turn it is, the opponent's character;
            // in a character step or after a desync, also once locked in: who is still choosing
            // or not ready yet, and the seconds left).
            if (rankedStep() && s.step.active && s.stepText[0] &&
                (!lockedForNext() || resumeStep() || s.step.type == PPOM::STEP_CHAR)) {
                setStatus(s.stepText, false);
                return;
            }
            // Slippi's lines when connected (LoadCSSText.asm): "Press START to lock in" /
            // "select stage", "Locked in" + "Waiting on opponent", and "Playing: <name>".
            // "Playing" once the match is set up: Direct's loser locks in first and then picks
            // the stage, so the others wait on the opponent until that pick.
            if (lockedForNext()) {
                const PPOM::Session& se = PPOM::g_block.session;
                int picker = otherPickerChoosing(false);
                if (se.state == PPOM::SS_MATCH_READY && se.game == s.lockedGame) {
                    sprintf(buf, "Playing: %s", s.peerName);
                    setStatus(buf, false);
                } else if (picker >= 0) {
                    sprintf(buf, "P%d choosing stage...", picker + 1);
                    setStatus(buf, false);
                } else {
                    setStatus("Waiting on opponent", false);
                }
            } else if (lockChar() < 0) {
                setStatus("Select your character", false);
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
    // In a room: up to the RULES button (rules_button.cpp, left of the ITEM / STAGE buttons; it was
    // 222 / 530 before RULES). The fit decides where a line ends (the window does not cut it): a
    // font unit is about 1.5 px at 2000 wide ("Waiting for players" 318 units), and the room's
    // line has about 570 px from its start to RULES.
    static const float ROOM_STATUS_X2 = 158.0f;
    static const int ROOM_STATUS_FIT = 378;
    static const float STATUS_X2 = 330.0f;
    static const float WRAP_SCALE = 0.7f;
    // A text too wide for one line at scale 1 is one line, smaller (down to this), rather than
    // two lines at WRAP_SCALE (staging feedback 2026-10-12: "Press START to select stage" on two
    // lines in a room). Down to the two lines' own size (the user's, 2026-10-19: "sarahkitty3 quit
    // - Press START" broke onto two lines at 0.75; two lines are only for what one line at that
    // size can't hold).
    static const float ONE_LINE_MIN = WRAP_SCALE;
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
    // fits; one line made smaller to fit if it fits at ONE_LINE_MIN; else two lines at WRAP_SCALE, split at the space that makes the longer line
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
        if (total * ONE_LINE_MIN <= fit) return LAYOUT_ONE_SMALL;
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
    static char s_roomTop[24];
    static const char* ownText()
    {
        if (roomCss()) {
            // The room's code where Direct shows the player's own: "KFQB Public" / "KFQB Private"
            // (rooms.md #5, #17). Not in a room: the player's own connect code, as Direct.
            const PPOM::Session& se = PPOM::g_block.session;
            if ((se.roomFlags & PPOM::RF_IN) && PPOM::g_block.local.room == PPOM::RP_IN) {
                int n = 0;
                for (int i = 0; i < 4; i++) {
                    char ch = se.roomCode[i];
                    if (ch >= 'A' && ch <= 'Z') s_roomTop[n++] = ch;
                }
                if (n == 4) {
                    s_roomTop[n] = 0;   // Public / Private and the mode: roomButtons
                    return s_roomTop;
                }
            }
            return s.ownCode;
        }
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
        // Attached only once the CSS has printed its rule line (the status window is known):
        // attached earlier, while the CSS was still being built, the window once stayed a small
        // left-aligned overlay over LEAVE (found live on a first Direct CSS after boot; staging
        // feedback 2026-10-10: "KITY#371" over LEAVE).
        if (!s.haveWindow) return;
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
        ws->x2 = OWN_X2;   // a room's code lined up as the connect code
        ws->y1 = s.windowY1;
        ws->y2 = s.windowY2;
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
            ws->x2 = roomCss() && roomIn() ? ROOM_STATUS_X2 : STATUS_X2;
            int fit = roomCss() && roomIn() ? ROOM_STATUS_FIT : STATUS_FIT;
            layout = layoutStatus(s.status, text, fit);
            bool two = layout == LAYOUT_TWO;
            // one small line: as large as fits (at least WRAP_SCALE: a single long word, or
            // ONE_LINE_MIN)
            int total = statusWidths(s.status, (int)strlen(s.status), NULL);
            float small = total > 0 ? (float)fit / (float)total : WRAP_SCALE;
            if (small > 1.0f) small = 1.0f;
            if (small < WRAP_SCALE) small = WRAP_SCALE;
            ws->scaleX = ws->scaleY = layout == LAYOUT_ONE ? 1.0f : layout == LAYOUT_ONE_SMALL ? small : WRAP_SCALE;
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

    // Any costume in a team battle (the user's, 2026-10-12: a room's team battle gave everyone
    // their team's costume, whatever they had picked). Brawl's team battle CSS (task+0x5C8) ties
    // the costume to the team: A on the player's own panel (hand target 0x1B) cycles team and
    // P+'s team set (area+0x1C0, +0x1C4) instead of the costume, and every portrait is drawn in
    // the team's costume (setCharPic's isTeam). Online the costume stays the player's own pick
    // and the team is the room's, so:
    // sel_char+0x88B4 `lbz r0,0x5C8(r27)`, the panel click's "team battle?" test (team: on at
    // +0x88C0, else the costume at +0x8A24, as in a free-for-all): online, the costume; in a
    // team room the next team, as P+'s own team battle with the players' costumes (task+0x5CB
    // 2, its VERSUS button's rainbow TEAM BATTLE, where X / Y change the costume; the user's,
    // 2026-10-15, after bug #22: a P+ player pressed X / Y for a colour and changed team), sent
    // by tickRoom (g_cssPanelTeam), the click itself doing nothing more (the exit). While
    // locked in, nothing (the function's exit, +0x95A0), as X / Y (cssCostume): the click
    // changed the costume shown but not the one locked in (harness, 2026-10-12; FFA too).
    __attribute__((naked)) void cssPanelClick()
    {
        asm volatile(
            "lis 12, g_onlineCssLock@ha\n\t"
            "lbz 12, g_onlineCssLock@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "bne 2f\n\t"
            "lis 12, g_onlineCss@ha\n\t"
            "lbz 12, g_onlineCss@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "bne 1f\n\t"
            "lbz 0, 0x5C8(27)\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0xB17C\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "1:\n\t"
            "lis 12, g_cssTeamRoom@ha\n\t"
            "lbz 12, g_cssTeamRoom@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "beq 3f\n\t"
            "li 0, 1\n\t"
            "lis 12, g_cssPanelTeam@ha\n\t"
            "stb 0, g_cssPanelTeam@l(12)\n\t"
            "b 2f\n\t"
            "3:\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0xB2E8\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "2:\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0xBE64\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // setCharPic(area, css, kind, costume, isTeam, team, teamSet), sel_char+0x14B68 (0x8069742C)
    // `stwu r1,-0x80(r1)`, its first instruction: online, isTeam (r7) 0, so the portrait is the
    // costume's (with isTeam it is findCharTeamColorNo's, the team's). Back at +0x14B6C.
    // While g_cssPicHold (a room panel's showTeam with another player number, RoomCss's
    // showTeamAt) it returns at once: no picture.
    __attribute__((naked)) void cssCharPic()
    {
        asm volatile(
            "lis 12, g_cssPicHold@ha\n\t"
            "lbz 12, g_cssPicHold@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "bnelr\n\t"
            "lis 12, g_onlineCss@ha\n\t"
            "lbz 12, g_onlineCss@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "beq 1f\n\t"
            "li 7, 0\n\t"
            "1:\n\t"
            "stwu 1, -0x80(1)\n\t"
            "lis 12, 0x8069\n\t"
            "ori 12, 12, 0x7430\n\t"
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
    extern "C" void pponline_cssLockedButton(u32 button)
    {
        if (roomCss()) s_headerPress = (u8)button;   // the room's buttons (tickRoom)
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
            "mr 3, 29\n\t"
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
    // and the match starts from SESSION; the banner shows only while START would ready the
    // player (hideHeaderArt, startReadies) and is never hit:
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

    static bool roomLocked();
    static bool lockedIn()
    {
        if (roomCss()) return roomLocked();
        // Connected: locked while locked in for the next game (between games the character can
        // change until START locks it in again, as on Slippi).
        if (s.phase == PH_CONNECTED) return lockedForNext() || g_onlineMatchGame != 0;
        return s.phase == PH_SEARCHING || s.phase == PH_CONNECTING || s.phase == PH_ERROR;
    }

    void install(CoreApi* api)
    {
        CodeEntry::install(api);
        OnlineMatch::install(api);
        RoomRules::install(api);
        api->sySimpleHookRel(0x726C, reinterpret_cast<void*>(cssBPressed), 10 /* sora_menu_sel_char */);
        api->sySimpleHookRel(0x7280, reinterpret_cast<void*>(cssCostume), 10);
        api->sySimpleHookRel(0x75B4, reinterpret_cast<void*>(cssCoinAPress), 10);
        api->sySimpleHookRel(0x7CCC, reinterpret_cast<void*>(cssSwitchButtons), 10);
        api->sySimpleHookRel(0x88B4, reinterpret_cast<void*>(cssPanelClick), 10);
        api->sySimpleHookRel(0x14B68, reinterpret_cast<void*>(cssCharPic), 10);
        api->sySimpleHookRel(0xDDB0, reinterpret_cast<void*>(cssReadyBanner), 10);
        RulesButton::install(api);
    }

    void enter(int mode)
    {
        saveRules();
        codeMenuOff();
        runbackOff();
        s.mode = mode;
        s.phase = PH_IDLE;
        s.rankText[0] = 0;
        s.rankFrames = 0;   // Ranked: ask for the rating at once
        s.cssMsg = NULL;
        s.code[0] = 0;
        s.codePreset = false;
        s.startPad = 0;
        s.startAlt = 0;
        s.lastButtons = 0xFFFFFFFF;   // ignore buttons held from the menu
        s.searchSeq = 0;
        s.pollPending = false;
        s.status[0] = 0;
        unlock();
        s.lastCss = -1;
        s.lastCostume = 0;
        g_onlineMatchGame = 0;
        g_onlinePickStage = 0;
        g_onlineCss = 1;
        s.roomEntry = ROOM_ENTRY_NONE;
        s.roomCreateSent = false;
        s.roomPollPending = false;
        s.roomPhase = PPOM::RP_NONE;
        s.roomError = false;
        s.roomText[0] = 0;
        s.roomSeen = false;
        s.roomReload = false;
        s.roomKickPort = -1;
        s.roomRHeld = false;
        s.playback = false;
        s.playbackPending = false;
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

    static void postRoom(u8 op, u8 arg, u8 arg2, const char* code)
    {
        PPOM::RoomRequest req;
        memset(&req, 0, sizeof(req));
        req.op = op;
        req.arg = arg;
        req.arg2 = arg2;
        if (code) PPOM::asciiToU16(req.code, code, PPOM::CODE_LEN);
        PPOM::post(PPOM::CMD_ROOM, &req, sizeof(req));
    }

    // In a room (or joining one), as Dolphin reports it.
    static bool roomActive() { return PPOM::g_block.local.room != PPOM::RP_NONE || s.roomPhase != PPOM::RP_NONE; }

    static void leave()
    {
        // Leaving a room's CSS other than for its match: leave the room (Dolphin would after 5 s
        // on the menus anyway).
        if (roomCss() && roomActive()) postRoom(PPOM::ROOM_LEAVE, 0, 0, NULL);
        s.roomEntry = ROOM_ENTRY_NONE;
        s.mode = -1;
        s.codePreset = false;
        s.playback = false;
        s.playbackPending = false;
        s.phase = PH_IDLE;
        s.searchSeq = 0;
        s.pollPending = false;
        s.cssMsg = NULL;
        unlock();
        g_onlineMatchGame = 0;
        g_onlinePickStage = 0;
        g_onlineCss = 0;
        g_cssTeamRoom = 0;
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
        // A launcher invite's code is searched once: cancelled, failed or played, START opens
        // the keypad afterwards as in a plain Direct (the code is in the recent codes then).
        s.codePreset = false;
        // The search locks the player in for game 1 (Slippi FN_LOCK_IN_AND_SEARCH); the session
        // gets the lock-in from LOCAL as soon as it exists. Ranked's lock-in is for no game (0):
        // its game 1 starts with a character step on the CSS once the opponent's name shows (the
        // user's, 2026-10-19; Dolphin GameSetup), and the CSS starts on this character. The
        // waiting room plays it either way (TrainingRoom::fillPlayer).
        lockIn(s.mode == PPOM::MODE_RANKED ? 0 : 1, 0xFFFF, 0);
        s.searchSeq = PPOM::post(PPOM::CMD_FIND_OPPONENT, &req, sizeof(req));
        s.pollPending = true;   // Dolphin answers FIND_OPPONENT with the match state
        s.pollFrames = 0;
        // A stage select left after the last connection ended (Direct's pick) leaves its stage
        // behind; it is not this connection's.
        OnlineMatch::clearPickedStage();
        s.phase = PH_SEARCHING;
        // Unranked and Ranked wait in Brawl's own waiting room (training_room.cpp), as Brawl's
        // "With Anyone" and Brawlback's Quickplay do (the user's, 2026-10-15): the CSS leaves
        // for it at once.
        // The CSS fades to black first (TrainingRoom's fade) and leaves for the room once the
        // screen is black (tickRoomLeave): the frame frozen on the screen while the room loads
        // is black, not the CSS (the user's, 2026-10-16, after Brawlback's Quickplay).
        if (usesWaitingRoom()) {
            TrainingRoom::enter();
            s.roomLeaveFrames = 0;
            s.roomHoldStart = 0;
            s.roomStartUp = false;
        }
    }

    int currentMode() { return s.mode; }

    // Slippi's HandleOnlineLockedOptions.asm: logged out (0) or with an update required (2),
    // Ranked, Unranked, Direct and Teams are locked. Not before Dolphin has answered.
    bool modesLocked() { return s.appState >= 0 && s.appState != 1; }

    void codeEntered(const char* code)
    {
        if (roomCss() && CodeEntry::rulesetMode()) {
            // The room's Ruleset line's "Enter code" (room_play.cpp): a 6-letter ruleset code; a
            // bad one comes back as the room's error ("Ruleset not found.").
            postRoom(PPOM::ROOM_RULESET, 0, 0, code);
            s.roomError = false;
            s.lastButtons = 0xFFFFFFFF;   // as a room code's START, below
            s.startGuard = START_GUARD_FRAMES;
            return;
        }
        if (roomCss()) {
            // Join Room: the code typed on the keypad's room-code mode (4 of the 20 letters).
            postRoom(PPOM::ROOM_JOIN, 0, 0, code);
            s.roomError = false;
            s.roomText[0] = 0;
            // The keypad's START is still down: no press counts until every button is up (or it
            // opens the keypad again before Dolphin's answer is in). The pad system can also
            // report the keypad's START press again a few frames after the keypad closed, with
            // START already up (seen in harness/tests/test_rooms_game.py: the joiner locked in
            // on arriving), so START presses are dropped for a short while as well.
            s.lastButtons = 0xFFFFFFFF;
            s.startGuard = START_GUARD_FRAMES;
            return;
        }
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

    // After a pause-screen quit in Ranked or a room's set (the user's, 2026-10-12; docs/game-code.md
    // §11, the quit per mode): every player stays on the CSS ("<name> quit - Press START") until
    // each pressed START, a fresh ready with the character on the CSS, which may change first (B
    // takes it back). Then the steps go on as after any game: Ranked the winner's bans and the
    // loser's pick (the character step after them is already done: both are locked in for the
    // game), a set's quit game from its first step. Dolphin is asked no step meanwhile (pollStep),
    // so no step's timer runs. True while holding.
    static bool stepSession();
    static bool quitHolding(u32 pressed, int game, bool locked)
    {
        if (!s.quitHold) return false;
        if (!stepSession() || !game) {
            s.quitHold = false;   // the session is over (Ranked: the set was decided)
            return false;
        }
        if (!locked) {
            if ((pressed & BTN_START) && lockChar() >= 0) lockIn(game, 0xFFFF, 0);
        } else if (PPOM::g_block.local.remoteReady) {
            s.quitHold = false;
            s.quitNoticeFrames = 0;
            s.stepFloor = s.stepSeq;   // the steps from a fresh answer
            return false;
        } else if (pressed & BTN_B) {
            maskPressed(BTN_B);
            unlock();
            playSE(SE_BACK);
        }
        return true;
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
        if (ranked && quitHolding(pressed, game, lockedForNext())) return;
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
                return;
            }
            if (s.lockStage != PPOM::STAGE_PENDING) return;
            // Direct's loser, locked in with the character: the stage select once every other
            // player is locked in too, and the setup waits for the pick (Dolphin STAGE_PENDING).
            const u16 pick = OnlineMatch::pickedStage();
            if (pick == OnlineMatch::PICK_CANCELLED) {
                // Left the stage select without a pick (B): unlocked, so the character can change
                // again, and START locks in again.
                unlock();
            } else if (pick != 0xFFFF) {
                // Back from the stage select: locked in with the stage (ExitSSSUponStageSelect).
                // The pick is for this game only.
                lockIn(game, pick, OnlineMatch::pickedAsl());
                OnlineMatch::clearPickedStage();
            } else if (PPOM::g_block.local.remoteReady) {
                g_onlinePickStage = 1;
                leaveCss(1);
            }
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
        lockIn(game, picksStage() ? PPOM::STAGE_PENDING : 0xFFFF, 0);
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

    // GameSetup's steps run (Dolphin answers GP_FETCH_STEP with an active step): Ranked connected,
    // or a room's game session (a room's set, docs/design/room-rules.md section 5; a room game
    // without a set answers an inactive step).
    static bool stepSession()
    {
        if (s.mode == PPOM::MODE_RANKED) return s.phase == PH_CONNECTED;
        return roomCss() && PPOM::g_block.local.state >= PPOM::LS_CONNECTED;
    }

    // Ranked, connected (or a room's set): Slippi's GP_FETCH_STEP every frame (one at a time), on
    // the CSS and the stage select, so turns and strikes show at once.
    static void pollStep()
    {
        if (!stepSession()) {
            s.step.active = 0;
            s.stepPending = false;
            return;
        }
        if (s.stepPending && ++s.stepFrames < 60) return;
        if (s.quitHold) return;   // no step asked for, so none of its timers runs (quitHolding)
        s.stepSeq = PPOM::post(PPOM::CMD_GP_FETCH_STEP, NULL, 0);
        s.stepPending = true;
        s.stepFrames = 0;
    }

    // Ranked (or a room's set on the stage select for its steps): the last step (active or not:
    // an inactive step means the stage is decided, or the connection is gone, so the stage select
    // leaves). NULL in every other case (a room's loser's pick is the stage select's own).
    const PPOM::GameStep* rankedStep()
    {
        return s.mode == PPOM::MODE_RANKED || (roomCss() && s.roomStepSss) ? &s.step : NULL;
    }

    bool stepsDecideStage() { return s.mode == PPOM::MODE_RANKED || (roomCss() && s.roomStepSss); }

    bool nobodyPlaysHere()
    {
        return s.playback || (roomCss() && (PPOM::g_block.session.roomFlags & PPOM::RF_WATCH));
    }

    // On the stage select (Ranked, a room's set) the step's line, after the quit notice while it is
    // on: the steps can follow a quit at once, and the CSS is then only passed through.
    const char* rankedText()
    {
        static char line[sizeof(s.quitNotice) + sizeof(s.stepText) + 4];
        if (!s.quitNoticeFrames || !s.stepText[0]) return s.stepText;
        sprintf(line, "%s - %s", s.quitNotice, s.stepText);
        return line;
    }

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

    static void onRoomStatus(const PPOM::Response& r);
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
            if (!stepSession()) s.step.active = 0;
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
            if (st.state == 1) u16Name(s.ownName, st.name);
            else s.ownName[0] = 0;
            if (!s.tagChosen) {
                u16 tag[PPOM::TAG_CHARS + 1];
                memcpy(tag, st.lastTag, sizeof(st.lastTag));
                tag[PPOM::TAG_CHARS] = 0;
                s.lastTag = findTag(tag);
            }
            s.appState = st.state;
            u32& lockDebug = PPOM::g_block.debug.scratch[7];
            lockDebug = (lockDebug & ~0xFFu) | (u8)(st.state + 1);   // tests: the state seen
            return;
        }
        if (r->cmd == PPOM::CMD_ROOM) {
            onRoomStatus(*r);
            return;
        }
        if (r->cmd == PPOM::CMD_RULESETS) {
            RoomPlay::onRulesets(*r);
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


    // ----------------------------------------------------------------------------------------
    // Rooms (docs/design/rooms.md; the protocol with Dolphin: docs/rooms-game-interface.md).
    //
    // A room's CSS is the Wi-Fi CSS in MODE_TEAMS with a room entry: Create Room asks Dolphin to
    // create one as soon as the CSS is up; Join Room waits for START, which opens the keypad in
    // its room-code mode (code_entry.cpp), and its OK sends the code. In a room:
    //   START       lock in (ready) with Session.game, the room's next game; B takes it back
    //   hold B      Brawl's own: back to WITH FRIENDS, which leaves the room (leave())
    //   L           host, any time: public / private
    //   R           host, between games: Teams on / off
    //   A           host, between games, with the hand over another player's panel: open / close
    //               that slot (closing an occupied one removes the player)
    //   A           Teams on, between games, on the player's own panel or its flag: the next
    //               team colour (X / Y the costume, as in a free-for-all)
    // The others' panels (room_css.cpp) and the line come from Dolphin: SESSION's room view and
    // ROOM_POLL's answer, asked one at a time as the match state is.

    void enterRoom(int entry)
    {
        enter(PPOM::MODE_TEAMS);
        s.roomEntry = entry;
        // Leaving goes back to WITH FRIENDS' page (sqMenuMain 0x1C) with this room button
        // highlighted again (netmenu.cpp).
        g_onlineReturnPage = 0;
        g_onlineFriendsCursor = 1;
        PPOM::g_block.debug.menuState = 0x10 | (u32)entry;
    }

    bool roomCss() { return s.mode == PPOM::MODE_TEAMS && s.roomEntry != ROOM_ENTRY_NONE; }

    static int roomGame() { return PPOM::g_block.session.game; }

    static bool roomIn()
    {
        return (PPOM::g_block.session.roomFlags & PPOM::RF_IN) && PPOM::g_block.local.room == PPOM::RP_IN;
    }

    static int roomMe()
    {
        int p = PPOM::g_block.local.localPort;
        return p >= 0 && p < PPOM::SESSION_PLAYERS ? p : -1;
    }

    static bool roomLockedForNext() { return roomGame() && s.lockedGame == roomGame(); }

    // A set's character step (Ranked's, a room's set; Slippi's order) that is not this player's
    // yet: the winner picks first, the loser after seeing it. The token stays where it is until
    // then (staging 2026-10-12: picked up during the winner's turn it could not be put down).
    static bool waitsForCharTurn()
    {
        return stepSession() && s.step.active && s.step.type == PPOM::STEP_CHAR && !s.step.myTurn;
    }

    // A desynced game starts again (Ranked, a room's set; GameSetup's Resume step): the characters
    // stay as they were (the token cannot be picked up), START readies up, and when the step's
    // time is up the game readies the player itself.
    static bool resumeStep()
    {
        return stepSession() && s.step.active && s.step.type == PPOM::STEP_RESUME;
    }

    static bool roomLocked()
    {
        // Ready, or the room is starting (the lock-in must not change then), or a game is armed.
        // In a set's session also when Dolphin has this player locked in for the next game (its
        // panel bit, from the session's lock-in), whatever this CSS thinks: a locked-in token is
        // never picked up (staging 2026-10-12: a ready player picked it up and could not put it
        // down).
        const PPOM::Session& se = PPOM::g_block.session;
        const int me = roomMe();
        const bool sessionReady = stepSession() && me >= 0 && me < PPOM::SESSION_PLAYERS &&
                                  (se.players[me].roomSlot & PPOM::SLOT_READY);
        return roomLockedForNext() || g_onlineMatchGame != 0 || sessionReady ||
               (roomIn() && se.roomStatus == 1);
    }

    // Every open slot has a player (the room can start once all are ready, rooms.md #7).
    // This player sits out the room's games (SIT OUT, or outside a set that is on).
    static bool roomSittingOut()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        int me = roomMe();
        return roomIn() && me >= 0 && (se.players[me].roomSlot & PPOM::SLOT_SITTING_OUT);
    }

    // This player picks the next room game's stage (the last game's loser; of several pickers,
    // a draw, the lowest port), as Direct's loser.
    static bool roomPicksStage()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        int me = roomMe();
        if (!roomIn() || me < 0) return false;
        for (int i = 0; i < PPOM::SESSION_PLAYERS; i++) {
            if (se.players[i].picksStage) return i == me;
        }
        return false;
    }

    void u16Name(char* out, const u16* in)
    {
        int n = 0;
        for (; n < RoomCss::NAME_CHARS && in[n]; n++) {
            u16 c = in[n];
            if (c >= 0xFF01 && c <= 0xFF5E) c = (u16)(c - 0xFEE0);
            out[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        out[n] = 0;
    }

    // The CSS id of a gmCharacterKind (the inverse of charKindOf over the CSS's ids).
    static int cssOfKind(int kind)
    {
        for (int css = 0; css < 0x40; css++) {
            if (css == 0x28 || css == 0x29) continue;
            if (charKindOf(css) == kind) return css;
        }
        return -1;
    }

    // The other three areas' panels (areas 1-3): each shows the room slot it stands at
    // (RoomCss::portOfArea: panels are in slot order on every screen, the local player's own
    // panel, area 0, at the local slot).
    // A panel's stars (RoomCss::PanelView): while the room has a set (on, paused or finished)
    // the wins of the slot's side (RoomPlay sideSlots, a bit per slot; without them, a 1v1's
    // sides are the in-game ports, which are the slots), so the score is on the panels; the host
    // is then shown by "Host: <name>" alone. Otherwise the host's one star (rooms.md #8).
    static int roomStars(int slot, bool host)
    {
        const PPOM::RoomPlay& p = PPOM::g_block.session.play;
        if (!p.valid || !p.setOn) return host ? 1 : 0;
        for (int side = 0; side < 2; side++) {
            bool on = (p.sideSlots[0] | p.sideSlots[1]) ? (p.sideSlots[side] >> slot) & 1 : p.sides[side] == slot;
            if (on) return p.score[side] < 100 ? p.score[side] : 99;
        }
        return 0;
    }

    // The room's rules have P+'s STOCK CONTROL, in a stock match (P+ shows no STOCKS box in time
    // and coin: its hook at sel_char 0x80693138).
    static bool roomStockControl()
    {
        const PPOM::Rules& ru = PPOM::g_block.session.rules;
        return roomIn() && ru.valid && ru.stockControl && ru.style == PPOM::STYLE_STOCK;
    }

    static void roomPanels(RoomCss::PanelView v[3], int* myTeam)
    {
        const PPOM::Session& se = PPOM::g_block.session;
        int me = roomMe();
        bool in = roomIn();
        u8 taken = 0;
        *myTeam = in && me >= 0 ? se.players[me].roomTeam : -1;
        if (in && me >= 0 && (se.players[me].roomSlot & PPOM::SLOT_TAKEN)) taken |= (u8)(1 << me);
        for (int k = 0; k < 3; k++) {
            int i = in ? RoomCss::portOfArea(k + 1) : -1;
            RoomCss::PanelView& p = v[k];
            memset(&p, 0, sizeof(p));
            p.css = 0x28;
            p.team = PPOM::TEAM_NONE;
            if (i < 0 || i >= PPOM::SESSION_PLAYERS || i == me) continue;   // PV_CLOSED
            const PPOM::SessionPlayer& pl = se.players[i];
            u8 bits = pl.roomSlot;
            p.team = pl.roomTeam < PPOM::TEAM_COUNT ? pl.roomTeam : PPOM::TEAM_NONE;
            if (!(bits & PPOM::SLOT_OPEN)) {
                p.kind = RoomCss::PV_CLOSED;
            } else if (!(bits & PPOM::SLOT_TAKEN)) {
                p.kind = RoomCss::PV_SEARCHING;
            } else {
                taken |= (u8)(1 << i);
                u16Name(p.name, pl.name);
                p.stars = (u8)roomStars(i, (bits & PPOM::SLOT_HOST) || se.roomHost == i);
                // Ready: the lock-in; not yet: the coin they have placed (the user's, 2026-10-18).
                int css = pl.roomChar != 0xFF ? cssOfKind(pl.roomChar) : -1;
                p.sittingOut = (bits & PPOM::SLOT_WATCHING) ? 2 : (bits & PPOM::SLOT_SITTING_OUT) ? 1 : 0;
                if (bits & PPOM::SLOT_IN_GAME) {
                    p.kind = RoomCss::PV_IN_GAME;
                } else if (css >= 0) {
                    p.kind = (bits & PPOM::SLOT_READY) ? RoomCss::PV_READY : RoomCss::PV_CHOOSING;
                    p.css = (u8)css;
                    p.costume = pl.roomCostume & PPOM::COSTUME_COLOUR;   // (an alt shows its colour)
                    // Locked in on RANDOM: the panel shows RANDOM, as that player's own does (the
                    // drawn character is not seen before the match).
                    if (pl.roomCostume & PPOM::COSTUME_RANDOM) {
                        p.css = 0x29;
                        p.costume = 0;
                    }
                    p.stocks = pl.roomStocks <= PPOM::MAX_PICKED_STOCKS ? pl.roomStocks : 0;
                } else if (bits & PPOM::SLOT_READY) {
                    // Ready without a character shown: game 1 of a set is a blind pick (the
                    // server keeps the character from the others, room-rules.md §2).
                    p.kind = RoomCss::PV_READY;
                    p.css = 0x28;
                } else {
                    p.kind = RoomCss::PV_CHOOSING;
                }
            }
        }
        // Sounds (rooms.md #16): a player joining gets Brawl's own join sound (SE 0x2051, what
        // its Wi-Fi CSS plays when a member arrives, sel_char text+0x12DE8); one leaving the back
        // sound, as an opponent leaving Direct.
        if (in && s.roomSeen && taken != s.roomTaken) {
            if (taken & ~s.roomTaken) playSE(0x2051);
            else playSE(SE_BACK);
        }
        s.roomTaken = taken;
        s.roomSeen = in;
    }

    static void onRoomStatus(const PPOM::Response& r)
    {
        s.roomPollPending = false;
        if (r.status != 0) return;
        const PPOM::RoomStatus& st = *(const PPOM::RoomStatus*)r.payload;
        char text[sizeof(s.roomText)];
        PPOM::u16ToAscii(text, st.text, sizeof(text));
        bool error = st.error != 0;
        // The error sound when an error appears (Slippi's CSS; rooms-game-interface.md).
        if (error && (!s.roomError || strcmp(text, s.roomText) != 0)) playSE(SE_ERROR);
        s.roomPhase = st.phase <= PPOM::RP_IN ? st.phase : PPOM::RP_NONE;
        s.roomError = error;
        strncpy(s.roomText, text, sizeof(s.roomText) - 1);
        s.roomText[sizeof(s.roomText) - 1] = 0;
    }

    static void pollRoom()
    {
        if (s.roomPollPending && ++s.roomPollFrames < 60) return;
        postRoom(PPOM::ROOM_POLL, 0, 0, NULL);
        s.roomPollPending = true;
        s.roomPollFrames = 0;
    }

    static void updateRoomStatus()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        if (stepSession() && s.step.active && s.stepText[0] &&
            (!roomLockedForNext() || resumeStep() || s.step.type == PPOM::STEP_CHAR) && !s.roomError) {
            setStatus(s.stepText, false);   // the set's step: whose turn, the seconds left
            return;
        }
        bool rulesRed = false;
        const char* rulesText = s.roomError ? NULL : RoomRules::statusText(&rulesRed);
        if (rulesText && roomIn()) {
            setStatus(rulesText, rulesRed);
            return;
        }
        if (roomIn() && se.roomStatus == 0 && roomPicksStage() && !roomLockedForNext() && lockChar() >= 0 &&
            !s.roomError && !roomSittingOut()) {
            setStatus("Press START to select stage", false);   // Direct's (rooms.md §3)
            return;
        }
        // In the room's game session, before its match: the picker on the stage select (the
        // server's line says "In game" for the whole session).
        int picker = s.roomError ? -1 : otherPickerChoosing(true);
        if (picker >= 0) {
            char buf[32];
            sprintf(buf, "P%d choosing stage...", picker + 1);
            setStatus(buf, false);
            return;
        }
        if (s.roomText[0] && (roomActive() || s.roomError)) {
            setStatus(s.roomText, s.roomError);
            return;
        }
        if (s.roomEntry == ROOM_ENTRY_CREATE && !roomIn()) {
            setStatus("Creating room", false);
            return;
        }
        if (selectedChar() < 0) {
            setStatus("Select your character", false);
        } else if (!roomIn()) {
            setStatus("Press START to enter code", false);
        } else if (roomLockedForNext()) {
            setStatus("Ready", false);
        } else {
            setStatus("Press START to lock in", false);
        }
    }

    // Connected for a room game: Direct's path (tickConnected), with Session.game as the room's
    // next game also between games.
    static void roomMatch()
    {
        int game = roomGame();
        if (!game || s.lockedGame != game || g_onlineMatchGame) return;
        const PPOM::Session& se = PPOM::g_block.session;
        if (se.state == PPOM::SS_MATCH_READY && se.game == game) {
            g_onlineMatchGame = (u8)game;
            PPOM::g_block.debug.scratch[10] = 0x100 * game;
            leaveCss(1);
            return;
        }
        if (s.lockStage != PPOM::STAGE_PENDING) return;
        const u16 pick = OnlineMatch::pickedStage();
        if (pick == OnlineMatch::PICK_CANCELLED) {
            unlock();
        } else if (pick != 0xFFFF) {
            lockIn(game, pick, OnlineMatch::pickedAsl());
            OnlineMatch::clearPickedStage();
            // Not locked in with the pick (no character): unlocked, START again, rather than the
            // stage select again at once.
            if (s.lockStage == PPOM::STAGE_PENDING) unlock();
        } else if (PPOM::g_block.local.remoteReady) {
            g_onlinePickStage = 1;
            leaveCss(1);
        }
    }


    // ----------------------------------------------------------------------------------------
    // The room's settings on screen (staging feedback 2026-10-10, two rounds): Public / Private
    // and FFA / Teams are the CSS's own ITEM and STAGE buttons at the right end of the bar,
    // relabelled (RoomCss::headerButtons) and pressed as the game presses them (the hand's
    // buttons 0x19 / 0x1A, cssSwitchButtons -> s_headerPress); L and R do the same. Under
    // LEAVE: the room's host, "Host: <name>", or for the host "A: open/close / Hold A: remove"
    // (window 2 of the rule line's MuMsg, attached as the own-code window). Window units are 1.3
    // screen pixels (2000 wide) from x = 1265 px (found live).
    static const u32 ROOM_HOST_WINDOW = 2;
    static const float ROOM_HOST_X1 = -905.0f, ROOM_HOST_X2 = -690.0f;
    static const float ROOM_HOST_Y = 95.0f, ROOM_HOST_HALF = 40.0f, ROOM_HOST_SCALE = 0.72f;
    // The room's ruleset and set (room_play.cpp cssLine), centred on the gap between the character
    // grid and the panels: window 3 (unused by the CSS), attached as window 2.
    static const u32 ROOM_PLAY_WINDOW = 3;
    static const float ROOM_PLAY_X1 = -817.0f, ROOM_PLAY_X2 = 260.0f;
    static const float ROOM_PLAY_Y = 348.0f, ROOM_PLAY_HALF = 20.0f, ROOM_PLAY_SCALE = 0.6f;
    // Staging feedback 2026-10-12: a long ruleset name with the score did not fit and was cut. One
    // line while it fits (ROOM_PLAY_FIT: font units at scale 1, as statusWidths measures, about
    // 1.5 px each at 2000 wide, for the window's 1400 px), else the ruleset and the set on two
    // lines at ROOM_PLAY_SCALE2, smaller still if a line is wider than the window.
    static const int ROOM_PLAY_FIT = 900;
    static const float ROOM_PLAY_SCALE2 = 0.5f, ROOM_PLAY_LINES = 17.0f;
    static char s_playPrinted[140];

    static void roomButtons(bool in, bool host)
    {
        const PPOM::Session& se = PPOM::g_block.session;
        RoomCss::headerButtons(in && (se.roomFlags & PPOM::RF_PUBLIC), in && (se.roomFlags & PPOM::RF_TEAMS));
        MuMsg* m = s.cssMsg;
        if (!m || !s.haveWindow) return;
        if (!s.roomBtnsShown) {
            u8* task = cssTask();
            u32 obj = task ? *(u32*)(task + 0x150) : 0;   // MenSelchrRule
            u32 mdl = isPtr(obj) ? *(u32*)(obj + 0x10) : 0;
            u8* message = (u8*)m->m_message;
            u32 bufs = isPtr((u32)message) ? *(u32*)(message + 0x1D8) : 0;
            if (!isPtr(mdl) || !isPtr(bufs) || *(u32*)((u8*)m + 0x10) <= ROOM_PLAY_WINDOW) return;
            float size = *(float*)(*(u32*)bufs + 0x28);
            typedef void (*AttachFn)(MuMsg*, u32, u32 scnMdl, u32 node, float size);
            ((AttachFn)0x800B8C90)(m, ROOM_HOST_WINDOW, mdl, 0, size);
            WindowSetting* ws = (WindowSetting*)(*(u32*)((u8*)m + 0xC) + 0x48 * ROOM_HOST_WINDOW);
            ws->x1 = ROOM_HOST_X1;
            ws->x2 = ROOM_HOST_X2;
            // the bar's own vertical place (the status window's), lower (window y grows down)
            float mid = (s.windowY1 + s.windowY2) * 0.5f;
            ws->y1 = mid + ROOM_HOST_HALF + ROOM_HOST_Y;
            ws->y2 = mid - ROOM_HOST_HALF + ROOM_HOST_Y;
            ws->scaleX = ws->scaleY = ROOM_HOST_SCALE;
            ws->lineSpace = 30.0f;
            m->setAlignMode(ROOM_HOST_WINDOW, MuMsg::Align_Left);
            m->setFontColor(ROOM_HOST_WINDOW, 0xFF, 0xFF, 0xFF, 0xFF);
            s.roomBtnPrinted[0][0] = 1;   // not printed yet
            ((AttachFn)0x800B8C90)(m, ROOM_PLAY_WINDOW, mdl, 0, size);
            WindowSetting* wp = (WindowSetting*)(*(u32*)((u8*)m + 0xC) + 0x48 * ROOM_PLAY_WINDOW);
            wp->x1 = ROOM_PLAY_X1;
            wp->x2 = ROOM_PLAY_X2;
            wp->y1 = mid + ROOM_PLAY_HALF + ROOM_PLAY_Y;
            wp->y2 = mid - ROOM_PLAY_HALF + ROOM_PLAY_Y;
            wp->scaleX = wp->scaleY = ROOM_PLAY_SCALE;
            m->setAlignMode(ROOM_PLAY_WINDOW, MuMsg::Align_Center);
            m->setFontColor(ROOM_PLAY_WINDOW, 0xFF, 0xFF, 0xFF, 0xFF);
            s_playPrinted[0] = 1;
            s.roomBtnsShown = true;
        }
        // The ruleset and the set: the sides' players by their in-game ports (the room's slots).
        char play[128];
        play[0] = 0;
        if (in && !RoomRules::isOpen()) {   // (under the rules menu it showed below the STAGE SWITCH)
            char side[2][RoomCss::NAME_CHARS + 1];
            for (int i = 0; i < 2; i++) {
                u8 port = se.play.sides[i];
                side[i][0] = 0;
                if (port < PPOM::SESSION_PLAYERS) u16Name(side[i], se.players[port].name);
                if (!side[i][0]) strcpy(side[i], i ? "P2" : "P1");
            }
            RoomPlay::cssLine(play, sizeof(play), side[0], side[1]);
        }
        // One line ("Ruleset: X   Bo3: a 1 - 0 b") when it fits, else two.
        char* nl = NULL;   // (the DOL has no strchr)
        for (char* q = play; *q && !nl; q++) if (*q == '\n') nl = q;
        char line[140];
        float scale = ROOM_PLAY_SCALE;
        bool two = false;
        if (nl) {
            int a = (int)(nl - play);
            memcpy(line, play, a);
            strcpy(line + a, "   ");
            strcpy(line + a + 3, nl + 1);
            int w = statusWidths(line, (int)strlen(line), NULL);
            if (w * ROOM_PLAY_SCALE > ROOM_PLAY_FIT) {
                two = true;
                strcpy(line, play);
                int w1 = statusWidths(play, a, NULL), w2 = statusWidths(nl + 1, (int)strlen(nl + 1), NULL);
                int wmax = w1 > w2 ? w1 : w2;
                scale = ROOM_PLAY_SCALE2;
                if (wmax * scale > ROOM_PLAY_FIT) scale = (float)ROOM_PLAY_FIT / (float)wmax;
            }
        } else {
            strcpy(line, play);
        }
        if (strcmp(line, s_playPrinted) != 0) {
            WindowSetting* wp = (WindowSetting*)(*(u32*)((u8*)m + 0xC) + 0x48 * ROOM_PLAY_WINDOW);
            float mid = (s.windowY1 + s.windowY2) * 0.5f + ROOM_PLAY_Y;
            float half = two ? ROOM_PLAY_HALF * 2.0f : ROOM_PLAY_HALF;
            wp->y1 = mid + half;
            wp->y2 = mid - half;
            wp->scaleX = wp->scaleY = scale;
            if (two) wp->lineSpace = ROOM_PLAY_LINES;
            m->printf(ROOM_PLAY_WINDOW, "%s", line);
            strcpy(s_playPrinted, line);
        }
        char text[40];
        text[0] = 0;
        // (not under the rules menu, where it read as the menu's own help)
        if (in && !RoomRules::isOpen()) {
            char hostName[RoomCss::NAME_CHARS + 1];
            hostName[0] = 0;
            if (se.roomHost < PPOM::SESSION_PLAYERS) u16Name(hostName, se.players[se.roomHost].name);
            if (host) strcpy(text, "A: open/close\nHold A: remove");
            else sprintf(text, "Host:\n%s", hostName);
        }
        if (strcmp(text, s.roomBtnPrinted[0]) == 0) return;
        m->printf(ROOM_HOST_WINDOW, "%s", text);
        strncpy(s.roomBtnPrinted[0], text, sizeof(s.roomBtnPrinted[0]) - 1);
        s.roomBtnPrinted[0][sizeof(s.roomBtnPrinted[0]) - 1] = 0;
    }

    // Not ready: the coin placed on the CSS goes to the others before START (the user's,
    // 2026-10-18), as a lock-in without its ready (Dolphin sends it as room-ready {ready: false,
    // character}; the gameplay session takes only a ready lock-in). Its character is the coin's,
    // never drawn: RANDOM is sent as RANDOM (cssChar 0x29), which Dolphin shows the others as
    // RANDOM. The coin in the hand: no character. Only between games outside a session (a set's
    // session takes its lock-ins itself), and never while locked in.
    static void roomPreview(bool may)
    {
        if (!may || roomLocked() || stepSession() || CodeEntry::active()) return;
        int c = selectedChar();
        if (c < 0) {
            PPOM::writeLockIn(false, 0xFF, 0xFF, 0, 0xFFFF, 0, 0, NULL, s.startPad);
            return;
        }
        int coin = plainCss(c);
        u8 kind = charKindOf(coin == 0x29 ? 0x00 : coin);
        PPOM::writeLockIn(false, (u8)coin, kind, (u8)selectedCostume(), 0xFFFF, 0, (u8)roomGame(), NULL, s.startPad);
    }

    static void tickRoom(u32 pressed, u32 held)
    {
        const PPOM::Session& se = PPOM::g_block.session;
        if (s.roomEntry == ROOM_ENTRY_CREATE && !s.roomCreateSent) {
            postRoom(PPOM::ROOM_CREATE, 1, 0, NULL);   // public by default (rooms.md #17)
            s.roomCreateSent = true;
        }
        pollRoom();
        bool in = roomIn();
        int me = roomMe();
        bool host = in && me >= 0 && se.roomHost == me;
        bool waiting = in && se.roomStatus == 0;
        const u32 BTN_A = 0x100, BTN_L = 0x40, BTN_R = 0x20;
        // P+'s rules menu over the CSS (room_rules.cpp): while it is open its pages take the
        // buttons; the room's own (L/R, START, B, the header buttons, the panels) wait.
        // On the CSS between a running set's games (the room in game, its session on, not in a
        // match): the host may open RULES for End set alone, while not locked in for the next game
        // (a lock-in could start the match under the menu).
        const PPOM::RoomPlay& play = se.play;
        bool setBetween = in && !waiting && host && play.valid && play.setOn && !(play.setFlags & PPOM::SET_FINISHED) &&
                          stepSession() && !g_onlineMatchGame && !roomLockedForNext();
        bool rulesOpen = RoomRules::tick(in, host, waiting, setBetween);
        // RULES next to PUBLIC / FFA (rules_button.cpp); greyed while the menu would not open
        // (a game starting, no room rules), and passed over by the hand under the open menu.
        RulesButton::tick(in, (waiting || setBetween) && se.rules.valid);
        // SIT OUT / PLAY under it (the user's, 2026-10-18): greyed in this player's own game and
        // while a set is on (its sides play it to the end; mm refuses it then).
        const bool sitting = roomSittingOut();
        const bool meInGame = in && me >= 0 && (se.players[me].roomSlot & PPOM::SLOT_IN_GAME);
        const bool setOn = play.valid && play.setOn && !(play.setFlags & PPOM::SET_FINISHED);
        const bool maySitOut = in && me >= 0 && se.roomStatus != 1 && !meInGame && !g_onlineMatchGame && !setOn;
        SitOutButton::tick(in, maySitOut, sitting);
        // The ruleset and the set on the RULES page's RULESET / SET lines (room_play.cpp).
        RoomPlay::tick(in, host);
        // rules.md #5: a change of the rules or of the ruleset (its set length) took back every ready
        if ((RoomRules::changedNow() || RoomPlay::changedNow()) && waiting && roomLockedForNext()) unlock();
        if (rulesOpen) {
            s_headerPress = 0;
            s.roomKickPort = -1;
            s.roomRHeld = false;
            roomButtons(in, host);   // (the host's "A: open/close" hint goes while the menu is open)
            return;
        }
        // The RULESET line's "Enter code": the keypad's ruleset mode once the menu has closed.
        if (RoomPlay::takeKeypadRequest() && in && host && waiting) {
            CodeEntry::openRuleset(LOCAL_PORT);
            return;
        }
        // Watching the room's game (the user's, 2026-10-19; docs/rooms-game-interface.md): Dolphin
        // put a game this player is not in into SESSION (RF_WATCH). Its match starts from here as
        // the room's own game does, and the game comes back to this CSS after it. A ready from
        // before is taken back: the room does not wait for a watcher, so it would not hold.
        if (in && (se.roomFlags & PPOM::RF_WATCH) && se.state == PPOM::SS_MATCH_READY && se.game &&
            !g_onlineMatchGame && !CodeEntry::active()) {
            if (roomLockedForNext()) unlock();
            g_onlineMatchGame = se.game;
            PPOM::g_block.debug.scratch[9] += 0x10000;   // tests: (with launcher joins) the CSS left
            leaveCss(1);
            return;
        }
        // Back on the CSS: no stage select for the steps any more.
        s.roomStepSss = false;
        // A room's set (Dolphin's GameSetup, as Ranked): the stage steps on the stage select, the
        // characters on the CSS when the step allows (the winner first), START only then.
        const bool holding = in && quitHolding(pressed, roomGame(), roomLockedForNext());
        bool setStep = stepSession() && s.step.active && !holding;
        if (setStep && stepFresh() && !g_onlineMatchGame) {
            if (s.step.toSss) {
                s.roomStepSss = true;
                g_onlinePickStage = 1;
                leaveCss(1);
                return;
            }
            if (!roomLockedForNext() && s.step.mayLock && s.step.timeUp && lockChar() >= 0)
                lockIn(roomGame(), 0xFFFF, 0);
        }
        if (!in) {
            // Not in a room (Join Room, or no longer in one): START enters a room code.
            if ((pressed & BTN_START) && selectedChar() >= 0 && s.roomPhase != PPOM::RP_JOINING) {
                CodeEntry::open(LOCAL_PORT, true);
            }
            s.roomKickPort = -1;
        } else {
            // Sitting out (the host's, or a set that started without us) takes back a ready. So
            // does the stage pick moving to or from this player (someone sat out or plays again;
            // mm took the ready back): the lock-in said whether they pick (STAGE_PENDING).
            if (roomLockedForNext() && !setStep && !g_onlineMatchGame && waiting &&
                (sitting || (s.lockStage == PPOM::STAGE_PENDING) != roomPicksStage())) {
                unlock();
            }
            s_sitPressHeld &= held | pressed;
            pressed &= ~s_sitPressHeld;
            if (pressed & BTN_START) {
                if (setStep) {
                    if (!roomLockedForNext() && s.step.mayLock && lockChar() >= 0) lockIn(roomGame(), 0xFFFF, 0);
                } else if (sitting) {
                    // START while sitting out plays again; it does not ready (the user's, 2026-10-18).
                    if (maySitOut) {
                        postRoom(PPOM::ROOM_SIT_OUT, (u8)me, 0, NULL);
                        s_sitPressHeld |= BTN_START;
                        playSE(1);
                    } else {
                        playSE(SE_ERROR);
                    }
                } else if (!roomLockedForNext() && waiting && lockChar() >= 0) {
                    lockIn(roomGame(), roomPicksStage() ? PPOM::STAGE_PENDING : 0xFFFF, 0);
                }
            }
            // SIT OUT / PLAY, and A on a card's name plate (Brawl's player / CPU switch offline):
            // the player's own, or the host's on anyone's. Sitting out takes a ready back first.
            int plate = (pressed & BTN_A) ? RoomCss::plateUnderHand() : -1;
            if (plate >= 0 && !(se.players[plate].roomSlot & PPOM::SLOT_TAKEN)) plate = -1;   // (an empty slot: the host's open / close below)
            int sitSlot = SitOutButton::pressed() && !(s_sitPressHeld & BTN_A) ? me : -1;
            if (plate >= 0) {
                maskPressed(BTN_A);
                if (plate == me || host) sitSlot = plate;
                else playSE(SE_ERROR);
            }
            if (sitSlot >= 0) {
                bool now = (se.players[sitSlot].roomSlot & PPOM::SLOT_SITTING_OUT) != 0;
                bool slotInGame = (se.players[sitSlot].roomSlot & PPOM::SLOT_IN_GAME) != 0;
                if (!in || se.roomStatus == 1 || slotInGame || setOn || (sitSlot == me && !maySitOut)) {
                    playSE(SE_ERROR);
                } else {
                    if (sitSlot == me && !now && roomLockedForNext()) unlock();
                    postRoom(PPOM::ROOM_SIT_OUT, (u8)sitSlot, now ? 0 : 1, NULL);
                    s_sitPressHeld |= BTN_A;
                    playSE(1);
                }
            }
            if ((pressed & BTN_B) && roomLockedForNext() && waiting && !g_onlineMatchGame) {
                maskPressed(BTN_B);   // the coin stays where it is (as Direct's B)
                unlock();
                playSE(SE_BACK);
            }
            // ITEM (Public / Private) and STAGE (FFA / Teams), pressed with A as the game does
            u8 hdr = s_headerPress;
            s_headerPress = 0;
            // R switches TEAMS when it is let go, unless START was pressed while it was down: that
            // is P+'s "hold R for AltR" lock-in (s.startAlt), which the room's switch must not eat.
            bool teamsSwitch = false;
            if ((pressed & BTN_R) && host && waiting) s.roomRHeld = true;
            if (s.roomRHeld && (pressed & BTN_START)) s.roomRHeld = false;
            if (s.roomRHeld && !(held & BTN_R) && !(pressed & BTN_R)) {
                s.roomRHeld = false;
                teamsSwitch = host && waiting;
            }
            if (hdr) {
                if (host && hdr == 0x1A && waiting) teamsSwitch = true;   // as R
                else if (host && hdr == 0x19) pressed |= BTN_L;           // as L
                else playSE(SE_ERROR);
            }
            if (RulesButton::pressed()) RoomRules::open(!waiting);   // host edits, others view; End set
            if ((pressed & BTN_L) && host) {
                postRoom(PPOM::ROOM_PUBLIC, (se.roomFlags & PPOM::RF_PUBLIC) ? 0 : 1, 0, NULL);
                playSE(1);
            }
            if (teamsSwitch) {
                postRoom(PPOM::ROOM_TEAMS, (se.roomFlags & PPOM::RF_TEAMS) ? 0 : 1, 0, NULL);
                playSE(1);
            }
            // A on a slot's panel: an empty slot opens or closes at once. An occupied one closes
            // (its player removed, free to join again once a slot is open) only while A is held
            // on it for KICK_HOLD_DELAY frames (rooms.md #10), so a press meant for something else
            // there (Brawl's team flag) removes nobody; let go early, the buzzer.
            int port = host && waiting ? RoomCss::panelUnderHand() : -1;   // panels in slot order
            if ((pressed & BTN_A) && port >= 0 && plate < 0) {
                maskPressed(BTN_A);
                u8 bits = se.players[port].roomSlot;
                if (bits & PPOM::SLOT_TAKEN) {
                    s.roomKickPort = port;
                    s.roomKickHeld = 0;
                } else {
                    postRoom(PPOM::ROOM_SLOT, (u8)port, (bits & PPOM::SLOT_OPEN) ? 0 : 1, NULL);
                    playSE(1);
                }
            }
            // The held field (+0x8) has A a frame after the press (harness, 2026-10-12: the hold
            // was let go on the frame it began and nobody could be removed), so the press counts.
            if (s.roomKickPort >= 0) {
                if (port != s.roomKickPort || !(se.players[port].roomSlot & PPOM::SLOT_TAKEN)) {
                    s.roomKickPort = -1;   // the hand moved off, the player left, or a game started
                } else if (!((held | pressed) & BTN_A)) {
                    s.roomKickPort = -1;
                    playSE(SE_ERROR);
                } else if (++s.roomKickHeld > KICK_HOLD_DELAY) {
                    postRoom(PPOM::ROOM_SLOT, (u8)port, 0, NULL);
                    s.roomKickPort = -1;
                    playSE(1);
                }
            }
            // Teams on: A on the local panel, its flag included, picks the next team colour; X / Y
            // change the costume, as in a free-for-all (Brawl's costume buttons). That is P+'s own
            // team battle with the players' costumes (its VERSUS button's rainbow TEAM BATTLE,
            // task+0x5CB 2, which RoomCss sets); until 2026-10-15 X / Y were the team and A the
            // costume, the other way round, and a P+ player pressing X / Y for a colour changed
            // team (bug #22; the user's). The panel's A is Brawl's own click (cssPanelClick,
            // g_cssPanelTeam: the plate and the hand's other targets stay theirs); the flag is no
            // target of Brawl's, so its A is taken here.
            bool teamsOn = (se.roomFlags & PPOM::RF_TEAMS) && se.roomMode == 2;
            g_cssTeamRoom = teamsOn ? 1 : 0;
            bool teamPress = g_cssPanelTeam != 0;
            g_cssPanelTeam = 0;
            if (teamsOn && (pressed & BTN_A) && RoomCss::handOverMyFlag()) {
                maskPressed(BTN_A);
                teamPress = true;
            }
            if (teamsOn && teamPress && me >= 0 && waiting && !roomLockedForNext()) {
                int cur = se.players[me].roomTeam < PPOM::TEAM_COUNT ? se.players[me].roomTeam : 0;
                postRoom(PPOM::ROOM_TEAM, (u8)((cur + 1) % PPOM::TEAM_COUNT), 0, NULL);
                playSE(1);
            }
            roomMatch();
            roomPreview(me >= 0 && !meInGame && se.roomStatus != 1 && !g_onlineMatchGame);
        }
        RoomCss::setLocalSlot(in && me >= 0 ? me : 0, in);
        RoomCss::PanelView views[3];
        int myTeam = -1;
        roomPanels(views, &myTeam);
        bool teams = in && (se.roomFlags & PPOM::RF_TEAMS) && se.roomMode == 2;
        // The local plate: the name tag picked on it (staging feedback 2026-10-11: with the
        // account name there the tag was not to be seen), else the name the room has for us,
        // else the account's (Join Room). The others see the account name.
        char myName[RoomCss::NAME_CHARS + 1];
        tagName(selectedTag(), myName);
        if (!myName[0]) {
            if (in && me >= 0) u16Name(myName, se.players[me].name);
            else strcpy(myName, s.ownName);
        }
        // Stock control: Brawl's picker on the local STOCKS box opens while locked in too; the pick
        // stays the one the lock-in carries (as the character, cssCoinAPress).
        if (g_onlineCssLock && (pressed & BTN_A) && RoomCss::handOnMyStocks()) maskPressed(BTN_A);
        RoomCss::tick(views, teams, myTeam, in && me >= 0 ? roomStars(me, host) : 0, myName, roomLockedForNext(),
                      roomStockControl(), roomLockedForNext() ? s.lockedStocks : -1, sitting);
        roomButtons(in, host);
    }

    // LOCAL.screen (rooms-game-interface.md §2): where the game is, for the launcher's joins.
    static u32 s_jumpFrame = 0;     // the last frame a menu page we can jump from ran
    static u8* s_jumpPage = NULL;
    static void reportScreen(const char* scene)
    {
        u8 screen = PPOM::SCREEN_OTHER;
        if (strcmp(scene, "scMelee") == 0) {
            // The waiting room is a search, not a match (the launcher's "In game").
            screen = TrainingRoom::active() ? PPOM::SCREEN_ONLINE_BUSY : PPOM::SCREEN_MATCH;
        } else if (strcmp(scene, "muMenuMain") == 0) {
            screen = PPOM::g_block.debug.frames - s_jumpFrame < 3 ? PPOM::SCREEN_MENUS : PPOM::SCREEN_OTHER;
        } else if (g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0) {
            if (roomCss()) screen = PPOM::SCREEN_ROOM;
            else screen = s.phase == PH_IDLE ? PPOM::SCREEN_ONLINE_CSS : PPOM::SCREEN_ONLINE_BUSY;
        } else if (g_onlineCss && s.mode >= 0) {
            screen = PPOM::SCREEN_ONLINE_BUSY;   // the online stage select, the loading screens
        } else if (strncmp(scene, "sc", 2) == 0) {
            screen = PPOM::SCREEN_OFFLINE;
        }
        PPOM::g_block.local.screen = screen;
    }

    // A menu page the launcher's join can leave from (the top page, ONLINE, WITH FRIENDS, WITH
    // ANYONE): its update runs (netmenu.cpp / anyone_menu.cpp report it every frame).
    void menuPageRunning(u8* page)
    {
        s_jumpFrame = PPOM::g_block.debug.frames;
        s_jumpPage = page;
        NetMenu::ensureWifiTask();   // the boot to the ONLINE page opens no connect window
    }

    // The main menu's top page (GROUP, SOLO, PLAY ONLINE, ...; netmenu.cpp topUpdate) is one too:
    // a player who backed out of the ONLINE page after a set was told "Finish your current game
    // first." (prod, 2026-10-12). Its Wi-Fi task is made on the way out (decideOnlineCss), as the
    // connect window makes it on PLAY ONLINE, not while the player may still go offline.
    void topPageRunning(u8* page)
    {
        s_jumpFrame = PPOM::g_block.debug.frames;
        s_jumpPage = page;
    }

    // A menu page the launcher's requests can leave from is running (menuPageRunning).
    static bool onJumpPage(const char* scene)
    {
        return strcmp(scene, "muMenuMain") == 0 && PPOM::g_block.debug.frames - s_jumpFrame < 3 && s_jumpPage;
    }

    // Leave that page for the Wi-Fi CSS (sqNetAnyOkiraku): muProcMenu's decide (sora_menu_rule,
    // module 18, text+0x1AE0) with decision 0x1E, as the WITH FRIENDS buttons do. The caller says
    // which CSS it is (enter / enterRoom) right after.
    static bool decideOnlineCss()
    {
        u32 m = *(u32*)0x800030C8;
        u32 rule = 0;
        for (int n = 0; isPtr(m) && n < 64; n++) {
            if (*(u32*)m == 18) {
                u32 sec = *(u32*)(m + 0x10);
                rule = isPtr(sec) ? (*(u32*)(sec + 8) & ~1u) : 0;
                break;
            }
            m = *(u32*)(m + 4);
        }
        if (!rule) return false;
        NetMenu::ensureWifiTask();   // from the top page none has been made yet
        typedef void (*DecideFn)(u8*, int, int);
        ((DecideFn)(rule + 0x1AE0))(s_jumpPage, 0x1E, 0);
        playSE(1);
        return true;
    }

    // The launcher's join (LOCAL.roomJoin bumped: Dolphin has already sent the join): go to the
    // room's CSS from a menu page (muProcMenu's decision 0x1E, as the WITH FRIENDS buttons) or an
    // idle online CSS (built again as a room's).
    static u8 s_roomJoinSeen = 0;
    static bool s_roomJoinInit = false;
    static void launcherJoin(const char* scene)
    {
        u8 j = PPOM::g_block.local.roomJoin;
        if (!s_roomJoinInit) {
            s_roomJoinSeen = j;   // only later bumps are joins (the value at boot is old)
            s_roomJoinInit = true;
            return;
        }
        if (j == s_roomJoinSeen) return;
        if (onJumpPage(scene)) {
            if (!decideOnlineCss()) return;
            enterRoom(ROOM_ENTRY_JOIN);
            NetMenu::setReturnButton(2);
            s_roomJoinSeen = j;
            s_jumpPage = NULL;
            PPOM::g_block.debug.scratch[9] += 0x10000;   // tests: launcher joins taken
            return;
        }
        if (g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0 && !CodeEntry::active()) {
            s_roomJoinSeen = j;
            if (roomCss()) return;   // already on a room's CSS: its panels follow
            if (s.phase != PH_IDLE) return;
            enterRoom(ROOM_ENTRY_JOIN);
            s.roomReload = true;   // the four panels need the CSS built again
            PPOM::g_block.debug.scratch[9] += 0x10000;
            leaveCss(1);           // OnlineMatch: no match armed -> back to the CSS (restoreCss)
        }
    }

    // The launcher's create (LOCAL.roomCreate bumped: Dolphin has left any room and sent the
    // create, public as Create Room's): the room CSS as WITH FRIENDS > Create Room left it, its
    // create already sent ("Creating room", a refusal in red, then the new room), from a menu page,
    // an idle online CSS (built again as a room's) or a room's CSS (its panels follow).
    static u8 s_createSeen = 0;
    static bool s_createInit = false;
    static void enterCreated()
    {
        s.roomEntry = ROOM_ENTRY_CREATE;
        s.roomCreateSent = true;   // Dolphin sent it
        s.roomText[0] = 0;
        s.roomError = false;
        s.roomSeen = false;        // the join sounds start over with the new room
        s.roomKickPort = -1;
        NetMenu::setReturnButton(0);   // back from the CSS: WITH FRIENDS, Create Room
        PPOM::g_block.debug.scratch[9] += 0x100;   // tests: launcher creates taken
    }
    static void launcherCreate(const char* scene)
    {
        u8 j = PPOM::g_block.local.roomCreate;
        if (!s_createInit) {
            s_createSeen = j;   // only later bumps are creates (the value at boot is old)
            s_createInit = true;
            return;
        }
        if (j == s_createSeen) return;
        if (onJumpPage(scene)) {
            if (!decideOnlineCss()) return;
            enterRoom(ROOM_ENTRY_CREATE);
            enterCreated();
            s_createSeen = j;
            s_jumpPage = NULL;
            return;
        }
        if (g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0 && !CodeEntry::active()) {
            s_createSeen = j;
            if (roomCss()) {
                unlock();   // a ready in the old room is not one in the new
                enterCreated();
                return;
            }
            if (s.phase != PH_IDLE) return;
            enterRoom(ROOM_ENTRY_CREATE);
            enterCreated();
            s.roomReload = true;   // the four panels need the CSS built again
            leaveCss(1);           // OnlineMatch: no match armed -> back to the CSS (restoreCss)
        }
    }

    // The launcher's Direct invite (LOCAL.directJoin bumped: Dolphin accepted it and has left any
    // room): the other player's code from LOCAL, checked again (2-4 letters, '#', 1-9999, at most
    // 8 characters, as the server's codes). False: no code (a plain Direct CSS).
    static bool inviteCode(char* out)
    {
        const PPOM::Local& l = PPOM::g_block.local;
        int n = 0;
        while (n < 4 && l.directPrefix[n]) {
            char c = l.directPrefix[n];
            if (c < 'A' || c > 'Z') return false;
            out[n++] = c;
        }
        for (int k = n; k < 4; k++) {
            if (l.directPrefix[k]) return false;   // NUL-padded
        }
        u32 num = l.directNumber;
        int digits = num >= 1000 ? 4 : num >= 100 ? 3 : num >= 10 ? 2 : 1;
        if (n < 2 || num < 1 || num > 9999 || n + 1 + digits > PPOM::CODE_LEN - 1) return false;
        sprintf(out + n, "#%u", num);
        return true;
    }

    static void presetCode()
    {
        char code[PPOM::CODE_LEN + 1];
        s.codePreset = inviteCode(code);
        if (s.codePreset) strcpy(s.code, code);
        else s.code[0] = 0;
        s.statusDirty = true;
    }

    // The launcher's Play unranked / Play ranked: LOCAL.directJoin bumped with a zero prefix and
    // directNumber 1 / 2 (docs/rooms-game-interface.md). That mode, else -1 (a Direct invite).
    static const int QUEUE_PLAYBACK = 100;   // launcherQueue: a replay (PPOM v8)
    static int launcherQueue()
    {
        const PPOM::Local& l = PPOM::g_block.local;
        for (int k = 0; k < 4; k++) {
            if (l.directPrefix[k]) return -1;
        }
        if (l.directNumber == PPOM::QUEUE_PLAYBACK) return QUEUE_PLAYBACK;
        return l.directNumber == PPOM::QUEUE_UNRANKED ? PPOM::MODE_UNRANKED :
               l.directNumber == PPOM::QUEUE_RANKED ? PPOM::MODE_RANKED : -1;
    }

    // A replay: the CSS of the mode it was played in (SESSION's), which then starts its match.
    static void enterPlayback(int mode)
    {
        enter(mode);
        s.playback = true;
        s.playbackPending = true;
    }

    // The CSS of `mode`: Direct with the invite's code (back from the CSS: WITH FRIENDS, Direct
    // 1v1); Unranked / Ranked as WITH ANYONE's (enter: back to the ONLINE page).
    static void enterLauncherMode(int mode)
    {
        enter(mode);
        if (mode != PPOM::MODE_DIRECT) return;
        presetCode();
        NetMenu::setReturnButton(1);
    }

    // Go to the Direct CSS with that code from a menu page (as a room join), an idle online CSS of
    // another mode or a room's CSS (built again as Direct's, the character kept), or stay on an
    // idle Direct CSS with the new code. START then searches it once the character is picked.
    // Play unranked / Play ranked go to that mode's CSS the same way (an idle one of it stays).
    static u8 s_directSeen = 0;
    static bool s_directInit = false;
    static void launcherDirect(const char* scene)
    {
        u8 j = PPOM::g_block.local.directJoin;
        if (!s_directInit) {
            s_directSeen = j;   // only later bumps are invites (the value at boot is old)
            s_directInit = true;
            return;
        }
        if (j == s_directSeen) return;
        int queue = launcherQueue();
        if (queue == QUEUE_PLAYBACK) {
            u8 sm = PPOM::g_block.session.mode;
            int mode = sm <= PPOM::MODE_TEAMS ? sm : PPOM::MODE_UNRANKED;
            if (onJumpPage(scene)) {
                if (!decideOnlineCss()) return;
                enterPlayback(mode);
                s_directSeen = j;
                s_jumpPage = NULL;
                return;
            }
            if (g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0 && !CodeEntry::active()) {
                s_directSeen = j;
                if (s.mode == mode && !roomCss() && s.phase == PH_IDLE) {
                    s.playback = true;
                    s.playbackPending = true;
                    return;
                }
                bool room = roomCss();
                enterPlayback(mode);
                if (room) RoomCss::releaseRecords();
                leaveCss(1);   // OnlineMatch: no match armed -> back to the CSS (restoreCss)
            }
            return;
        }
        int mode = queue >= 0 ? queue : PPOM::MODE_DIRECT;
        if (onJumpPage(scene)) {
            if (!decideOnlineCss()) return;
            enterLauncherMode(mode);
            s_directSeen = j;
            s_jumpPage = NULL;
            PPOM::g_block.debug.scratch[9] += 0x1000000;   // tests: launcher Direct invites / Plays taken
            return;
        }
        if (g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0 && !CodeEntry::active()) {
            s_directSeen = j;
            bool room = roomCss();
            if (!room && s.phase != PH_IDLE) return;   // searching or connected: not taken
            PPOM::g_block.debug.scratch[9] += 0x1000000;
            if (s.mode == mode && !room) {
                if (mode == PPOM::MODE_DIRECT) presetCode();   // already on Direct's CSS: only the code changes
                return;
            }
            // A room's CSS: Dolphin has left the room; its panels go with the CSS built again.
            enterLauncherMode(mode);
            if (room) RoomCss::releaseRecords();
            leaveCss(1);   // OnlineMatch: no match armed -> back to the CSS (restoreCss)
        }
    }

    // START would ready the player: a character is selected, nothing is locked in and START
    // searches, opens the code keypad or locks in (the lines "Press START to ..."). Brawl's
    // READY TO FIGHT banner shows only then (hideHeaderArt), as Slippi's online CSS shows it
    // until START locks in.
    static bool startReadies()
    {
        if (CodeEntry::active() || s.playback) return false;
        if (roomCss()) {
            const PPOM::Session& se = PPOM::g_block.session;
            if (!roomIn()) {
                // Join Room, or no longer in one: START enters a room code ("Creating room":
                // the room is on its way, START is not the next step).
                if (s.roomEntry == ROOM_ENTRY_CREATE && !s.roomError) return false;
                return selectedChar() >= 0 && s.roomPhase != PPOM::RP_JOINING;
            }
            if (stepSession() && s.step.active)
                return s.step.mayLock && !roomLockedForNext() && lockChar() >= 0;
            // Sitting out, START plays again: it does not ready.
            return se.roomStatus == 0 && !roomLocked() && lockChar() >= 0 && !roomSittingOut();
        }
        if (s.phase == PH_IDLE) return selectedChar() >= 0;
        if (s.phase != PH_CONNECTED || !sessionGame() || lockedIn() || lockChar() < 0) return false;
        // Ranked: only in the steps that take a lock-in (not the strikes on the stage select).
        return s.mode != PPOM::MODE_RANKED || (s.step.active && s.step.mayLock && !s.step.toSss);
    }

    // The waiting room (training_room.cpp): Unranked and Ranked search from Brawl's online waiting
    // room. Not an online match (Dolphin's session never runs in it), so the mailbox works as on
    // the CSS: the search is polled, its line goes into the room's banner, and the room ends:
    //  - Unranked, once the first game is set up (SESSION): straight into the match;
    //  - Ranked, once connected: the CSS, where the game setup steps begin (Slippi's order);
    //  - START held for ROOM_HOLD_START_FRAMES: the search is cancelled (as B on the CSS), back
    //    to the CSS. A press does nothing (bug report #37, 2026-10-16: a player pressed START in
    //    the room, as Training's pause, just as a ranked match was made, and that set counted as
    //    his abandon). Holding B does not cancel (the user's, 2026-10-16; B is the fighter's);
    //  - the search ended by itself (an error, the opponent gone): back to the CSS, which says so.
    // START is taken out of the room (it would pause it). A START still held from the CSS's search
    // press counts only once it has been let go in the room.
    static const u32 ROOM_HOLD_START_FRAMES = 30;
    static void tickWaitingRoom()
    {
        pollMailbox();
        pollMatchState();
        TrainingRoom::tick();
        if (!TrainingRoom::running()) return;
        blockCodeMenu();
        const bool startHeld = (padButtons(0x0) & BTN_START) != 0;
        maskButtons(BTN_START);
        if (!startHeld) s.roomStartUp = true;
        s.roomHoldStart = (startHeld && s.roomStartUp) ? s.roomHoldStart + 1 : 0;
        updateStatus();
        if (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING) {
            char banner[sizeof(s.status) + 32];
            sprintf(banner, "%s - Hold START to cancel", s.status);
            TrainingRoom::setBanner(banner, s.statusRed);
        } else {
            TrainingRoom::setBanner(s.status, s.statusRed);
        }
        if (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING) {
            if (s.roomHoldStart >= ROOM_HOLD_START_FRAMES) {
                s.roomHoldStart = 0;
                cleanup();
                playSE(SE_BACK);
                TrainingRoom::end(TrainingRoom::END_CSS);
            }
            return;
        }
        if (s.phase == PH_CONNECTED) {
            const int game = sessionGame();
            if (!game) return;
            if (s.mode == PPOM::MODE_RANKED) {
                TrainingRoom::end(TrainingRoom::END_CSS);
                return;
            }
            const PPOM::Session& se = PPOM::g_block.session;
            if (s.lockedGame == game && se.state == PPOM::SS_MATCH_READY && se.game == game) {
                g_onlineMatchGame = (u8)game;
                PPOM::g_block.debug.scratch[10] = 0x100 * game;   // tests: the match started
                TrainingRoom::end(TrainingRoom::END_MATCH);
            }
            return;
        }
        TrainingRoom::end(TrainingRoom::END_CSS);   // idle (the search ended) or an error
    }

    // The search started for the waiting room: the CSS leaves once the screen is black (or after
    // ROOM_LEAVE_MAX_FRAMES, should the fade not be drawn), once.
    static const u32 ROOM_LEAVE_MAX_FRAMES = 30;
    static void tickRoomLeave()
    {
        if (!TrainingRoom::active() || s.roomLeaveFrames > ROOM_LEAVE_MAX_FRAMES) return;
        if (TrainingRoom::screenBlack() || ++s.roomLeaveFrames >= ROOM_LEAVE_MAX_FRAMES) {
            s.roomLeaveFrames = ROOM_LEAVE_MAX_FRAMES + 1;
            leaveCss(1);
        }
    }

    void waitingRoomEnded(int how)
    {
        // Ended by the game itself (not by tickWaitingRoom): no search may go on behind the CSS.
        if (how == TrainingRoom::END_NONE && (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING)) cleanup();
    }

    // START with the coin not put down (the user's, 2026-10-19, bug reports #63-#65: since
    // 4a39b3be such a START locked in, connected, the last game's character, while the panel
    // showed the one under the hand). With the coin in the hand over the character grid, START is
    // A on that controller: the coin goes down on the character under the hand; it locks in
    // nothing, START again does. While the coin flies to its place (A just pressed), a START waits
    // for it to land. Not while the CSS is locked (A does nothing then), nor for a room's player
    // sitting out (START plays again). The START that put the coin down does nothing more until
    // it is let go (the pad's pressed field can have it again on the next frame, which readied).
    static const u32 HAND_TARGET = 0x80;            // muSelCharHand: what the hand is over
    static const int HAND_GRID_HOLDING = 7;         // the character grid, holding the coin
    static void startWithCoinUp(u32& pressed, u32 held)
    {
        if (s.startDropHeld) {
            if ((held | pressed) & BTN_START) pressed &= ~BTN_START;
            else s.startDropHeld = false;
        }
        u8* area = playerArea();
        if (!area || g_onlineCssLock || CodeEntry::active() || (roomCss() && roomSittingOut())) {
            s.startOnLanding = false;
            return;
        }
        if (area[0x1F9]) {
            if (pressed & BTN_START) s.startOnLanding = true;
            pressed &= ~BTN_START;
            return;
        }
        if (area[0x1F8]) {
            s.startOnLanding = false;
            if (!(pressed & BTN_START)) return;
            pressed &= ~BTN_START;
            u8* hand = *(u8**)(area + 0x1A8);
            if (isPtr((u32)hand) && *(int*)(hand + HAND_TARGET) == HAND_GRID_HOLDING) {
                addPressed(s.startPad, 0x100);
                s.startDropHeld = true;
            }
            return;
        }
        if (s.startOnLanding) {
            s.startOnLanding = false;
            if (selectedChar() >= 0) pressed |= BTN_START;
        }
    }

    // The menus' texture copies (the ONLINE page's pill, the room CSS's labels) are freed once the
    // game is in any other scene than the two that make them: every scene that loads fighters
    // (scMemoryChange before a match or the waiting room, the stage select's preload) has them
    // gone before the costumes' files are read. Brawl decompresses each costume in a block of
    // the Network heap, and FitSamus27.pac's did not fit beside them (bug #75, Labels::copyBlock).
    static void releaseMenuCopies(const char* scene)
    {
        if (!Labels::holdsCopies() || !scene[0]) return;
        if (strcmp(scene, "muMenuMain") == 0 || strcmp(scene, "scSelctCharacter") == 0) return;
        NetMenu::forgetCopies();
        RoomCss::forgetCopies();
        RulesButton::forgetCopies();
        RoomPlay::forgetCopies();
        StageSwitch::forgetCopies();
        Labels::releaseCopies();
    }

    void tick()
    {
        AnyoneMenu::tick();
        const char* scene = Online::currentSceneName();
        releaseMenuCopies(scene);
        reportScreen(scene);
        if (TrainingRoom::active() && strcmp(scene, "scSelctCharacter") != 0) {
            // The CSS has left for the room: the room's tick, also on its load screens. Anywhere
            // else the room is over without the sequence having said so (it never was so far):
            // forget it rather than keep the menus' own tick from running.
            if (strcmp(scene, "scMelee") != 0 && strcmp(scene, "scMemoryChange") != 0) TrainingRoom::abandon();
            if (TrainingRoom::active() && g_onlineCss && s.mode >= 0) {
                if (strcmp(scene, s.lastScene) != 0) {
                    // As below: the CSS's message windows were freed with it.
                    strncpy(s.lastScene, scene, sizeof(s.lastScene) - 1);
                    s.cssMsg = NULL;
                    s.haveWindow = false;
                    s.ownShown = false;
                    s.roomBtnsShown = false;
                }
                tickWaitingRoom();
                return;
            }
        }
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
                RoomCss::reset();
                RoomRules::reset();
                RoomPlay::reset();
                s.cssMsg = NULL;
                s.haveWindow = false;
                s.ownShown = false;
                s.roomBtnsShown = false;
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
        launcherJoin(scene);
        launcherDirect(scene);
        launcherCreate(scene);

        bool onCss = g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0;
        if (!onCss) s.stepFloor = s.stepSeq;   // only a step asked for on the CSS counts there
        g_onlineCssLock = (onCss && (lockedIn() || waitsForCharTurn() || resumeStep())) ? 1 : 0;
        if (g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelStage") == 0) {
            if (s.quitNoticeFrames) --s.quitNoticeFrames;   // shown with the step (rankedText)
            // Direct's loser picks the stage on P+'s stage select. The pick is the stage and its
            // alternate (L/R); P+'s hazard toggle (Z on a stage or on Random) is a rule, which the
            // match setup forces on both machines, so Z is taken out here; so is the Code Menu.
            // Ranked's stage select is Slippi's game setup screen, which nothing leaves before
            // the steps are done: START (P+'s random pick) and B (back to the CSS) are taken out
            // too. Taking them out of buttonProc's buttons (stage_legal.cpp) is not enough: the
            // stage select reads them elsewhere as well, and left for the CSS mid-strike.
            // A room's loser is on it after every player readied: the room has started and can
            // no longer be unreadied (mm: "The game is starting."), so B is taken out there too
            // (it went back to the CSS unlocked, where START waits for a room that never waits
            // again); START, P+'s random pick, is a pick.
            // The alternate stage: the buttons down on A/START (P+'s ASL Helper does not record
            // them in this sequence), Z included, before Z is taken out. The current buttons
            // (+0x0): the held field (+0x8) is built from the last frame's, from which Z was
            // taken out, so it never has Z.
            OnlineMatch::noteSssButtons(padButtons(0x0), padButtons(0xC));
            u32 mask = BTN_Z;
            if ((s.mode == PPOM::MODE_RANKED || s.roomStepSss) && g_onlinePickStage) mask |= BTN_START | BTN_B;
            if (roomCss() && g_onlinePickStage) mask |= BTN_B;
            maskButtons(mask);
            blockCodeMenu();
        }
        if (onCss) blockCodeMenu();
        if (strcmp(scene, "scSelctCharacter") != 0 && !CodeEntry::active()) CodeEntry::forgetLabels();
        if (!onCss) {
            s.cssSeen = false;
            s.startOnLanding = false;
            PPOM::g_block.debug.scratch[1] = 0;
            return;
        }
        holdBGuard();
        tickRoomLeave();

        hideHeaderArt();
        // The panel's tag, for the next online CSS (and Dolphin's, with the lock-in). Once the
        // CSS has printed its rule line its panel has the tag restoreTag gave it.
        if (s.cssMsg) {
            s.lastTag = selectedTag();
            s.tagChosen = true;
        }
        u32 b = padButtons(0x8);
        u32 pressed = padButtons(0xC);
        if (s.lastButtons == 0xFFFFFFFF) {
            // ignore buttons still held from the menu until they are released
            pressed = 0;
            if (!b) s.lastButtons = 0;
        }
        if (s.startGuard) {
            s.startGuard--;
            pressed &= ~BTN_START;
        }
        if (pressed & BTN_START) {
            int p = pressedPort(BTN_START);
            if (p >= 0) s.startPad = (u8)p;
            if (p >= 0 && !CodeEntry::active()) s.startAlt = altHeld(p);
        }
        maskStart();
        if (s.playback) {
            // A replay: nothing the player presses here does anything (START would search, B and
            // Z would leave). Its match starts once SESSION has it, as a connected game's does.
            maskButtons(BTN_START | BTN_B | BTN_Z);
            const PPOM::Session& se = PPOM::g_block.session;
            if (s.playbackPending && se.state == PPOM::SS_MATCH_READY && se.game) {
                s.playbackPending = false;
                g_onlineMatchGame = se.game;
                PPOM::g_block.debug.scratch[10] = 0x100 * se.game;   // tests: the match started
                leaveCss(1);
            }
            g_onlineCssLock = 1;
            setStatus(s.playbackPending ? "Loading replay" : "Replay", false);
            if (s.statusDirty && s.cssMsg) {
                printCssStatus();
                s.statusDirty = false;
            }
            return;
        }
        startWithCoinUp(pressed, b);
        g_cssTeamRoom = 0;   // (tickRoom sets it again in a team room)
        if (CodeEntry::active()) {
            CodeEntry::tick(pressed & BTN_START);
        } else if (roomCss()) {
            tickRoom(pressed, b);
        } else {
            // START: lock in and search (Unranked) or enter the code (Direct, Teams), once a
            // character is selected (Slippi shows the START prompt only then).
            if ((pressed & BTN_START) && s.phase == PH_IDLE && selectedChar() >= 0) {
                if (usesCode() && s.codePreset && s.code[0]) {
                    startSearch();   // the launcher's invite: the code is known, no keypad
                } else if (usesCode()) {
                    CodeEntry::open(LOCAL_PORT, false);
                } else {
                    startSearch();
                }
            }
            // Z (HandleInputsOnCSS.asm:172-205): press to cancel a search or clear an error. B
            // cancels and clears as well (Slippi's B does nothing while locked in); that press is
            // used up, so the coin stays where it is and the next B takes it back. Holding on
            // still leaves the CSS, which disconnects. Slippi's hold Z to disconnect is not here
            // (the user's, 2026-10-14: nobody knew it, and hold B does the same): P+'s "hold Z
            // for AltZ" holds Z on the CSS, and a still 48-frame hold before START disconnected.
            if (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING || s.phase == PH_ERROR) {
                if (pressed & (BTN_Z | BTN_B)) {
                    if (pressed & BTN_B) maskPressed(BTN_B);
                    // Cancelled while the CSS fades out for the room: no room, the CSS fades in.
                    TrainingRoom::abandon();
                    cleanup();
                    playSE(SE_BACK);
                }
            } else if (s.phase == PH_CONNECTED) {
                tickConnected(pressed);
            }
        }
        g_onlineCssLock = (lockedIn() || waitsForCharTurn() || resumeStep()) ? 1 : 0;
        PPOM::g_block.debug.scratch[1] = (u32)g_onlineCssLock | ((u32)s.phase << 4);   // harness/tests
        if (s.quitNoticeFrames) --s.quitNoticeFrames;
        updateStatus();
        if (s.statusDirty && s.cssMsg) {
            printCssStatus();
            s.statusDirty = false;
        }
        pollRank();
        showOwnCode();
    }

    void noteQuit(int port)
    {
        if (port < 0 || port >= PPOM::SESSION_PLAYERS) return;
        char name[PPOM::NAME_LEN + 1];
        PPOM::u16ToAscii(name, PPOM::g_block.session.players[port].name, sizeof(name));
        if (!name[0]) sprintf(name, "P%d", port + 1);
        sprintf(s.quitNotice, "%s quit", name);
        s.quitNoticeFrames = QUIT_NOTICE_FRAMES;
        // Ranked, or a room's set that goes on: the steps wait for every player's START.
        const PPOM::RoomPlay& play = PPOM::g_block.session.play;
        s.quitHold = s.mode == PPOM::MODE_RANKED ||
                     (roomCss() && play.valid && play.setOn && !(play.setFlags & PPOM::SET_FINISHED));
        // The quit's own START may still be held when the CSS comes: no ready until it is let go.
        if (s.quitHold) s.lastButtons = 0xFFFFFFFF;
        s.quitNoticePort = (u8)port;
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
        if (msg != s.cssMsg || window != s.cssWindow) s.haveWindow = s.ownShown = s.roomBtnsShown = false;   // a new CSS
        s.cssMsg = msg;
        s.cssWindow = window;
        s.statusDirty = true;   // re-printed by the next tick with our colour
        // (returning " " here froze the display on the CSS: presents stopped; keep real text)
        updateStatus();
        return s.status[0] ? s.status : "Select your character";
    }

    struct Init {
        Init() { s.mode = -1; s.lastScene[0] = 0; s.appState = -1; s.lastTag = -1; }
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
        // WITH FRIENDS page (Brawl's WITH ANYONE page): Create Room (its SPECTATOR button),
        // Direct 1v1 (BASIC VERSUS), Join Room (TEAM BATTLE); labels drawn by netmenu.cpp
        {"muMenuMain", "Watch someone else fight", "Make a room for 2 to 4 players."},
        {"muMenuMain", "quick fight with someone", "Play a specific person."},
        {"muMenuMain", "Form a team with someone", "Join a room with its code."},
    };

    const char* overrideFor(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller)
    {
        const char* css = OnlineMenu::cssLine(msg, window, line, msbin, caller);
        if (css) return css;
        const char* room = TrainingRoom::line(msg, window, line, msbin);   // the waiting room's banner
        if (room) return room;
        const char* play = RoomPlay::menuText(msbin, line);   // the RULESET / SET descriptions
        if (play) return play;
        const char* sw = StageSwitch::menuText(msg, msbin, line);  // the room's STAGE SWITCH
        if (sw) return sw;
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
