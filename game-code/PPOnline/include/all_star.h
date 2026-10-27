#pragma once
#include <types.h>

#include "ppom.h"

// P+'s All-Star Versus online (all_star.cpp; docs/design/special-rules.md): each player's roster,
// picked on their CSS as P+'s All-Star CSS keeps it, goes with their lock-in (PortExtra), and every
// machine writes every player's into P+'s roster tables before the match is set up.
namespace AllStar {
    // Before the Special setup (OnlineMatch::setupMatch), with the session's rules on All-Star:
    // P+'s tables from every present player's roster (Session.extra), the same on every machine.
    void writeTables(const PPOM::Session& se);
}
