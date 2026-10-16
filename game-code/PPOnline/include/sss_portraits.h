#pragma once
#include <sy_core.h>
#include <types.h>

#include "ppom.h"

// Ranked's stage select shows who plays whom: each player's CSS card (the CSS's own models, the
// port's colour, the portrait), the name on its plate as the CSS writes a tag (sss_portraits.cpp).
namespace SssPortraits {
    void install(CoreApi* api);
    void tick();   // every frame
    // Ranked's last step when it carries the players (GameStep pvCount), else NULL.
    const PPOM::GameStep* previewStep();
}
