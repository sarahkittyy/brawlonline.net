// The waiting room (docs/game-code.md, "The waiting room"; include/training_room.h).
//
// Brawl's own "With Anyone" search waits in a room: sqNetAnyOkiraku (sora_scene module 1) goes
// from the CSS to state 5, which builds a one-player match on the Online Training stage (stage
// kind 0x37, players 1-6 none) and plays it as scMelee (states 6/7) until the network says every
// player is in; state 7 then takes the scene's exit code to state 8, the network match setup.
// Brawlback Gen 1 used that room the same way (Rollback_Hooks.cpp, setNextAnyOkirakuCaseFive).
//
// Ours, for Unranked and Ranked (not Direct, not rooms, not replays):
//  - the CSS's START locks in and searches as before, then leaves the CSS for the room
//    (OnlineMenu::startSearch -> enter(); state 3 -> state 5, onSelStage);
//  - state 5 is Brawl's own; at its end (hookAfterSetup) nothing changes in the setup;
//  - in the room the online menu's tick keeps polling the search and gives the banner its line
//    (OnlineMenu::tickWaitingRoom); this file spawns the Sandbag and prints the banner;
//  - the room ends as Brawl's own ends when its search has found everyone (the room operator's
//    result, end/tick); state 7 (hookState7) then goes on by how it was ended: to OnlineMatch's setup
//    (state 3, g_onlineMatchGame armed: Unranked), or back to the CSS (Ranked's game setup, a
//    cancel, an error), never to Brawl's network match setup (state 8).
//
// Dolphin never takes the room for the online match: its scMelee checks skip the Online
// Training stage (GameplayRollback.cpp IsWaitingRoom).
#include <memory.h>
#include <string.h>

#include <it/it_manager.h>
#include <mu/mu_msg.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "training_room.h"

extern "C" {
    extern u8 g_onlineMatchGame;
}

namespace TrainingRoom {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    enum State {
        ST_NONE = 0,
        ST_CSS_LEFT = 1,   // the CSS is leaving for the room (state 3 sets up the room)
        ST_SETUP = 2,      // state 5 ran: the room loads
        ST_ROOM = 3,       // the room's scMelee runs
    };
    static u8 s_state;
    static u8 s_end;            // End: how the room was ended (END_NONE: by the game itself)
    static u32 s_roomFrames;    // frames in the room's scMelee
    static u32 s_stopFrames;    // frames since the end was asked for

    // The banner (Brawl's own scrolling message line in the room).
    static MuMsg* s_bannerMsg;
    static u32 s_bannerWindow;
    static char s_banner[128];
    static bool s_bannerRed;
    static bool s_bannerDirty;

    // The Sandbag (the Sandbag item, Item_Sandbag 0x31, in the middle of the stage).
    static const u32 SANDBAG_FIRST_FRAME = 30;      // after the room's start
    static const u32 SANDBAG_RESPAWN_FRAMES = 90;   // after it is gone (knocked out)
    static u8 s_sandbagStep;        // 0 nothing, 1 archive asked for, 2 out
    static u32 s_sandbagWait;

    // sora_melee's player slots: the pointer at its .bss+0x5910 (sqNetAnyOkiraku's decide
    // function keeps its address in r28) points to seven objects whose +0x5D flags say whether the
    // slot is in use (0x04) or not (0x08). State 5 sets them for the room (slot 0 in use, the
    // others not), and nothing on the way into the match sets them back: the match then made no
    // fighters at all. So they are kept from before the room and put back after it.
    static const u32 SLOT_FLAGS = 0x5D, SLOT_USED = 0x04, SLOT_UNUSED = 0x08;
    static u8 s_slotFlags[7];
    static bool s_haveSlotFlags;

    static u8* meleeSlot(int i)
    {
        u32 m = *(u32*)0x800030C8;   // the loaded modules (OSModuleInfo list)
        for (int n = 0; isPtr(m) && n < 64; n++, m = *(u32*)(m + 4)) {
            if (*(u32*)m != 27) continue;   // sora_melee
            u32 nsec = *(u32*)(m + 0xC), secs = *(u32*)(m + 0x10);
            if (nsec <= 6 || !isPtr(secs)) return NULL;
            u32 bss = *(u32*)(secs + 6 * 8) & ~1u;   // OSLink puts the .bss address there
            u32 arr = isPtr(bss) ? *(u32*)(bss + 0x5910) : 0;
            u32 p = isPtr(arr) ? *(u32*)(arr + 4 * i) : 0;
            return isPtr(p) ? (u8*)p : NULL;
        }
        return NULL;
    }

