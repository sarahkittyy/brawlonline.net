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
}
