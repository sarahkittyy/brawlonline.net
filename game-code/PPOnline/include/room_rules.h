#pragma once
#include <sy_core.h>
#include <types.h>

// The room's rules on the room CSS (docs/design/room-rules.md, docs/game-code.md §14 "Room rules
// RE"): P+'s own rules menu (muRuleTask, sora_menu_rule.rel) opened over the room CSS. The host
// edits the room's rules there and they go to the server when the menu closes; everyone else
// sees the same screens read-only.
namespace RoomRules {
    void install(CoreApi* api);   // the read-only input filter (module 18)
    // Every frame on the room CSS, before the room's own controls: returns true while the rules
    // menu is open (the room's buttons, START, B and Z wait then).
    // `setBetween`: on the CSS between a running set's games (the host only): the menu may open
    // there for End set alone (the user's, 2026-10-12: End set is a CSS action, never mid-match).
    bool tick(bool inRoom, bool host, bool waiting, bool setBetween);
    // The RULES button (or the tests' CFG_TEST_OPEN_RULES). `setOnly`: between a set's games,
    // everything read-only but the host's SET line (End set), nothing sent back on close.
    void open(bool setOnly);
    bool isOpen();
    // The room's rules changed this frame (the server took back everyone's ready: the game takes
    // back its lock-in too, or Dolphin would send it again).
    bool changedNow();
    // A line to show instead of the room's for a moment ("Host changed the rules"), or NULL;
    // `red` for an error.
    const char* statusText(bool* red);
    void reset();                 // the CSS is gone
}