    static void keepSlotFlags()
    {
        // (Four slots exist; the other pointers are null.)
        for (int i = 0; i < 7; i++) {
            u8* p = meleeSlot(i);
            s_slotFlags[i] = p ? p[SLOT_FLAGS] : 0;
        }
        s_haveSlotFlags = true;
    }

    static void restoreSlotFlags()
    {
        if (!s_haveSlotFlags) return;
        s_haveSlotFlags = false;
        for (int i = 0; i < 7; i++) {
            u8* p = meleeSlot(i);
            if (p) p[SLOT_FLAGS] = (u8)((p[SLOT_FLAGS] & ~(SLOT_USED | SLOT_UNUSED)) | (s_slotFlags[i] & (SLOT_USED | SLOT_UNUSED)));
        }
    }

    static void fadeOut();
    static bool fadedOut();
    static void fadeBackIn();

    bool active() { return s_state != ST_NONE; }

    bool running() { return s_state == ST_ROOM && Online::inScene("scMelee"); }

    void abandon()
    {
        if (s_state == ST_NONE) return;
        if (s_state == ST_CSS_LEFT) fadeBackIn();   // still on the CSS: it comes back out of the fade
        restoreSlotFlags();
        s_state = ST_NONE;
        s_end = END_NONE;
        s_bannerMsg = NULL;
        PPOM::g_block.debug.lastError = 0x7A1B;   // tests: the room was abandoned
    }

    void enter()
    {
        s_state = ST_CSS_LEFT;
        s_end = END_NONE;
        s_roomFrames = 0;
        s_stopFrames = 0;
        s_bannerMsg = NULL;
        s_banner[0] = 0;
        s_bannerDirty = true;
        s_sandbagStep = 0;
        s_sandbagWait = 0;
        fadeOut();
        // tests: the room's flow: 0xF the step (State, 7 after the room), 0xF0 the room's
        // exit code, 0xF00 how it was ended, 0x10000 banner seen, 0x20000 Sandbag made, 0x40000
        // a Sandbag refused. (scratch[5] is otherwise the keypad's, which never runs meanwhile.)
        PPOM::g_block.debug.scratch[5] = 1;
    }

    // The room's operator, StOperatorInfoWifiPractice (scMelee+0x68; sora_melee, vtable at sec5
    // +0x53868). Its process (text+0x259188) is Brawl's network search, a state machine that runs
    // while OP_ACTIVE is set; it ends with OP_MATCHED (everyone found) or OP_ERROR, and clears
    // OP_ACTIVE. Its result (vtable +0x70, text+0x25A554) is the room's exit code: OP_ERROR 4-6 by
    // the network error, else OP_LEFT 3, else OP_MATCHED (1, "go on", state 7 -> 8). The room
    // ends once the operator is inactive with a result. Setting these is how the room is ended
    // here; the exit code does not matter (state 7 goes on by s_end). (Forcing the scene's exit
    // through the scene manager hung the game in the task scheduler's teardown.)
    static const u32 OP_ERROR = 0x14C, OP_LEFT = 0x14D, OP_MATCHED = 0x14E, OP_ACTIVE = 0x14F;

    static u8* roomOperator()
    {
        u32 mgr = *(u32*)0x805A0060;
        u32 scene = isPtr(mgr) ? *(u32*)(mgr + 4) : 0;
        u32 op = isPtr(scene) ? *(u32*)(scene + 0x68) : 0;
        if (!isPtr(op)) return NULL;
        u32 name = *(u32*)op;   // gfTask::m_taskName
        if (!isPtr(name) || strcmp((const char*)name, "StOperatorInfoWifiPractice") != 0) return NULL;
        return (u8*)op;
    }

    void end(int how)
    {
        if (s_state != ST_ROOM || s_end != END_NONE) return;
        s_end = (u8)how;
        s_stopFrames = 0;
        fadeOut();
        PPOM::g_block.debug.scratch[5] = (PPOM::g_block.debug.scratch[5] & ~0xF00u) | ((u32)how << 8);
    }

