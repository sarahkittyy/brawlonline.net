#pragma once
#include <types.h>

// An nw4r font's character map, for the files that read one (labels.cpp;
// source/variant/other_fonts.cpp). Local to each file that includes it, as their other helpers.
namespace FontCmap {
    static inline bool ptr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // A CMAP chain's glyph for `code` (0xFFFF: a direct table's "none"), -1 if no block maps it.
    static int index(u32 p, u16 code)
    {
        for (int guard = 0; ptr(p) && guard < 64; guard++) {
            u8* b = (u8*)p;
            u16 lo = *(u16*)b, hi = *(u16*)(b + 2), method = *(u16*)(b + 4);
            if (code >= lo && code <= hi) {
                if (method == 0) return *(u16*)(b + 0xC) + (code - lo);
                if (method == 1) return *(u16*)(b + 0xC + 2 * (code - lo));
                if (method == 2) {
                    u16 n = *(u16*)(b + 0xC);
                    u16* t = (u16*)(b + 0xE);
                    for (u16 i = 0; i < n; i++) {
                        if (t[2 * i] == code) return t[2 * i + 1];
                    }
                }
            }
            p = *(u32*)(b + 8);
        }
        return -1;
    }
}
