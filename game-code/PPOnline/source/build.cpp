// What the build of the game offers (build.h). The plugin ran on P+ v3.2 alone until 2026-11-14,
// with P+'s roster and its slot table's address written into it; a P+ mod then could lock in
// none of its own characters (Cobalt: 41 of them, ids up to 0x7F), and its tables are elsewhere.
//
// Every Brawl build keeps the character select's slot table behind the same two functions of
// main.dol, which P+ and BrawlEx patch in place rather than replace:
//   0x800AF708  exchangeGmCharacterKind2MuSelchkind: searches the table for a character kind
//               (lis r4 / li r0,<count> / addi r4: the table and how many slots it searches; Brawl
//               and P+ compare eight slots a turn, BrawlEx one)
//   0x800AF80C  exchangeMuSelchkind2GmCharacterKind: the slot's kind (hooked by both)
// P+'s table is at 0x80585B00 (0x38 slots searched), Cobalt's at 0x817D52A0 (0x80) and REX's at
// 0x817D2C60 (0x100), with the same 16-byte entries. So the table and its length are read from
// that code.
#include <string.h>

#include "variant.h"
#include "build.h"
#include "build_parts.h"
#include "online.h"
#include "ppom.h"

namespace Build {
    // A pointer into the game's memory (MEM1 or MEM2).
    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    static const u32 TO_SELCH = 0x800AF708;
    static const u32 TO_KIND = 0x800AF80C;

    u32 slotTable()
    {
        u32 hi = *(u32*)TO_SELCH, lo = *(u32*)(TO_SELCH + 8);
        if ((hi & 0xFFFF0000) != 0x3C800000 || (lo & 0xFFFF0000) != 0x38840000) return 0;   // lis r4 / addi r4,r4
        u32 t = (hi << 16) + (u32)(s32)(s16)(lo & 0xFFFF);
        int n = slotCount();
        return n > 0 && isPtr(t) && isPtr(t + n * 0x10 - 1) ? t : 0;
    }

    int slotCount()
    {
        u32 li = *(u32*)(TO_SELCH + 4);
        if ((li & 0xFFFF0000) != 0x38000000) return 0;        // li r0, count
        int n = (int)(li & 0xFFFF);
        // Brawl's loop compares eight slots a turn (its fourth compare loads +0x10 of the table);
        // BrawlEx branches out of the first pair to the loop's end: one slot a turn.
        if ((*(u32*)(TO_SELCH + 0x2C) >> 26) == 34) n *= 8;   // lbz
        return n > 0 && n <= CSS_IDS ? n : 0;
    }

    u8 charKindOf(int css)
    {
        typedef int (*ExchangeFn)(int);
        return (u8)((ExchangeFn)TO_KIND)(css);
    }

    static bool realSlot(int css)
    {
        return css >= 0 && css < slotCount() && css != CSS_NONE && css != CSS_RANDOM;
    }

    int cssOfKind(int kind)
    {
        u32 t = slotTable();
        int n = slotCount();
        if (!t || kind < 0 || kind >= 0xFF) return -1;   // 0xFF: "no second character" in the table
        int second = -1;
        for (int css = 0; css < n; css++) {
            if (!realSlot(css)) continue;
            const u8* e = (const u8*)(t + css * 0x10);
            if (e[0] == kind) return css;
            if (e[1] == kind && second < 0) second = css;
        }
        return second;
    }

    bool playableCharKind(int kind)
    {
        if (kind < 0 || kind > 0xFF || kind == KIND_GIGA_BOWSER || kind == KIND_WARIOMAN) return false;
        // A build whose slot table this cannot find (another patch at 0x800AF708): P+ v3.2's
        // roster, which is also what Dolphin checks by when the game published none (publish).
        if (!slotTable()) return kind <= 0x11 || (kind >= 0x13 && kind <= 0x2B) || kind == 0x32 || kind == 0x33 || kind == 0x35;
        return cssOfKind(kind) >= 0;
    }

    // The character select's task (the scene's +0x400), on the character select only.
    static u8* cssTask()
    {
        if (!Online::inScene("scSelctCharacter")) return NULL;
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        return isPtr(task) ? (u8*)task : NULL;
    }

