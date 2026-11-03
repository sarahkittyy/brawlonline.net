// WITH ANYONE -> Unranked / Ranked / 2V2 (docs/design/2v2.md, 2026-11-02: "a third entry on that
// menu ... UNRANKED, RANKED, 2V2").
//
// The page is Brawl's own WITH ANYONE page (muProcWifiAnybody, page 0x1B), the one WITH FRIENDS
// also opens (netmenu.cpp): three buttons, the first small at the top left, the second large at
// the right, the third at the bottom, their labels drawn with the game's font. WITH FRIENDS shows
// CREATE ROOM / DIRECT 1V1 / JOIN ROOM on it; opened from WITH ANYONE (g_anyonePage) it shows
// UNRANKED / RANKED / 2V2 (netmenu.cpp drawButtonLabels, anybodyDecision), keeps its own title
// (WITH ANYONE), and its descriptions are Slippi's ("Play unranked matches." ...). Until
// 2026-11-02 WITH ANYONE was Brawl's Wi-Fi OPTIONS page (muProcOptWifi), which has room for two.
//
//   ONLINE page, WITH ANYONE (A)  -> muProcWifiAnybody, as the game opens it (g_anyonePage set)
//   B                             -> the ONLINE page, WITH ANYONE highlighted
//   A                             -> menu decision 0x1E (sqNetAnyOkiraku, the Wi-Fi CSS) in
//                                    Unranked, Ranked or the 2V2 CSS (netmenu.cpp)
#include <sy_core.h>
#include <mu/mu_msg.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"

extern "C" {
    // 1 from WITH ANYONE until the ONLINE page or the CSS takes over.
    u8 g_anyonePage = 0;
}

namespace AnyoneMenu {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // .text address of a loaded REL (OSModuleInfo list at 0x800030C8), 0 when not loaded.
    static u32 moduleText(u32 id)
    {
        u32 m = *(u32*)0x800030C8;
        for (int n = 0; isPtr(m) && n < 64; n++) {
            if (*(u32*)m == id) {
                u32 sec = *(u32*)(m + 0x10);
                if (!isPtr(sec)) return 0;
                return *(u32*)(sec + 8) & ~1u;   // section 1 = .text
            }
            m = *(u32*)(m + 4);
        }
        return 0;
    }

    static const u32 MOD_MENU_MAIN = 2;
    static const u32 ANYBODY_ENTER = 0x2E4F8;       // muProcWifiAnybody vtable slot 3
    static const u32 ANYBODY_ENTER_END = 0x2E790;

    // ------------------------------------------------------------------------------------
    // 0x8014FEDC `bool wc24Available(u32 mask)` (flags of the WiiConnect24 status block). The
    // WITH ANYONE page (muProcWifiAnybody) has three buttons: its enter shows the first (decision
    // 0x1D, Brawl's spectator button) only when 0x10 is available (text+0x2E534), and lays the
    // other two out for three then. Answer 1 for that call.
    typedef u32 (*FlagFn)(u32);
    static FlagFn s_origFlag = NULL;
    static u32 hkFlag(u32 mask)
    {
        if (mask == 0x10 && (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS)) {
            u32 lr = (u32)__builtin_return_address(0);
            u32 t = moduleText(MOD_MENU_MAIN);
            if (t && lr >= t + ANYBODY_ENTER && lr < t + ANYBODY_ENTER_END) return 1;
        }
        return s_origFlag(mask);
    }

    // ------------------------------------------------------------------------------------
    // muProcWifi (the ONLINE page) A handler: `li r0,0x1B` (next page = muProcWifiAnybody) at
    // sora_menu_main text+0x16524 / +0x166C4 / +0x168C4: the page opens as the game opens it, as
    // WITH ANYONE's (netmenu.cpp friendsToAnybody clears the flag for WITH FRIENDS).
    __attribute__((noinline)) void anyoneToAnybody()
    {
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) g_anyonePage = 1;
    }

    // ------------------------------------------------------------------------------------
    // The page's descriptions (main menu lines 0x32 + cursor: Brawl's "Watch someone else fight",
    // "...quick fight with someone", "Form a team with someone"), Slippi's for its modes.
    const char* line(MuMsg* msg, u32 window, u32 lineIdx, const void* msbin)
    {
        (void)msg;
        (void)window;
        if (!g_anyonePage) return NULL;
        if (Online::msbinLineContains(msbin, (int)lineIdx, "Watch someone else fight")) return "Play unranked matches.";
        if (Online::msbinLineContains(msbin, (int)lineIdx, "quick fight with someone")) return "Play ranked matches.";
        if (Online::msbinLineContains(msbin, (int)lineIdx, "Form a team with someone")) return "Play 2v2 with random teams.";
        return NULL;
    }

    void tick()
    {
        if (g_anyonePage && !Online::inScene("muMenuMain")) g_anyonePage = 0;
    }

    void install(CoreApi* api)
    {
        api->syReplaceFunc(0x8014FEDC, reinterpret_cast<void*>(hkFlag), (void**)&s_origFlag);
        api->syInlineHookRel(0x00016524, reinterpret_cast<void*>(anyoneToAnybody), MOD_MENU_MAIN);
        api->syInlineHookRel(0x000166C4, reinterpret_cast<void*>(anyoneToAnybody), MOD_MENU_MAIN);
        api->syInlineHookRel(0x000168C4, reinterpret_cast<void*>(anyoneToAnybody), MOD_MENU_MAIN);
    }
}
