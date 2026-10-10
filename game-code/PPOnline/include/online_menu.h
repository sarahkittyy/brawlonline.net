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
    void prepareCss();                   // the CSS is about to be built (sqNetAnyOkiraku)
    void menuPageRunning(u8* page);      // a menu page the launcher's join can leave from ran
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
        char name[NAME_CHARS + 1];
    };
    void prepareRecords();               // before the CSS is built: four full panels
    void reset();                        // the CSS is gone
    void tick(const PanelView views[3], bool teams, int myTeam);
    int myTeamClicked();                 // the local player's flag changed: the team, else -1
    int panelUnderHand();                // 1-3: the local hand is over that panel, else -1
}

namespace OnlineMatch {
    // Online matches from the online CSS (online_match.cpp): sqNetAnyOkiraku hooks, the match
    // setup from SESSION, the in-match disconnect.
    void install(CoreApi* api);
    void tickMatch();                 // every frame in scMelee
    void offMatch();                  // every frame outside scMelee
    void onFrameDrawn();              // every drawn frame (MatchHud's hook)
    bool disconnectShown();
    u16 pickedStage();                // the loser's stage pick (0xFFFF none, PICK_CANCELLED)
    const u16 PICK_CANCELLED = 0xFFFD; // the stage select was left without a pick (B)
    u8 pickedAsl();
    void clearPickedStage();
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
}

namespace CodeEntry {
    // Open the connect-code entry (Direct, Teams). See code_entry.cpp.
    void install(CoreApi* api);
    void open(int port, bool room);   // room: Join Room's room-code mode
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
