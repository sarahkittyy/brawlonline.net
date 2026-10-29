#pragma once
#include <types.h>

// The other players' cursors on an online stage select, labelled P1-P4 (sss_cursors.cpp).
namespace SssCursors {
    void tick();                // every frame
    void taskGone(u8* task);    // the stage select task's destructor (sss_portraits.cpp's hook)
    int pageToFollow();         // anyone picks: the page another player turned to, -1 none
}
