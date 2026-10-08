#pragma once
#include <sy_core.h>
#include <types.h>

// Legal stages only on P+'s stage select (stage_legal.cpp).
namespace StageLegal {
    void install(CoreApi* api);
    void tick();                           // every frame: puts the random switch back after the SSS
    bool active();                         // Direct's loser's pick, or PPOM CFG_SSS_LEGAL
    void setList(const u8* kinds, int n);  // srStageKind list (SESSION later); n <= 0: P+'s legal list
    bool allowedKind(int kind);
    bool selectableKind(int kind);         // a srStageKind on any page of P+'s stage select
}
