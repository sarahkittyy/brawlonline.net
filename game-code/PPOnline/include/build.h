#pragma once
#include <types.h>
#include "variant.h"

// What the build of the game this plugin runs in offers (build.cpp): P+ itself or a build of its
// lineage (Cobalt and Project+ Ex, P+ v3.2 with BrawlEx; REX, an older P+). Read from the game's
// own code and tables, never from a list of P+'s and never by the build's name, so that a build
// with more characters needs no change here.
//
// The contract (docs/design/pplus-mods.md section 0). The plugin is built on P+'s codes, so a
// build plays online when it has them where one of the known designs keeps them: the character
// select's slot table behind Brawl's own lookup, one of P+'s two stage select designs, and the
// File Patch Code's folder (which names the build to mm). profile() says what was found, in one
// word, for Dolphin's log and the tests; a build that does not meet the contract has its online
// modes locked on the ONLINE page (OnlineMenu::modesLocked) instead of failing in the middle of
// a match setup. (Kept small: the plugin's memory is the Network heap's, which the biggest
// costumes need too, and it is within a KB or two of what they leave.) A table that is not found is not there: it reads as
// empty and nothing is written, never P+'s address used anyway.
namespace Build {
    enum { STAGES_NONE, STAGES_PPLUS_OLD, STAGES_PPLUS };          // the stage select's design
    enum { ALTS_NONE, ALTS_PPLUS, ALTS_HIDDEN_SETS };              // "hold Z / R on START"
    // The build as it is now, in one word (its tables are set up while it boots: read again on
    // every call, a few words each), as PPOM's BUILD carries it to Dolphin and the tests: the
    // stage select's design in bits 0-1, the alt costumes in 2-3, 0x10 P+'s Code Menu is loaded,
    // 0x20 P+'s alternate stages (the ASL Helper's hook), 0x40 the File Patch Code's folder is
    // set, 0x80 the slot table is found; and what the contract asks for and the build has not in
    // bits 8-15: 0x100 the slot table, 0x200 a stage select design, 0x400 the folder, 0x800
    // this plugin: the build is of a kind another of the plugin's variants is built for (BrawlEx
    // under Project+'s own plugin, P+'s older stage select under a v3.2 one; variant.h). Bits
    // 16-17: this plugin's variant (0 pplus, 1 pex, 2 rex).
    u32 profile();
    // The build meets the contract (none of the bits 8-15).
    bool conforms();
    // The ONLINE page's description line on a build that does not (Dolphin's log says what it
    // misses).
    extern const char NOT_ONLINE[];

    // The character select's ids (MuSelchkind), a byte: a build's slot table has up to 0x100
    // entries (REX's; Cobalt's BrawlEx 0x80, Brawl's and P+'s fewer). 0x28 is "none" and 0x29
    // RANDOM in every build.
    const int CSS_IDS = 0x100;
    const int CSS_NONE = 0x28;
    const int CSS_RANDOM = 0x29;
    // Giga Bowser and Wario-Man: Final Smash forms in every build (P+ and P+Ex give them a
    // character select slot, held shield on Bowser and Wario). Never locked in online.
    const int KIND_GIGA_BOWSER = 0x2C;
    const int KIND_WARIOMAN = 0x2D;
    const int KIND_BOWSER = 0x0C;
    const int KIND_WARIO = 0x17;

