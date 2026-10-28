#pragma once
#include "BrawlbackConstants.h"
#include "PlayerFrameData.h"
#include "BrawlbackTypes.h"
#pragma pack(push, 4)
struct FrameData {
    #if __cplusplus == 199711L
    bu32 randomSeed;
    bool skipFrame;
    PlayerFrameData playerFrameDatas[MAX_NUM_PLAYERS];
    FrameData();
    FrameData(const FrameData& D);
    FrameData& operator=(const FrameData &D );
    #else
    bu32 randomSeed = 0;
    bool skipFrame = false;
    PlayerFrameData playerFrameDatas[MAX_NUM_PLAYERS];
    #endif
};
#pragma pack(pop)
