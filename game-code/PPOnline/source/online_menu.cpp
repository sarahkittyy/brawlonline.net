// Online menu flow (design 5.4), built only from Brawl/P+ screens and the game's message system.
//
//   main menu PLAY ONLINE  -> (Brawl's Wi-Fi connect dialogs, WFC faked by netmenu.cpp)
//   ONLINE page            WITH FRIENDS = DIRECT, WITH ANYONE = UNRANKED (descriptions relabelled)
//   either button          -> Brawl's Wi-Fi character select (sqNetAnyOkiraku), timers/errors off
//   CSS                    START = search (mailbox 0xB4 FIND_OPPONENT), Z = cancel (0xBA);
//                          status text in the CSS's own rule-line message window
#include <mu/mu_msg.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "netmenu.h"

extern "C" {
    extern u8 g_onlineCss;
    int sprintf(char* buf, const char* fmt, ...);
}

namespace OnlineMenu {

    enum Phase { PH_IDLE = 0, PH_SEARCHING = 1, PH_CONNECTING = 2, PH_CONNECTED = 3, PH_ERROR = 4 };

    struct State {
        int mode;               // PPOM::Mode, or -1 when not in the online flow
        int phase;
        char code[PPOM::CODE_LEN + 1];      // Direct: the opponent's code
        char status[128];
        bool statusDirty;
        u32 lastButtons;
        u32 searchSeq;
        char lastScene[24];
        // the CSS rule-line message window, captured from its MuMsg::printIndex call
        MuMsg* cssMsg;
        u32 cssWindow;
        const void* cssMsbin;
        u32 cssLine;
        // account line from 0xB9 GET_ONLINE_STATUS
        bool haveAccount;
        char accountName[PPOM::NAME_LEN + 1];
        char accountCode[PPOM::CODE_LEN + 1];
    };
    static State s;

    static const u32 BTN_START = 0x1000;
    static const u32 BTN_Z = 0x0010;

    // P+ resident module bases (see netmenu.cpp). Verified once per tick on the CSS.
    static const u32 SEL_CHAR_TEXT = 0x806828C4;
    static const u32 SEL_CHAR_TEXT_END = 0x806A07F0;

    static u32 padButtons()
    {
        // Held buttons of the four controller ports (GC layout), written by the pad system.
        u32 b = 0;
        for (int p = 0; p < 4; p++) {
            b |= *(volatile u32*)(0x805BA684 + 0x40 * p);
        }
        return b;
    }

    static const char* modeName(int mode)
    {
        switch (mode) {
        case PPOM::MODE_RANKED: return "Ranked";
        case PPOM::MODE_UNRANKED: return "Unranked";
        case PPOM::MODE_DIRECT: return "Direct";
        case PPOM::MODE_TEAMS: return "Teams";
        }
        return "";
    }

    static void setStatus(const char* text)
    {
        strncpy(s.status, text, sizeof(s.status) - 1);
        s.status[sizeof(s.status) - 1] = 0;
        s.statusDirty = true;
    }

    static void idleStatus()
    {
        // Slippi's CSS prompt for each mode: Direct asks for a code, the queues search.
        if (s.mode == PPOM::MODE_DIRECT) {
            char buf[96];
            if (s.code[0]) {
                sprintf(buf, "%s: START to search %s", modeName(s.mode), s.code);
            } else {
                sprintf(buf, "%s: START to enter code", modeName(s.mode));
            }
            setStatus(buf);
        } else {
            char buf[96];
            sprintf(buf, "%s: START to search", modeName(s.mode));
            setStatus(buf);
        }
    }

    void install(CoreApi* api) { (void)api; }

    // The rule line is a fixed-width, right-aligned window: MuMsg::printf does not apply the
    // msbin's style tags, so set the colour (black, as the original line) and let the font
    // shrink to fit.
    static void printCssStatus()
    {
        s.cssMsg->setFontColor(s.cssWindow, 0, 0, 0, 255);
        if (PPOM::g_block.debug.cfg & PPOM::CFG_CSS_AUTOWIDTH) s.cssMsg->setFontWidthModeAuto(s.cssWindow);
        s.cssMsg->printf(s.cssWindow, "%s", s.status);
    }

