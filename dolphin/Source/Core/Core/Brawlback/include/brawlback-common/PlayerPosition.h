#pragma once
#pragma pack(push, 1)
struct PlayerPosition
{
  float xPos;
  float yPos;
  float zPos;
  PlayerPosition() 
  {
    xPos = 0.0;
    yPos = 0.0;
    zPos = 0.0;
  }
};
#pragma pack(pop)
