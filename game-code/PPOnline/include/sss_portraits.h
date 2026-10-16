#pragma once
#include <sy_core.h>
#include <types.h>

#include "ppom.h"

// Ranked's stage select shows who plays whom: each player's CSS portrait in a panel of their
// in-game port's colour, the name under it (sss_portraits.cpp).
namespace SssPortraits {
    void tick();   // every frame
    // Ranked's last step when it carries the players (GameStep pvCount), else NULL.
    const PPOM::GameStep* previewStep();
    // Drawn by match_hud.cpp after the frame, in its 640x480 2D space (ms::CharWriter's GX setup
    // done): the panels and portraits. False: nothing to show.
    bool drawPanels();
    // A side's name (0 this player, 1 the opponent): UTF-16, NUL-ended; its centre x, top y and
    // widest. False: none.
    bool nameLine(int side, u16* out, int max, float* x, float* y, float* maxW);
}
