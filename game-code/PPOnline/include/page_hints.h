#pragma once
#include <types.h>

// The rules menu's page hints (Sarah's board, 2026-10-27: "pulsing [R] and [L] buttons, in the style
// of the actual gamecube button icons, with arrows below them ... use whatever the game files
// already have built-in"): the pause screen's own L and R buttons and its red arrow (the info
// archive's InfPauseButton00), drawn left and right of the menu while there is a page that way,
// pulsing. Drawn with GX from a draw callback on one of the menu's models, as a MuMsg draws its
// text (page_hints.cpp).
namespace PageHints {
    void open();                                  // the rules menu opened
    // Every frame it is open: which ways a page is (L: to the left, R: to the right).
    void tick(u8* rt, bool left, bool right);
    // It closed. `menuThere`: its models still exist (closed with B), so the callback comes off.
    void close(bool menuThere);
}