    static bool drawable(int css)
    {
        return realSlot(css) && playableCharKind(charKindOf(css));
    }

    // ---- The character select's models (build.h) ------------------------------------------------
    // P+ v3.2's places in muSelCharTask, in the enum's order (a build whose task keeps them
    // elsewhere: found by name, build_parts.h).
    static const u16 CSS_MODEL_V32[CSS_MODELS] = {0x150, 0x3C8, 0x418, 0x158, 0x15C, 0x420, 0x424, 0x3C4};
    u8* cssModel(u8* task, int which)
    {
        if (!isPtr((u32)task) || which < 0 || which >= CSS_MODELS) return NULL;
        u8* named;
        if (cssModelByName(task, which, &named)) return named;
        return *(u8**)(task + CSS_MODEL_V32[which]);
    }

    int randomCss()
    {
        typedef int (*RandiFn)(int);
        u8 pool[CSS_IDS];
        int n = 0;
        u8* task = cssTask();
        // BrawlEx's own random list where the character select has its roster file (build_parts.h).
        int listed = task ? rosterRandomList(task, pool) : -1;
        if (listed >= 0) {
            for (int i = 0; i < listed; i++) {
                if (drawable(pool[i])) pool[n++] = pool[i];   // (kept in place: n <= i)
            }
        } else if (task) {
            // Brawl and P+: the icons' ids at +0x70 (32 bits each), their count at +0x13C.
            u32 count = *(u32*)(task + 0x13C);
            if (count > 51) count = 0;
            for (u32 i = 0; i < count; i++) {
                u32 css = *(u32*)(task + 0x70 + 4 * i);
                if (css < (u32)CSS_IDS && drawable((int)css)) pool[n++] = (u8)css;
            }
        }
        if (n == 0) {
            // Not on a character select (or one this does not know): any slot of a playable kind.
            for (int css = 0; css < slotCount(); css++) {
                if (drawable(css)) pool[n++] = (u8)css;
            }
        }
        if (n == 0) return 0;
        int i = ((RandiFn)0x8003FC7C)(n);
        return pool[(u32)i % (u32)n];
    }

    void publish()
    {
        PPOM::Build& b = PPOM::g_block.build;
        // The table is the build's own data, set up while it boots: once published it is looked
        // at again about once a second, not every frame (this runs in matches and in every
        // re-run frame of a rollback too).
        if (b.seq != 0 && (PPOM::g_block.debug.frames & 63) != 0) return;
        // No table found: no character is published (Dolphin then keeps P+ v3.2's list, as
        // before the game told it), but the profile is, which says so.
        u32 t = slotTable();
        // The stage select's kinds, every page (none: all 0, and Dolphin keeps its own range).
        // Not while a stage select is up: Ranked's shows the set's stages in P+'s first page
        // (StageLegal::setPage) until it is gone.
        u8 stages[sizeof(b.stageKinds)];
        memset(stages, 0, sizeof(stages));
        if (b.seq != 0 && Online::inScene("scSelStage")) memcpy(stages, b.stageKinds, sizeof(stages));
        for (int page = 0; page < STAGE_PAGES && !(b.seq != 0 && Online::inScene("scSelStage")); page++) {
            for (int pos = 0; pos < stagePageCount(page); pos++) {
                int kind = stageKindOfSlot(stageSlotAt(page, pos));
                if (kind > 0) stages[kind >> 3] |= (u8)(1 << (kind & 7));
            }
        }
        u8 kinds[sizeof(b.charKinds)];
        memset(kinds, 0, sizeof(kinds));
        // One pass over the slots: each slot's two characters, as playableCharKind has them.
        int n = t ? slotCount() : 0;
        for (int css = 0; css < n; css++) {
            if (css == CSS_NONE || css == CSS_RANDOM) continue;
            const u8* e = (const u8*)(t + css * 0x10);
            for (int k = 0; k < 2; k++) {
                u8 kind = e[k];
                if (kind == 0xFF || kind == KIND_GIGA_BOWSER || kind == KIND_WARIOMAN) continue;
                kinds[kind >> 3] |= (u8)(1 << (kind & 7));
            }
        }
        bool same = b.seq != 0;
        for (u32 i = 0; same && i < sizeof(kinds); i++) same = kinds[i] == b.charKinds[i];
        for (u32 i = 0; same && i < sizeof(stages); i++) same = stages[i] == b.stageKinds[i];
        const u32 word = profile();
        if (same && word == b.profile) return;
        memcpy(b.charKinds, kinds, sizeof(kinds));
        memcpy(b.stageKinds, stages, sizeof(stages));
        b.slots = (u16)n;
        b.profile = word;
        b.seq++;
        if (b.seq == 0) b.seq = 1;
    }

