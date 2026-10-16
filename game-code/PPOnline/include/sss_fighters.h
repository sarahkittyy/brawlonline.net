#pragma once
#include <sy_core.h>
#include <types.h>

#include "ppom.h"

// Ranked's stage select shows both players' fighters standing in their idle animation, each with
// its name and rating (sss_fighters.cpp).
namespace SssFighters {
    void install(CoreApi* api);
    void tick();   // every frame
    // Ranked's last step when it carries a fighter preview (GameStep pvCount), else NULL.
    const PPOM::GameStep* previewStep();
    // A preview entry the game can load: present, a character and costume this CSS could lock in.
    bool validPreview(const PPOM::GameStep::PreviewPlayer& pv);
    // online_match.cpp put the preview's players into gmSelCharData for Brawl's fighter preload.
    void notePreload();
}
