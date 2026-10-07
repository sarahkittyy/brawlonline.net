#pragma once
#include <sy_core.h>
#include <types.h>

class MuMsg;

namespace OnlineMenu {
    void install(CoreApi* api);
    void tick();
    void enter(int mode);               // PPOM::Mode
    void setDirectCode(const char* code);
    void codeEntered(const char* code);  // from CodeEntry: set the code and start the search
    const char* cssLine(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller);
    const char* accountLine();

    enum MenuChoice { MENU_NONE = 0, MENU_FRIENDS = 1, MENU_ANYONE = 2 };
}

namespace NetMenu {
    void onlineMenuEntered(int choice);
}

namespace CodeEntry {
    // Open the connect-code entry (Direct). See code_entry.cpp.
    void open();
    void tick();
    bool active();
}
