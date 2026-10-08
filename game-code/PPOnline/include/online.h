#pragma once
#include <sy_core.h>
#include <types.h>

class MuMsg;

namespace Online {
    void install(CoreApi* api);

    // Scene helpers (gfSceneManager chain, docs/brawl-memory-map.md section 1).
    const char* currentSceneName();
    bool inScene(const char* name);

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
