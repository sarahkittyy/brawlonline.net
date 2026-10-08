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
    // sqMenuMain entry used when the online CSS is left, 0 = the game's own (exitToOnlinePage).
    u8 g_onlineReturnPage = 0;
    // Set while Direct owns the CSS: the ONLINE page then opens on WITH FRIENDS (onlinePageCursor).
    u8 g_onlineFriendsCursor = 0;
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

    // sora_menu_main+0x2E4F8 is muProcWifiAnybody's vtable slot 3 (vtable .data+0x3AC8; class
    // name string "muProcWifiAnybody" at .data+0x3AF0), run when its page opens. The page is
    // WITH FRIENDS' (design 5.4 screen 1, the user's mode set): Slippi's code-based modes, BASIC
    // VERSUS = Direct (1v1) and TEAM BATTLE = Teams (2v2), both entered with a connect code.
    // WITH FRIENDS is rerouted to it (friendsToAnybody below); WITH ANYONE opens the Unranked /
    // Ranked page instead (anyone_menu.cpp). The page's own title (WITH ANYONE) becomes
    // WITH FRIENDS (friendsTitle), and B from it goes back to the ONLINE page with WITH FRIENDS
    // highlighted (onlinePageCursor).
    static bool s_friendsPicked = false;   // set by friendsToAnybody() below
    typedef int (*ProcFn)(void*, void*, void*);  // (this, menu, resources)
    static ProcFn s_origAnybodyEnter = NULL;

    static int anybodyEnter(void* proc, void* a, void* b)
    {
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) {
            s_friendsPicked = false;
            g_onlineFriendsCursor = 1;
        }
        return s_origAnybodyEnter(proc, a, b);
    }

    // The page's title: text+0x2E760 `li r4,0x12` (WITH ANYONE), passed with the description to
    // the menu panel (this+0x658, vtable +0x74). Title ids (sora_menu_main's title model): 9 ONLINE,
    // 0x11 WITH FRIENDS, 0x12 WITH ANYONE, 0x1F OPTIONS. Inline hook (Syriinge saves r3-r31 with
    // stmw r3,0xC(r1) in its stub frame): the saved r4 becomes 0x11.
    __attribute__((noinline)) void friendsTitle()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            *(u32*)(stubFrame + 0x10) = 0x11;
        }
        (void)keepFrame[0];
    }

    // sora_menu_main+0x2E934 / +0x2EB70 `mr r3,r30` in muProcWifiAnybody's two A handlers,
    // just after the `beq` that skips "no button" (an inline hook does not keep CR): r4 is the
    // menu decision for the highlighted button: 0x1E BASIC VERSUS, 0x1F TEAM BATTLE (0x1D is
    // a third cursor slot this page does not show). The game then leaves muMenuMain with it
    // (sora_menu_rule text+0x1AE0) and sqMenuMain starts sqNetAnyOkiraku or
    // sqNetAnyTeamMelee. We only note which mode was picked: Direct or Teams.
    void anybodyDecision(u32 r3, u32 decision)
    {
        (void)r3;
        PPOM::g_block.debug.scratch[8] = decision;
        if (!(PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS)) return;
        if (decision == 0x1E) onlineMenuEntered(PPOM::MODE_DIRECT);
        else if (decision == 0x1F) onlineMenuEntered(PPOM::MODE_TEAMS);
    }

    // Brawl's "Connect to Nintendo WFC?" window (WifiCnctWnd task, sora_menu_main text+0x385C0),
    // with its "Connected." and first-time "Choose a profile name." steps. Slippi has no such
    // screens: PLAY ONLINE goes straight to the mode list. The main menu creates the window at
    // four places and stores it in this+0x66C; each frame it polls the window: state
    // (wnd+0x138) 0xC = finished, result (wnd+0x128) 0 = connected, which changes to the ONLINE
    // page (this+0x634 = 3) and deletes the window (text+0x14FD8). Marking the new window as
    // finished and connected skips all of it; nothing of the window is shown or run. The WFC
    // login itself is already faked by the hooks above, so no online state is missing.
    void skipConnectWindow(u8* wnd)
    {
        if (!wnd || !(PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS)) return;
        *(u32*)(wnd + 0x128) = 0;
        *(u32*)(wnd + 0x138) = 0xC;
        PPOM::g_block.debug.scratch[9]++;
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

    // ---------------------------------------------------------------------------------------
    // sqNetAnyOkiraku (sora_scene, module 1, resident: .text 0x806BB554). Its scene-decide
    // function (text+0x36D74) is a state machine on this+8 (jump table 0x80703B3C).

    // sora_scene is resident (.text 0x806BB554), but it is loaded after the plugins, so its
    // hooks must be module hooks (an absolute patch is overwritten by the module load). The
    // jumps back are absolute, as for sel_char.

    // State 0 (text+0x36E50) writes Brawl's Wi-Fi rules into g_GameGlobal->m_setRule (time,
    // 2 minutes, no pause, ...). text+0x36EA0 `stw r0,0xC(r15)` is right after: put the online
    // rules there instead (OnlineMenu::applyRules, design 4.6 / P+ competitive defaults).
    // sqNetAnyTeamMelee's state 0 does the same at text+0x3B7E0; its text+0x3B81C
    // `stw r26,8(r15)` is right after. Both are followed only by stores of non-volatile
    // registers, so calling C here is safe; the function saved LR in its prologue.
    extern "C" void pponline_afterWifiRules()
    {
        if (g_onlineCss) OnlineMenu::applyRules();
    }
    __attribute__((naked)) void afterWifiRulesAny()
    {
        asm volatile(
            "stw 0, 0xC(15)\n\t"
            "lis 12, pponline_afterWifiRules@ha\n\t"
            "addi 12, 12, pponline_afterWifiRules@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x806F\n\t"
            "ori 12, 12, 0x23F8\n\t"   // text+0x36EA4
            "mtctr 12\n\t"
            "bctr\n\t");
    }
    __attribute__((naked)) void afterWifiRulesTeam()
    {
        asm volatile(
            "stw 26, 8(15)\n\t"
            "lis 12, pponline_afterWifiRules@ha\n\t"
            "addi 12, 12, pponline_afterWifiRules@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x806F\n\t"
            "ori 12, 12, 0x6D74\n\t"   // text+0x3B820
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // State 0x10 (text+0x376F0) leaves the CSS for the menus: setNextSequence("sqMenuMain",
    // 0x1C). 0x1C reopens the WITH ANYONE page with BASIC VERSUS (Unranked) highlighted, which
    // is Slippi's "back to the online menu on the current mode" (OnMenuPrep.asm:134-162).
    // Direct came from WITH FRIENDS: 0x1F opens the ONLINE page instead, with the cursor on the
    // button last used. (sqNetAnyTeamMelee returns with 0x1D, TEAM BATTLE highlighted.)
    __attribute__((naked)) void exitToOnlinePage()
    {
        asm volatile(
            "li 5, 0x1c\n\t"
            "lis 12, g_onlineReturnPage@ha\n\t"
            "lbz 12, g_onlineReturnPage@l(12)\n\t"
            "cmpwi 12, 0\n\t"
            "beq 1f\n\t"
            "mr 5, 12\n\t"
            "1:\n\t"
            "lis 12, 0x806F\n\t"
            "ori 12, 12, 0x2C64\n\t"   // sora_scene text+0x37710
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

    // muProcWifi (the ONLINE page) enter, sora_menu_main text+0x1616C, picks the cursor from
    // where the menu came from (text+0x16318..+0x16408, this+0x42). Coming back from the CSS
    // (sqMenuMain entry 0x1F) it lands on WITH ANYONE; after Direct it must be WITH FRIENDS.
    // Inline hook at text+0x1640C `mr r3,r30` (r3 = this): set the cursor and the r31 the
    // function passes on to the highlight call (mod18 text+0x1D48), in the stub's saved
    // registers (stmw r3,0xC(r1): r31 at +0x7C).
    __attribute__((noinline)) void onlinePageCursor(u8* page)
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        if (!g_onlineFriendsCursor) return;
        g_onlineFriendsCursor = 0;
        u32 stubFrame = *(u32*)__builtin_frame_address(0);
        *(u16*)(page + 0x42) = 0;
        *(u32*)(stubFrame + 0x7C) = 0;
        (void)keepFrame[0];
    }

    // muProcWifi enter, back from muProcWifiAnybody (page 0x1B, now WITH FRIENDS' page):
    // text+0x16388 sets the cursor to 1 (WITH ANYONE) and highlights it. Inline hook on
    // text+0x1639C `sth r5,0x42(r30)` (before the highlight call at +0x163A4, which reads the
    // cursor): the cursor goes back to WITH FRIENDS, the button the page was opened from.
    __attribute__((noinline)) void backFromFriendsPage()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            u8* page = *(u8**)(stubFrame + 0x78);   // saved r30 (stmw r3,0xC(r1))
            if ((u32)page >= 0x80000000 && (u32)page < 0x81800000) *(u16*)(page + 0x42) = 0;
            g_onlineFriendsCursor = 0;
        }
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

        api->syReplaceFuncRel(0x0002E4F8, reinterpret_cast<void*>(anybodyEnter), (void**)&s_origAnybodyEnter, 2 /* SORA_MENU_MAIN */);
        api->syInlineHookRel(0x0002E934, reinterpret_cast<void*>(anybodyDecision), 2);
        api->syInlineHookRel(0x0002EB70, reinterpret_cast<void*>(anybodyDecision), 2);
        api->syInlineHookRel(0x0002E760, reinterpret_cast<void*>(friendsTitle), 2);
        // the four `stw r3,0x66C(r29)` after WifiCnctWnd's create (text+0x38530)
        api->syInlineHookRel(0x00015288, reinterpret_cast<void*>(skipConnectWindow), 2);
        api->syInlineHookRel(0x00015460, reinterpret_cast<void*>(skipConnectWindow), 2);
        api->syInlineHookRel(0x0001674C, reinterpret_cast<void*>(skipConnectWindow), 2);
        api->syInlineHookRel(0x0001694C, reinterpret_cast<void*>(skipConnectWindow), 2);
        api->sySimpleHookRel(0x00036EA0, reinterpret_cast<void*>(afterWifiRulesAny), 1 /* SORA_SCENE */);
        api->sySimpleHookRel(0x0003B81C, reinterpret_cast<void*>(afterWifiRulesTeam), 1);
        api->sySimpleHookRel(0x0003770C, reinterpret_cast<void*>(exitToOnlinePage), 1);
        api->syInlineHookRel(0x0001640C, reinterpret_cast<void*>(onlinePageCursor), 2 /* SORA_MENU_MAIN */);
        api->syInlineHookRel(0x0001639C, reinterpret_cast<void*>(backFromFriendsPage), 2 /* SORA_MENU_MAIN */);
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