    void setBanner(const char* text, bool red)
    {
        if (!text) text = "";
        if (red == s_bannerRed && strncmp(text, s_banner, sizeof(s_banner) - 1) == 0) return;
        strncpy(s_banner, text, sizeof(s_banner) - 1);
        s_banner[sizeof(s_banner) - 1] = 0;
        s_bannerRed = red;
        s_bannerDirty = true;
    }

    const char* line(MuMsg* msg, u32 window, u32 line, const void* msbin)
    {
        if (s_state != ST_ROOM && s_state != ST_SETUP) return NULL;
        if (!Online::msbinLineContains(msbin, (int)line, "Registering")) return NULL;
        s_bannerMsg = msg;
        s_bannerWindow = window;
        s_bannerDirty = true;
        PPOM::g_block.debug.scratch[5] |= 0x10000;   // tests: the banner line was seen
        return s_banner[0] ? s_banner : NULL;
    }

    static void printBanner()
    {
        if (!s_bannerMsg || !s_bannerDirty) return;
        s_bannerDirty = false;
        if (s_bannerRed) s_bannerMsg->setFontColor(s_bannerWindow, 255, 0, 0, 255);
        else s_bannerMsg->setFontColor(s_bannerWindow, 255, 255, 255, 255);
        s_bannerMsg->printf(s_bannerWindow, "%s", s_banner);
    }

    static void tickSandbag()
    {
        itManager* im = itManager::getInstance();
        if (!im || !isPtr((u32)im)) return;
        if (s_sandbagStep == 2) {
            if (im->getItemNum(Item_Sandbag)) return;
            // Gone (knocked out): a new one after a while.
            s_sandbagStep = 1;
            s_sandbagWait = SANDBAG_RESPAWN_FRAMES;
            return;
        }
        if (s_sandbagStep == 0) {
            if (s_roomFrames < SANDBAG_FIRST_FRAME) return;
            im->preloadItemKindArchive(Item_Sandbag, 0, itArchive::Common, false);
            s_sandbagStep = 1;
            return;
        }
        if (s_sandbagWait) {
            s_sandbagWait--;
            return;
        }
        if (!im->isCompItemKindArchive(Item_Sandbag, 0, false)) return;
        Vec3f pos = {0.0f, 20.0f, 0.0f};
        BaseItem* it = im->createItem(&pos, 1.0f, Item_Sandbag);
        PPOM::g_block.debug.scratch[5] |= it ? 0x20000 : 0x40000;   // tests: made / failed
        if (!it) {
            s_sandbagWait = SANDBAG_RESPAWN_FRAMES;
            return;
        }
        s_sandbagStep = 2;
    }

    // ---- The fade (the user's, 2026-10-16) ----
    // Each way into and out of the room loads for a second or two, with the last frame frozen on
    // the screen. The screen goes black over FADE_OUT_FRAMES instead, stays black through the load,
    // and the next scene (the room, the match, the CSS) comes out of black over FADE_IN_FRAMES:
    // a black quad over the frame, drawn by MatchHud's frame hook (after the frame is drawn, before
    // its copy to the screen) with the alpha fadeAlpha gives. Out: from the START that searched,
    // and before the room ends (end() waits for it). In: from the first frame of the first scene
    // after a load screen. Only the picture changes; nothing of the game's state does.
    static const u32 FADE_OUT_FRAMES = 12, FADE_IN_FRAMES = 18;
    static const u32 FADE_MAX_FRAMES = 60 * 20;   // never stuck black: give up after 20 s
    enum Fade { FADE_NONE = 0, FADE_OUT = 1, FADE_IN = 2 };
    static u8 s_fade;
    static bool s_fadeLoaded;     // a load screen has run since the fade out began
    static bool s_fadeWaitGame;   // in the next scMelee, waiting for its first game frame
    static u32 s_fadeGameFrame;   // the game frame counter when that scMelee was first seen
    static const u32 GAME_FRAME = 0x901812A4;    // g_GameFrame's frame counter
    static const u32 GAME_FRAME_P = 0x901812B4;  // its persistent counter (from the scene's start)
    static u32 s_fadeFrames;
    static u32 s_fadeTotal;