    // P+ (LegacyTE/AltCostume.asm): the hook at 0x8068478C (the character select being left)
    // stores a word per slot at gmSelCharData + 0x43AD8 + slot * 0x5C, and its Gecko lines set
    // the slot's costume from it every frame.
    static const u32 PPLUS_ALT_HOOK = 0x8068478C;
    static const u32 PPLUS_ALT_WORDS = 0x901C4618;
    static const u32 PPLUS_ALT_STEP = 0x5C;
    // P+Ex's (Hidden Alt Costume Sets, in Cobalt and P+Ex): request bytes, build_parts.h.
    // ---------------------------------------------------------------------------------------
    // Stage select. Two designs of P+'s stage select codes are known (build.h), with the same
    // pieces at other addresses (the codesets' Random.asm aliases):
    //                         P+ v3.2 and its mods         P+'s older design (REX 0.5)
    //   pages                 0x8042C524: 5 x {count,      0x80495D04: 5 pointers to slot
    //                         slot[39]}                    arrays; counts at 0x806B929C and
    //                                                      0x806B92A4 (pages 1-2, the stage
    //                                                      select's page records) and 0x80496002..
    //   slot -> {kind, cos.}  0x8042C5EC                   *(0x80495D00)
    //   random switch         0x8042C4E8                   0x806AEDA0
    //   .rss path             0x8042C808                   0x806AEDE0
    // and, in both, the page index, the strike table and the random bonus byte 0x19, 0x1A and
    // 0x38 after the .rss path ("%s%s%s%s/%s%02X.rss", RSS_EXDATA_PATH_FULL). That string where a
    // design keeps it says the random switch and strike table are there too. A build with
    // neither has no stage select this knows (STAGES_NONE): every table reads as empty, nothing
    // is written, and the build does not meet the contract (build.h). It read P+ v3.2's pages
    // at their address before, whatever the build kept there.
    // (The older design's own places and reads: source/variant/old_stage_select.cpp, in the
    // plugins built for such a build; to the others a build of that design has no stage select,
    // and profile() says it needs another plugin.)
    static const u32 V32_PAGES = 0x8042C524, V32_SLOTS = 0x8042C5EC, V32_SWITCH = 0x8042C4E8,
                     V32_PATH = 0x8042C808;
    static const u32 PATH_TO_PAGE_INDEX = 0x19, PATH_TO_STRIKE = 0x1A, PATH_TO_BONUS = 0x38;

    int stageDesign()
    {
        if (olderDesign()) return STAGES_PPLUS_OLD;
        return hasRssPath(V32_PATH) ? STAGES_PPLUS : STAGES_NONE;
    }

    int stagePageCount(int page)
    {
        if (page < 0 || page >= STAGE_PAGES) return 0;
        int n;
        if (olderDesign()) n = olderPageCount(page);
        else if (stageDesign() == STAGES_PPLUS) n = *(const u8*)(V32_PAGES + 0x28 * page);
        else return 0;   // (STAGES_NONE)
        return n > STAGE_PAGE_MAX ? STAGE_PAGE_MAX : n;
    }

    int stageSlotAt(int page, int pos)
    {
        if (pos < 0 || pos >= stagePageCount(page)) return -1;
        if (olderDesign()) return olderSlotAt(page, pos);
        return *(const u8*)(V32_PAGES + 0x28 * page + 1 + pos);
    }

    int stageKindOfSlot(int slot)
    {
        if (slot < 0 || slot >= 0x80 || stageDesign() == STAGES_NONE) return -1;
        const u8* t = olderDesign() ? olderSlots() : (const u8*)V32_SLOTS;
        return t[2 * slot];
    }

