#pragma once
#include <sy_core.h>

namespace NetMenu {
    // Nintendo WFC / Wiimmfi login bypass (Brawlback Gen 1 NetMenu hooks).
    void install(CoreApi* api);
    void pollPendingExit();   // per-frame (WITH FRIENDS -> CSS, delayed)
}
