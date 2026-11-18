#pragma once
#include <types.h>

// Where the plugin's own things go on a build's screens: the places that depend on the build's
// menu art, which nothing in the game says. The plugin has P+'s; a build whose art differs has
// its own in its entry of mm's games table (`layout`, docs/design/pplus-mods.md section 3), as
// numbers, and Dolphin writes them into PPOM's LAYOUT (ppom.h). So a build with other art needs
// no change here: its places are data of its entry.
//
// The four groups, each P+'s unless the build's entry has it (in the units each is used in). They
// are PPOM's LAYOUT itself (ppom.h Layout: no second copy; the plugin's memory is the Network
// heap's, which the biggest costumes need too): layout.cpp puts P+'s there when the plugin is
// loaded, and Dolphin writes the build's own over the groups its entry has:
//   g_sssLine         the stage select's turn line: its window's x1 y1 x2 y2 and its scale
//                     (stage_legal.cpp showTurn)
//   g_cardPlates      a room's cards' name plates: P1's plate's middle, the step to the next
//                     card's, half its width (room_css.cpp plateUnderHand)
//   g_cssPlaces       the character select's bar: the status line, the own code, the room's
//                     lines and their text size (online_menu.cpp, where each number is said)
//   g_cssTextButtons  the room's buttons as words, on a character select without Brawl's online
//                     art: per button (RULES, PUBLIC, SIT OUT, FFA) the window's x1 x2 y and the
//                     hand's rect (rules_button.cpp)
#include "ppom.h"
#define g_sssLine (PPOM::g_block.layout.sssLine)
#define g_cardPlates (PPOM::g_block.layout.cardPlates)
#define g_cssPlaces (PPOM::g_block.layout.cssBar)
#define g_cssTextButtons (PPOM::g_block.layout.cssButtons)
