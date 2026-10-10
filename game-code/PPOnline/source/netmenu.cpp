// Nintendo WFC / Wiimmfi bypass for Brawl's Wi-Fi menus.
//
// Ported from Brawlback-Online (Brawlback-Team/brawlback-asm, MIT), source/Rollback_Hooks.cpp,
// namespace NetMenu (branches master and savestate-efficiency). The DOL addresses are the
// same on NTSC-U Rev 1 and Rev 2 and are not touched by P+ v3.2's codeset
// (tools/gamecode/pplus_hooks.py). They patch the network thread's DWC login sequence so the
// game believes it is logged in and has a friend code, without any network traffic.
#include <OS/OSCache.h>
#include <gf/gf_heap_manager.h>
#include <memory.h>
#include <string.h>
#include <sy_core.h>
#include "labels.h"
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

    void* g_friendsPage = NULL;   // the WITH FRIENDS page (muProcWifiAnybody) last entered

    // WITH FRIENDS' three buttons (docs/design/rooms.md #2). The page is Brawl's WITH ANYONE
    // page, whose first button (decision 0x1D, SPECTATOR) the game shows only when 0x10 is
    // available (anyone_menu.cpp hkFlag answers it); with it the page lays the three out itself:
    // the first small at the top left, BASIC VERSUS large at the right, TEAM BATTLE at the bottom.
    //   cursor 0, decision 0x1D: Create Room   (label texture MenMainWifi21, 128x24)
    //   cursor 1, decision 0x1E: Direct 1v1    (MenMainWifi22, 144x88; today's Direct, unchanged)
    //   cursor 2, decision 0x1F: Join Room     (MenMainWifi23, 160x80; was TEAM BATTLE = Teams)
    // Their labels are drawn into those textures with the game's own font each time the page
    // opens (labels.cpp): the page's archive is loaded again with the menus.
    struct FriendsButton {
        u32 obj;            // the menu's MuObject (menu+0x33C + 4 * cursor)
        const char* tex;    // the label's texture
        const char* text;
        s16 box[4];         // where the game's own letters sit in it
    };
    static const FriendsButton BUTTONS[3] = {
        {0x33C, "MenMainWifi22", "CREATE\nROOM", {13, 6, 135, 81}},   // Direct's pill (below)
        {0x340, "MenMainWifi22", "DIRECT\n1V1", {13, 6, 135, 81}},
        {0x344, "MenMainWifi23", "JOIN\nROOM", {21, 4, 139, 76}},
    };
    static int s_returnButton = -1;   // the button to highlight when the page opens again

    static bool menuPtr(u32 p) { return p >= 0x80000000 && p < 0x94000000; }

    // ------------------------------------------------------------------------------------------
    // Create Room in Direct 1v1's pill (the same shape, size, icon circle and two-line label as
    // the other two). Each of the page's buttons is its own small archive file, a brres with one
    // model and its animations (MenMainIcon0321_TopN: the spectator button, a smaller white
    // card; 0322: BASIC VERSUS's pill; 0323: TEAM BATTLE's), laid out one after the other in the
    // menu's archive and made into MuObjects when the menu is built (MuObject::create(&file,
    // model index, ...), 0x800B2404). When 0321 is made, the plugin makes it from
    // a copy of 0322's file instead (in the Network heap, kept for the next menus). The copy
    // keeps its textures bound to the menu's (texture infos +8; the materials' GXTexObj image
    // words, physical address >> 5), except three, which it gets of its own:
    //   MenMainWifi22 (the label)   a copy, labelled CREATE / ROOM (drawButtonLabels);
    //   MenMainWifi25 (the spiral)  the spectator button's megaphone (MenMainWifi26, an I4
    //                               half drawn twice) mirrored into one icon for the circle;
    //   MenMainWifi25_02 (marks)    blank: the spiral's eight marks have no place round it.
    // The layout animations are 0322's (the pill at BASIC VERSUS's place): the page's enter
    // moves it left with MuObject::setPos (0x800B6838, as the enter itself moves the others for
    // the two-button layout). The menu's highlight, cursor and decision code use the button by
    // its slot (menu+0x33C) and its own animations, so nothing else changes.
    typedef void* (*MuCreateFn)(u8** file, int index, int prio, int arg, int heap);
    static MuCreateFn s_origMuCreate = NULL;
    static u8* s_pillFile = NULL;      // the copy of 0322's brres
    static u32 s_pillCap = 0;
    static u8* s_pillLabel = NULL;     // our TEX0s (header + image), 32-byte aligned
    static u8* s_pillIcon = NULL;
    static u8* s_pillBlank = NULL;
    static bool s_pillMade = false;    // the current menu's button 0 is the pill
    static u32 s_pillFrom[3], s_pillTo[3];   // the three images: the original's, ours (phys >> 5)
    static const float PILL_SHIFT_X = -28.0f;   // from BASIC VERSUS's place (found live)

    static u8* alignedAlloc(u32 size)
    {
        u8* p = (u8*)gfHeapManager::alloc(Heaps::Network, size + 32);
        return p ? (u8*)(((u32)p + 31) & ~31u) : NULL;
    }

    // A TEX0 of our own: `src`'s header and image copied (its name offset is left as it was).
    static u8* copyTex(u8** slot, const u8* src)
    {
        u32 data = *(u32*)(src + 0x10);
        int w = *(u16*)(src + 0x1C), h = *(u16*)(src + 0x1E);
        u32 fmt = *(u32*)(src + 0x20);
        int bw = fmt == 5 ? 4 : 8, bh = fmt == 5 ? 4 : 8, bpp = fmt == 5 ? 16 : 4;
        u32 bytes = (u32)(((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bw * bh * bpp / 8);
        if (data != 0x40 || bytes > 0x10000) return NULL;
        // Exactly its size, once (the same texture every time): the Network heap must keep
        // room for the HOME menu's archive, which the game loads there after a match
        // (docs/game-code.md §2, "Heap budget").
        if (!*slot) *slot = alignedAlloc(0x40 + bytes);
        if (!*slot) return NULL;
        memcpy(*slot, src, 0x40 + bytes);
        DCFlushRange(*slot, 0x40 + bytes);
        return *slot;
    }

    // The copy's references to `oldTex`'s image point at `newTex`'s: the materials' GXTexObj
    // image words (physical address >> 5) and the texture image registers the bind wrote into
    // the materials' display lists (BP 0x61, register 0x94-0x97 / 0xB4-0xB7, 24-bit address >> 5).
    static void retargetImage(u8* file, u32 size, const u8* oldTex, const u8* newTex)
    {
        u32 from = ((u32)(oldTex + 0x40) & 0x1FFFFFFF) >> 5;
        u32 to = ((u32)(newTex + 0x40) & 0x1FFFFFFF) >> 5;
        for (u32 off = 0; off + 4 <= size; off += 4) {
            if (*(u32*)(file + off) == from) *(u32*)(file + off) = to;
        }
        for (u32 off = 0; off + 5 <= size; off++) {
            u8* b = file + off;
            if (b[0] != 0x61) continue;
            u8 r = b[1];
            if (!((r >= 0x94 && r <= 0x97) || (r >= 0xB4 && r <= 0xB7))) continue;
            u32 v = ((u32)b[2] << 16) | ((u32)b[3] << 8) | b[4];
            if (v != (from & 0xFFFFFF)) continue;
            b[2] = (u8)(to >> 16);
            b[3] = (u8)(to >> 8);
            b[4] = (u8)to;
        }
    }

    // The file's model (each of these files has one; MDL0 sub-files are 32-byte aligned).
    static u8* findModel(u8* bres)
    {
        u32 size = *(u32*)(bres + 8);
        for (u32 off = 0x20; off + 0x40 <= size && off < 0x2000; off += 0x20) {
            if (*(u32*)(bres + off) == 0x4D444C30 /* MDL0 */) return bres + off;
        }
        return NULL;
    }

    static const char* modelName(u8* bres)
    {
        u8* mdl = findModel(bres);
        if (!mdl || *(u32*)(mdl + 8) != 9) return NULL;   // MDL0 version 9: name offset at +0x3C
        s32 no = *(s32*)(mdl + 0x3C);
        return no > 0 && (u32)no < *(u32*)(mdl + 4) + 0x10000 ? (const char*)(mdl + no) : NULL;
    }

    // A TEX0 by name in the archive a bound TEX0 came from (TEX0 +0xC: its brres, relative).
    static u8* textureNamed(u8* boundTex, const char* name)
    {
        if (!menuPtr((u32)boundTex) || *(u32*)boundTex != 0x54455830) return NULL;
        u8* bres = boundTex + *(s32*)(boundTex + 0xC);
        if (!menuPtr((u32)bres) || *(u32*)bres != 0x62726573) return NULL;
        u8* root = bres + *(u16*)(bres + 0xC);
        u8* groups = root + 8;   // root's ResDic: one group per resource kind
        u32 ng = *(u32*)(groups + 4);
        for (u32 g = 1; g <= ng && g < 32; g++) {
            u8* e = groups + 8 + 16 * g;
            if (strcmp((const char*)(groups + *(s32*)(e + 8)), "Textures(NW4R)") != 0) continue;
            u8* dic = groups + *(s32*)(e + 12);
            u32 n = *(u32*)(dic + 4);
            for (u32 i = 1; i <= n && i < 512; i++) {
                u8* t = dic + 8 + 16 * i;
                if (strcmp((const char*)(dic + *(s32*)(t + 8)), name) == 0) return dic + *(s32*)(t + 12);
            }
        }
        return NULL;
    }

    // A copy of BASIC VERSUS's file, made when the menu makes the spectator button's object.
    static u8* copyPill(u8* file0321)
    {
        // 0322's file follows 0321's (an archive entry header of 0x20 bytes between them).
        u32 size0321 = *(u32*)(file0321 + 8);
        if (size0321 < 0x100 || size0321 > 0x40000) return NULL;
        u8* f = file0321 + size0321;
        u8* file0322 = NULL;
        for (int i = 0; i <= 0x80; i += 4) {
            if (*(u32*)(f + i) == 0x62726573 /* bres */) { file0322 = f + i; break; }
        }
        if (!file0322) return NULL;
        u32 size = *(u32*)(file0322 + 8);
        if (size < 0x100 || size > 0x20000) return NULL;
        const char* name = modelName(file0322);
        if (!name || strcmp(name, "MenMainIcon0322_TopN") != 0) return NULL;
        if (!s_pillFile || s_pillCap < size) {
            s_pillFile = alignedAlloc(size);
            s_pillCap = s_pillFile ? size : 0;
        }
        if (!s_pillFile) return NULL;
        memcpy(s_pillFile, file0322, size);
        DCFlushRange(s_pillFile, size);
        return s_pillFile;
    }

    // The button's ScnMdl keeps its own copies of the materials' GXTexObjs (with their GX
    // register ids, e.g. 0x94 << 24 | image >> 5, found at ScnMdl+0x15CC for these models):
    // those are what it loads, so they get our images too. Only the three images are matched.
    // They are filled from the model after the page opens, so this runs every frame of the
    // page (anybodyUpdate).
    static void pillScnMdl(u8* obj)
    {
        u8* scn = menuPtr((u32)obj) ? *(u8**)(obj + 0xC) : NULL;
        if (!menuPtr((u32)scn)) return;
        for (u32 off = 0; off < 0x2000; off += 4) {
            u32 w = *(u32*)(scn + off);
            u32 r = w >> 24;
            if (!((r >= 0x94 && r <= 0x97) || (r >= 0xB4 && r <= 0xB7))) continue;
            for (int k = 0; k < 3; k++) {
                if (s_pillFrom[k] && (w & 0xFFFFFF) == s_pillFrom[k]) {
                    *(u32*)(scn + off) = (w & 0xFF000000u) | s_pillTo[k];
                    break;
                }
            }
        }
    }

    // The copy's own textures, once the game has bound its model (to the menu's textures, as
    // the original). Done again each time the page opens (cheap; nothing if already done).
    static void pillTextures(u8* obj)
    {
        if (!s_pillMade || !s_pillFile) return;
        u8* cmdl = findModel(s_pillFile);
        if (!cmdl) return;
        u8* label = Labels::boundTexture(cmdl, "MenMainWifi22");
        u8* spiral = Labels::boundTexture(cmdl, "MenMainWifi25");
        u8* marks = Labels::boundTexture(cmdl, "MenMainWifi25_02");
        if (!label || !spiral || !marks) return;   // not bound yet
        if (label == s_pillLabel && spiral == s_pillIcon && marks == s_pillBlank) {   // done
            pillScnMdl(obj);
            return;
        }
        u8* megaphone = textureNamed(label, "MenMainWifi26");
        if (!megaphone) return;
        if (!copyTex(&s_pillLabel, label) || !copyTex(&s_pillIcon, spiral) || !copyTex(&s_pillBlank, marks)) return;
        Labels::mirroredIcon(s_pillIcon, megaphone);
        u32 blankBytes = 16 * 16 / 2;
        memset(s_pillBlank + 0x40, 0, blankBytes);
        DCFlushRange(s_pillBlank, 0x40 + blankBytes);
        u32 size = *(u32*)(s_pillFile + 8);
        Labels::rebindTexture(cmdl, "MenMainWifi22", s_pillLabel);
        Labels::rebindTexture(cmdl, "MenMainWifi25", s_pillIcon);
        Labels::rebindTexture(cmdl, "MenMainWifi25_02", s_pillBlank);
        retargetImage(s_pillFile, size, label, s_pillLabel);
        retargetImage(s_pillFile, size, spiral, s_pillIcon);
        retargetImage(s_pillFile, size, marks, s_pillBlank);
        DCFlushRange(s_pillFile, size);
        const u8* from[3] = {label, spiral, marks};
        const u8* to[3] = {s_pillLabel, s_pillIcon, s_pillBlank};
        for (int k = 0; k < 3; k++) {
            s_pillFrom[k] = ((u32)(from[k] + 0x40) & 0x1FFFFFFF) >> 5;
            s_pillTo[k] = ((u32)(to[k] + 0x40) & 0x1FFFFFFF) >> 5;
        }
        pillScnMdl(obj);
    }

    static void* hkMuCreate(u8** file, int index, int prio, int arg, int heap)
    {
        if ((PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) && file && menuPtr((u32)*file) &&
            *(u32*)*file == 0x62726573 /* bres */) {
            const char* name = modelName(*file);
            if (name && strcmp(name, "MenMainIcon0321_TopN") == 0) {
                s_pillMade = false;
                u8* pill = copyPill(*file);
                if (pill) {
                    static u8* s_pillPtr;
                    s_pillPtr = pill;
                    void* obj = s_origMuCreate(&s_pillPtr, index, prio, arg, heap);
                    if (obj) {
                        s_pillMade = true;
                        pillTextures((u8*)obj);
                        return obj;
                    }
                }
            }
        }
        return s_origMuCreate(file, index, prio, arg, heap);
    }

    static void placePill(u8* proc)
    {
        if (!s_pillMade) return;
        u8* menu = *(u8**)(proc + 0x64C);
        u8* obj = menuPtr((u32)menu) ? *(u8**)(menu + 0x33C) : NULL;
        if (!menuPtr((u32)obj) || *(u8**)(obj + 4) != s_pillFile) return;
        float pos[3] = {PILL_SHIFT_X, *(float*)(obj + 0x40), *(float*)(obj + 0x44)};
        typedef void (*SetPosFn)(u8*, float*);
        ((SetPosFn)0x800B6838)(obj, pos);
    }

    static void drawButtonLabels(u8* proc)
    {
        u8* menu = *(u8**)(proc + 0x64C);
        if (!menuPtr((u32)menu)) return;
        int drawn = 0;
        for (int i = 0; i < 3; i++) {
            u8* obj = *(u8**)(menu + BUTTONS[i].obj);
            if (!menuPtr((u32)obj)) continue;
            u8* tex = Labels::boundTexture(*(u8**)(obj + 8), BUTTONS[i].tex);
            const s16* b = BUTTONS[i].box;
            if (tex && Labels::render(tex, BUTTONS[i].text, b[0], b[1], b[2], b[3], 2)) drawn++;
        }
        PPOM::g_block.debug.scratch[6] = (PPOM::g_block.debug.scratch[6] & 0xFFFF) | ((u32)drawn << 16);
    }

    // .text of a loaded REL (anyone_menu.cpp has the same walk).
    static u32 relText(u32 id)
    {
        u32 m = *(u32*)0x800030C8;
        for (int n = 0; menuPtr(m) && n < 64; n++) {
            if (*(u32*)m == id) {
                u32 sec = *(u32*)(m + 0x10);
                return menuPtr(sec) ? (*(u32*)(sec + 8) & ~1u) : 0;
            }
            m = *(u32*)(m + 4);
        }
        return 0;
    }

    // Highlight button `b` the way the page's own cursor code does (text+0x2E8A0): the cursor
    // (this+0x42) moves, then muProcMenu's highlight (sora_menu_rule text+0x1D48) is called with
    // the button it moved FROM (it plays that one's leave animation, then highlights the
    // cursor's), and the description line (the panel this+0x658, vtable at +0x3C, slot 0x64:
    // line 0x32 + cursor). (Called with the new button, the old one stayed highlighted.)
    static void highlightButton(u8* proc, int b)
    {
        u32 rule = relText(18);
        u8* panel = *(u8**)(proc + 0x658);
        if (!rule || !menuPtr((u32)panel)) return;
        int old = *(u16*)(proc + 0x42);
        *(u16*)(proc + 0x42) = (u16)b;
        typedef void (*HighlightFn)(u8*, int, int);
        ((HighlightFn)(rule + 0x1D48))(proc, old, 1);
        typedef void (*DescFn)(u8*, int, int, int);
        ((DescFn)(*(u32*)(*(u32*)(panel + 0x3C) + 0x64)))(panel, 0x32 + b, 1, 0);
    }

    static int anybodyEnter(void* proc, void* a, void* b)
    {
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) {
            s_friendsPicked = false;
            g_onlineFriendsCursor = 1;
        }
        g_friendsPage = proc;
        int r = s_origAnybodyEnter(proc, a, b);
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) {
            u8* menu = *(u8**)((u8*)proc + 0x64C);
            if (menuPtr((u32)menu)) pillTextures(*(u8**)(menu + 0x33C));
            placePill((u8*)proc);
            drawButtonLabels((u8*)proc);
            // Back from a room's CSS: its button (the game puts the cursor on BASIC VERSUS, the
            // decision every button of ours leaves with).
            if (s_returnButton >= 0 && s_returnButton != *(u16*)((u8*)proc + 0x42)) highlightButton((u8*)proc, s_returnButton);
            s_returnButton = -1;
        }
        return r;
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
    // Ours: 0x1E Direct; 0x1D Create Room and 0x1F Join Room open the same Wi-Fi character
    // select (sqNetAnyOkiraku) as a room's CSS: the decision passed on (the stub's saved r4,
    // stmw r3,0xC(r1): +0x10) becomes 0x1E. (TEAM BATTLE's sqNetAnyTeamMelee is no longer used.)
    __attribute__((noinline)) void anybodyDecision(u32 r3, u32 decision)
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        (void)r3;
        PPOM::g_block.debug.scratch[8] = decision;
        if (PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            if (decision == 0x1E) {
                onlineMenuEntered(PPOM::MODE_DIRECT);
                s_returnButton = 1;
            } else if (decision == 0x1D || decision == 0x1F) {
                OnlineMenu::enterRoom(decision == 0x1D ? OnlineMenu::ROOM_ENTRY_CREATE : OnlineMenu::ROOM_ENTRY_JOIN);
                s_returnButton = decision == 0x1D ? 0 : 2;
                *(u32*)(stubFrame + 0x10) = 0x1E;
            }
        }
        (void)keepFrame[0];
    }

    void setReturnButton(int b) { s_returnButton = b; }

    // muProcWifiAnybody's update (vtable slot 5, sora_menu_main text+0x2E794): the WITH FRIENDS
    // page is one the launcher's join can leave from (OnlineMenu::launcherJoin).
    typedef int (*PageUpdateFn)(u8*);
    static PageUpdateFn s_origAnybodyUpdate = NULL;
    static int anybodyUpdate(u8* page)
    {
        OnlineMenu::menuPageRunning(page);
        int r = s_origAnybodyUpdate(page);
        // The pill's ScnMdl fills its texture copies from the model after the page has opened:
        // keep them on our images (only the three original images are matched; cheap).
        u8* menu = *(u8**)(page + 0x64C);
        if (s_pillMade && menuPtr((u32)menu)) pillScnMdl(*(u8**)(menu + 0x33C));
        return r;
    }

    // Brawl's "Connect to Nintendo WFC?" window (WifiCnctWnd task, sora_menu_main text+0x385C0),
    // with its "Connected." and first-time "Choose a profile name." steps. Slippi has no such
    // screens: PLAY ONLINE goes straight to the mode list. The main menu creates the window at
    // four places and stores it in this+0x66C; each frame it polls the window: state
    // (wnd+0x138) 0xC = finished, result (wnd+0x128) 0 = connected, which changes to the ONLINE
    // page (this+0x634 = 3) and deletes the window (text+0x14FD8). Marking the new window as
    // finished and connected skips all of it; nothing of the window is shown or run. The WFC
    // login itself is already faked by the hooks above.
    //
    // One thing the window does on the way: once connected it creates muWifiInterfaceTask
    // (text+0x38EB4: if g_muWifiInterfaceTask 0x805A02A4 is null and the Network heap has room,
    // create__19muWifiInterfaceTaskFv). The Wi-Fi CSS (sel_char) uses that task every frame
    // without a null check; without it every CSS frame read and wrote through a null pointer
    // (24 invalid accesses a frame, each one a formatted panic alert in Dolphin). So the task is
    // created here, as the window would have.
    void skipConnectWindow(u8* wnd)
    {
        if (!wnd || !(PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS)) return;
        *(u32*)(wnd + 0x128) = 0;
        *(u32*)(wnd + 0x138) = 0xC;
        PPOM::g_block.debug.scratch[9]++;
        if (*(void**)0x805A02A4 == 0) {
            typedef void* (*CreateFn)();
            reinterpret_cast<CreateFn>(0x800C7410)();   // muWifiInterfaceTask::create
        }
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
        if (g_onlineCss) {
            OnlineMenu::applyRules();
            OnlineMenu::prepareCss();   // a room's CSS: four full panels (room_css.cpp)
        }
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
        (void)keepFrame[0];
    }

    // The ONLINE page's update (muProcWifi) reads the menu input at text+0x16564 and keeps it in
    // r30; both of its A handlers (text+0x16688, +0x16888) test bit 0x10 of it, then open the
    // page under the cursor (this+0x42: 0 WITH FRIENDS, 1 WITH ANYONE). Every online mode is
    // behind those two buttons, so while the modes are locked (Slippi's
    // HandleOnlineLockedOptions.asm: logged out, or an update required) A on them only plays the
    // menus' buzzer. Inline hook at text+0x16568 `mr r30,r3`: the stub's saved r30 (stmw
    // r3,0xC(r1): +0x78) loses the A bit; r29 (+0x74) is the page.
    __attribute__((noinline)) void lockedOnlineButtons()
    {
        volatile u32 keepFrame[2];
        keepFrame[0] = 0;
        {
            u32 sf = *(u32*)__builtin_frame_address(0);
            u8* pg = *(u8**)(sf + 0x74);
            if ((u32)pg >= 0x80000000 && (u32)pg < 0x81800000) OnlineMenu::menuPageRunning(pg);
        }
        if ((PPOM::g_block.debug.cfg & PPOM::CFG_WIFI_HOOKS) && OnlineMenu::modesLocked()) {
            u32 stubFrame = *(u32*)__builtin_frame_address(0);
            u32* input = (u32*)(stubFrame + 0x78);
            u8* page = *(u8**)(stubFrame + 0x74);
            if ((*input & 0x10) && (u32)page >= 0x80000000 && (u32)page < 0x81800000 &&
                *(u16*)(page + 0x42) <= 1) {
                *input &= ~0x10u;
                void* snd = *(void**)0x805A01D0;   // g_sndSystem: the buzzer (SE 3)
                typedef void (*PlaySEFn)(void*, int, int, int, int, int);
                if (snd) ((PlaySEFn)0x800742b0)(snd, 3, -1, 0, 0, -1);
                PPOM::g_block.debug.scratch[7] += 0x100;   // tests: A presses refused
            }
        }
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
        api->syReplaceFunc(0x800B2404, reinterpret_cast<void*>(hkMuCreate), (void**)&s_origMuCreate);
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
        api->syReplaceFuncRel(0x0002E794, reinterpret_cast<void*>(anybodyUpdate), (void**)&s_origAnybodyUpdate, 2);
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
        api->syInlineHookRel(0x00016568, reinterpret_cast<void*>(lockedOnlineButtons), 2 /* SORA_MENU_MAIN */);

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
