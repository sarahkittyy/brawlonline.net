// P+'s older stage select design (build_parts.h; build.cpp "Stage select" has both designs'
// places; variant.h PPON_OLD_STAGE_SELECT: in the plugins built for a build of that design,
// empty in the others, to which such a build has no stage select and needs another plugin).
#include "variant.h"
#if PPON_OLD_STAGE_SELECT
#include <types.h>
#include "build.h"
#include "build_parts.h"

namespace Build {
    static const u32 OLD_SLOTS_PTR = 0x80495D00, OLD_PAGES_PTR = 0x80495D04;
    static const u32 OLD_COUNT_1 = 0x806B929C, OLD_COUNT_2 = 0x806B92A4, OLD_COUNT_EX = 0x80496002;

    static bool codePtr(u32 p) { return p >= 0x80000000 && p < 0x81800000; }

    // The slot table's and every page's pointer set (by the codeset) and its .rss path in place.
    bool olderDesign()
    {
        for (int i = 0; i <= STAGE_PAGES; i++) {
            if (!codePtr(*(const u32*)(OLD_SLOTS_PTR + 4 * i))) return false;
        }
        return hasRssPath(OLD_STAGE_PATH);
    }

    int olderPageCount(int page)
    {
        return page == 0 ? *(const u8*)OLD_COUNT_1 : page == 1 ? *(const u8*)OLD_COUNT_2
                                                             : *(const u8*)(OLD_COUNT_EX + page - 2);
    }

    int olderSlotAt(int page, int pos) { return ((const u8*)*(const u32*)(OLD_PAGES_PTR + 4 * page))[pos]; }

    const u8* olderSlots() { return (const u8*)*(const u32*)OLD_SLOTS_PTR; }
}
#endif
