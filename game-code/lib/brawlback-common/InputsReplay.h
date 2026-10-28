#pragma once
#include "BrawlbackTypes.h"
#pragma pack(push, 4)
struct InputsReplay
{
  #if __cplusplus == 199711L
  bu32 tapJump;
  bu32 dTaunt;
  bu32 sTaunt;
  bu32 uTaunt;
  bu32 cStick;
  bu32 shield;
  bu32 jump;
  bu32 special;
  bu32 attack;
  float leftStickX;
  float leftStickY;
  
  InputsReplay()
  {
    tapJump = 1;
    dTaunt = 1;
    sTaunt = 1;
    uTaunt = 1;
    cStick = 1;
    shield = 1;
    jump = 1;
    special = 1;
    attack = 1;
    leftStickX = 0.0;
    leftStickY = 0.0;
  }
  InputsReplay(const InputsReplay &D)
  {
    tapJump = D.tapJump;
    dTaunt = D.dTaunt;
    sTaunt = D.sTaunt;
    uTaunt = D.uTaunt;
    cStick = D.cStick;
    shield = D.shield;
    jump = D.jump;
    special = D.special;
    attack = D.attack;
    leftStickX = D.leftStickX;
    leftStickY = D.leftStickY;
  }
  InputsReplay& operator = (const InputsReplay &D ) 
  { 
    tapJump = D.tapJump;
    dTaunt = D.dTaunt;
    sTaunt = D.sTaunt;
    uTaunt = D.uTaunt;
    cStick = D.cStick;
    shield = D.shield;
    jump = D.jump;
    special = D.special;
    attack = D.attack;
    leftStickX = D.leftStickX;
    leftStickY = D.leftStickY;
    return *this;
  }
  #else
  bu32 tapJump = 0;
  bu32 dTaunt = 0;
  bu32 sTaunt = 0;
  bu32 uTaunt = 0;
  bu32 cStick = 0;
  bu32 shield = 0;
  bu32 jump = 0;
  bu32 special = 0;
  bu32 attack = 0;
  float leftStickX = 0.0;
  float leftStickY = 0.0;
  #endif
};
#pragma pack(pop)
