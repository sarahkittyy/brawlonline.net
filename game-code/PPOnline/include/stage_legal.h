#pragma once
#include <sy_core.h>
#include <types.h>

class MuMsg;

// Legal stages only on P+'s stage select (stage_legal.cpp).
namespace StageLegal {
    void install(CoreApi* api);
    void tick();                           // every frame: puts the random switch back after the SSS
    bool active();                         // Ranked's steps, a room's loser picking from its list, or CFG_SSS_LEGAL
    bool roomPicksFromList();              // the room's loser picks from its stages (SESSION RoomPlay, v7)
    void setList(const u8* kinds, int n);  // srStageKind list (SESSION later); n <= 0: P+'s legal list
    bool allowedKind(int kind);
    bool selectableKind(int kind);         // a srStageKind on any page of P+'s stage select
    int kindAt(int page, int pos);         // the stage at a page's position (P+'s tables), -1 none
    void onMsgCreate(MuMsg* m, u32 caller);  // the stage select's message object (its rule line)
    void onSssPrint(u32 caller);             // the stage select printed its rule line itself
    void anyoneRandom();                     // anyone picks: START, a random stage of the room's
    // P+'s secret stage lists (bug report #338): this frame's START with L / R held opens the
    // hovered stage's list (then START is P+'s, not a random pick); noted for the stage select's
    // hooks of the same frame. Called every frame of the online stage select, before its buttons.
    bool noteSecretStart(u32 held, u32 pressed);
    bool onSecretList();                     // the stage select shows a secret list (B goes back)
}