    // The build's character select slot table: 16 bytes a slot (+0 the slot's gmCharacterKind, +1
    // its second one or 0xFF, +8 its costume list), as the game's own lookup names it. 0 when the
    // game's code is not the lookup this expects.
    u32 slotTable();
    // How many slots that lookup searches (Brawl and P+ 0x38, Cobalt 0x80, REX 0x100).
    int slotCount();
    // The gmCharacterKind a slot selects (the game's conversion, which P+ and BrawlEx hook).
    u8 charKindOf(int css);
    // A slot whose own character is `kind`, else the slot that has it as its second one; -1 none.
    int cssOfKind(int kind);
    // A gmCharacterKind this build's character select can lock in: one of its slots has it, and
    // it is not a Final Smash form. What another machine sends is checked with this.
    bool playableCharKind(int kind);
    // A character for a coin on RANDOM: one of those the build's own RANDOM draws from (BrawlEx's
    // random list; else the roster the character select shows), the game's RNG.
    int randomCss();
    // The playable kinds into PPOM's BUILD, for Dolphin (GameBridge): it lets through only what
    // this build can produce. Called while the plugin ticks; writes when something changed.
    void publish();
    // The build's "hold Z / R for an alt costume", written for the four slots (alt[i]: 0 none, 1
    // AltZ, 2 AltR). P+'s code (LegacyTE/AltCostume.asm) keeps a word per slot and applies it every
    // frame; P+Ex's (Hidden Alt Costume Sets, in Cobalt and P+Ex) keeps a request byte per slot
    // that its fighter loader takes once. A build with neither has no alts: nothing is written.
    void setAltCostumes(const u8* alt);
    // The alternate of the stage the next match loads (P+'s alternate stages: the buttons held
    // on the stage select, L 0x40, R 0x20, Z 0x10). False, and nothing written, on a build
    // without P+'s code for them: the word it keeps them in is the game's own code there.
    bool setStageAlternate(u16 buttons);

    // The build's stage select tables (build.cpp, "Stage select"). Two designs of P+'s
    // Random.asm / StageTable.asm are known: P+ v3.2's (and its mods'), five pages of {u8 count,
    // u8 slot[39]} at 0x8042C524 with the slot table after them, and P+'s older one (REX 0.5),
    // pointers to the slot table and each page's slots at 0x80495D00 with the page counts in the
    // stage select's own page records. Both have the same random switch, strike table and page
    // index, at the design's addresses. Five pages; CURRENT_PAGE (0x80496000) is both designs'.
    const int STAGE_PAGES = 5;
    const int STAGE_PAGE_MAX = 39;
    const u32 STAGE_CURRENT_PAGE = 0x80496000;
    int stageDesign();                         // STAGES_*
    // The page the stage select shows (P+'s codes keep it there in both designs).
    inline int stageCurrentPage() { return *(const u8*)STAGE_CURRENT_PAGE; }
    int stagePageCount(int page);              // 0..39; 0 for a page with none (or no tables)
    int stageSlotAt(int page, int pos);        // the page's slot at a position, -1 none
    int stageKindOfSlot(int slot);             // the slot's srStageKind, -1 none
    // P+ v3.2's page table, which only that design lets be rewritten (Ranked's set page); 0 else.
    u32 stagePageTable();
    // The random switch (page p at +6p: u16 positions 31.., u32 positions 0..30), the strike
    // table (same layout), the page the strike art was last drawn for, and the byte whose bit 0
    // lets P+'s random draw a stage builder stage. 0 when this build's are not where its design
    // keeps them: nothing is written then.
    u32 stageRandomSwitch();
    u32 stageStrikeTable();
    u32 stageStrikePageIndex();
    u32 stageRandomBonus();
    // The character select's models the plugin uses: muSelCharTask's MuObjects (+8 the MDL0,
    // +0xC / +0x10 its scene model). P+ v3.2's task keeps them at fixed places (the rule line at
    // +0x150, READY at +0x3C4, ITEM / STAGE at +0x420 / +0x424, its face icons between), which is
    // where the pplus and pex plugins read them; REX's task (BrawlEx's) keeps them at +0x74..,
    // +0xA4 and +0x100 / +0x104 with its 242 face icons after them (P+'s offsets were face icons
    // there), so the rex plugin finds each among the task's pointers by its MDL0's name
    // (PPON_MODELS_BY_NAME). NULL if the task has none.
    enum {
        CSS_RULE,        // MenSelchrRule_TopN: the rule line
        CSS_RULE_BASE,   // MenSelchrRulebase_TopN: the rules menu's background
        CSS_TITLE,       // MenSelchrTitleW_TopN
        CSS_RNUM1,       // MenSelchrRnum1_TopN, Rnum2: the rule line's numerals
        CSS_RNUM2,
        CSS_ITEM,        // MenSelchrState0005_TopN: the ITEM button (a room's PUBLIC)
        CSS_STAGE,       // MenSelchrState0006_TopN: the STAGE button (a room's FFA)
        CSS_READY,       // MenSelchrReady_TopN: the READY TO FIGHT banner
        CSS_MODELS
    };
    u8* cssModel(u8* task, int which);
}
