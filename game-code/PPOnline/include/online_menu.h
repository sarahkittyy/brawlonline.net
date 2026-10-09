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
    bool modesLocked();                 // logged out or update required: no online mode
    void restoreCss();                  // before the CSS starts again: the remembered coin
    bool selectableCharKind(int kind);  // a gmCharacterKind P+'s CSS can lock in
    const char* cssLine(MuMsg* msg, u32 window, u32 line, const void* msbin, u32 caller);
}

namespace OnlineMatch {
    // Online matches from the online CSS (online_match.cpp): sqNetAnyOkiraku hooks, the match
    // setup from SESSION, the in-match disconnect.
    void install(CoreApi* api);
    void tickMatch();                 // every frame in scMelee
    void onFrameDrawn();              // every drawn frame (MatchHud's hook)
    bool disconnectShown();
    u16 pickedStage();                // the loser's stage pick (0xFFFF none)
    u8 pickedAsl();
    void clearPickedStage();
}

namespace MatchHud {
    // "DISCONNECTED" in the match HUD (match_hud.cpp): drawn while on and the scene is scMelee.
    void install(CoreApi* api);
    void show(bool on);
    bool shown();
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
    void forgetLabels();                          // off the CSS: its '#' label window is gone
}

namespace BootMenu {
    // The boot lands on the ONLINE page instead of P+'s Versus CSS (boot_menu.cpp).
    void install(CoreApi* api);
}

namespace AnyoneMenu {
    // WITH ANYONE -> Unranked / Ranked on Brawl's Wi-Fi OPTIONS page (anyone_menu.cpp).
    void install(CoreApi* api);
    void tick();
    const char* line(MuMsg* msg, u32 window, u32 line, const void* msbin);
}
