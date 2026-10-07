// Nintendo WFC / Wiimmfi bypass for Brawl's Wi-Fi menus.
//
// Ported from Brawlback-Online (Brawlback-Team/brawlback-asm, MIT), source/Rollback_Hooks.cpp,
// namespace NetMenu (branches master and savestate-efficiency). The DOL addresses are the
// same on NTSC-U Rev 1 and Rev 2 and are not touched by P+ v3.2's codeset
// (tools/gamecode/pplus_hooks.py). They patch the network thread's DWC login sequence so the
// game believes it is logged in and has a friend code, without any network traffic.
#include <sy_core.h>
#include "netmenu.h"
#include "online_menu.h"
#include "ppom.h"

extern "C" {
    // Set while our online flow owns the Wi-Fi CSS (read by removeDisconnectPanel).
    u8 g_onlineCss = 0;
}

namespace NetMenu {

    // 0x8014B5F8 `li r6,0` before the DWC login call: load "logged in" (3) into r4 ...
    __attribute__((naked)) void setToLoggedIn()
    {
        asm volatile(
            "li 4, 3\n\t"
            "lis 12, 0x8014\n\t"
            "ori 12, 12, 0xB5FC\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // ... and 0x8014B5FC (the login call itself): store it as the login state instead.
    __attribute__((naked)) void setToLoggedIn2()
    {
        asm volatile(
            "stw 4, -0x4048(13)\n\t"
            "lis 12, 0x8014\n\t"
            "ori 12, 12, 0xB600\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // 0x80033B48: skip the Mii renderer (no Mii data when offline).
    __attribute__((naked)) void disableMiiRender()
    {
        asm volatile(
            "lis 12, 0x8003\n\t"
            "ori 12, 12, 0x3b4c\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // 0x800CCF70: skip the matchmaking error check.
    __attribute__((naked)) void disableMatchmakingError()
    {
        asm volatile(
            "lis 12, 0x800c\n\t"
            "ori 12, 12, 0xcf94\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // 0x8014B4BC: don't wait for a friend code from the server.
    __attribute__((naked)) void forceFriendCode()
    {
        asm volatile(
            "lwz 0, 0x00F8 (29)\n\t"
            "lis 12, 0x8014\n\t"
            "ori 12, 12, 0xb4f0\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // 0x8014B3B8: treat the connection attempt as successful.
    __attribute__((naked)) void forceConnection()
    {
        asm volatile(
            "lis 12, 0x8014\n\t"
            "ori 12, 12, 0xB434\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // ---------------------------------------------------------------------------------------
    // Menu flow (sora_menu_main, module 2). P+ v3.2's sora_menu_main.rel is a patched vanilla
    // module with the same .text layout (tools/gamecode/reltool.py diff), so Gen 1's offsets hold.

    extern "C" void pp_startKeepScreen(void* keep);   // gfKeepFrameBuffer::startKeepScreen 0x80024E20 (EXTRA.lst)

    // sora_menu_main+0x2E4F8 is muProcWifiAnybody's vtable slot 3 (vtable .data+0x3AC8; class
    // name string "muProcWifiAnybody" at .data+0x3AF0), run when the WITH ANYONE page opens.
    // Gen 1's SkipDirectlyToCSS replaces it: leave muMenuMain with menu decision 0x1E (Wi-Fi
    // "anyone" basic), which sqMenuMain turns into sqNetAnyOkiraku -> the Wi-Fi character select.
    static bool s_friendsPicked = false;   // set by friendsToAnybody() below

    static void leaveMenuToWifiCss(int choice)
    {
        u8* mgr = *(u8**)0x805A0060;              // gfSceneManager::getInstance()
        *(int*)(mgr + 0x284) = 0x1E;              // menu decision / next sequence id
        *(int*)(mgr + 0x288) = 2;                 // processStep: leave the scene
        pp_startKeepScreen((void*)0x805b50a8);    // keep the last menu frame up during the load
        g_onlineCss = 1;
        onlineMenuEntered(choice);
    }

    void skipDirectlyToCSS()
    {
        leaveMenuToWifiCss(s_friendsPicked ? OnlineMenu::MENU_FRIENDS : OnlineMenu::MENU_ANYONE);
        s_friendsPicked = false;
    }

    // ---------------------------------------------------------------------------------------
    // Wi-Fi character select (sora_menu_sel_char, module 10) and stage select (module 11).
    // P+ v3.2 keeps both modules resident at fixed addresses (sel_char .text 0x806828C4,
    // sel_stage .text 0x806B0984, checked live with tools/gamecode/modules.py); the jumps back
    // below are absolute, as in Gen 1. OnlineMenu::tick() checks the bases and reports a
    // mismatch in PPOM debug.lastError.

    // sel_char+0x4220 `bl` creating the CSS countdown: skip it (and the store of its handle).
    __attribute__((naked)) void disableCreateCounterOnCSS()
    {
        asm volatile(
            "lis 3, 0x8068\n\t"
            "ori 3, 3, 0x6aec\n\t"   // sel_char+0x4228
            "mtctr 3\n\t"
            "bctr\n\t");
    }
    // sel_char+0x56A8 `lbz r0,0x61C(r30)` (timer running flag) -> 0.
    __attribute__((naked)) void turnOffCSSTimer()
    {
        asm volatile(
            "li 0, 0\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x7f70\n\t"  // sel_char+0x56AC
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // sel_char+0x53A4 `bl` network-error query -> "no error".
    __attribute__((naked)) void disableGetNetworkErrorOnCSS()
    {
        asm volatile(
            "li 3, 0\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x7C6C\n\t"  // sel_char+0x53A8
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // sel_char+0x4A70: the CSS sets its panel mode from r4; mode 6 is the "disconnected"
    // panel. While our online menu owns the CSS, show mode 4 instead. (Gen 1 used an inline
    // hook plus a simple hook and SaveRegs; this is one naked hook replaying the two
    // overwritten instructions.)
    __attribute__((naked)) void removeDisconnectPanel()
    {
        asm volatile(
            "cmpwi 4, 6\n\t"
            "bne 1f\n\t"
            "lis 12, g_onlineCss@ha\n\t"
            "lbz 12, g_onlineCss@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "beq 1f\n\t"
            "li 4, 4\n\t"
            "1:\n\t"
            "mr 29, 3\n\t"           // original sel_char+0x4A70
            "mr 30, 4\n\t"           // original sel_char+0x4A74
            "stw 4, 0x40(29)\n\t"    // redo the store done at +0x4A6C with the new mode
            "cmpwi 4, 6\n\t"         // redo the compare done at +0x4A68 (cr0 used at +0x4A7C)
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0x733C\n\t"  // sel_char+0x4A78
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    // sel_stage+0x141C `bl` creating the SSS countdown: skip to +0x1438 (Gen 1).
    __attribute__((naked)) void disableCreateCounterOnSSS()
    {
        asm volatile(
            "lis 3, 0x806b\n\t"
            "ori 3, 3, 0x1dbc\n\t"
            "mtctr 3\n\t"
            "bctr\n\t");
    }
    // sel_stage+0x30F0 `bl` network-error query -> "no error".
    __attribute__((naked)) void disableGetNetworkErrorOnSSS()
    {
        asm volatile(
            "li 3, 0\n\t"
            "lis 12, 0x806b\n\t"
            "ori 12, 12, 0x3a78\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // sora_menu_main+0x2DDB8 is muProcWifiFriend's vtable slot 3 (vtable .data+0x3A48, name
    // "muProcWifiFriend" at .data+0x3A70): WITH FRIENDS = Slippi's DIRECT. Same exit as above;
    // the plugin keeps the mode, the CSS is the same Wi-Fi CSS.
    // WITH FRIENDS = DIRECT. Opening Brawl's friend-roster page froze the display (presents
    // stop as soon as the page opens with the WFC bypass), and we don't need it: muProcWifi's
    // A handler stores the next page id in this+0x634 with `li r0,0x1A` (friends) or 0x1B
    // (anybody) at sora_menu_main+0x166B8 / +0x16518 / +0x168B8 (the A press on the page uses
    // +0x168B8). An inline hook there turns 0x1A into
    // 0x1B, so both buttons open the Anybody page (whose enter hook above leaves for the CSS),
    // and remembers that FRIENDS was picked.

    // Syriinge 0.6 inline hooks run the original instruction, open a 0x90-byte frame, save r0
    // at 0x8 of it, call us, and restore r0 from there: rewrite that slot. The volatile local
    // forces our own stack frame so its back chain is the stub's frame.
    __attribute__((noinline)) void friendsToAnybody()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        u32 stubFrame = *(u32*)__builtin_frame_address(0);
        *(u32*)(stubFrame + 8) = 0x1B;
        s_friendsPicked = true;
        PPOM::g_block.debug.scratch[6]++;
        PPOM::g_block.debug.scratch[7] = stubFrame;
        (void)keepFrame[0];
    }

    void pollPendingExit() {}

    void install(CoreApi* api)
    {
        api->sySimpleHook(0x8014B5F8, reinterpret_cast<void*>(setToLoggedIn));
        api->sySimpleHook(0x8014B5FC, reinterpret_cast<void*>(setToLoggedIn2));
        api->sySimpleHook(0x80033b48, reinterpret_cast<void*>(disableMiiRender));
        api->sySimpleHook(0x800CCF70, reinterpret_cast<void*>(disableMatchmakingError));
        api->sySimpleHook(0x8014b4bc, reinterpret_cast<void*>(forceFriendCode));
        api->sySimpleHook(0x8014b3b8, reinterpret_cast<void*>(forceConnection));

        api->sySimpleHookRel(0x0002E4F8, reinterpret_cast<void*>(skipDirectlyToCSS), 2 /* SORA_MENU_MAIN */);
        api->syInlineHookRel(0x000166B8, reinterpret_cast<void*>(friendsToAnybody), 2 /* SORA_MENU_MAIN */);
        api->syInlineHookRel(0x00016518, reinterpret_cast<void*>(friendsToAnybody), 2 /* SORA_MENU_MAIN */);
        api->syInlineHookRel(0x000168B8, reinterpret_cast<void*>(friendsToAnybody), 2 /* SORA_MENU_MAIN */);

        api->sySimpleHookRel(0x00004220, reinterpret_cast<void*>(disableCreateCounterOnCSS), 10);
        api->sySimpleHookRel(0x000056A8, reinterpret_cast<void*>(turnOffCSSTimer), 10);
        api->sySimpleHookRel(0x000053A4, reinterpret_cast<void*>(disableGetNetworkErrorOnCSS), 10);
        api->sySimpleHookRel(0x00004A70, reinterpret_cast<void*>(removeDisconnectPanel), 10);
        api->sySimpleHookRel(0x0000141C, reinterpret_cast<void*>(disableCreateCounterOnSSS), 11);
        api->sySimpleHookRel(0x000030F0, reinterpret_cast<void*>(disableGetNetworkErrorOnSSS), 11);
        // Not ported: Gen 1's turnOffSSSTimer is installed on sel_char+0x35A4 (a `blr`) but jumps
        // into sel_stage (+0x35A8); it was meant for sel_stage+0x35A4. Not needed while the
        // online flow waits on the CSS.
    }
}
