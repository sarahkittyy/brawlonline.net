#pragma once
#include "GameSettings.h"
#include "PlayerPosition.h"
#include "StartReplayPlayer.h"
#include "BrawlbackTypes.h"
const size_t MAX_REPLAY_NAME_SIZE = 17;

#pragma pack(push, 4)
struct StartReplay
{
  bu32 randomSeed;
  bu32 otherRandomSeed;
  bu32 firstFrame;
  bu8 nameSize;
  bu8 nameBuffer[MAX_REPLAY_NAME_SIZE];
  bu8 numPlayers;
  StartReplayPlayer players[MAX_NUM_PLAYERS];
  bu8 stage;
  char gameData[0x320];

  StartReplay() 
  {
    randomSeed = 0;
    otherRandomSeed = 0;
    firstFrame = 0;
    nameSize = 0;
    numPlayers = 0;
    stage = 0;
  }
};
#pragma pack(pop)
