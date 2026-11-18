#pragma once
#include <types.h>
#include "variant.h"

// The parts of Build (build.h, build.cpp) that only some kinds of build have (variant.h). Each is
// a file of source/variant, empty in the plugins built without it; there the functions below do
// nothing, so build.cpp has one path and no switch.
namespace Build {
    // ---- BrawlEx (PPON_BRAWLEX; variant/brawlex.cpp) -------------------------------------------
#if PPON_BRAWLEX
    // The slots BrawlEx's own RANDOM draws from on this character select (its roster file's
    // random list) into out (up to CSS_IDS of them): how many; -1 when the character select has
    // no roster file (the caller then takes the icons it shows).
    int rosterRandomList(u8* task, u8* out);
    // Hidden Alt Costume Sets' four request bytes (one per slot: 1 AltZ, 2 AltR), or NULL on a
    // build without that code.
    u8* hiddenAltRequests();
#else
    static inline int rosterRandomList(u8*, u8*) { return -1; }
    static inline u8* hiddenAltRequests() { return NULL; }
#endif

    // ---- P+'s older stage select design (PPON_OLD_STAGE_SELECT; variant/old_stage_select.cpp) --
    // Both designs keep this string a fixed distance before their strike table (build.cpp).
    static inline bool hasRssPath(u32 at)
    {
        static const char RSS_PATH[] = "%s%s%s%s/%s%02X.rss";
        const char* p = (const char*)at;
        for (u32 i = 0; i < sizeof(RSS_PATH); i++) {
            if (p[i] != RSS_PATH[i]) return false;
        }
        return true;
    }
    const u32 OLD_STAGE_PATH = 0x806AEDE0;     // the older design's .rss path
    const u32 OLD_STAGE_SWITCH = 0x806AEDA0;   // ... and its random switch
#if PPON_OLD_STAGE_SELECT
    // The build's stage select is of the older design (its pointers set and its path in place).
    bool olderDesign();
    int olderPageCount(int page);              // the page's stages (uncapped)
    int olderSlotAt(int page, int pos);        // the page's slot at a position
    const u8* olderSlots();                    // slot -> {srStageKind, cosmetic}
#else
    static inline bool olderDesign() { return false; }
    static inline int olderPageCount(int) { return 0; }
    static inline int olderSlotAt(int, int) { return -1; }
    static inline const u8* olderSlots() { return NULL; }
#endif

    // ---- The character select's models by name (PPON_MODELS_BY_NAME; variant/models_by_name.cpp)
#if PPON_MODELS_BY_NAME
    // One of build.h's CSS_ models, found among the task's pointers by its MDL0's name (NULL:
    // the task has none). True: this build's are found so, and *out is the answer.
    bool cssModelByName(u8* task, int which, u8** out);
#else
    static inline bool cssModelByName(u8*, int, u8**) { return false; }
#endif
}
