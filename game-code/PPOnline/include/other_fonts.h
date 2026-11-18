#pragma once
#include <mu/mu_msg.h>
#include "variant.h"

// A build whose fonts are not P+'s (REX 0.5; variant.h PPON_OTHER_FONTS,
// source/variant/other_fonts.cpp): its keypad font has the ASCII '#' and no full-width one (which
// drew as the font's box), and it has no bold 32x32 font for the labels.
namespace OtherFonts {
#if PPON_OTHER_FONTS
    // The font of this text object has the ASCII '#' and no full-width '＃' of its own: the
    // keypad types and prints the ASCII one (code_entry.cpp).
    bool asciiHash(MuMsg* m);
    // The labels take a font of any cell size (labels.cpp findFont), not the bold 32x32 one alone.
    const bool ANY_CELL = true;
#else
    static inline bool asciiHash(MuMsg*) { return false; }
    const bool ANY_CELL = false;
#endif
}
