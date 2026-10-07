#pragma once
#include <sy_core.h>
#include <types.h>
#include "ppom.h"

class MuMsg;

namespace OnlineMenu {
    void install(CoreApi* api);
    void tick();
    void enter(int mode);               // PPOM::Mode, picked on the ONLINE page
    void applyRules();                  // sqNetAnyOkiraku start: online rules into the set rule
    void codeEntered(const char* code);  // from CodeEntry: set the code and start the search
    int currentMode();                  // PPOM::Mode of the online CSS, -1 outside it
    const char* cssLine(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller);
}

namespace NetMenu {
    void onlineMenuEntered(int mode);
}

namespace CodeEntry {
    // Open the connect-code entry (Direct, Teams). See code_entry.cpp.
    void install(CoreApi* api);
    void open(int port);
    void tick(bool startPressed);
    bool active();
    void onSuggestion(const PPOM::Response& r);   // a FETCH_CODE_SUGGESTION answer
}
