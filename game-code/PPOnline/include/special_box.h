#pragma once
#include <types.h>

#include "ppom.h"

// P+'s Special Versus box on the room's rules menu (special_box.cpp; docs/design/special-rules.md
// §5): the SPECIAL page of rules_pages.cpp.
namespace SpecialBox {
    // The rules menu opened: the room's special rows and stamina type from SESSION.
    void open(bool canEdit);
    // The SPECIAL page shows or not (rules_pages.cpp turns the pages).
    void show(bool on);
    // Every frame the menu is open; `showing`: the SPECIAL page is the one shown.
    void tick(u8* rt, bool showing);
    // The page's input (P+'s bits); `canEdit`: the host's menu between games, no set on.
    void input(u32 bits, bool canEdit);
    // The menu closed: the box's objects and models go.
    void close();
    // The host's edits (the 8 rows, the stamina type): false when untouched.
    bool edits(u8 special[8], u8* staminaType);
    // A row's value as shown (0 off, 1, 2).
    u8 row(int i);
    // The footer for the row under the cursor.
    const char* footer();
    // Dolphin's answer to CMD_SPECIAL_ASSETS (online_menu.cpp's mailbox).
    void onAssets(const PPOM::Response& r);
}
