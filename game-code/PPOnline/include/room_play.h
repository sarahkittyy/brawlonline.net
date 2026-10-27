#pragma once
#include <types.h>
#include "ppom.h"

namespace RoomPlay {
    // What the room plays and its set (SESSION RoomPlay, docs/design/room-rules.md decisions 12,
    // 16, §2.1; docs/game-code.md §14 "The Ruleset line and the set"): two lines of P+'s RULES
    // page show them (room_play.cpp). RULESET (the STAGE CHOICE row, read-only in rooms) shows the
    // room's ruleset; the host browses the recent and own published rulesets with left / right
    // and picks one with A ("Enter code" opens the keypad's 6-letter mode). SET (the STOCK
    // CONTROL row, left out: decision 19) shows the set; when it is paused because the results
    // differed the host picks Replay game or End set there.

    // Every frame on the room CSS (also while the rules menu is open).
    void tick(bool inRoom, bool host);
    // The room's ruleset changed this frame (another ruleset, its set length, custom): the server
    // took back every ready, so the game takes back its lock-in too (or Dolphin sends it again).
    bool changedNow();
    // RULES page lines 2 and 4 (room_rules.cpp's input filter): the input left for the page.
    u32 rulesInput(u32 line, u32 bits, bool host);
    void onRulesets(const PPOM::Response& r);   // a CMD_RULESETS answer
    // "Enter code" was picked: once the rules menu has closed, the caller opens the keypad's
    // ruleset mode (true once).
    bool takeKeypadRequest();
    // The room CSS's line under the character grid: the ruleset, '\n', the set and its score
    // (`names`: the two sides' players), "" outside a room (the caller puts the two parts on one
    // line when they fit, else on two).
    void cssLine(char* out, int size, const char* name0, const char* name1);
    // The rules menu's description line for RULESET / SET (P+'s are for stage choice and stock
    // control), or NULL (Text::overrideFor).
    const char* menuText(const void* msbin, u32 line);
    // The set length the host picked on the SET line in the open menu (sent when it closes), -1 none.
    int pendingBestOf();
    // That length to the room now (ROOM_BEST_OF), before the menu's other edits that need it (the
    // MATCH SETUP page's bans, DSR and strikes are for a set); nothing more is sent for it at close.
    void flushBestOf();
    void reset();   // the CSS is gone
    void forgetCopies();   // its texture copies are about to be freed (Labels::releaseCopies)
}
