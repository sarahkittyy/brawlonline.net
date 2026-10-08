#pragma once
#include "BrawlbackTypes.h"
#pragma pack(push, 4)
struct SyncData {
    #if __cplusplus == 199711L
    float percent;
    float locX;
    float locY;
    bu32 anim;
    bu8 stocks;
    bs8 facingDir;

    SyncData();
    SyncData(const SyncData &D );
    SyncData& operator = (const SyncData &D );
    #else
    float percent = 0.0;
    float locX = 0.0;
    float locY = 0.0;
    bu32 anim = 0;
    bu8 stocks = 0;
    bs8 facingDir = 0;
    #endif
};
#pragma pack(pop)
