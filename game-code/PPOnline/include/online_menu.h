#pragma once
#include <sy_core.h>
#include <types.h>
#include "ppom.h"

class MuMsg;

namespace OnlineMenu {
    void install(CoreApi* api);
    void tick();
    void enter(int mode);               // PPOM::Mode, picked on the ONLINE page
    // sqNetAnyOkiraku start: online rules into the set rule; `stageKind` the match's stage (its
    // hazards: the rules' per-stage ones), 0 none yet
    void applyRules(int stageKind = 0);
    // The session's rules turn on one of P+'s special modes (docs/design/special-rules.md).
    bool specialOn();
    // P+'s STOCK CONTROL (set rule +5 = 2, each player's handicap box): the rules' stockControl in a
    // stock match, or with regen (ATTACK row 2) P+'s FRAME CONTROL in any style; never in stamina,
    // where the box is each player's HP (staminaOn).
    bool stockControlOn();
    bool staminaOn();                    // the STAMINA row is 2: each player picks their HP
    void codeEntered(const char* code);  // from CodeEntry: set the code and start the search
    int currentMode();                  // PPOM::Mode of the online CSS, -1 outside it
    bool modesLocked();                 // logged out or update required: no online mode
    void restoreCss();                  // before the CSS starts again: the remembered coin
    // P+'s four slot colour lines (0-12; -1: the port's own), the match's from SESSION
    void setSlotColours(const s8* colour);
    // The local panel's colour as drawn (P1's line) and the L / R / Z presses on its player box,
    // which change when P+ steps or resets the colour; -1 without P+'s Code Menu.
    int localColourKey();
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
    bool nobodyPlaysHere();              // the online match is a replay or a watched room game
    void prepareCss();                   // the CSS is about to be built (sqNetAnyOkiraku)
    void menuPageRunning(u8* page);      // a menu page the launcher's join can leave from ran
    void topPageRunning(u8* page);       // the main menu's top page ran (the same, no Wi-Fi task)
    void noteQuit(int port);             // the last game ended by this port's quit: the CSS says so
    void waitingRoomEnded(int how);      // the waiting room has ended (TrainingRoom::End)
    u8 lockedTagRecord();                // the last lock-in's name tag (record +0x18, 0x78 none)
}

namespace RoomCss {
    // The other players' panels on a room's CSS (room_css.cpp).
    enum PanelKind { PV_CLOSED = 0, PV_SEARCHING = 1, PV_CHOOSING = 2, PV_READY = 3, PV_IN_GAME = 4 };
    const int NAME_CHARS = 15;
    const int NAME_SIZE = NAME_CHARS * 3 + 1;   // a name as UTF-8 (BMP: at most 3 bytes a character)
    struct PanelView {
        u8 kind;          // PanelKind
        u8 css;           // CSS id of the character (PV_READY; PV_CHOOSING: the coin placed), else 0x28
        u8 costume;
        u8 team;          // PPOM::Team
        u8 stars;         // Brawl's win stars by the name plate: the side's set wins during a set,
                          // else 1 for the room's host (rooms.md #8)
        u8 stocks;        // the box's count the player picked (shown once ready), 0 none: stock control's
                          // stocks, or in stamina the HP / 10
        u8 sittingOut;    // SIT OUT: a CPU's grey card, "SIT OUT" on the plate; 2: "WATCHING"
                          // (watching the room's game), the same card
        char name[NAME_SIZE];
    };
    void prepareRecords();               // before the CSS is built: four full panels
    void releaseRecords();               // a room's CSS built again as another: records as before
    void reset();                        // the CSS is gone
    void forgetCopies();                 // its label copies are about to be freed (Labels::releaseCopies)
    // boxKind: the panels' boxes: BOX_STOCKS with the room's P+ STOCK CONTROL (a stock match, or
    // any with regen's FRAME CONTROL), BOX_HP in stamina (each player's HP), else BOX_NONE;
    // lockedStocks: while locked in, the box's count the lock-in carries (the local box is kept at
    // it), -1.
    // meSittingOut: the local player sits out (their own card grey, "SIT OUT" on its plate).
    void tick(const PanelView views[3], bool teams, int myTeam, int myStars, const char* myName, bool meReady,
              int boxKind, int lockedStocks, bool meSittingOut);
    enum BoxKind { BOX_NONE = 0, BOX_STOCKS = 1, BOX_HP = 2 };
    u8 myStocks();                       // the count picked on the local panel's STOCKS box, 0 none
    u16 myHp();                          // stamina: the HP picked on the local panel's box, 0 none (150)
    bool handOnMyStocks();               // the local hand is on its panel's STOCKS box
    int panelUnderHand();                // 1-3: the local hand is over that panel, else -1
    // The slot (0-3) whose name plate the local hand is on (Brawl's hand button 0x1D, the plate a
    // CPU / player toggle is offline), else -1.
    int plateUnderHand();
    bool handOverMyFlag();               // the local hand is over the local panel's team flag
    bool handOnMyPlayerBox();            // on the local card's player box (P+'s L / R colour)
    bool teamsShown();                   // the panels are in the team battle look
    // The local panel's costume set by the plugin, its picture drawn again: the room's TEAMS in the
    // teams' costumes gives the player their team's (OnlineMenu's tickRoom). Nothing without a
    // character on the panel (none, Random).
    void setMyCostume(int costume);
    void headerButtons(bool isPublic, int teams);   // ITEM / STAGE as the room's two settings (teams:
                                                    // 0 FFA, 1 rainbow TEAMS, 2 TEAMS)
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
    const char* quitHintText();       // its text: this player's controller's quit buttons
    u16 pickedStage();                // the loser's stage pick (0xFFFF none, PICK_CANCELLED)
    const u16 PICK_CANCELLED = 0xFFFD; // the stage select was left without a pick (B)
    u8 pickedAsl();
    // P+'s four alt costume words (one per slot, PPOM::COSTUME_ALT_Z / _R / 0 in alt[i])
    void setAltCostumes(const u8* alt);
    // A player's costume in a team battle in the teams' costumes: their own if it has the team's
    // colour, else the character's first costume of it (as P+'s CSS gives it); `costume` if the
    // character has none.
    u8 teamCostume(u8 charKind, u8 team, u8 costume);
    // Which of the character's costumes of the team's colour `costume` is (P+'s "team set", 0 the
    // first), -1 if it is not of that colour; and the costume of a shade (wrapping round the
    // character's costumes of the colour; `costume` if it has none).
    int teamShadeOf(u8 charKind, u8 team, u8 costume);
    u8 teamShadeCostume(u8 charKind, u8 team, int shade, u8 costume);
    void clearPickedStage();
    void noteSssButtons(u32 held, u32 pressed);   // each frame of the online stage select
    u8 sssAsl();                      // the buttons held at the last A/START there (P+'s ASL byte)
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