    void enter(int mode)
    {
        s.mode = mode;
        s.phase = PH_IDLE;
        s.cssMsg = NULL;
        s.lastButtons = 0xFFFFFFFF;   // ignore the press that brought us here
        if (mode != PPOM::MODE_DIRECT) s.code[0] = 0;
        idleStatus();
        PPOM::g_block.debug.menuState = (u32)(mode + 1);
    }

    static void leave()
    {
        if (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING) {
            PPOM::post(PPOM::CMD_CLEANUP_CONNECTION, NULL, 0);
        }
        s.mode = -1;
        s.phase = PH_IDLE;
        s.cssMsg = NULL;
        g_onlineCss = 0;
        PPOM::g_block.debug.menuState = 0;
    }

    void setDirectCode(const char* code)
    {
        strncpy(s.code, code, PPOM::CODE_LEN);
        s.code[PPOM::CODE_LEN] = 0;
        if (s.mode == PPOM::MODE_DIRECT && s.phase == PH_IDLE) idleStatus();
    }

    static void startSearch();

    void codeEntered(const char* code)
    {
        setDirectCode(code);
        if (s.mode == PPOM::MODE_DIRECT && s.phase == PH_IDLE && s.code[0]) startSearch();
    }

    static void startSearch()
    {
        PPOM::FindOpponent req;
        memset(&req, 0, sizeof(req));
        req.mode = (u8)s.mode;
        req.lockedChar = 0xFF;
        PPOM::asciiToU16(req.code, s.code, PPOM::CODE_LEN);
        s.searchSeq = PPOM::post(PPOM::CMD_FIND_OPPONENT, &req, sizeof(req));
        s.phase = PH_SEARCHING;
        char buf[96];
        if (s.mode == PPOM::MODE_DIRECT && s.code[0]) {
            sprintf(buf, "Searching for %s", s.code);
        } else {
            sprintf(buf, "Searching for opponent");
        }
        setStatus(buf);
    }

    static void onMatchState(const PPOM::MatchState& m)
    {
        char name[PPOM::NAME_LEN + 1], code[PPOM::CODE_LEN + 1], buf[128];
        PPOM::u16ToAscii(name, m.peerName, sizeof(name));
        PPOM::u16ToAscii(code, m.peerCode, sizeof(code));
        switch (m.mmState) {
        case PPOM::MM_INITIALIZING:
        case PPOM::MM_MATCHMAKING:
            return; // keep "Searching ..."
        case PPOM::MM_OPPONENT_CONNECTING:
            s.phase = PH_CONNECTING;
            sprintf(buf, "Connecting to %s", code[0] ? code : name);
            break;
        case PPOM::MM_CONNECTION_SUCCESS:
            s.phase = PH_CONNECTED;
            sprintf(buf, "Playing: %s", name);
            break;
        case PPOM::MM_ERROR:
        default: {
            s.phase = PH_ERROR;
            char err[PPOM::ERROR_LEN + 1];
            PPOM::u16ToAscii(err, m.errorText, 80);
            sprintf(buf, "%s", err[0] ? err : "Error");
            break;
        }
        }
        setStatus(buf);
    }

    static void pollMailbox()
    {
        const PPOM::Response* r = PPOM::pollResponse();
        if (!r) return;
        if (r->cmd == PPOM::CMD_GET_MATCH_STATE) {
            if (s.mode >= 0 && r->seq == s.searchSeq) {
                onMatchState(*(const PPOM::MatchState*)r->payload);
            }
        } else if (r->cmd == PPOM::CMD_GET_ONLINE_STATUS) {
            const PPOM::OnlineStatus& st = *(const PPOM::OnlineStatus*)r->payload;
            if (st.state == 1) {
                PPOM::u16ToAscii(s.accountName, st.name, sizeof(s.accountName));
                PPOM::u16ToAscii(s.accountCode, st.code, sizeof(s.accountCode));
                s.haveAccount = true;
            }
        }
    }

