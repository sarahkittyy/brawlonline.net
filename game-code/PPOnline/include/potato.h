#pragma once

// Potato mode (LOCAL.potato, v11; the launcher's setting): matches draw less, so a PC that can't hold
// 60 fps in a match gets closer. Only what this machine draws changes (potato.cpp).
namespace Potato {
    void tick();   // every game frame (online.cpp's pad tick)
}
