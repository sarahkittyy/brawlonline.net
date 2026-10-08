#pragma once
#include "BrawlbackTypes.h"
const size_t MAX_HEAPNAME_SIZE = 30;

#pragma pack(push, 4)
struct SavestateMemRegionInfo {
    #if __cplusplus == 199711L
    bu8 nameBuffer[MAX_HEAPNAME_SIZE];
    bu8 nameSize;
    bu32 address;
    bu32 size;
    SavestateMemRegionInfo();
    SavestateMemRegionInfo(const SavestateMemRegionInfo &D);
    SavestateMemRegionInfo& operator=(const SavestateMemRegionInfo &D);
    #else
    bu8 nameBuffer[MAX_HEAPNAME_SIZE];
    bu8 nameSize = 0;
    bu32 address = 0;
    bu32 size = 0;
    #endif
};
#pragma pack(pop)
