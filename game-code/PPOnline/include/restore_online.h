#pragma once
#include <types.h>
#include "variant.h"

// The main menu's PLAY ONLINE button on a build that took it off (REX 0.5; variant.h
// PPON_RESTORE_ONLINE, source/variant/restore_online.cpp).
namespace RestoreOnline {
#if PPON_RESTORE_ONLINE
    // Every frame of the main menu's top page (muProcTop): a top page whose ONLINE button rests
    // off the screen gets it back in the bottom row, with the cursor's links to it.
    void fix(u8* page);
#else
    static inline void fix(u8*) {}
#endif
}
