// BrawlEx's part of Build (build_parts.h; variant.h PPON_BRAWLEX: in the plugins built for a
// build with BrawlEx, empty in Project+'s own).
#include "variant.h"
#if PPON_BRAWLEX
#include <types.h>
#include "build.h"
#include "build_parts.h"

namespace Build {
    // BrawlEx's roster file ("RSTC"): in the character select's task (+0x42C), or where the
    // build's initProc loads it. REX's CSSFix.asm moves it to MEM2 (0x9012C800): initProc's
    // `addi r5,r31,0x42C` (sel_char 0x8068311C) is a branch to `lis r5,hi` / `ori r5,r5,lo`, and
    // task+0x42C is other data (read as P+'s icon list, the draw was a kind REX has no fighter for,
    // and the match never loaded). (The moved file: PPON_MOVED_ROSTER.)
    static const u32 RSTC = 0x52535443, ROSTER_LOAD = 0x8068311C;
    static const u8* rosterFile(u8* task)
    {
        if (*(u32*)(task + 0x42C) == RSTC) return task + 0x42C;
        if (!PPON_MOVED_ROSTER) return NULL;
        u32 w = *(const u32*)ROSTER_LOAD;
        if ((w >> 26) != 18 || (w & 3)) return NULL;
        s32 d = (s32)(w & 0x03FFFFFC);
        if (d & 0x02000000) d -= 0x04000000;
        const u32* t = (const u32*)(ROSTER_LOAD + d);
        if ((u32)t < 0x80000000 || (u32)t >= 0x817FFFF8 || (t[0] >> 16) != 0x3CA0 || (t[1] >> 16) != 0x60A5)
            return NULL;   // lis r5 / ori r5,r5
        u32 a = (t[0] << 16) | (t[1] & 0xFFFF);
        bool ram = (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000);
        return ram && *(const u32*)a == RSTC ? (const u8*)a : NULL;
    }

    int rosterRandomList(u8* task, u8* out)
    {
        const u8* f = rosterFile(task);
        if (!f) return -1;
        // +4 the file's size, +0x0D the roster's count, +0x0F the random list's; after the
        // 0x10-byte header the roster and then the random list, half of the rest each: 0x68 ids
        // in Cobalt's 0xE0-byte file, 0x100 in REX's 0x210-byte one.
        u32 size = *(u32*)(f + 4);
        u32 cap = size >= 0x12 && size <= 0x210 ? (size - 0x10) / 2 : 0;
        u32 count = f[0x0F] <= cap ? f[0x0F] : 0;
        int n = 0;
        for (u32 i = 0; i < count && n < CSS_IDS; i++) out[n++] = f[0x10 + cap + i];
        return n;
    }

    // P+Ex (Hidden Alt Costume Sets): the code skips Brawl's .pcs load at 0x8084CE34 and keeps
    // the address of its own memory in the skipped word after it: four request bytes (one per
    // slot: 1 AltZ, 2 AltR), then its strings. stLoaderPlayer takes a slot's request when it
    // loads the fighter (costume 62 / 61 if the file is there) and createFighter clears it.
    static const u32 PEX_ALT_SKIP = 0x8084CE34;
    u8* hiddenAltRequests()
    {
        if (*(const u32*)PEX_ALT_SKIP != 0x4800001C) return 0;
        u32 p = *(const u32*)(PEX_ALT_SKIP + 4);
        if (p < 0x80001800 || p >= 0x817FFF00) return 0;
        static const char SIGN[] = "EtcAlt\0AltZ\0AltR";
        const char* q = (const char*)(p + 4);
        for (u32 i = 0; i < sizeof(SIGN); i++) {
            if (q[i] != SIGN[i]) return 0;
        }
        return (u8*)p;
    }
}
#endif
