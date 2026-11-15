#pragma once

#include <sy_core.h>

// Any screen shape (LOCAL.aspect, v12; docs/game-code.md, Any screen shape): the game draws for the
// shape this machine shows the picture at, while the match is played with the same 16:9 cameras on
// every machine (local_aspect.cpp).
namespace LocalAspect {
    void install(CoreApi* api);
    // Once a frame, after it is drawn (every scene): the next frame's shape, read from LOCAL once
    // so that everything of a frame is drawn for the same one.
    void frame();
    // The screen's width over the 16:9 picture's on a wider screen, else 1: the plugin's 640-wide
    // 2D space is that picture, and the screen goes on past its side edges by half of the rest.
    float wider();
    // After a GXSetProjection of something drawn in the 16:9 picture's space: fits it to the screen.
    void fit();
    // After fit(), in a match: what is drawn next belongs at the screen's bottom edge (-1), its top
    // edge (1) or where it is (0). On a narrower screen the 16:9 picture's own edges are inside
    // the screen's, and the HUD's panels and timer are drawn at the screen's.
    void edge(int side);
}
