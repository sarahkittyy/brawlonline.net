#pragma once
#include "ItemIdName.h"
#include "BrawlbackTypes.h"
#pragma pack(push, 2)
struct ItemReplay
{
  ItemIdName itemId;
  bu16 itemVariant;
  ItemReplay()
  {
    itemVariant = 0;
  }
};
#pragma pack(pop)
