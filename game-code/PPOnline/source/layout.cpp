// The places of the plugin's own things on a build's screens (layout.h): P+'s here, a build's own
// from its entry of mm's games table through Dolphin, which writes them over these in PPOM's
// LAYOUT (the groups the entry has; the others stay P+'s).
#include <string.h>

#include "layout.h"

namespace Layout {
    // P+'s own (the constants each screen had), in LAYOUT's order: the turn line in the stage
    // select's header strip, on the "STAGE SELECT" line; P+'s card, the pencil in a strip left of
    // the plate; P+'s bar (online_menu.cpp, "Where the rule line's windows go"). P+'s character
    // select has Brawl's online art: no word buttons (LAYOUT's cssButtons stay 0).
    static const float PPLUS[22] = {237.0f, -347.0f, 537.0f, -387.0f, 0.8f,
                                    -21.5f, 16.0f, 6.0f,
                                    -318.0f, 330.0f, 158.0f, 378.0f, -640.0f, -395.0f, 0.0f,
                                    -905.0f, -690.0f, 95.0f, -817.0f, 260.0f, 348.0f, 1.0f};
    // Into LAYOUT when the plugin is loaded, long before Dolphin writes a build's own there (on
    // the menus, once mm has answered).
    struct Init {
        Init() { memcpy(PPOM::g_block.layout.sssLine, PPLUS, sizeof(PPLUS)); }
    };
    static Init s_init;
}
