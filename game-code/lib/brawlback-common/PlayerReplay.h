#pragma once
#include "InputsReplay.h"
#include "PlayerPosition.h"
#include "BrawlbackPad.h"
#include "BrawlbackTypes.h"
#pragma pack(push, 4)
struct PlayerReplay
{
  bu32 actionState;
  bu8 stockCount;
  double damage;
  PlayerPosition pos;
  BrawlbackPad inputs;

  PlayerReplay() 
  {
    actionState = 0;
    stockCount = 0;
    damage = 0.0;
  }
};
#pragma pack(pop)