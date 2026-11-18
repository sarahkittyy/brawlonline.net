// A build whose fonts are not P+'s (other_fonts.h; variant.h PPON_OTHER_FONTS: in the plugins
// built for such a build, empty in the others).
#include "variant.h"
#if PPON_OTHER_FONTS
#include <types.h>
#include "font_cmap.h"
#include "other_fonts.h"

namespace OtherFonts {
    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // Whether a font (an nw4r ResFont: +0x10 its RFNT file) has a glyph of its own for `code` (not
    // its stand-in for a missing one): P+'s keypad font has the full-width '＃' and no ASCII '#',
    // REX's the ASCII one only.
    static bool fontHas(const void* resFont, u16 code)
    {
        if (!isPtr((u32)resFont)) return false;
        u32 file = *(const u32*)((const u8*)resFont + 0x10);   // the RFNT (RFNU loaded)
        if (!isPtr(file)) return false;
        u32 finf = file + 0x10;
        if (*(const u32*)finf != 0x46494E46 /* FINF */) return false;
        int g = FontCmap::index(*(const u32*)(finf + 0x18), code);
        return g >= 0 && g != 0xFFFF;
    }

    bool asciiHash(MuMsg* m)
    {
        u8* message = isPtr((u32)m) ? *(u8**)((u8*)m + 8) : NULL;   // MuMsg::m_message
        const void* font = isPtr((u32)message) ? *(void**)(message + 0x48) : NULL;   // its ResFont
        return font && !fontHas(font, 0xFF03) && fontHas(font, '#');
    }
}
#endif