    static void fadeOut()
    {
        s_fade = FADE_OUT;
        s_fadeLoaded = false;
        s_fadeWaitGame = false;
        s_fadeFrames = 0;
        s_fadeTotal = 0;
    }

    static bool fadedOut() { return s_fade == FADE_OUT && s_fadeFrames >= FADE_OUT_FRAMES; }

    bool screenBlack() { return fadedOut(); }

    // A fade out given up before its load (the search cancelled on the CSS): back in from as dark
    // as it got.
    static void fadeBackIn()
    {
        if (s_fade != FADE_OUT) return;
        const u32 dark = FADE_IN_FRAMES * s_fadeFrames / FADE_OUT_FRAMES;   // in-frames of black left
        s_fade = FADE_IN;
        s_fadeFrames = FADE_IN_FRAMES - (dark < FADE_IN_FRAMES ? dark : FADE_IN_FRAMES);
    }

    static u8 fadeAlphaNow();
    u8 fadeAlpha()
    {
        const u8 a = fadeAlphaNow();
        // tests: the alpha drawn, scratch[5] bits 24-31
        PPOM::g_block.debug.scratch[5] = (PPOM::g_block.debug.scratch[5] & 0x00FFFFFFu) | ((u32)a << 24);
        return a;
    }

    static u8 fadeAlphaNow()
    {
        if (s_fade == FADE_NONE) return 0;
        if (++s_fadeTotal > FADE_MAX_FRAMES) {
            s_fade = FADE_NONE;
            return 0;
        }
        const char* scene = Online::currentSceneName();
        if (s_fade == FADE_OUT) {
            if (strcmp(scene, "scMemoryChange") == 0) {
                s_fadeLoaded = true;
            } else if (s_fadeLoaded) {
                // The next scene runs. An scMelee loads its stage and fighters first and shows
                // nothing meanwhile: it comes out of black at its first simulation frame, as
                // Dolphin's session finds it (GameplaySession IsSimStart: the frame counter has
                // started again while the persistent one runs on), with a counter new to this
                // scene (changed since the scene began).
                bool ready = true;
                if (strcmp(scene, "scMelee") == 0) {
                    const u32 gf = *(u32*)GAME_FRAME, pc = *(u32*)GAME_FRAME_P;
                    if (!s_fadeWaitGame) {
                        s_fadeWaitGame = true;
                        s_fadeGameFrame = gf;
                    }
                    ready = gf != s_fadeGameFrame && gf >= 1 && gf < 600 && pc > gf;
                }
                if (ready) {
                    s_fade = FADE_IN;
                    s_fadeFrames = 0;
                }
            }
        }
        if (s_fade == FADE_OUT) {
            if (s_fadeFrames < FADE_OUT_FRAMES) s_fadeFrames++;
            return (u8)(255 * s_fadeFrames / FADE_OUT_FRAMES);
        }
        if (++s_fadeFrames >= FADE_IN_FRAMES) {
            s_fade = FADE_NONE;
            return 0;
        }
        return (u8)(255 - 255 * s_fadeFrames / FADE_IN_FRAMES);
    }

    // The room's HUD (ifWifiprTask's interface object, the global at r13-0x4120 = 0x805A0300):
    // +0x18 the banner, +0x1C.. seven player panels ("Seeking...", the players' names), each a
    // small state machine (+8 its state; setState 0x800FD96C runs the new state's entry from the
    // table at 0x804090F0; 0 is none). The user does not want the panels in the room (2026-10-16):
    // every frame each one is put back to none; the banner stays.
    static const u32 WIFIPR_HUD = 0x805A0300;
    static void hidePanels()
    {
        typedef void (*SetStateFn)(void* panel, int state);
        u32 hud = *(u32*)WIFIPR_HUD;
        if (!isPtr(hud)) return;
        for (int i = 0; i < 7; i++) {
            u32 panel = *(u32*)(hud + 0x1C + 4 * i);
            if (isPtr(panel) && *(u32*)(panel + 8) != 0) ((SetStateFn)0x800FD96C)((void*)panel, 0);
        }
    }

