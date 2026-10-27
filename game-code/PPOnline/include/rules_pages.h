#pragma once
#include <types.h>

#include "ppom.h"

// The room's rules menu as L / R page tabs (rules_pages.cpp; docs/design/special-rules.md):
// RULES and MORE RULES (P+'s), then MATCH SETUP (P+'s MORE RULES rows relabelled: the ruleset's
// bans, DSR, stage choice, strike pattern, stock control and alternate stages) and SPECIAL (P+'s
// Special Versus box, special_box.cpp). The host edits them as the other rules, sent when the
// menu closes; everyone else views them.
namespace RulesPages {
    // The rules menu opened (room_rules.cpp RoomRules::open): the room's values from SESSION.
    // `canEdit`: the host's menu between games.
    void open(bool canEdit);
    // Every frame it is open (rt: the rules task).
    void tick(u8* rt);
    // The input of P+'s RULES page (`rulesPage`) or MORE RULES page, filtered: L / R turn the
    // pages; on MATCH SETUP and SPECIAL the rows are ours. Returns what P+'s page still gets.
    u32 input(u8* rt, u8* page, bool rulesPage, u32 bits);
    // The menu closed (or was closed): our textures and objects go, P+'s own are put back.
    // `send`: the host's edits are the room's (ROOM_FORMAT now; the rules' part with rulesEdits).
    void close(bool send);
    // The host's edits of what goes with the room's rules (ROOM_RULES): stock control and the
    // special rows. False when nothing of them was touched.
    bool rulesEdits(PPOM::Rules& ru, u8 special[8], u8* staminaType);
    // P+'s page that got this frame's input (room_rules.cpp's input filter): 0 RULES, 1 MORE RULES
    // (our pages are over it), 2 another (the item or stage switch).
    void notePage(int kind);
    // The menu's footer while one of our pages shows, or NULL.
    const char* menuText(void* msg, const void* msbin, u32 line);
    // Our pages are showing (P+'s MORE RULES page is under them).
    bool ours();
    // The footer printed again (a value changed).
    void reprintFooter();
}
