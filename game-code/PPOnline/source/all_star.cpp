// P+'s All-Star Versus online (docs/design/special-rules.md).
#include "all_star.h"
#include "online_menu.h"

#include <OS/OSCache.h>
#include <string.h>

namespace AllStar {

    // P+'s tables (AllStarVs.asm, 50CC.asm): per port a roster of 3-byte entries {character, P+'s
    // colour, the CSS's costume} up to an 0xCC, +0x99 0xFF, +0x9C the entry being played (its
    // match setup hook takes the first, a lost stock moves on), +0x9D how many were, +0x9E the
    // stock icons'; and its CSS twin, 2-byte entries {CSS id, costume}, +0x68 their bytes, +0x69
    // 0xAC. Both written whole, as P+'s CSS leaves them for a roster, so that every machine has the
    // same bytes (Dolphin's setup key compares them; gp-v24 rolls them back).
    static const u32 TABLE = 0x80588000, ENTRY_SIZE = 0xA0;
    static const u32 TWIN = 0x80588300, TWIN_SIZE = 0x70;

    void writeTables(const PPOM::Session& se)
    {
        if (se.special.rows[PPOM::SR_OTHER] != 2) return;
        OnlineMenu::allStarTablesWritten();   // (the CSS puts the player's own back, restoreCss)
        for (int i = 0; i < 4; i++) {
            u8* a = (u8*)(TABLE + i * ENTRY_SIZE);
            u8* b = (u8*)(TWIN + i * TWIN_SIZE);
            memset(a, 0, ENTRY_SIZE);
            memset(b, 0, TWIN_SIZE);
            a[0x99] = 0xFF;
            const bool present = i < se.numPlayers && se.players[i].present;
            const PPOM::PortExtra& ex = se.extra[i];
            int n = present ? ex.rosterCount : 0;
            if (n > PPOM::ROSTER_MAX) n = PPOM::ROSTER_MAX;
            for (int k = 0; k < n; k++) {
                const PPOM::RosterEntry& e = ex.roster[k];
                a[3 * k] = e.charKind;
                a[3 * k + 1] = e.colour;
                a[3 * k + 2] = e.costume;
                b[2 * k] = e.css;
                b[2 * k + 1] = e.costume;
            }
            // A player with none (an older lock-in): the character they locked in.
            if (present && n == 0) {
                const PPOM::SessionPlayer& pl = se.players[i];
                a[0] = pl.charKind;
                a[2] = pl.costume & PPOM::COSTUME_COLOUR;
                a[1] = OnlineMatch::costumeFile(pl.charKind, a[2]);   // (its file, as P+'s)
                n = 1;
            }
            if (present) {
                a[3 * n] = 0xCC;   // (+0x9C 0: the first entry plays, the next after a lost stock)
                b[0x68] = (u8)(2 * n);
                b[0x69] = 0xAC;
            }
            DCFlushRange(a, ENTRY_SIZE);
            DCFlushRange(b, TWIN_SIZE);
        }
    }
}
