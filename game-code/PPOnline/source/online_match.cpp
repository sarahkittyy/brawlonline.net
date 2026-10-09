// Online matches started from the online character select (docs/backend-design.md 5.1 C, the
// gameplay-only session; docs/game-code.md "Online matches").
//
// Slippi's flow, in Brawl's Wi-Fi scene sequence (sqNetAnyOkiraku, sora_scene module 1). Its
// scene-decide function (text+0x36D74) is a state machine on seq+8 (jump table 0x80703B3C):
//    1/2   scSelctCharacter, then its exit code (gfSceneManager+0x284): 1 = go on (state 3)
//    3/4   scSelStage ("vote"), then its exit code
//    5     training room setup (Brawl's own "waiting for others" room) -> 6/7 scMelee
//    8     the real Wi-Fi match setup from the network -> 9/10 scMelee
//    11-13 scVsResult, then back to 1 (the CSS)
//    14/15 scMemoryChange (load screen) of type seq+0x11, then seq+0xC is the next state
//    16    back to the menus (sqMenuMain)
//
// What we change, only while our online flow owns the CSS (g_onlineCss):
//  - the CSS (online_menu.cpp) leaves with exit code 1 once SESSION has the setup of the game it
//    is locked in for (or to the stage select, for Direct's loser's pick);
//  - state 3: no stage select for a match: the match is set up at once (setupMatch);
//  - state 4: after the loser's stage select: back to the CSS, locked in with that stage
//    (Slippi's ExitSSSUponStageSelect);
//  - state 5 (never reached in our flow; kept as a fallback): set up the match;
//  - state 10: after the match: straight back to the CSS, no results screen (Slippi's
//    VSSceneDecide: "default to going back to CSS" for Unranked/Direct).
//
// The match itself is an ordinary Versus match built by the Versus sequence's own setup
// function (sqVsMelee, sora_scene text+0x218E40 = 0x806DCE94, which P+ also patches): from
// gmSelCharData (the players, which we fill from SESSION in in-game port order: the host is P1)
// and the rules (OnlineMenu::applyRules). Both machines build it from the same SESSION, so the
// setup the gameplay session compares at its barrier is the same.
#include <memory.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "stage_legal.h"

extern "C" {
    extern u8 g_onlineCss;
    // The game number of the match the online CSS started (0 = none); set by the CSS when it
    // leaves for the match, read by the sequence hooks below.
    u8 g_onlineMatchGame = 0;
    // The CSS left for the stage select for Direct's loser's pick.
    u8 g_onlinePickStage = 0;
}

namespace OnlineMatch {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    static u8* gameGlobal()
    {
        u32 gg = *(u32*)0x805A00E0;
        return isPtr(gg) ? (u8*)gg : NULL;
    }
    static u8* gg(u32 off)
    {
        u8* g = gameGlobal();
        if (!g) return NULL;
        u32 p = *(u32*)(g + off);
        return isPtr(p) ? (u8*)p : NULL;
    }
    static u8* sceneManager()
    {
        u32 m = *(u32*)0x805A0060;
        return isPtr(m) ? (u8*)m : NULL;
    }

    static const u32 SEL_PLAYERS = 0xB8;       // gmSelCharData: gmPlayerInitData[7] at +0xB8
    static const u32 PLAYER_SIZE = 0x5C;
    static const u32 MM_PLAYERS = 0x98;        // gmGlobalModeMelee players
    static const u32 ASL_BUTTONS = 0x800B9EA2; // P+: buttons held on the stage select (u16)

    // gmSelCharData of the local CSS, saved before the match overwrites it and put back after,
    // so the CSS comes back with this player's own character and not the host's.
    static u8 s_savedSel[4 * PLAYER_SIZE];
    static bool s_haveSaved = false;
    static u16 s_pickedStage = 0xFFFF;
    static u8 s_pickedAsl = 0;
    static int s_discFrames = -1;   // frames since the opponent was lost in a match

    // Each port's controls for this match (SESSION's port values, kept at the setup: SESSION is
    // the same on both machines, so are these). Applied by the ipPadConfig hook below.
    static u8 s_portLayout[4][PPOM::LAYOUT_SIZE];
    static u8 s_portHasLayout[4];