    static void noteScene();
    void tick()
    {
        noteScene();
        if (!running()) return;
        s_roomFrames++;
        PPOM::g_block.debug.scratch[5] = (PPOM::g_block.debug.scratch[5] & ~0xFu) | ST_ROOM;
        hidePanels();
        printBanner();
        if (s_end == END_NONE) {
            tickSandbag();
            return;
        }
        // Ended: once the screen is black (the fade), the room's operator ends it, as when
        // Brawl's own search has found everyone (its result, below); asked again until it is
        // the operator's.
        s_stopFrames++;
        if (!fadedOut() && s_stopFrames < FADE_OUT_FRAMES + 30) return;   // (the fade is drawn per frame)
        u8* op = roomOperator();
        if (op && op[OP_ACTIVE]) {
            op[OP_MATCHED] = s_end == END_MATCH ? 1 : 0;
            op[OP_LEFT] = s_end == END_MATCH ? 0 : 1;
            op[OP_ACTIVE] = 0;
        }
    }

    // The room's player from this player's lock-in (the character, Random already drawn, and the
    // costume): Brawl's Wi-Fi CSS writes no selection when it leaves, and state 5 copies the
    // player from the CSS's record (gmSelCharData+0xB8: +0 character, +1 state, +5 colour).
    static void fillPlayer()
    {
        u32 gg = *(u32*)0x805A00E0;
        u32 selp = isPtr(gg) ? *(u32*)(gg + 0x10) : 0;
        if (!isPtr(selp)) return;
        const PPOM::LockIn& li = PPOM::g_block.local.lockIn;
        if (!li.ready || li.charKind >= 0x3E) return;
        u8* rec = (u8*)selp + 0xB8;
        rec[0x00] = li.charKind;
        rec[0x01] = 0;                                   // human
        rec[0x05] = li.costume & PPOM::COSTUME_COLOUR;   // colour
        // The controller that searched (LOCAL lockPad, 0-3), as the match's controller numbers
        // are (OnlineMatch's setup: 1-4). State 5 copies the record's byte as it is; the Wi-Fi
        // CSS leaves 0 there, which is no controller: the room's fighter stood still.
        const u8 pad = PPOM::g_block.local.lockPad;
        rec[0x07] = (u8)((pad < 4 ? pad : 0) + 1);
    }

    int onSelStage(u8* seq)
    {
        if (s_state != ST_CSS_LEFT) return 0;
        fillPlayer();
        keepSlotFlags();
        *(u32*)(seq + 8) = 5;   // Brawl's own room setup
        PPOM::g_block.debug.scratch[5] = (PPOM::g_block.debug.scratch[5] & ~0xFu) | ST_CSS_LEFT;
        return 1;
    }

    // ---- sora_scene hooks ----
    // At the end of state 5 (text+0x371C4 `li r0,8`, before it sets the load screen and state 6):
    // the room is set up.
    extern "C" void pptraining_afterSetup(u8* seq)
    {
        (void)seq;
        if (s_state != ST_CSS_LEFT) return;
        s_state = ST_SETUP;
        PPOM::g_block.debug.scratch[5] = (PPOM::g_block.debug.scratch[5] & ~0xFu) | ST_SETUP;
    }

