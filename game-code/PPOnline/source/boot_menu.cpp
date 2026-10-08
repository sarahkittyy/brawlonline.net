// Boot to the ONLINE page (WITH FRIENDS / WITH ANYONE) instead of P+'s Versus character select.
//
// P+ v3.2's codeset boots straight to its VERSUS CSS: "Boot Directly to CSS v5.4"
// (/Project+/Source/Project+/BootToCSS.asm on the SD card) hooks sqBoot::setNext at 0x806DD5F8
// and, unless a pad holds L/R (Training), Start (the title) or Z (Replays), calls
// setNextSequence("sqVsMelee", 0) in place of the game's setNextSequence("sqPrizeCheck", 0x14).
// The hook's branch is rewritten by the code handler every frame; the code it reaches is the
// expansion codeset's own words (NETBOOST.GCT for the Netplay Launcher, BOOST.GCT for the
// Offline Launcher, read to 0x80550010), which nothing writes after the load.
//
// Going to the main menu straight from sqBoot runs out of the OverlayMenu heap (Orca), so the
// boot takes the game's own way to the menus, the one P+'s Start case takes: sqPrizeCheck with
// 0x14 (P+'s Netplay.asm skips the prize screens) -> sqTitle -> sqMenuMain. On that way, and only
// on it:
//   - the title's opening movie and "press Start" logo both become the title's exit (state 16,
//     result 0), so the title shows nothing;
//   - sqTitle's setNextSequence("sqMenuMain", ...) gets 30, the menu's argument for the ONLINE
//     page with WITH FRIENDS highlighted (sqMenuMain turns 30/31 into the menu modes 34/35, the
//     ONLINE page on WITH FRIENDS / WITH ANYONE; the online CSS's way back uses 0x1F).
// B on the ONLINE page goes to the main menu's top page as before (netmenu.cpp), and B there to
// the title as in P+. The title later in the session is the game's own.
//
// Both rewrites are made in the setNextSequence call itself (DOL 0x8002D640, replaced), never in
// the codeset's memory, and only while P+'s default case is exactly the words of BootToCSS v5.4
// (the guard Orca's PPLUS32.patches uses, at 0x80559C20 for NETBOOST.GCT and 0x80559C30 for
// BOOST.GCT). Another codeset (a P+ update, a player's own boot code) keeps its own boot.
//
// The approach and the addresses are Orca's (GPL-3.0-or-later; ORCA.md "Boot and friends from
// the menus", Data/Sys/Orca/PPLUS32.patches: the BootToCSS default case to sqPrizeCheck 0x14, the
// scTitle skip at 0x806CA1F8/0x806CA204), used with attribution. Orca patches memory from
// Dolphin; this is our own plugin code, no Orca code is copied.
#include <string.h>
#include "online_menu.h"
#include "ppom.h"

namespace BootMenu {

    typedef void (*SetNextSequenceFn)(void* mgr, const char* name, int arg);
    static SetNextSequenceFn s_origSetNextSequence = NULL;

    // Set when the boot was sent to the menus, until sqMenuMain is asked for (or the boot goes
    // anywhere else).
    static bool s_toOnline = false;

    // sqMenuMain's argument for the ONLINE page with WITH FRIENDS highlighted (menu mode 34).
    static const int MENU_ONLINE_PAGE = 30;

    // DEBUG scratch[14] (tests): bit 0 the boot redirected, bit 1 the opening movie skipped,
    // bit 2 the logo skipped, bit 3 the menu sent to the ONLINE page; bits 16-23 the argument
    // sqTitle asked sqMenuMain for.
    static void note(u32 bits) { PPOM::g_block.debug.scratch[14] |= bits; }

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // gfSceneManager (0x805A0060) +0x10: the current sequence; its first word is its name.
    static const char* currentSequence()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return "";
        u32 seq = *(u32*)(mgr + 0x10);
        if (!isPtr(seq)) return "";
        u32 name = *(u32*)seq;
        return isPtr(name) ? (const char*)name : "";
    }

    // BootToCSS v5.4's last port check and its default case, as assembled into the codeset:
    //   blt LOOP_START; addi r4,r21,0x1B54 ("sqVsMelee"); li r5,0; b %END%
    // followed by the Start case: addi r4,r21,0x1C94 ("sqPrizeCheck"); li r5,0x14.
    static bool bootToCssDefaultAt(u32 addr)
    {
        const u32* w = (const u32*)addr;
        return w[0] == 0x41A0FF84 && w[1] == 0x38951B54 && w[2] == 0x38A00000 && w[3] == 0x48000038
            && w[4] == 0x38951C94 && w[5] == 0x38A00014;
    }

    static bool pplusBootsToCss()
    {
        return bootToCssDefaultAt(0x80559C20)    // NETBOOST.GCT (Netplay Launcher)
            || bootToCssDefaultAt(0x80559C30);   // BOOST.GCT (Offline Launcher)
    }

    static void hkSetNextSequence(void* mgr, const char* name, int arg)
    {
        if (s_toOnline) {
            if (strcmp(name, "sqMenuMain") == 0) {
                PPOM::g_block.debug.scratch[14] |= ((u32)arg & 0xFF) << 16;
                arg = MENU_ONLINE_PAGE;
                s_toOnline = false;
                note(8);
            } else if (strcmp(name, "sqTitle") != 0) {
                s_toOnline = false;   // not the way to the menus: leave it alone
            }
        } else if (arg == 0 && strcmp(name, "sqVsMelee") == 0 && strcmp(currentSequence(), "sqBoot") == 0
                   && pplusBootsToCss()) {
            // The game's own string, which the Start case passes (0x80701C94).
            const char* prize = (const char*)0x80701C94;
            if (strcmp(prize, "sqPrizeCheck") == 0) {
                name = prize;
                arg = 0x14;
                s_toOnline = true;
                note(1);
            }
        }
        s_origSetNextSequence(mgr, name, arg);
    }

    // scTitle's process (sora_scene, resident at .text 0x806BB554; state at +0x370): once the
    // title is built it sets state 3 (the opening movie, sora_scene+0xECA4 `li r0,3`) or 12 (the
    // "press Start" logo, +0xECB0 `li r0,12`). On the boot's way to the menus both become 16, the
    // title's clean-up and exit with result 0, which sqTitle takes to sqMenuMain. Inline hooks
    // run after the original instruction; Syriinge's stub saved r0 at 8 of its frame and restores
    // it from there (the volatile local gives us our own frame, whose back chain is the stub's).
    __attribute__((noinline)) void titleMovie()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        if (s_toOnline) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            *(u32*)(stubFrame + 8) = 16;
            note(2);
        }
        (void)keepFrame[0];
    }

    __attribute__((noinline)) void titleLogo()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        if (s_toOnline) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            *(u32*)(stubFrame + 8) = 16;
            note(4);
        }
        (void)keepFrame[0];
    }

    void install(CoreApi* api)
    {
        api->syReplaceFunc(0x8002D640, reinterpret_cast<void*>(hkSetNextSequence),
                           (void**)&s_origSetNextSequence);
        api->syInlineHookRel(0x0000ECA4, reinterpret_cast<void*>(titleMovie), 1 /* SORA_SCENE */);
        api->syInlineHookRel(0x0000ECB0, reinterpret_cast<void*>(titleLogo), 1 /* SORA_SCENE */);
    }
}
