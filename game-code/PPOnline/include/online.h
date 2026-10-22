#pragma once
#include <sy_core.h>
#include <types.h>

class MuMsg;

namespace Online {
    void install(CoreApi* api);

    // Scene helpers (gfSceneManager chain, docs/brawl-memory-map.md section 1).
    const char* currentSceneName();
    bool inScene(const char* name);

    // Rumble (online.cpp, PADControlMotor and WPADControlMotor): mapped, only pad `from`'s
    // rumble (the local player's port's pad in the match: GameCube 0-3, Wii Remote 4-7) reaches a
    // controller, pad `to` (theirs, 0-7); `from` 0xFF: none. Not mapped: every pad's own.
    void setRumblePorts(bool mapped, u8 from, u8 to);

    // Message system.
    const char* msbinLine(const void* msbin, int line, int* outLen);
    bool msbinLineContains(const void* msbin, int line, const char* needle);
}

namespace Text {
    // Replacement for a MuMsg::printIndex call, or NULL to print the original.
    const char* overrideFor(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller);
    // Print `text` into a message window keeping the original line's style tags.
    void printStyled(MuMsg* msg, u32 window, const void* msbin, u32 line, const char* text);
}
