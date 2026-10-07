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

    // muSelCharPlayerArea+0x1B8: the character on the player's coin, 0x28 = none.
    static int selectedChar()
    {
        u8* task = cssTask();
        if (!task) return -1;
        u32 area = *(u32*)(task + 0x44 + 4 * LOCAL_PORT);
        if (!isPtr(area)) return -1;
        int c = *(int*)(area + 0x1B8);
        return (c >= 0 && c < 0x28) ? c : -1;
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
            sprintf(buf, "Playing: %s", s.peerName);
            setStatus(buf, false);
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

    void install(CoreApi* api) { CodeEntry::install(api); }

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
        g_onlineCss = 1;
        // Leaving the CSS goes back to the page the mode was picked on (netmenu.cpp
        // exitToOnlinePage): Direct to the ONLINE page, Unranked/Teams to WITH ANYONE.
        g_onlineReturnPage = (mode == PPOM::MODE_DIRECT) ? 0x1F : 0;
        g_onlineFriendsCursor = (mode == PPOM::MODE_DIRECT) ? 1 : 0;
        PPOM::g_block.debug.menuState = (u32)(mode + 1);
    }

    static void leave()
    {
        s.mode = -1;
        s.phase = PH_IDLE;
        s.searchSeq = 0;
        s.pollPending = false;
        s.cssMsg = NULL;
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
        PPOM::asciiToU16(req.code, s.code, PPOM::CODE_LEN);
        s.searchSeq = PPOM::post(PPOM::CMD_FIND_OPPONENT, &req, sizeof(req));
        s.pollPending = true;   // Dolphin answers FIND_OPPONENT with the match state
        s.pollFrames = 0;
        s.phase = PH_SEARCHING;
    }

    void codeEntered(const char* code)
    {
        strncpy(s.code, code, PPOM::CODE_LEN);
        s.code[PPOM::CODE_LEN] = 0;
        if (usesCode() && s.phase == PH_IDLE && s.code[0]) startSearch();
    }

    static void cleanup()
    {
        PPOM::post(PPOM::CMD_CLEANUP_CONNECTION, NULL, 0);
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

    static void pollMailbox()
    {
        const PPOM::Response* r = PPOM::pollResponse();
        if (!r) return;
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
        const char* scene = Online::currentSceneName();
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
        if (!onCss) return;

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
            }
        }
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
    // 0x644, Unranked 0x646, Direct 0x647, Teams 0x64B. Brawl groups Unranked and Teams under
    // WITH ANYONE, which gets Slippi's group description.
    static const Relabel RELABELS[] = {
        // main menu PLAY ONLINE = Slippi's 1P-menu "Online Play"
        {"muMenuMain", "Play different modes online", "Compete against online opponents."},
        // ONLINE page (button art stays WITH FRIENDS / WITH ANYONE)
        {"muMenuMain", "registered as Friends", "Play a specific person."},
        {"muMenuMain", "Play against random people", "Compete against online opponents."},
        // WITH ANYONE page (button art stays BASIC VERSUS / TEAM BATTLE)
        {"muMenuMain", "quick fight with someone", "Play unranked matches."},
        {"muMenuMain", "Form a team with someone", "Play teams games."},
    };

    const char* overrideFor(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller)
    {
        const char* css = OnlineMenu::cssLine(msg, window, line, msbin, caller);
        if (css) return css;
        const char* scene = Online::currentSceneName();
        for (u32 i = 0; i < sizeof(RELABELS) / sizeof(RELABELS[0]); i++) {
            const Relabel& r = RELABELS[i];
            if (strcmp(scene, r.scene) != 0) continue;
            if (Online::msbinLineContains(msbin, (int)line, r.match)) return r.text;
        }
        return NULL;
    }
}
