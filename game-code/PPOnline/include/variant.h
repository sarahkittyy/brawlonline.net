#pragma once

// Which builds of the game this plugin is built for (the Makefile's VARIANT, game-code/build.sh;
// docs/design/pplus-mods.md section 0). One source, a plugin per kind of build: each build is
// tested on its own anyway, the plugin's memory is the Network heap's, which Project+'s biggest
// costumes need too (docs/game-code.md section 2), and what one build needs must not be able to
// break another. A build's entry of mm's games table names the plugin it takes (`plugin`).
//
//   pplus  Project+ itself (PPOnline.rel): nothing below.
//   pex    Project+ v3.2 with BrawlEx (Project+ Ex, Cobalt): PPON_BRAWLEX.
//   rex    an older Project+ with BrawlEx and its own menus (REX): everything.
//
// The switches are by what a build has, not by its name. A variant is a set of them (below): a
// build that needs another set gets a variant of its own here, in the Makefile (VDEFINE_) and in
// mm's list of them (server/crates/mm/src/games.rs). Each is 1 or 0.
//
// What a switch turns on is a file of its own in source/variant, empty in the plugins built
// without it, behind a header whose functions do nothing there (word_buttons.h, rules_rebuild.h,
// restore_online.h, other_fonts.h, build_parts.h). The other files have no #if on a switch: they
// call those, and a change to one build's own code cannot change another build's plugin.
#if defined(PPON_VARIANT_REX)
#define PPON_VARIANT 2
#define PPON_BRAWLEX 1
#define PPON_OLD_STAGE_SELECT 1
#define PPON_MOVED_ROSTER 1
#define PPON_MODELS_BY_NAME 1
#define PPON_OWN_BOOT 1
#define PPON_WORD_BUTTONS 1
#define PPON_RESTORE_ONLINE 1
#define PPON_REBUILD_RULES 1
#define PPON_OTHER_FONTS 1
#elif defined(PPON_VARIANT_PEX)
#define PPON_VARIANT 1
#define PPON_BRAWLEX 1
#else
#define PPON_VARIANT 0
#endif

// BrawlEx: its roster file ("RSTC": what RANDOM draws from), QuickLava's Hidden Alt Costume Sets
// (alt costumes by request bytes), and a Code Menu of up to 256 value lines (P+'s has 85).
#ifndef PPON_BRAWLEX
#define PPON_BRAWLEX 0
#endif
// P+'s older stage select design (pointers to the tables, the counts in the page records).
#ifndef PPON_OLD_STAGE_SELECT
#define PPON_OLD_STAGE_SELECT 0
#endif
// BrawlEx's roster file moved out of the character select's task (REX's CSSFix.asm).
#ifndef PPON_MOVED_ROSTER
#define PPON_MOVED_ROSTER 0
#endif
// BrawlEx's own character select task: its models found by name, not at P+ v3.2's places.
#ifndef PPON_MODELS_BY_NAME
#define PPON_MODELS_BY_NAME 0
#endif
// A build without P+'s BootToCSS: the game's own way from the boot to the title and the menus.
#ifndef PPON_OWN_BOOT
#define PPON_OWN_BOOT 0
#endif
// A character select without Brawl's online art: the room's buttons as words in the bar.
#ifndef PPON_WORD_BUTTONS
#define PPON_WORD_BUTTONS 0
#endif
// A main menu whose PLAY ONLINE button was moved off the screen: brought back.
#ifndef PPON_RESTORE_ONLINE
#define PPON_RESTORE_ONLINE 0
#endif
// A character select whose own rules button is not Brawl's and whose archive has no rules menu:
// the room's menu made the vanilla way, from the build's main-menu archive, in a borrowed heap.
#ifndef PPON_REBUILD_RULES
#define PPON_REBUILD_RULES 0
#endif
// A keypad font with the ASCII '#' and no full-width one, and no 32x32 bold font for the labels.
#ifndef PPON_OTHER_FONTS
#define PPON_OTHER_FONTS 0
#endif