    // A layout from another machine reaches the game only as its own menus could have set it
    // (Orca's NameTags.cpp LayoutByte): actions up to 0xE ("none"), the flag bytes only their
    // flags (GameCube and Classic tap jump 0x80 plus P+'s "controls set" 0x70; Nunchuk 0xC3),
    // else the game's default byte. Every machine sanitises the same bytes the same way.
    static u8 layoutByte(int i, u8 v, u8 def)
    {
        if (i == 0x0B || i == 0x2C) return v & 0xF0;   // GameCube / Classic flags
        if (i == 0x1F) return v & 0xC3;                // Nunchuk flags
        return v <= 0x0E ? v : def;
    }

    static void keepPortValues(const PPOM::Session& se)
    {
        const u8* def = (const u8*)0x80406938;   // the game's default layout (new tags copy it)
        PPOM::g_block.debug.scratch[13] = 0;
        for (int i = 0; i < 4; i++) {
            const PPOM::PortValues& pv = se.players[i].pv;
            s_portHasLayout[i] = (i < se.numPlayers && se.players[i].present && (pv.flags & PPOM::PV_TAG)) ? 1 : 0;
            for (int k = 0; k < PPOM::LAYOUT_SIZE; k++) {
                s_portLayout[i][k] = s_portHasLayout[i] ? layoutByte(k, pv.layout[k], def[k]) : def[k];
            }
        }
    }

    typedef void (*VsSetupFn)(void* seq, int stageKind);
    static const u32 VS_SETUP = 0x806DCE94;    // sqVsMelee's match setup (sora_scene)

    typedef void (*PlaySEFn)(void*, int, int, int, int, int);
    static void playSE(int id)
    {
        void* snd = *(void**)0x805A01D0;   // g_sndSystem
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }

    static void memoryChangeTo(u8* seq, u32 next, u8 type)
    {
        *(u32*)(seq + 8) = 0xE;      // scMemoryChange ...
        *(u32*)(seq + 0xC) = next;   // ... then this state
        seq[0x11] = type;
    }

    u16 pickedStage() { return s_pickedStage; }
    u8 pickedAsl() { return s_pickedAsl; }
    void clearPickedStage() { s_pickedStage = 0xFFFF; s_pickedAsl = 0; }