    void tick()
    {
        NetMenu::pollPendingExit();
        const char* scene = Online::currentSceneName();
        bool sceneChanged = strcmp(scene, s.lastScene) != 0;
        if (sceneChanged) {
            strncpy(s.lastScene, scene, sizeof(s.lastScene) - 1);
            if (strcmp(scene, "muMenuMain") == 0) {
                // Slippi sends CLEANUP_CONNECTION whenever the menus load, and the menu
                // shows the account (GET_ONLINE_STATUS).
                if (s.mode >= 0) leave();
                PPOM::post(PPOM::CMD_GET_ONLINE_STATUS, NULL, 0);
            }
        }
        pollMailbox();

        bool onCss = g_onlineCss && s.mode >= 0 && strcmp(scene, "scSelctCharacter") == 0;
        if (!onCss) {
            if (s.mode >= 0 && strcmp(scene, "scSelctCharacter") != 0 && strcmp(scene, "scMemoryChange") != 0
                && strcmp(scene, "muMenuMain") != 0) {
                // left the online CSS for anything but a load (e.g. a match): keep the mode
            }
            return;
        }

        if (CodeEntry::active()) {
            CodeEntry::tick();
            s.lastButtons = 0xFFFFFFFF;   // the keypad owns the buttons
            return;
        }
        u32 b = padButtons();
        u32 pressed = (s.lastButtons == 0xFFFFFFFF) ? 0 : (b & ~s.lastButtons);
        s.lastButtons = b;
        if (pressed & BTN_START) {
            if (s.phase == PH_IDLE) {
                if (s.mode == PPOM::MODE_DIRECT && !s.code[0]) {
                    CodeEntry::open();
                } else {
                    startSearch();
                }
            }
        }
        if (pressed & BTN_Z) {
            if (s.phase == PH_SEARCHING || s.phase == PH_CONNECTING || s.phase == PH_ERROR) {
                PPOM::post(PPOM::CMD_CLEANUP_CONNECTION, NULL, 0);
                s.phase = PH_IDLE;
                idleStatus();
            }
        }
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
        s.cssMsbin = msbin;
        s.cssLine = line;
        s.statusDirty = true;   // re-printed by the next tick with the original line's colour
        // (returning " " here froze the display on the CSS: presents stopped; keep real text)
        return s.status;
    }

    const char* accountLine()
    {
        static char buf[96];
        if (!s.haveAccount) return NULL;
        sprintf(buf, "Play online: Ranked, Unranked, Direct, Teams. (%s, %s)", s.accountName, s.accountCode);
        return buf;
    }

    struct Init {
        Init() { s.mode = -1; s.lastScene[0] = 0; }
    };
    static Init s_init;
}

namespace NetMenu {
    void onlineMenuEntered(int choice)
    {
        OnlineMenu::enter(choice == OnlineMenu::MENU_FRIENDS ? PPOM::MODE_DIRECT : PPOM::MODE_UNRANKED);
    }
}

namespace Text {
    struct Relabel {
        const char* scene;   // only in this scene
        const char* match;   // substring of the original message line
        const char* text;    // replacement
    };

    // Mode names and order follow Slippi's online submenu (Online.s): Ranked, Unranked,
    // Direct, Teams. Descriptions are ours: Slippi's own description strings live in its
    // patched Melee menu files, which are not in refs/ (see docs/game-code.md).
    static const Relabel RELABELS[] = {
        // main menu
        {"muMenuMain", "Play different modes online", "Play online: Ranked, Unranked, Direct, Teams."},
        // ONLINE page (button art stays WITH FRIENDS / WITH ANYONE)
        {"muMenuMain", "registered as Friends", "DIRECT: play a friend using their connect code."},
        {"muMenuMain", "Play against random people", "UNRANKED: play someone at your level, random stage."},
        // Brawl's connect dialogs (the WFC/Wiimmfi login is bypassed, nothing goes online here)
        {"muMenuMain", "Wiimmfi is not enabled", "Connect to online play?"},
        {"muMenuMain", "Connecting to Wiimmfi", "Connecting..."},
        {"muMenuMain", "Connected to Wiimmfi", "Connected."},
    };

    const char* overrideFor(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller)
    {
        const char* css = OnlineMenu::cssLine(msg, window, line, msbin, caller);
        if (css) return css;
        const char* scene = Online::currentSceneName();
        if (strcmp(scene, "muMenuMain") == 0 && Online::msbinLineContains(msbin, (int)line, "Play different modes online")) {
            const char* acct = OnlineMenu::accountLine();
            if (acct) return acct;
        }
        for (u32 i = 0; i < sizeof(RELABELS) / sizeof(RELABELS[0]); i++) {
            const Relabel& r = RELABELS[i];
            if (strcmp(scene, r.scene) != 0) continue;
            if (Online::msbinLineContains(msbin, (int)line, r.match)) return r.text;
        }
        return NULL;
    }
}
