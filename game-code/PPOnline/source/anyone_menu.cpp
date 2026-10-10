// WITH ANYONE -> Unranked / Ranked, on Brawl's own Wi-Fi OPTIONS page (muProcOptWifi).
//
// Brawl's button labels are images and none says "Ranked". The one menu page of muMenuMain whose
// buttons carry font-rendered labels is the Wi-Fi OPTIONS page (sora_menu_main muProcOptWifi,
// vtable .data+0x3C20, page id 0x1C): two bar buttons, "Allow Spectators" and "Smash Service",
// whose labels and values are MuMsg windows 0-3 printed from its msbin (lines 0/1 labels,
// 2/3 Yes/No, 4/5 Accept/Decline). The ONLINE page shows the page's entry button (OPTIONS, the
// third one) and the page shows its two buttons only when WiiConnect24 reports Spectator (0x20)
// or the Smash Service (0x40) as available (0x8014FEDC), which they never are offline; so the
// page is unused on P+ (whose sora_menu_main differs from vanilla nowhere in these functions).
//
//   ONLINE page, WITH ANYONE (A)  -> page 0x1C instead of 0x1B (muProcWifiAnybody)
//   page enter                    -> both buttons shown (0x8014FEDC answers 1 for its two calls)
//   labels                        -> "Unranked" / "Ranked" (printIndex hook, Text::overrideFor)
//   value windows (Yes/No ...)    -> printed empty, their pill (bone button0) hidden
//   descriptions                  -> Slippi's "Play unranked matches." / "Play ranked matches."
//   up/down, highlight, B         -> the page's own (B goes back to the ONLINE page)
//   A                             -> menu decision 0x1E (sqNetAnyOkiraku, the Wi-Fi CSS), mode
//                                    Unranked or Ranked, through muProcMenu's own decide
//                                    (sora_menu_rule text+0x1AE0), as muProcWifiAnybody does
#include <sy_core.h>
#include <mu/mu_msg.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"