    // Fill gmSelCharData from SESSION and run the Versus setup.
    static bool setupMatch(u8* seq)
    {
        PPOM::Session& se = PPOM::g_block.session;
        u8* sel = gg(0x10);
        u8* mm = gg(0x08);
        if (!sel || !mm || se.state != PPOM::SS_MATCH_READY || se.game != g_onlineMatchGame) {
            PPOM::g_block.debug.lastError = 0x5E70;   // "setup": no ready session
            return false;
        }
        // SESSION's setup is the host's decision and its characters, costumes and stage are the
        // players' lock-ins: another machine's bytes. The game indexes its tables with them, so
        // only what this game's own menus could produce is used (Dolphin checks the same ranges
        // when it receives them, Gprb::PeerData). Anything else: no match, back to the CSS (the
        // other machine then fails the barrier's setup check and the session ends).
        if (!StageLegal::selectableKind(se.stageKind) || se.numPlayers < 1 || se.numPlayers > 4) {
            PPOM::g_block.debug.lastError = 0x5E71;   // "setup": a stage this SSS cannot pick
            return false;
        }
        for (int i = 0; i < se.numPlayers; i++) {
            if (!se.players[i].present) continue;
            if (!OnlineMenu::selectableCharKind(se.players[i].charKind) || se.players[i].costume >= 0x20) {
                PPOM::g_block.debug.lastError = 0x5E72;   // "setup": a character this CSS cannot pick
                return false;
            }
        }
        if (!s_haveSaved) {
            memcpy(s_savedSel, sel + SEL_PLAYERS, sizeof(s_savedSel));
            s_haveSaved = true;
        }
        // Every player record starts from the same fixed template on every machine: the empty
        // record the online CSS starts with (no character, no name tag: m_nameIndex 0x78), so
        // that nothing of this machine's own CSS (a name tag and its controls, rumble) gets into
        // the match. Per-player name tags and controls would be the design's port values (5.1),
        // not done yet: every player has the default controls.
        static const u8 TEMPLATE[PLAYER_SIZE] = {
            0x3E, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x78, 0, 0, 0, 0, 0, 0, 0x08, 0, 0, 0, 0, 0, 0x96};
        u8 tmpl[PLAYER_SIZE];
        memcpy(tmpl, TEMPLATE, PLAYER_SIZE);
        for (int i = 0; i < 4; i++) {
            u8* p = sel + SEL_PLAYERS + i * PLAYER_SIZE;
            memcpy(p, tmpl, PLAYER_SIZE);
            if (i < se.numPlayers && se.players[i].present) {
                p[0x00] = se.players[i].charKind;
                p[0x01] = 0;                         // human
                p[0x05] = se.players[i].costume;     // colour number
                p[0x07] = (u8)i;                     // controller
                p[0x0B] = 0;                         // team
            } else {
                p[0x00] = 0x28;
                p[0x01] = 3;                         // none
            }
        }
        *(u16*)ASL_BUTTONS = se.asl;
        keepPortValues(se);
        PPOM::g_block.local.hudDisconnected = 0;
        // The online ruleset, written again right before the setup reads it: nothing set on this
        // machine since the Wi-Fi sequence started (or left over from offline play) gets in.
        OnlineMenu::applyRules();
        ((VsSetupFn)VS_SETUP)(seq, se.stageKind);
        // The match's controller numbers follow the in-game ports on both machines.
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            mm[MM_PLAYERS + i * PLAYER_SIZE + 0x07] = (u8)(i + 1);
        }
        // Colour clash (Slippi's prepareOnlineMatchState: the players are counted in port order
        // and each one with the same character and colour as an earlier one gets the next shade).
        // Brawl has the shades for team battles (two of a character on one team): gmPlayerInitData
        // +0x0A is the shade, and the fighter's colour blend module draws it (sub colour, alpha
        // 0x80) and sets it again after a respawn. 3 is the lighter one, as Slippi's first shade;
        // then darker (1) and grey (2). The same SESSION on both machines gives the same shades.
        static const u8 SHADES[4] = {0, 3, 1, 2};
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            if (!se.players[i].present) continue;
            int n = 0;
            for (int j = 0; j < i; j++) {
                if (se.players[j].present && se.players[j].charKind == se.players[i].charKind &&
                    se.players[j].costume == se.players[i].costume) n++;
            }
            mm[MM_PLAYERS + i * PLAYER_SIZE + 0x0A] = SHADES[n];
        }
        PPOM::g_block.debug.scratch[11] = ((u32)se.stageKind << 16) |
                                          ((u32)se.players[0].charKind << 8) | se.players[1].charKind;
        memoryChangeTo(seq, 9, 0);   // then state 9: scMelee
        s_discFrames = -1;
        return true;
    }

    // Back from a match (or the loser's stage select): the CSS with this player's own record.
    static void backToCss(u8* seq)
    {
        u8* sel = gg(0x10);
        if (sel && s_haveSaved) memcpy(sel + SEL_PLAYERS, s_savedSel, sizeof(s_savedSel));
        s_haveSaved = false;
        OnlineMenu::restoreCss();
        memoryChangeTo(seq, 1, 0xD);
    }

    typedef void (*SetNextSceneFn)(void* mgr, const char* name, int arg);
    typedef void (*VoidFn)();

    int onSelStage(u8* seq)
    {
        if (g_onlineCss && g_onlinePickStage && !g_onlineMatchGame) {
            // Direct's loser picks the stage on P+'s stage select, opened as the Versus sequence
            // opens it (sqVsMelee state 5: text+0x3D2D8 then setNextScene("scSelStage", 0)). The
            // Wi-Fi sequence opens it with 1, Brawl's network stage vote, which waits for the
            // other players' votes forever.
            u8* mgr = sceneManager();
            u8* sel = gg(0x10);
            if (!mgr || !sel) return 0;
            // gmSelCharData+4 is the menus' mode: 0x10 Wi-Fi (sqNetAnyOkiraku state 1 sets it on
            // every CSS entry), 0 Versus (sqVsMelee state 3). The stage select runs its Wi-Fi
            // paths on 0x10 even when opened with 0.
            sel[4] = 0;
            ((VoidFn)0x806F882C)();
            ((SetNextSceneFn)0x8002D5AC)(mgr, "scSelStage", 0);
            *(u32*)(seq + 8) = 4;
            PPOM::g_block.debug.scratch[10] = (PPOM::g_block.debug.scratch[10] & ~0xFF) | 0x33;
            return 2;   // state set; leave the decide function (the stage select runs now)
        }
        if (!g_onlineCss) return 0;
        if (!g_onlineMatchGame) {
            // The CSS was left by the game's own path (its READY TO FIGHT start: A on the banner,
            // or any other way), not by ours: no match and no stage pick is armed. Brawl's state
            // 3 would open its network stage vote (scSelStage with 1 in Wi-Fi mode), which hangs
            // the game. Online the game's own start never runs: straight back to the CSS.
            PPOM::g_block.debug.scratch[10] = (PPOM::g_block.debug.scratch[10] & ~0xFF) | 0x3F;
            PPOM::g_block.debug.lastError = 0x5E7F;   // "setup": the CSS left by its own start
            backToCss(seq);
            return 1;
        }
        PPOM::g_block.debug.scratch[10] = (PPOM::g_block.debug.scratch[10] & ~0xFF) | 3;
        if (setupMatch(seq)) return 1;
        // No setup (should not happen): back to the CSS instead of Brawl's stage vote.
        g_onlineMatchGame = 0;
        backToCss(seq);
        return 1;
    }

    int afterSelStage(u8* seq)
    {
        if (!g_onlineCss || !g_onlinePickStage) return 0;
        g_onlinePickStage = 0;
        u8* mgr = sceneManager();
        int code = mgr ? *(int*)(mgr + 0x284) : 0;
        PPOM::g_block.debug.scratch[10] = (PPOM::g_block.debug.scratch[10] & ~0xFF) | 4 | (code << 8);
        // Ranked: the steps decide the stage (GameSetup); the stage select's own pick is not kept.
        if ((code == 1 || code == 3) && OnlineMenu::currentMode() != PPOM::MODE_RANKED) {
            // The stage select keeps its choice where sqVsMelee's setup reads it (GameGlobal+0x14,
            // +0x22); P+ keeps the alternate-stage buttons at 0x800B9EA2.
            u8* st = gg(0x14);
            s_pickedStage = st ? *(u16*)(st + 0x22) : 0xFFFF;
            s_pickedAsl = (u8)*(u16*)ASL_BUTTONS;
        }
        backToCss(seq);
        return 1;
    }

    int onSetup(u8* seq)
    {
        if (!g_onlineCss || !g_onlineMatchGame) return 0;
        return setupMatch(seq) ? 1 : 0;
    }

    int afterMatch(u8* seq)
    {
        if (!g_onlineCss || !g_onlineMatchGame) return 0;
        PPOM::g_block.debug.scratch[10] = (PPOM::g_block.debug.scratch[10] & ~0xFF) | 10;
        g_onlineMatchGame = 0;
        s_discFrames = -1;
        backToCss(seq);
        return 1;
    }

    // The match's stOperatorInfoMelee (scMelee+0x68, sora_melee vtable data+0x53100): its flags
    // byte +0x11B is 0x80 running, 0x40 stop requested (its stopOperatorRequest, vtable slot 24 =
    // text+0x257574), 0x20|0x10 quit by the pause screen's L+R+A+START (text+0x256F60). Its
    // process (text+0x256B70) ends the match on the next frame when 0x40 is set (text+0x25748C)
    // and getEndType (text+0x257534) then reads 3: no "GAME!", no announcer, results decision 9
    // (no contest), and the scene leaves scMelee about 17 frames later through Brawl's ordinary end
    // (scMemoryChange, then the sequence's next state: state 10, afterMatch, online).
    // Leaving the scene directly (the scene manager's exit code) hangs in scMelee's teardown.
    static u8* operatorInfo()
    {
        u32 mgr = *(u32*)0x805A0060;
        u32 scene = isPtr(mgr) ? *(u32*)(mgr + 4) : 0;
        u32 oi = isPtr(scene) ? *(u32*)(scene + 0x68) : 0;
        if (!isPtr(oi)) return NULL;
        u32 name = *(u32*)oi;   // gfTask::m_taskName
        if (!isPtr(name) || strncmp((const char*)name, "StOperatorInfoMelee", 19) != 0) return NULL;
        return (u8*)oi;
    }

    static const int DISC_END_FRAMES = 90;   // Slippi: the end screen of an LRAS-type end, 1.5 s

    // In a match (scMelee), every frame, also in resimulated frames: nothing here may depend on
    // anything that differs between the machines while the match runs under rollback. LOCAL's
    // `disconnected` only changes once Dolphin has ended the rollback session.
    // DEBUG cfg CFG_TEST_DISCONNECT (tests): act as if disconnected now, in any scMelee (also a
    // local Versus match); cleared when seen.
    void tickMatch()
    {
        PPOM::Debug& dbg = PPOM::g_block.debug;
        bool test = (dbg.cfg & PPOM::CFG_TEST_DISCONNECT) != 0;
        if (test && s_discFrames >= DISC_END_FRAMES) s_discFrames = -1;   // a new test
        if (s_discFrames >= 0) return;   // counted in onFrameDrawn
        if (!test) {
            if (!g_onlineCss || !g_onlineMatchGame || !PPOM::g_block.local.disconnected) return;
        }
        dbg.cfg &= ~(u32)PPOM::CFG_TEST_DISCONNECT;
        // Slippi (design 5.6): the error sound, DISCONNECTED in red at the top, the game ends as
        // an LRAS-type end (no "GAME!") after a 90-frame end screen, then the CSS (state 10,
        // afterMatch) without a results screen.
        s_discFrames = 0;
        playSE(3);
        MatchHud::show(true);
        dbg.scratch[10] |= 0x10000;
    }

    // Every drawn frame (MatchHud's frame hook in gfApplication's loop): the 90-frame end screen,
    // then the LRAS-type end.
    void onFrameDrawn()
    {
        if (s_discFrames < 0 || s_discFrames >= DISC_END_FRAMES) return;
        if (!Online::inScene("scMelee")) { s_discFrames = DISC_END_FRAMES; return; }
        PPOM::Debug& dbg = PPOM::g_block.debug;
        if (++s_discFrames < DISC_END_FRAMES) return;
        u8* oi = operatorInfo();
        if (!oi) { dbg.lastError = 0xD15C; s_discFrames--; return; }   // try again next frame
        oi[0x11B] |= 0x70;   // quit (0x30, as L+R+A+START) and stop now (0x40)
        dbg.scratch[10] |= 0x20000;
    }

    bool disconnectShown() { return s_discFrames >= 0; }

    // ---- Each player's own controls (the design's port values) ----
    // At the start of a match the game gives every player's controller its layout:
    // fn_801104C4(gmGlobalModeMelee*) calls ipPadConfig's setter fn_8004A2C8(g_PadConfig, player,
    // pad, layout) for players 0-3 at 0x80110550, with the layout of the player's name tag
    // (gmPlayerInitData+0x18) or the default layout. The layout is copied into g_PadConfig
    // (0x805B7480: GameCube pads 12 bytes each at pad * 0xC, Wii Remotes 0x21 bytes at 0x30), which
    // every frame's input goes through. Online, the match has no name tags (both machines build
    // the same records, online_match setupMatch), so here each port gets its player's layout
    // from SESSION instead, the same on both machines. Outside online matches nothing changes.
    typedef void (*PadConfigSetFn)(void* cfg, int player, int pad, const u8* layout);
    extern "C" void pponline_setPadConfig(void* cfg, int player, int pad, const u8* layout)
    {
        if (g_onlineCss && g_onlineMatchGame && player >= 0 && player < 4 && s_portHasLayout[player]) {
            layout = s_portLayout[player];
            PPOM::g_block.debug.scratch[13] |= 1u << player;   // tests: a player's layout applied
        }
        ((PadConfigSetFn)0x8004A2C8)(cfg, player, pad, layout);
    }

    // 0x80110550 `bl fn_8004A2C8` (DOL; nothing of P+'s codeset is near it). The call's arguments
    // are in r3-r6; the function saved LR in its prologue, so bctrl may clobber it.
    __attribute__((naked)) void hookPadConfigSet()
    {
        asm volatile(
            "lis 12, pponline_setPadConfig@ha\n\t"
            "addi 12, 12, pponline_setPadConfig@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x8011\n\t"
            "ori 12, 12, 0x0554\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // ---- sora_scene hooks (module hooks: the module is loaded after the plugins) ----
    // Each runs at the first instruction of a state of the decide function, where only its
    // non-volatile registers are live (r15 = the sequence, r17 = "loop again", r19 = the scene
    // manager). If our C function returns 1 it has set the next state: loop (r17 = 1, to the
    // loop test at text+0x37794 = 0x806F2CE8); 2: it has set the next state and a scene to run,
    // so leave the function (r17 = 0, as the states' own code does); 0: run the original
    // instruction and go on.
#define PPON_STATE_HOOK(name, cfunc, origInstr, backHi, backLo)            \
    __attribute__((naked)) void name()                                      \
    {                                                                       \
        asm volatile(                                                       \
            "mr 3, 15\n\t"                                                  \
            "lis 12, " #cfunc "@ha\n\t"                                     \
            "addi 12, 12, " #cfunc "@l\n\t"                                 \
            "mtctr 12\n\t"                                                  \
            "bctrl\n\t"                                                     \
            "cmpwi 3, 0\n\t"                                                \
            "beq 1f\n\t"                                                    \
            "li 17, 1\n\t"                                                  \
            "cmpwi 3, 2\n\t"                                                \
            "bne 2f\n\t"                                                    \
            "li 17, 0\n\t"                                                  \
            "2:\n\t"                                                        \
            "lis 12, 0x806F\n\t"                                            \
            "ori 12, 12, 0x2CE8\n\t"                                        \
            "mtctr 12\n\t"                                                  \
            "bctr\n\t"                                                      \
            "1:\n\t" origInstr "\n\t"                                       \
            "lis 12, " #backHi "\n\t"                                       \
            "ori 12, 12, " #backLo "\n\t"                                   \
            "mtctr 12\n\t"                                                  \
            "bctr\n\t");                                                    \
    }
}

extern "C" {
    int pponline_seqState3(u8* seq) { return OnlineMatch::onSelStage(seq); }
    int pponline_seqState4(u8* seq) { return OnlineMatch::afterSelStage(seq); }
    int pponline_seqState5(u8* seq) { return OnlineMatch::onSetup(seq); }
    int pponline_seqState10(u8* seq) { return OnlineMatch::afterMatch(seq); }
}

namespace OnlineMatch {
    // state 3 at text+0x36F64 (0x806F24B8) `mr r3,r19`
    PPON_STATE_HOOK(hookState3, pponline_seqState3, "mr 3, 19", 0x806F, 0x24BC)
    // state 4 at text+0x36F8C (0x806F24E0) `lwz r0,0x284(r19)`
    PPON_STATE_HOOK(hookState4, pponline_seqState4, "lwz 0, 0x284(19)", 0x806F, 0x24E4)
    // state 5 at text+0x36FDC (0x806F2530) `lwz r3,0xE0(r26)`
    PPON_STATE_HOOK(hookState5, pponline_seqState5, "lwz 3, 0xE0(26)", 0x806F, 0x2534)
    // state 10 at text+0x37580 (0x806F2AD4) `lwz r0,0x284(r19)`
    PPON_STATE_HOOK(hookState10, pponline_seqState10, "lwz 0, 0x284(19)", 0x806F, 0x2AD8)

    void install(CoreApi* api)
    {
        MatchHud::install(api);
        api->sySimpleHookRel(0x00036F64, reinterpret_cast<void*>(hookState3), 1 /* SORA_SCENE */);
        api->sySimpleHookRel(0x00036F8C, reinterpret_cast<void*>(hookState4), 1);
        api->sySimpleHookRel(0x00036FDC, reinterpret_cast<void*>(hookState5), 1);
        api->sySimpleHookRel(0x00037580, reinterpret_cast<void*>(hookState10), 1);
        api->sySimpleHook(0x80110550, reinterpret_cast<void*>(hookPadConfigSet));
    }
}
