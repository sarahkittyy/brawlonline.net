#pragma once
#include <sy_core.h>
#include <types.h>
#include "ppom.h"

class MuMsg;

namespace OnlineMenu {
    void install(CoreApi* api);
    void tick();
    void enter(int mode);               // PPOM::Mode, picked on the ONLINE page
    void applyRules();                  // sqNetAnyOkiraku start: online rules into the set rule
    void codeEntered(const char* code);  // from CodeEntry: set the code and start the search
    int currentMode();                  // PPOM::Mode of the online CSS, -1 outside it
    bool modesLocked();                 // logged out or update required: no online mode
    void restoreCss();                  // before the CSS starts again: the remembered coin
    bool selectableCharKind(int kind);  // a gmCharacterKind P+'s CSS can lock in
    const char* cssLine(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller);
    // Ranked's game setup step in Ranked (NULL otherwise; inactive while not connected), and
    // its line.
    const PPOM::GameStep* rankedStep();
    const char* rankedText();

    // Rooms (docs/design/rooms.md, docs/rooms-game-interface.md). A room's CSS is the Wi-Fi CSS
    // in MODE_TEAMS (Slippi's mode 3, which the room's game ticket uses) with a room entry.
    enum RoomEntry { ROOM_ENTRY_NONE = 0, ROOM_ENTRY_CREATE = 1, ROOM_ENTRY_JOIN = 2 };
    void enterRoom(int entry);           // WITH FRIENDS > Create Room / Join Room
    bool roomCss();                      // the online CSS is a room's
    bool stepsDecideStage();             // Ranked or a room's set: GameSetup's steps pick the stage
    void prepareCss();                   // the CSS is about to be built (sqNetAnyOkiraku)
    void menuPageRunning(u8* page);      // a menu page the launcher's join can leave from ran
    void topPageRunning(u8* page);       // the main menu's top page ran (the same, no Wi-Fi task)
    void noteQuit(int port);             // the last game ended by this port's quit: the CSS says so
}

namespace RoomCss {
    // The other players' panels on a room's CSS (room_css.cpp).
    enum PanelKind { PV_CLOSED = 0, PV_SEARCHING = 1, PV_CHOOSING = 2, PV_READY = 3, PV_IN_GAME = 4 };
    const int NAME_CHARS = 15;
    struct PanelView {
        u8 kind;          // PanelKind
        u8 css;           // CSS id of the character (PV_READY), else 0x28
        u8 costume;
        u8 team;          // PPOM::Team
        u8 stars;         // Brawl's win stars by the name plate: the side's set wins during a set,
                          // else 1 for the room's host (rooms.md #8)
        u8 stocks;        // stock control: the count the player picked (shown once ready), 0 none
        char name[NAME_CHARS + 1];
    };
    void prepareRecords();               // before the CSS is built: four full panels
    void releaseRecords();               // a room's CSS built again as another: records as before
    void reset();                        // the CSS is gone
    // stockControl: the room's rules have P+'s STOCK CONTROL in a stock match (the STOCKS boxes);
    // lockedStocks: while locked in, the count the lock-in carries (the local box is kept at it), -1.
    void tick(const PanelView views[3], bool teams, int myTeam, int myStars, const char* myName, bool meReady,
              bool stockControl, int lockedStocks);
    u8 myStocks();                       // the count picked on the local panel's STOCKS box, 0 none
    bool handOnMyStocks();               // the local hand is on its panel's STOCKS box
    int panelUnderHand();                // 1-3: the local hand is over that panel, else -1
    bool handOverMyFlag();               // the local hand is over the local panel's team flag
    void headerButtons(bool isPublic, bool teams);   // ITEM / STAGE as the room's two settings
    void setLocalSlot(int slot, bool inRoom);   // the room slot the local player's panel is shown at
    int portOfArea(int area);            // the room slot an area (1-3) shows
    // (panelUnderHand: the slot (0-3, not the player's own) of the panel under the hand)
}

namespace OnlineMatch {
    // Online matches from the online CSS (online_match.cpp): sqNetAnyOkiraku hooks, the match
    // setup from SESSION, the in-match disconnect.
    void install(CoreApi* api);
    void tickMatch();                 // every frame in scMelee
    void offMatch();                  // every frame outside scMelee
    void onFrameDrawn();              // every drawn frame (MatchHud's hook)
    bool disconnectShown();
    bool quitHintShown();             // Unranked: this player's START shows the quit hint
    u16 pickedStage();                // the loser's stage pick (0xFFFF none, PICK_CANCELLED)
    const u16 PICK_CANCELLED = 0xFFFD; // the stage select was left without a pick (B)
    u8 pickedAsl();
    void clearPickedStage();
    void noteSssButtons(u32 held, u32 pressed);   // each frame of the online stage select
}

namespace MatchHud {
    // "DISCONNECTED" or "DESYNC DETECTED" in the match HUD (match_hud.cpp): drawn while on and
    // the scene is scMelee.
    void install(CoreApi* api);
    void show(bool on, bool desync);
    bool shown();
}

namespace NetMenu {
    void onlineMenuEntered(int mode);
    void setReturnButton(int b);         // WITH FRIENDS' button to highlight when it opens again
    void ensureWifiTask();               // muWifiInterfaceTask, which the Wi-Fi CSS needs
}

namespace CodeEntry {
    // Open the connect-code entry (Direct, Teams). See code_entry.cpp.
    void install(CoreApi* api);
    void open(int port, bool room);   // room: Join Room's room-code mode
    void openRuleset(int port);       // the room's Ruleset line: a 6-letter ruleset code
    bool rulesetMode();               // the keypad open now (or last) is the ruleset mode
    void tick(bool startPressed);
    bool active();
    void onSuggestion(const PPOM::Response& r);   // a FETCH_CODE_SUGGESTION answer
    void forgetLabels();                          // off the CSS: its '#' label window is gone
}

namespace BootMenu {
    // The boot lands on the ONLINE page instead of P+'s Versus CSS (boot_menu.cpp).
    void install(CoreApi* api);
}

namespace AnyoneMenu {
    // WITH ANYONE -> Unranked / Ranked on Brawl's Wi-Fi OPTIONS page (anyone_menu.cpp).
    void install(CoreApi* api);
    void tick();
    const char* line(MuMsg* msg, u32 window, u32 line, const void* msbin);
}
