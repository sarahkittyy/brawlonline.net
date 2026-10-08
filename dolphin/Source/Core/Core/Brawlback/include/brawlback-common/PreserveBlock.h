#pragma once
#include "BrawlbackTypes.h"
#pragma pack(push, 4)
struct PreserveBlock {
    #if __cplusplus == 199711L
    bu32 address;
    bu32 length;
    PreserveBlock();
    PreserveBlock(const PreserveBlock &D);
    PreserveBlock& operator = (const PreserveBlock &D );
    #else
    bu32 address = 0;
    bu32 length = 0;
    #endif
};
#pragma pack(pop)
