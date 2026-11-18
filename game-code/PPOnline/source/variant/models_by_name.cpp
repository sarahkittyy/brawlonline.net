// The character select's models found by name (build_parts.h; build.h cssModel; variant.h
// PPON_MODELS_BY_NAME: in the plugins built for a build with BrawlEx's own character select
// task, empty in the others, which read them at P+ v3.2's places).
#include "variant.h"
#if PPON_MODELS_BY_NAME
#include <string.h>
#include <types.h>
#include "build.h"
#include "build_parts.h"

namespace Build {
    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // Each of build.h's CSS_ models' MDL0 name, in the enum's order.
    static const char* const NAMES[CSS_MODELS] = {
        "MenSelchrRule_TopN",      "MenSelchrRulebase_TopN", "MenSelchrTitleW_TopN",
        "MenSelchrRnum1_TopN",     "MenSelchrRnum2_TopN",    "MenSelchrState0005_TopN",
        "MenSelchrState0006_TopN", "MenSelchrReady_TopN",
    };

    static const char* mdlName(u32 obj)
    {
        if (!isPtr(obj)) return NULL;
        u32 mdl0 = *(const u32*)(obj + 8);
        if (!isPtr(mdl0) || *(const u32*)mdl0 != 0x4D444C30 /* MDL0 */) return NULL;
        u32 ver = *(const u32*)(mdl0 + 8);
        u32 sections = ver >= 10 ? 14 : 11;   // (the name's offset follows the section offsets)
        s32 off = *(const s32*)(mdl0 + 0x10 + 4 * sections);
        if (off <= 0 || off > 0x800000 || !isPtr(mdl0 + off)) return NULL;
        return (const char*)(mdl0 + off);
    }

    bool cssModelByName(u8* task, int which, u8** out)
    {
        // Where each was last found (another CSS at the same address is looked through again).
        static struct { u8* task; u32 off; } s_found[CSS_MODELS];
        *out = NULL;
        const char* name = NAMES[which];
        if (s_found[which].task == task) {
            u32 obj = *(u32*)(task + s_found[which].off);
            const char* n = mdlName(obj);
            if (n && strcmp(n, name) == 0) {
                *out = (u8*)obj;
                return true;
            }
            s_found[which].task = NULL;
        }
        for (u32 off = 0x40; off < 0x440; off += 4) {
            u32 obj = *(u32*)(task + off);
            const char* n = mdlName(obj);
            if (!n || strcmp(n, name) != 0) continue;
            s_found[which].task = task;
            s_found[which].off = off;
            *out = (u8*)obj;
            return true;
        }
        return true;
    }
}
#endif
