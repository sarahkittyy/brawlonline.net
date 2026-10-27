#pragma once
#include <types.h>

#include "ppom.h"

// The room's STAGE SWITCH (stage_switch.cpp): P+'s RANDOM STAGE SWITCH page of the rules menu,
// opened in a room, shows and edits the room's stages (docs/design/room-rules.md §2).
namespace StageSwitch {
    // Before the rules menu opens in a room: the room's stages become P+'s switch for the menu.
    // `canEdit`: the host's menu (between games).
    void open(bool canEdit);
    // Every frame the menu is open (rt: the rules task).
    void tick(u8* rt);
    // The menu closed: the player's own switch is put back. True if the host changed anything
    // (send() posts it).
    bool close();
    void send();
    // The CSS is gone: the menu's label copies are about to be freed (Labels::releaseCopies).
    void forgetCopies();
    // The STAGE SWITCH page's input (module 18's input reader, filtered).
    u32 input(u8* page, u32 bits);
    // The hazards the host chose, into `ru` (sent with the room's rules): `hazards` and
    // `hazardStages`.
    void hazards(PPOM::Rules& ru);
    // The menu's bottom-line texts while it is open in a room, or NULL.
    // `msg`: the MuMsg printing (the footer is printed again when A on the bar or Z changes it).
    const char* menuText(void* msg, const void* msbin, u32 line);
}