extern "C" {
    extern u8 g_onlineReturnPage;
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
    static const u32 MOD_MENU_RULE = 18;            // sora_menu_rule: the muProcMenu base class
    static const u32 OPTWIFI_ENTER = 0x2EFA4;       // muProcOptWifi vtable slot 3
    static const u32 OPTWIFI_ENTER_END = 0x2F3B8;   // = its update (slot 5)
    static const u32 PAGE_ANYONE = 0x1C;            // muProcOptWifi's page id
    static const u32 ANYBODY_ENTER = 0x2E4F8;       // muProcWifiAnybody vtable slot 3
    static const u32 ANYBODY_ENTER_END = 0x2E790;

    // ------------------------------------------------------------------------------------
    // 0x8014FEDC `bool wc24Available(u32 mask)` (flags of the WiiConnect24 status block):
    // muProcOptWifi's enter pushes its two buttons only when it answers 1 for 0x20 (Spectator)
    // and 0x40 (Smash Service). Answer 1 for those two calls while our page is wanted.
    typedef u32 (*FlagFn)(u32);
    static FlagFn s_origFlag = NULL;
    static u32 hkFlag(u32 mask)
    {
        if (g_anyonePage && (mask == 0x20 || mask == 0x40)) {
            u32 lr = (u32)__builtin_return_address(0);
            u32 t = moduleText(MOD_MENU_MAIN);
            if (t && lr >= t + OPTWIFI_ENTER && lr < t + OPTWIFI_ENTER_END) return 1;
        }
        // WITH FRIENDS' page (muProcWifiAnybody) has three buttons: its enter shows the first
        // (decision 0x1D, Brawl's spectator button) only when 0x10 is available (text+0x2E534),
        // and lays the other two out for three then. Ours: Direct 1v1, Create Room, Join Room.
        if (mask == 0x10 && (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS)) {
            u32 lr = (u32)__builtin_return_address(0);
            u32 t = moduleText(MOD_MENU_MAIN);
            if (t && lr >= t + ANYBODY_ENTER && lr < t + ANYBODY_ENTER_END) return 1;
        }
        return s_origFlag(mask);
    }

    // ------------------------------------------------------------------------------------
    // muProcWifi (the ONLINE page) A handler: `li r0,0x1B` (next page = muProcWifiAnybody) at
    // sora_menu_main text+0x16524 / +0x166C4 / +0x168C4. Inline hook: rewrite the stub's saved
    // r0 (see netmenu.cpp friendsToAnybody) to our page.
    __attribute__((noinline)) void anyoneToOptions()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            *(u32*)(stubFrame + 8) = PAGE_ANYONE;
            g_anyonePage = 1;
        }
        (void)keepFrame[0];
    }

    // muProcWifi enter, coming back from page 0x1C (text+0x163AC: cursor = 2, the OPTIONS
    // button, hidden offline). Inline hook on text+0x163C4 `sth r5,0x42(r30)`: the cursor goes to
    // WITH ANYONE (1) instead; the highlight and the description are set from it right after.
    __attribute__((noinline)) void backToOnlinePage()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        u32 stubFrame = *(u32*)__builtin_frame_address(0);
        u8* page = *(u8**)(stubFrame + 0x78);   // saved r30 (stmw r3,0xC(r1))
        if (g_anyonePage && isPtr((u32)page)) *(u16*)(page + 0x42) = 1;
        g_anyonePage = 0;
        (void)keepFrame[0];
    }

    // ------------------------------------------------------------------------------------
    // muProcOptWifi update (vtable slot 5, text+0x2F3B8): a state machine on this+0x664; state 0
    // is the button list, read through the menu's own input (sora_menu_rule text+0x12E0): up/down
    // move the cursor (this+0x42), B goes back to the ONLINE page, and A (text+0x2FF1C) plays the
    // cursor sound and calls text+0x30290, which opens the value list (state 1). On our page that
    // call is the decision instead (hkOpenList), and the page then takes no more input.
    typedef int (*UpdateFn)(u8*);
    static UpdateFn s_origUpdate = NULL;
    typedef void (*OpenListFn)(u8*);
    static OpenListFn s_origOpenList = NULL;
    static bool s_decided = false;

    static void playSE(int id)
    {
        typedef void (*PlaySEFn)(void*, int, int, int, int, int);
        void* snd = *(void**)0x805A01D0;
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }

    static void hkOpenList(u8* page)
    {
        u32 rule = moduleText(MOD_MENU_RULE);
        if (!g_anyonePage || s_decided || !rule) {
            s_origOpenList(page);
            return;
        }
        int mode = *(u16*)(page + 0x42) == 0 ? PPOM::MODE_UNRANKED : PPOM::MODE_RANKED;
        typedef void (*DecideFn)(u8*, int, int);
        // muProcMenu's decide, as muProcWifiAnybody's A does (text+0x2E93C): menu decision 0x1E
        // -> sqMenuMain starts sqNetAnyOkiraku, the Wi-Fi character select.
        ((DecideFn)(rule + 0x1AE0))(page, 0x1E, 0);
        playSE(1);   // the menus' "decide" sound (muProcWifiAnybody: SE 1)
        s_decided = true;
        NetMenu::onlineMenuEntered(mode);
        g_onlineReturnPage = 0x1F;   // back from the CSS: the ONLINE page, WITH ANYONE highlighted
        PPOM::g_block.debug.scratch[8] = 0x100 | (u32)mode;
    }

    static int hkUpdate(u8* page)
    {
        if (g_anyonePage) OnlineMenu::menuPageRunning(page);
        if (g_anyonePage && s_decided) return 0;   // leaving for the CSS
        return s_origUpdate(page);
    }

    // ------------------------------------------------------------------------------------
    // The value pill. The button model (MenWifiOption0001_TopN) draws the value (Yes/No ...)
    // on a pink pill, bone "button0"; its visibility animation (VIS0 MenWifiOption0001_TopN__0)
    // keeps that bone constant-visible (entry flags 3 = constant | on). Our page has no values,
    // so the bone is made constant-invisible (2) in the loaded resource while it is ours, and
    // visible again when the page is entered otherwise (never offline). The VIS0 is found through the button's MuObject:
    // +0x14 gfModelAnimation -> +0x08 AnmObjVisRes -> +0x2C ResAnmVis data.
    static bool nameIs(const char* s, const char* want)
    {
        int i = 0;
        for (; want[i]; i++) if (s[i] != want[i]) return false;
        return s[i] == 0;
    }

    static void setPill(u8* page, bool visible)
    {
        u8* menu = *(u8**)(page + 0x64C);
        if (!isPtr((u32)menu)) return;
        u32 obj = *(u32*)(menu + 0x34C);
        if (!isPtr(obj)) return;
        u32 anim = *(u32*)(obj + 0x14);
        if (!isPtr(anim)) return;
        u32 visRes = *(u32*)(anim + 0x8);
        if (!isPtr(visRes)) return;
        u8* vis0 = *(u8**)(visRes + 0x2C);
        if (!isPtr((u32)vis0) || *(u32*)vis0 != 0x56495330 /* "VIS0" */) return;
        u8* grp = vis0 + *(s32*)(vis0 + 0x10);   // index group of the bone entries
        u32 n = *(u32*)(grp + 4);
        for (u32 i = 1; i <= n && i < 64; i++) {
            u8* e = grp + 8 + 16 * i;
            const char* name = (const char*)(grp + *(s32*)(e + 8));
            if (!nameIs(name, "button0")) continue;
            u32* flags = (u32*)(grp + *(s32*)(e + 12) + 4);
            if (*flags & 2) *flags = visible ? 3 : 2;   // only a constant entry
            return;
        }
    }

    // muProcOptWifi enter (vtable slot 3, text+0x2EFA4).
    typedef int (*EnterFn)(u8*, void*, int);
    static EnterFn s_origEnter = NULL;
    static int hkEnter(u8* page, void* menu, int from)
    {
        int r = s_origEnter(page, menu, from);
        s_decided = false;
        setPill(page, !g_anyonePage);
        return r;
    }

    // Its header: text+0x2F2E8 `li r4,0x1F` is the title (OPTIONS) passed with the description
    // to the menu panel (this+0x658, vtable +0x74). Ours is the WITH ANYONE page's title, 0x12
    // (muProcWifiAnybody passes 0x12, text+0x2E760). Inline hook: rewrite the saved r4.
    __attribute__((noinline)) void anyoneTitle()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        if (g_anyonePage) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            *(u32*)(stubFrame + 0x10) = 0x12;   // saved r4 (stmw r3,0xC(r1))
        }
        (void)keepFrame[0];
    }

    // ------------------------------------------------------------------------------------
    // Text: the labels, the value windows and the descriptions.
    const char* line(MuMsg* msg, u32 window, u32 lineIdx, const void* msbin)
    {
        if (!g_anyonePage) return NULL;
        if (Online::msbinLineContains(msbin, (int)lineIdx, "Allow Spectators")) return "Unranked";
        if (lineIdx == 1 && Online::msbinLineContains(msbin, (int)lineIdx, "Smash Service")) return "Ranked";
        // the values (Yes/No, Accept/Decline) of muProcOptWifi's windows 2 and 3: printed empty,
        // and their pill (bone button0) is hidden by setPill() below
        if ((window == 2 || window == 3) && lineIdx >= 2 && lineIdx <= 5 &&
            Online::msbinLineContains(msbin, 1, "Smash Service")) {
            (void)msg;
            return "";   // (font colour alpha 0 does not hide it; an empty line does)
        }
        // main menu descriptions 150-153 (0x96 + 2 * cursor + value)
        if (Online::msbinLineContains(msbin, (int)lineIdx, "viewable in Spectator mode")) return "Play unranked matches.";
        if (Online::msbinLineContains(msbin, (int)lineIdx, "the Smash Service.")) return "Play ranked matches.";
        return NULL;
    }

    void tick()
    {
        if (g_anyonePage && !Online::inScene("muMenuMain")) g_anyonePage = 0;
    }

    void install(CoreApi* api)
    {
        api->syReplaceFunc(0x8014FEDC, reinterpret_cast<void*>(hkFlag), (void**)&s_origFlag);
        api->syInlineHookRel(0x00016524, reinterpret_cast<void*>(anyoneToOptions), MOD_MENU_MAIN);
        api->syInlineHookRel(0x000166C4, reinterpret_cast<void*>(anyoneToOptions), MOD_MENU_MAIN);
        api->syInlineHookRel(0x000168C4, reinterpret_cast<void*>(anyoneToOptions), MOD_MENU_MAIN);
        api->syInlineHookRel(0x000163C4, reinterpret_cast<void*>(backToOnlinePage), MOD_MENU_MAIN);
        api->syReplaceFuncRel(0x0002F3B8, reinterpret_cast<void*>(hkUpdate), (void**)&s_origUpdate, MOD_MENU_MAIN);
        api->syReplaceFuncRel(0x00030290, reinterpret_cast<void*>(hkOpenList), (void**)&s_origOpenList, MOD_MENU_MAIN);
        api->syReplaceFuncRel(OPTWIFI_ENTER, reinterpret_cast<void*>(hkEnter), (void**)&s_origEnter, MOD_MENU_MAIN);
        api->syInlineHookRel(0x0002F2E8, reinterpret_cast<void*>(anyoneTitle), MOD_MENU_MAIN);
    }
}
