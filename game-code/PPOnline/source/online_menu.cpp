// Online menu flow (design 5.4). Phase 0: hello world relabel of the main-menu
// "PLAY ONLINE" description.
#include <mu/mu_msg.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"

namespace OnlineMenu {
    void install(CoreApi* api) { (void)api; }
    void tick() {}
}

namespace Text {
    struct Relabel {
        const char* scene;   // only in this scene
        const char* match;   // substring of the original message line
        const char* text;    // replacement
    };

    static const Relabel RELABELS[] = {
        {"muMenuMain", "Play different modes online", "Hello from PPOnline (game message system)"},
    };

    const char* overrideFor(MuMsg* msg, u32 window, u32 line, const void* msbin)
    {
        (void)msg; (void)window;
        const char* scene = Online::currentSceneName();
        for (u32 i = 0; i < sizeof(RELABELS) / sizeof(RELABELS[0]); i++) {
            const Relabel& r = RELABELS[i];
            if (strcmp(scene, r.scene) != 0) continue;
            if (Online::msbinLineContains(msbin, (int)line, r.match)) return r.text;
        }
        return NULL;
    }
}