    u32 stagePageTable() { return stageDesign() == STAGES_PPLUS ? V32_PAGES : 0; }

    static u32 rssPath()
    {
        if (olderDesign()) return OLD_STAGE_PATH;
        return hasRssPath(V32_PATH) ? V32_PATH : 0;
    }
    u32 stageRandomSwitch()
    {
        if (olderDesign()) return OLD_STAGE_SWITCH;
        return rssPath() ? V32_SWITCH : 0;
    }
    u32 stageStrikeTable() { u32 p = rssPath(); return p ? p + PATH_TO_STRIKE : 0; }
    u32 stageStrikePageIndex() { u32 p = rssPath(); return p ? p + PATH_TO_PAGE_INDEX : 0; }
    u32 stageRandomBonus() { u32 p = rssPath(); return p ? p + PATH_TO_BONUS : 0; }

    static int altCostumes()
    {
        if (hiddenAltRequests()) return ALTS_HIDDEN_SETS;
        return *(const u32*)PPLUS_ALT_HOOK >> 26 == 18 ? ALTS_PPLUS : ALTS_NONE;
    }

    void setAltCostumes(const u8* alt)
    {
        switch (altCostumes()) {
        case ALTS_HIDDEN_SETS: {
            u8* req = hiddenAltRequests();
            for (int i = 0; i < 4; i++) req[i] = alt[i];
            break;
        }
        case ALTS_PPLUS:
            for (int i = 0; i < 4; i++) *(volatile u32*)(PPLUS_ALT_WORDS + i * PPLUS_ALT_STEP) = alt[i];
            break;
        }
    }

    // P+'s ASL Helper (LegacyTE/ASLHelper.asm): its hook at 0x800B99FC keeps the buttons held on
    // the stage select in a word of the function it replaced (0x800B9EA2), which P+'s stage file
    // loader reads. Without that hook the address is the game's own code (Brawl's disc:
    // `stw r0,0x20(r30)`), so the word is written only where the hook is.
    static const u32 ASL_HOOK = 0x800B99FC, ASL_BUTTONS = 0x800B9EA2;
    static bool stageAlternates() { return (*(const u32*)ASL_HOOK >> 26) == 18; }
    bool setStageAlternate(u16 buttons)
    {
        if (!stageAlternates()) return false;
        *(u16*)ASL_BUTTONS = buttons;
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // The profile and the contract (build.h).
    // P+'s Code Menu (online_menu.cpp): loaded at 0x804E0000, its header's +4 the main page.
    static bool codeMenu()
    {
        u32 page = *(const u32*)0x804E0004;
        return page > 0x804E0000 && page < 0x804F0000;
    }
    // The File Patch Code's folder ("/Project+/", "/Cobalt/./"), which names the build to mm
    // (Dolphin's GameBuild): a path of at least a letter between slashes.
    static bool cardFolder()
    {
        const char* p = (const char*)0x80406920;
        return p[0] == '/' && p[1] > ' ' && p[1] != '/';
    }

    u32 profile()
    {
        const u32 design = (u32)stageDesign();
        u32 w = design | ((u32)altCostumes() << 2) | (codeMenu() ? 0x10 : 0) | (stageAlternates() ? 0x20 : 0);
        w |= cardFolder() ? 0x40 : 0x400;
        w |= slotTable() ? 0x80 : 0x100;
        if (design == STAGES_NONE) w |= 0x200;
        // A build of a kind this variant leaves to another (variant.h): BrawlEx's slot search
        // (one slot a turn where Brawl's and P+'s compare eight: slotCount) under Project+'s own
        // plugin, whose RANDOM and alt costumes would be P+'s; P+'s older stage select under a
        // plugin that knows v3.2's alone.
        if (!PPON_BRAWLEX && (*(const u32*)(TO_SELCH + 0x2C) >> 26) != 34) w |= 0x800;
        if (!PPON_OLD_STAGE_SELECT && hasRssPath(OLD_STAGE_PATH)) w |= 0x800;
        return w | ((u32)PPON_VARIANT << 16);
    }

    bool conforms() { return (profile() & 0xFF00) == 0; }

    const char NOT_ONLINE[] = "This game can't play online here.";
}
