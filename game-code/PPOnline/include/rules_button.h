#pragma once
#include <sy_core.h>
#include <types.h>

namespace RulesButton {
    // The room CSS's RULES button (docs/design/room-rules.md decision 4; docs/game-code.md §14
    // "RULES button"): a third button in the bar next to PUBLIC / PRIVATE and FFA / TEAMS, made
    // and hit-tested as Brawl makes and hit-tests those two (rules_button.cpp).
    void install(CoreApi* api);
    // Every frame on the room CSS: show / hide the button; enabled = false draws it greyed and
    // the hand passes over it (non-hosts normally get enabled = true: they press it to view the
    // rules read-only). Not called (or show = false) on the other CSSs.
    void tick(bool show, bool enabled);
    // True once per A press on the button (the game's own handling of that press is skipped).
    // Never while the rules menu is open (muSelCharTask+0x630). The caller opens the menu
    // (online_menu.cpp tickRoom: RoomRules::open(), which also plays the decide sound).
    bool pressed();
}