    __attribute__((naked)) void hookAfterSetup()
    {
        asm volatile(
            "mr 3, 15\n\t"
            "lis 12, pptraining_afterSetup@ha\n\t"
            "addi 12, 12, pptraining_afterSetup@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "li 0, 8\n\t"
            "lis 12, 0x806F\n\t"
            "ori 12, 12, 0x271C\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // State 6 (text+0x371E0) starts the room's scMelee: the room runs once it is the scene.
    // State 7 (text+0x371FC `lwz r0,0x284(r19)`): the room's scMelee has ended. Ours goes on by
    // how it was ended; 1 = state set (loop), 0 = Brawl's own.
    extern "C" int pptraining_afterRoom(u8* seq)
    {
        if (s_state == ST_NONE) return 0;
        u8* mgr = (u8*)*(u32*)0x805A0060;
        int code = isPtr((u32)mgr) ? *(int*)(mgr + 0x284) : -1;
        const u8 how = s_end;
        s_state = ST_NONE;
        s_end = END_NONE;
        s_bannerMsg = NULL;
        PPOM::g_block.debug.scratch[5] = (PPOM::g_block.debug.scratch[5] & 0xFFFF0F00u) | 7 | ((u32)(code & 0xF) << 4);
        restoreSlotFlags();
        OnlineMenu::waitingRoomEnded(how);
        if (how == END_MATCH && g_onlineMatchGame) {
            *(u32*)(seq + 8) = 3;   // OnlineMatch's setup (onSelStage), then the match
            return 1;
        }
        // Back to the CSS (load screen type 0xD, as from a match: OnlineMatch::backToCss), the
        // character still selected.
        OnlineMenu::restoreCss();
        *(u32*)(seq + 8) = 0xE;
        *(u32*)(seq + 0xC) = 1;
        seq[0x11] = 0xD;
        return 1;
    }

    // As OnlineMatch's PPON_STATE_HOOK: r15 = the sequence; 1 = loop again (r17 = 1, to the loop
    // test at text+0x37794 = 0x806F2CE8), 0 = the original instruction and on.
    __attribute__((naked)) void hookState7()
    {
        asm volatile(
            "mr 3, 15\n\t"
            "lis 12, pptraining_afterRoom@ha\n\t"
            "addi 12, 12, pptraining_afterRoom@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "cmpwi 3, 0\n\t"
            "beq 1f\n\t"
            "li 17, 1\n\t"
            "lis 12, 0x806F\n\t"
            "ori 12, 12, 0x2CE8\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "1:\n\t"
            "lwz 0, 0x284(19)\n\t"
            "lis 12, 0x806F\n\t"
            "ori 12, 12, 0x2754\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // ---- The stage module's name (bug reports #31, #32, #33, 2026-10-16) ----
    // sora_melee's stage loader (text+0x23F5C0) keeps the stage module's name at +0x88 from
    // moGetStageModuleName (text+0x23F734), loads that module, and when its stage is torn down
    // destroys the module by that name (text+0x23FA54, gfModuleManager::destroy). P+'s STEX hook
    // (Net-StageFiles.asm, 0x80043B28) returns a pointer into the stage's param file, which every
    // stage reads into the same buffer (0x8053F000). Unranked's room ends straight into game 1,
    // and game 1's setup reads its param before the room's scMelee is torn down: the room's loader
    // then destroyed game 1's module (not loaded) instead of its own, the room's st_mansion.rel
    // stayed loaded, and the next room did not load it again but had no stage class for it (the
    // stage factory, text+0x221A44, read through NULL: the game hung as the room loaded). The
    // loader gets its own copy of the name: it destroys the module it loaded.
    static char s_stageModuleNames[8][40];   // (P+'s names are at most 32 characters)
    static u8 s_stageModuleNext;

    extern "C" const char* pptraining_keepStageModuleName(const char* name)
    {
        if (!isPtr((u32)name)) return name;
        char* copy = s_stageModuleNames[s_stageModuleNext++ & 7];
        strncpy(copy, name, sizeof(s_stageModuleNames[0]) - 1);
        copy[sizeof(s_stageModuleNames[0]) - 1] = 0;
        return copy;
    }

    // text+0x23F73C (0x8094A150) `stw r3,0x88(r31)`, after `cmpwi r3,0`: the beq after it reads
    // cr0. r31 is the loader; nothing else volatile is live; the function saved LR.
    __attribute__((naked)) void hookStageModuleName()
    {
        asm volatile(
            "lis 12, pptraining_keepStageModuleName@ha\n\t"
            "addi 12, 12, pptraining_keepStageModuleName@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "stw 3, 0x88(31)\n\t"
            "cmpwi 3, 0\n\t"
            "lis 12, 0x8094\n\t"
            "ori 12, 12, 0xA154\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // The room's scMelee is the scene: the room runs (state 6 set it as the next scene).
    static void noteScene()
    {
        if (s_state == ST_SETUP && Online::inScene("scMelee")) {
            s_state = ST_ROOM;
            s_roomFrames = 0;
        }
    }

    void install(CoreApi* api)
    {
        api->sySimpleHookRel(0x000371C4, reinterpret_cast<void*>(hookAfterSetup), 1 /* SORA_SCENE */);
        api->sySimpleHookRel(0x000371FC, reinterpret_cast<void*>(hookState7), 1);
        api->sySimpleHookRel(0x0023F73C, reinterpret_cast<void*>(hookStageModuleName), 27 /* SORA_MELEE */);
    }
}
