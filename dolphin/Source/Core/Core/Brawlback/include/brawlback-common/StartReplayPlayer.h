#pragma once
#include "PlayerPosition.h"
#include "BrawlbackTypes.h"
#pragma pack(push, 2)
struct StartReplayPlayer
{
  // Fighter kind = Fighter ID; ID that points to attributes about the Fighter
  // TODO: Consider refactoring to not requiring the fighter ID
  bu8 fighterKind;
  // Actual ID that "points" to the Fighter object
  bu8 slotID;
  PlayerPosition startPlayer;

  StartReplayPlayer() 
  {
    fighterKind = 0;
    slotID = 0;
  }
};
#pragma pack(pop)
