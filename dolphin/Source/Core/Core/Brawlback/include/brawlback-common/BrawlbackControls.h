#pragma once
#include "BrawlbackTypes.h"

#pragma pack(push, 4)
struct BrawlbackControls {
    #if __cplusplus == 199711L
    bu8 L;
    bu8 R;
    bu8 Z;
    bu8 UpDPad;
    bu8 LeftRightDPad;
    bu8 DownDPad;
    bu8 A;
    bu8 B;
    bu8 subStick;
    bu8 Y;
    bu8 X;
    bu8 tapJumpToggle;
    
    BrawlbackControls();
    BrawlbackControls& operator=(const BrawlbackControls &D );
    BrawlbackControls(const BrawlbackControls& D);
    #else
    bu8 L = 0;
    bu8 R = 0;
    bu8 Z = 0;
    bu8 UpDPad = 0;
    bu8 LeftRightDPad = 0;
    bu8 DownDPad = 0;
    bu8 A = 0;
    bu8 B = 0;
    bu8 subStick = 0;
    bu8 Y = 0;
    bu8 X = 0;
    bu8 tapJumpToggle = 0;
    #endif
};
#pragma pack(pop)