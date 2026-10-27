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
#include "training_room.h"

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
    static const u32 MM_TIME_LIMIT = 0x20;     // gmGlobalModeMelee: the time limit (s32 frames)
    static const u32 MM_HANDICAP = 0x26;       // gmPlayerInitData: the handicap (u16; P+: tenths of a stock)
    static const u32 MM_NAME = 0x0C;           // gmPlayerInitData: the name tag's name (wchar[5] + 0)
    static const u32 ASL_BUTTONS = 0x800B9EA2; // P+: buttons held on the stage select (u16)
    // P+'s alt costume word per slot (LegacyTE/AltCostume.asm: gmSelCharData + 0x43AD8 + slot *
    // 0x5C, which its Gecko lines read as 0x90180F20 + 0x436F8): 1 AltZ, 2 AltR, else none. Every
    // frame, while it is 1 or 2, P+ sets the slot's costume in gmGlobalModeMelee to 62 (AltZ) or
    // 61 (AltR), and the loader then takes Fit<Char>AltZ/AltR.pac. P+ writes the words when its
    // CSS is left; the online CSS never is, so setupMatch writes all four from SESSION.
    static const u32 ALT_COSTUME_WORDS = 0x901C4618;

    // gmSelCharData of the local CSS, saved before the match overwrites it and put back after,
    // so the CSS comes back with this player's own character and not the host's.
    static u8 s_savedSel[4 * PLAYER_SIZE];
    static u8 s_savedTeams = 0;
    static bool s_haveSaved = false;
    // gmSelCharData+0x33: the CSS's team battle switch; sqVsMelee's setup copies it into the
    // match (gmMeleeInitData.m_isTeams, sora_scene text+0x219A4). Each player's team is its
    // record's +0x0B, which the setup copies too.
    static const u32 SEL_TEAMS = 0x33;
    static u16 s_pickedStage = 0xFFFF;
    static u8 s_pickedAsl = 0;
    // The buttons held at the last A/START press on the online stage select (noteSssButtons). P+'s
    // ASL Helper (LegacyTE/ASLHelper.asm, a hook at 0x800B99FC) writes the held buttons to
    // ASL_BUTTONS only in sqVsMelee, sqSpMelee, sqTraining and a few other sequences, not in
    // sqNetAnyOkiraku, so online the address kept 0 (the stage load clears it) and every pick
    // was the base stage. Its low byte is what P+ v3.2's stage files select by: L 0x40,
    // R 0x20, Z 0x10 (the D-pad below them).
    static u8 s_sssAsl = 0;
    static int s_discFrames = -1;   // frames since the opponent was lost in a match
    // A pause-screen quit waiting for the rollback session to end (pponline_pauseResult).
    static u8* s_quitOp = NULL;   // the operator a quit is pending on
    static u8 s_quitKind = 0;     // the pause screen's result: 3 or 4
    static u16 s_quitFrames = 0;
    // This match's quits, for the game's own end (afterMatch): SESSION's quitBy, quitRunback and
    // quitPorts are rewritten by Dolphin as soon as the session ends, before the scene leaves the
    // match. Like the pending quit, these are in the rolled-back .bss.
    static u8 s_matchQuitBy = 0;      // SESSION quitBy of the quit that ended the match
    static u8 s_matchQuitRunback = 0;
    static u8 s_matchQuitPorts = 0;   // 3-4 players: the ports that quit
    // Unranked's quit without a pause (clientPause): marked on the operator, the match ended
    // once the session is over (tickMatch); frames since, for an end without one.
    static const u16 QUIT_MAX_FRAMES = 600;   // a quit's end without the session's (an older Dolphin)
    static u8 s_clientQuit = 0;
    static u16 s_clientQuitFrames = 0;
    static u8 s_hintPorts = 0;        // Unranked: the ports whose START shows the quit hint
    static u8 s_startPorts = 0;       // Unranked: the ports holding START last frame
    static u32 s_seen = 0;          // ports seen with stocks in this match (tickPlayers)
    static bool s_inMatch = false;  // a match ran since the last frame outside scMelee (offMatch)
    static bool s_restore = false;  // this match plays a desynced game again (SESSION restore)

    // Each port's controls for this match (SESSION's port values, kept at the setup: SESSION is
    // the same on both machines, so are these). Applied by the ipPadConfig hook below.
    static u8 s_portLayout[4][PPOM::LAYOUT_SIZE];
    static u8 s_portHasLayout[4];
    // Each port's controller in this match (SESSION's port values, the same on every machine):
    // its pad, the port's GameCube pad (0-3) or, for a player who locked in with a Wii Remote
    // (PV_WII), the port's Wii Remote pad (4-7). The match's controller number is the pad + 1.
    static u8 s_portPad[4] = {0, 1, 2, 3};

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

    // The character's costume list: its length and each costume's colour. P+ v3.2 keeps it in
    // the character's CSS slot table (0x80585B00 + slot * 0x10, +8: 2-byte entries whose first
    // byte is the costume's colour, ending with colour 0x0C; Legacy TE UnboundedTeamEngine.asm).
    static int costumeList(u8 charKind, u8* colours)
    {
        typedef int (*ToSelchFn)(int);
        int slot = ((ToSelchFn)0x800AF708)(charKind);   // exchangeGmCharacterKind2MuSelchkind
        u32 list = *(u32*)(0x80585B08 + ((u32)slot & 0xFF) * 0x10);
        if (!isPtr(list)) return 0;
        int n = 0;
        for (; n < 0x20; n++) {
            u8 c = *(u8*)(list + 2 * n);
            if (c == 0x0C) break;
            colours[n] = c;
        }
        return n;
    }

    // A player's costume in a team battle in the teams' costumes (the room's TEAMS, 2026-10-21; until
    // 2026-10-12 every room team battle): the one the CSS would give them for their team. The CSS
    // calls muMenu::findCharTeamColorNo (0x800AF520) with the CSS character, the team and P+'s "team
    // set" (which of the team's costumes), and P+ v3.2 replaces that function (Legacy TE
    // UnboundedTeamEngine.asm) with a walk over the character's costume list (costumeList); a
    // team's colour is the byte at r2 - 0x7130 + team (0x805A21F0: 0 red, 1 blue, 3 green). The
    // same walk here: the player's own costume if it already has the team's colour (they picked
    // that shade, e.g. P+'s second green), else the character's first costume of it.
    int teamShadeOf(u8 charKind, u8 team, u8 costume)
    {
        if (team >= PPOM::TEAM_COUNT) return -1;
        u8 colours[0x20];
        int n = costumeList(charKind, colours);
        u8 colour = *(u8*)(0x805A21F0 + team);
        if (costume >= n || colours[costume] != colour) return -1;
        int shade = 0;
        for (int i = 0; i < costume; i++) {
            if (colours[i] == colour) shade++;
        }
        return shade;
    }

    u8 teamShadeCostume(u8 charKind, u8 team, int shade, u8 costume)
    {
        if (team >= PPOM::TEAM_COUNT) return costume;
        u8 colours[0x20];
        int n = costumeList(charKind, colours);
        u8 colour = *(u8*)(0x805A21F0 + team);
        int count = 0;
        for (int i = 0; i < n; i++) {
            if (colours[i] == colour) count++;
        }
        if (!count) return costume;
        shade %= count;
        if (shade < 0) shade += count;
        for (int i = 0; i < n; i++) {
            if (colours[i] == colour && shade-- == 0) return (u8)i;
        }
        return costume;
    }

    u8 teamCostume(u8 charKind, u8 team, u8 costume)
    {
        if (team >= PPOM::TEAM_COUNT) return costume;
        u8 colours[0x20];
        int n = costumeList(charKind, colours);
        u8 colour = *(u8*)(0x805A21F0 + team);
        if (costume < n && colours[costume] == colour) return costume;
        for (int i = 0; i < n; i++) {
            if (colours[i] == colour) return (u8)i;
        }
        return costume;
    }

    // Two players with the same character and costume (staging feedback 2026-10-10; in a team battle
    // too, since its costumes are the players' own, 2026-10-12): P+'s Versus never shows that. Its CSS
    // does not let a costume another player has be picked: X skips it, and a player taking a character
    // whose costume is taken gets the next free one (found live: three Game & Watch on P1-P3 came out
    // 0, 1, 2; cycling skipped the others'). So the later player (in port order) gets the next costume
    // in the character's list that no other player of that character has, of the same colour if the
    // list has one (P+'s variants: a second green), else the next free one. Deterministic: the same
    // SESSION gives the same costumes on every machine. Only if the list has none left (fewer costumes
    // than players) is the shading below the fallback.
    // P+'s alts (PPOM::COSTUME_ALT_Z / _R in `alt`): an alt is one look whatever the colour number
    // (P+ sets the costume to 62 / 61), so it clashes only with the same alt of the same character
    // and is never moved to another costume: two of them are told apart by the shading below, as
    // Slippi's colour clash (offline P+ lets two players hold Z on one character too).
    static bool sameLook(const u8* costume, const u8* alt, int i, int j)
    {
        return alt[i] == alt[j] && (alt[i] != 0 || costume[i] == costume[j]);
    }
    static void distinctCostumes(const PPOM::Session& se, u8* costume, const u8* alt)
    {
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            if (!se.players[i].present || alt[i]) continue;
            u8 kind = se.players[i].charKind;
            bool clash = false;
            for (int j = 0; j < i; j++) {
                if (se.players[j].present && se.players[j].charKind == kind && sameLook(costume, alt, i, j)) clash = true;
            }
            if (!clash) continue;
            u8 colours[0x20];
            int n = costumeList(kind, colours);
            if (n <= 1 || costume[i] >= n) continue;
            int pick = -1;
            for (int pass = 0; pass < 2 && pick < 0; pass++) {
                for (int d = 1; d < n && pick < 0; d++) {
                    int c = (costume[i] + d) % n;
                    if (pass == 0 && colours[c] != colours[costume[i]]) continue;
                    bool used = false;
                    for (int j = 0; j < se.numPlayers && j < 4; j++) {
                        if (j != i && se.players[j].present && se.players[j].charKind == kind && !alt[j] && costume[j] == c)
                            used = true;
                    }
                    if (!used) pick = c;
                }
            }
            if (pick >= 0) costume[i] = (u8)pick;
        }
    }

    u16 pickedStage() { return s_pickedStage; }
    u8 pickedAsl() { return s_pickedAsl; }

    // Every place that loads fighters from the online CSS writes all four (the match setup, the
    // waiting room): P+ applies them every frame, and what the last match left would come back.
    void setAltCostumes(const u8* alt)
    {
        for (int i = 0; i < 4; i++) {
            *(volatile u32*)(ALT_COSTUME_WORDS + i * PLAYER_SIZE) =
                alt[i] == PPOM::COSTUME_ALT_Z ? 1 : alt[i] == PPOM::COSTUME_ALT_R ? 2 : 0;
        }
    }
    void clearPickedStage() { s_pickedStage = 0xFFFF; s_pickedAsl = 0; }
    void noteSssButtons(u32 held, u32 pressed)
    {
        if (g_onlineCss && g_onlinePickStage && (pressed & (0x100u | 0x1000u))) s_sssAsl = (u8)held;
    }
    u8 sssAsl() { return s_sssAsl; }

    // P+'s slot colours for the match (online_menu.cpp, PortValues.slotColour), the same on every
    // machine: each player's pick on their port, a player without one their port's own colour,
    // or, when a pick has it (P1 picked blue, P2 did not), the first of red, blue, yellow, green
    // nobody has. Two who picked the same colour keep it, as offline. A team battle is drawn in
    // the team colours whatever the lines say (P+'s code reads their defaults then).
    static void slotColours(const PPOM::Session& se)
    {
        s8 colour[4] = {-1, -1, -1, -1};
        bool used[PPOM::SLOT_COLOURS] = {};
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            u8 c = se.players[i].pv.slotColour;
            if (!se.players[i].present || c == 0 || c > PPOM::SLOT_COLOURS) continue;
            colour[i] = (s8)(c - 1);
            used[c - 1] = true;
        }
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            if (!se.players[i].present || colour[i] >= 0) continue;
            int c = i;
            for (int d = 0; d < 4 && used[c]; d++) c = d;
            colour[i] = (s8)c;
            used[c] = true;
        }
        OnlineMenu::setSlotColours(colour);
    }

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
        // 2 to 4 players on their own ports (gaps allowed: P1, P3, P4), a free-for-all or a team
        // battle, where every player has one of Brawl's three team colours (Dolphin refuses a
        // team battle with everyone on one colour, "Pick different teams", before it gets here).
        const bool teams = se.teams != 0;
        // The room's TEAMS in the teams' costumes: every player in their team's (no alts, P+'s
        // AltZ / AltR having no team colours), and two of a character on one team the game's
        // shading tells apart, as P+'s plain team battle.
        const bool teamColours = se.teams == PPOM::TEAMS_COLORS;
        for (int i = 0; i < se.numPlayers; i++) {
            const PPOM::SessionPlayer& pl = se.players[i];
            if (!pl.present) continue;
            if (!OnlineMenu::selectableCharKind(pl.charKind) || (pl.costume & ~PPOM::COSTUME_ALTS) > PPOM::COSTUME_COLOUR ||
                (pl.costume & PPOM::COSTUME_ALTS) == PPOM::COSTUME_ALTS ||
                (teams && pl.team >= PPOM::TEAM_COUNT)) {
                PPOM::g_block.debug.lastError = 0x5E72;   // "setup": a character or team this CSS cannot pick
                return false;
            }
        }
        if (!s_haveSaved) {
            memcpy(s_savedSel, sel + SEL_PLAYERS, sizeof(s_savedSel));
            s_savedTeams = sel[SEL_TEAMS];
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
        u8 costume[4];
        u8 alt[4] = {0, 0, 0, 0};
        // Each port's controller (s_portPad): the port's GameCube pad, or its Wii Remote pad for
        // a player who locked in with a Wii Remote (Dolphin writes each player's input into that
        // pad by its controller type). The Versus setup takes the record's controller (+0x07)
        // into the match player's controller numbers, and the game's controls setter
        // (fn_8004A2C8) gives a Wii Remote pad the Wii layouts of the player's tag.
        for (int i = 0; i < 4; i++) {
            const bool wii = i < se.numPlayers && (se.players[i].pv.controller & PPOM::PV_WII);
            s_portPad[i] = (u8)(wii ? i + 4 : i);
        }
        for (int i = 0; i < 4; i++) {
            u8* p = sel + SEL_PLAYERS + i * PLAYER_SIZE;
            memcpy(p, tmpl, PLAYER_SIZE);
            se.players[i].out = 0;
            se.gone[i] = 0;
            if (i < se.numPlayers && se.players[i].present) {
                const PPOM::SessionPlayer& pl = se.players[i];
                // The player's own costume, in a team battle too (the user's, 2026-10-12: any
                // costume on any team; Brawl's team battle CSS would give the team's).
                costume[i] = pl.costume & PPOM::COSTUME_COLOUR;
                alt[i] = pl.costume & PPOM::COSTUME_ALTS;
                if (teamColours) {
                    costume[i] = teamCostume(pl.charKind, pl.team, costume[i]);
                    alt[i] = 0;
                }
                p[0x00] = pl.charKind;
                p[0x01] = 0;                         // human
                p[0x05] = costume[i];                // colour number
                p[0x07] = s_portPad[i];              // controller (its pad)
                p[0x0B] = teams ? pl.team : 0;       // team
            } else {
                p[0x00] = 0x28;
                p[0x01] = 3;                         // none
            }
        }
        if (!teamColours) distinctCostumes(se, costume, alt);
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            if (se.players[i].present) sel[SEL_PLAYERS + i * PLAYER_SIZE + 0x05] = costume[i];
        }
        // P+'s alt words for all four slots, the same on every machine: an empty slot and a player
        // without an alt 0. (Left over from offline play, P+'s CSS leaves 2 in empty slots and the
        // last offline pick's in the others, which gave this machine alone an alt costume.) Offline
        // play does not read them back: P+'s CSS writes all four again when it is left.
        setAltCostumes(alt);
        // P+'s team battle kind (Project+/Teams.asm, which its CSS writes here from task+0x5CB): 1
        // its plain team battle, 2 its rainbow one, where Fighter::start and ::restart give each
        // fighter its team's glow (effect 0x88 + team), the outline of the user's request
        // (2026-10-21). Any non-zero is a team battle to the Versus setup. A replay recorded
        // before then (TEAMS_LEGACY) plays as it was: 1, no glow.
        sel[SEL_TEAMS] = se.teams == PPOM::TEAMS_RAINBOW ? 2 : teams ? 1 : 0;
        se.outCount = 0;
        se.quitPorts = 0;
        se.quitStays = 0;
        se.quitBy = 0;
        se.quitRunback = 0;
        s_seen = 0;
        *(u16*)ASL_BUTTONS = se.asl;
        keepPortValues(se);
        PPOM::g_block.local.hudDisconnected = 0;
        // The online ruleset, written again right before the setup reads it: nothing set on this
        // machine since the Wi-Fi sequence started (or left over from offline play) gets in.
        // (Team attack is on in P+'s set rule; it only matters in a team battle.)
        OnlineMenu::applyRules(se.stageKind);
        slotColours(se);
        ((VsSetupFn)VS_SETUP)(seq, se.stageKind);
        // The match's controller numbers follow the in-game ports on both machines: the pad + 1
        // (gmPlayerInitData +0x07: GameCube 1-4, Wii Remotes 5-8).
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            if (se.players[i].present) mm[MM_PLAYERS + i * PLAYER_SIZE + 0x07] = (u8)(s_portPad[i] + 1);
        }
        // Each player's name tag over their fighter, as offline (Sarah, 2026-10-23): the tag picked
        // on the CSS, carried by the lock-in's port values, as the match player's name
        // (gmPlayerInitData +0x0C, 5 UTF-16 characters and a 0), which the HUD renders into the
        // tag over the fighter (fn_800E10B0; match_hud.cpp makes sure it is drawn). The tag index
        // (+0x18) stays 0x78: nothing reads this machine's save for another player's tag, and no
        // tag records are written. The name is another machine's bytes and the HUD prints it as a
        // format string, so '%' and control characters become spaces.
        for (int i = 0; i < se.numPlayers && i < 4; i++) {
            if (!se.players[i].present) continue;
            u16* name = (u16*)(mm + MM_PLAYERS + i * PLAYER_SIZE + MM_NAME);
            const PPOM::PortValues& pv = se.players[i].pv;
            int n = 0;
            for (; (pv.flags & PPOM::PV_TAG) && n < PPOM::TAG_CHARS && pv.tag[n]; n++) {
                u16 c = pv.tag[n];
                name[n] = (c < 0x20 || c == '%') ? (u16)' ' : c;
            }
            for (; n <= PPOM::TAG_CHARS; n++) name[n] = 0;
        }
        // Stock control (the rules' stockControl, in a stock match: set rule +5 = 2, applyRules):
        // each player's count, picked on their panel's STOCKS box and carried by their lock-in's
        // port values, into their match player's handicap (+0x26, u16, in tenths as the CSS keeps
        // it: 10 = 1 stock). P+'s "Handicap = Stock Count" (Handicap.asm, hook 0x8094690C) makes
        // that the player's stocks when the fighter is made; 0 (none picked) leaves the rules'.
        // Written for every present player whatever the setup copied, so every machine has the
        // same (Dolphin's setup key compares it under stock control).
        const PPOM::Rules& ru = se.rules;
        const bool stockControl = ru.valid && ru.stockControl && ru.style == PPOM::STYLE_STOCK;
        for (int i = 0; i < se.numPlayers && i < 4 && stockControl; i++) {
            if (!se.players[i].present) continue;
            u8 n = se.players[i].pv.stocks;
            *(u16*)(mm + MM_PLAYERS + i * PLAYER_SIZE + MM_HANDICAP) = (u16)(n <= PPOM::MAX_PICKED_STOCKS ? n * 10 : 0);
        }
        // A desynced game played again (SESSION restore; docs/game-code.md, desync recovery): a
        // fresh game with each player's stocks at the agreed frame (a player out of the game
        // starts with one and leaves at RESTORE_FRAME, tickPlayers) and the time left then. The
        // damage follows at RESTORE_FRAME. Stocks and time are in the setup the barrier compares.
        s_restore = se.restore != 0;
        if (s_restore) {
            for (int i = 0; i < se.numPlayers && i < 4; i++) {
                if (!se.players[i].present) continue;
                u8 st = se.players[i].restoreStocks;
                u8* pm = mm + MM_PLAYERS + i * PLAYER_SIZE;
                pm[0x04] = (u8)(st == 0 ? 1 : st > 99 ? 99 : st);
                // A handicap (stock control's count) would be the player's stocks instead (P+'s
                // hook reads +0x26 when the fighter is made, over +0x04): the agreed stocks there too.
                u16* hc = (u16*)(pm + MM_HANDICAP);
                if (*hc != 0) *hc = (u16)(pm[0x04] * 10);
            }
            s32* limit = (s32*)(mm + MM_TIME_LIMIT);
            u32 left = se.rules.restoreFrames;
            if (*limit > 0 && left > 0 && left < (u32)*limit) *limit = (s32)left;
            PPOM::g_block.debug.scratch[10] |= 0x400000;   // tests: this match is a restart
        }
        // Colour clash, the fallback when distinctCostumes found no free costume (Slippi's
        // prepareOnlineMatchState: the players are counted in port order
        // and each one with the same character and colour as an earlier one gets the next shade).
        // Brawl has the shades for team battles (two of a character on one team): gmPlayerInitData
        // +0x0A is the shade, and the fighter's colour blend module draws it (sub colour, alpha
        // 0x80) and sets it again after a respawn. 3 is the lighter one, as Slippi's first shade;
        // then darker (1) and grey (2). The same SESSION on both machines gives the same shades.
        // A team battle is left to the game: when the match starts it shades a second one of a
        // character on a team itself (seen: the second red Mario gets 1, whatever is written here).
        static const u8 SHADES[4] = {0, 3, 1, 2};
        for (int i = 0; i < se.numPlayers && i < 4 && !teams; i++) {
            if (!se.players[i].present) continue;
            int n = 0;
            for (int j = 0; j < i; j++) {
                if (se.players[j].present && se.players[j].charKind == se.players[i].charKind &&
                    sameLook(costume, alt, i, j)) n++;
            }
            mm[MM_PLAYERS + i * PLAYER_SIZE + 0x0A] = SHADES[n];
        }
        PPOM::g_block.debug.scratch[11] = ((u32)se.stageKind << 16) |
                                          ((u32)se.players[0].charKind << 8) | se.players[1].charKind;
        memoryChangeTo(seq, 9, 0);   // then state 9: scMelee
        s_discFrames = -1;
        s_quitOp = NULL;
        s_matchQuitBy = s_matchQuitRunback = s_matchQuitPorts = 0;
        s_clientQuit = s_hintPorts = s_startPorts = 0;
        s_clientQuitFrames = 0;
        return true;
    }

    // Back from a match (or the loser's stage select): the CSS with this player's own record.
    static void backToCss(u8* seq)
    {
        u8* sel = gg(0x10);
        if (sel && s_haveSaved) {
            memcpy(sel + SEL_PLAYERS, s_savedSel, sizeof(s_savedSel));
            sel[SEL_TEAMS] = s_savedTeams;
        }
        s_haveSaved = false;
        OnlineMenu::restoreCss();
        memoryChangeTo(seq, 1, 0xD);
    }

    typedef void (*SetNextSceneFn)(void* mgr, const char* name, int arg);
    typedef void (*VoidFn)();

    int onSelStage(u8* seq)
    {
        // The CSS left for the waiting room (Unranked / Ranked search): Brawl's room setup.
        if (g_onlineCss && TrainingRoom::onSelStage(seq)) return 1;
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
        // Ranked (and a room's set): the steps decide the stage (GameSetup); the stage select's own
        // pick is not kept.
        if (!OnlineMenu::stepsDecideStage()) {
            // The stage select keeps its choice where sqVsMelee's setup reads it (GameGlobal+0x14,
            // +0x22); the alternate-stage buttons are the ones held on the pick. Left with B: no
            // pick.
            u8* st = gg(0x14);
            bool picked = (code == 1 || code == 3) && st;
            s_pickedStage = picked ? *(u16*)(st + 0x22) : OnlineMatch::PICK_CANCELLED;
            // The held buttons always: Dolphin's host plays them only where the match has
            // alternate stages (Direct, Teams, a room whose ruleset has them on).
            s_pickedAsl = picked ? s_sssAsl : 0;
        }
        s_sssAsl = 0;
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
        const PPOM::Session& se = PPOM::g_block.session;
        const u8 played = g_onlineMatchGame;
        g_onlineMatchGame = 0;
        s_discFrames = -1;
        // P+'s salty runback (pponline_pauseResult): where it restarts (Unranked, Direct, a room's
        // game, 3-4 players too), Dolphin has set the next game up as this one again (same players, stage
        // and alternate stage) when the session ended, the same on every machine: straight into
        // it, as P+'s own runback, without the CSS. Ranked counts it as a lost game and sets
        // nothing up: the CSS as after any quit.
        if (s_matchQuitBy && s_matchQuitRunback && se.state == PPOM::SS_MATCH_READY && se.game == played + 1) {
            g_onlineMatchGame = se.game;
            PPOM::g_block.debug.scratch[10] = (PPOM::g_block.debug.scratch[10] & ~0xFF) | 0x1B;
            if (setupMatch(seq)) return 1;
            g_onlineMatchGame = 0;
        }
        // Who quit, for the CSS's status line (both machines; a 3-4 player match: the first port
        // that quit).
        int quitter = s_matchQuitBy ? s_matchQuitBy - 1 : -1;
        for (int p = 0; quitter < 0 && p < PPOM::SESSION_PLAYERS; p++) {
            if ((s_matchQuitPorts >> p) & 1) quitter = p;
        }
        if (quitter >= 0) OnlineMenu::noteQuit(quitter);
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

    // ---- 3-4 players: a dropped player's fighter leaves the match; the elimination order ----
    // The fighters (sora_melee, at fixed addresses during a match): the ftEntryManager at
    // 0x80624780 (+0 its ftEntry array, stride 0x244, +4 the number of entries: 9 in a match, the
    // used ones first, in port order, with no entry for an empty port); an ftEntry's +0x04 is
    // its entry id, +0x58 its player number (the in-game port), +0x28 its ftOwner, whose data
    // (+0) has the stock count at +0x34.
    static const u32 FT_ENTRIES = 0x80624780;
    static const u32 FT_MANAGER = 0x80B87C28;   // g_ftManager
    static const u32 SET_DEAD = 0x80816018;     // ftManager::setDead (sora_melee text+0x10B604)
    // A small Fighter function (sora_melee text+0x12D188): its status module changes to status
    // 0x10B, the status a fighter whose last stock was lost stays in (not drawn, not on the
    // stage, out of the camera; checked against a fighter that fell off the stage on its last
    // stock: 0xBD, the dead status, then 0x10B).
    static const u32 SET_OUT = 0x80837B9C;
    static const u32 GAME_FRAME = 0x901812A4;   // g_GameFrame's frame counter (rolled back)
    // ftOwner::setDamage(float damage, bool log) (sora_melee text+0x1113B8, the ftOwner vtable's
    // slot 4 at 0x80B05F58; seen live: slot 5 is getDamage, text+0x111850): stores the damage
    // (data+0x24) and tells the HUD; `log` also adds the change to the match's stats.
    static const u32 SET_DAMAGE = 0x8081BDCC;
    // A desynced game played again (setupMatch): every fighter exists by game frame 92 of the
    // countdown, and the rollback session starts at 120 (Gprb DEFAULT_START_FRAME): the restored
    // damage and the players out of the game are applied between the two, on the same frame on
    // every machine, before anything can be rolled back.
    static const u32 RESTORE_FRAME = 100;

    // Every frame of an online match (also resimulated ones), at the start of the frame
    // (gfPadSystem::updateSystem, before the frame's game code), from the rolled-back state only,
    // so every machine and every resimulation does the same on the same frame:
    //  - a port whose SESSION gone flag is set and whose fighter still has stocks leaves the match
    //    as a fighter that loses its last stock does, without the blast-zone death: put on its
    //    last stock, ftManager::setDead with reason 5 and no killer (Fighter text+0x131220 calls
    //    it so: the stock count, no KO credited to anyone), then the fighter's status 0x10B, which
    //    a fighter out of stocks stays in (setDead alone left it standing on the stage). Its HUD
    //    panel breaks as on any elimination; the game ends by itself once one player or team is
    //    left (Brawl's own game set).
    //  - SESSION's `out`: the order in which the ports ran out of stocks (Dolphin's placings).
    // s_seen: ports seen with stocks in this match, so that what an earlier match left in the
    // fighter tables before this one builds its own is never read as "out".
    static void tickPlayers()
    {
        typedef void (*SetDeadFn)(u32 mgr, int entryId, int reason, int killer);
        typedef void (*SetOutFn)(u32 fighter);
        PPOM::Session& se = PPOM::g_block.session;
        PPOM::Debug& dbg = PPOM::g_block.debug;
        u32 test = 0;
        if (dbg.cfg & PPOM::CFG_TEST_GONE) {
            u32 t = dbg.testGonePorts;
            s32 f = (s32)(*(u32*)GAME_FRAME - dbg.testGoneFrame);
            if (f >= 0) test = t & 0xF;
            if (f >= (s32)(t >> 16)) test |= (t >> 8) & 0xF;
        }
        u32 entries = *(u32*)FT_ENTRIES;
        u32 n = *(u32*)(FT_ENTRIES + 4);
        u32 mgr = *(u32*)FT_MANAGER;
        if (!isPtr(entries) || !isPtr(mgr) || n > 9) return;   // 9 entries in a match; unused ones have no player number
        u8 next = se.outCount + 1;   // ports out on the same frame share their place in the order
        // A desynced game played again (setupMatch): from RESTORE_FRAME on, the players who were
        // out of the game leave it as a dropped player does; on that frame the others get their
        // damage back.
        const u32 frame = *(u32*)GAME_FRAME;
        const bool restoreNow = s_restore && g_onlineCss && g_onlineMatchGame && frame >= RESTORE_FRAME;
        for (u32 k = 0; k < n; k++) {
            u8* e = (u8*)(entries + k * 0x244);
            u32 p = *(u32*)(e + 0x58);
            u32 owner = *(u32*)(e + 0x28);
            u32 data = isPtr(owner) ? *(u32*)owner : 0;
            if (p > 3 || !isPtr(data)) continue;
            s32* stocks = (s32*)(data + 0x34);
            const PPOM::SessionPlayer& rp = se.players[p];
            const bool restored = restoreNow && (int)p < se.numPlayers && rp.present;
            if (restored && frame == RESTORE_FRAME && rp.restoreStocks > 0 && *stocks > 0) {
                typedef void (*SetDamageFn)(u32 owner, float damage, int log);
                float d = rp.restoreDamage;
                if (d >= 0.0f && d <= 999.0f) {
                    ((SetDamageFn)SET_DAMAGE)(owner, d, 0);
                    dbg.scratch[10] |= 0x800000;   // tests: the damage was restored
                }
            }
            if (*stocks > 0) {
                s_seen |= 1u << p;
                u32 fighter = *(u32*)(e + 0x34 + 8 * (e[0x0A] & 3));   // the active instance
                bool quit = (se.quitPorts >> p) & 1;   // the pause screen's quit (pponline_pauseResult)
                bool out = restored && rp.restoreStocks == 0;   // out at the agreed frame
                if ((se.gone[p] || quit || out || ((test >> p) & 1)) && isPtr(fighter)) {
                    if ((test >> p) & 1) se.gone[p] = 1;
                    *stocks = 1;
                    ((SetDeadFn)SET_DEAD)(mgr, *(int*)(e + 0x04), 5, -1);
                    ((SetOutFn)SET_OUT)(fighter);
                    // Ice Climbers (gmCharacterKind 0x10, ftEntry+0x5C): Nana is the entry's
                    // second fighter (+0x3C, ftManager::getSubFighter) and leaves too.
                    u32 nana = *(u32*)(e + 0x3C);
                    if (*(u32*)(e + 0x5C) == 0x10 && isPtr(nana)) ((SetOutFn)SET_OUT)(nana);
                }
            }
            if (*stocks <= 0 && ((s_seen >> p) & 1) && !se.players[p].out) {
                se.players[p].out = next;
                se.outCount = next;
            }
        }
    }

    // How many players (free-for-all) or teams (team battle) are still in the match once the
    // ports in `leaving` are out: fighters with stocks whose port has not dropped or quit. From
    // the rolled-back state only.
    static int sidesLeftWithout(u32 leaving)
    {
        const PPOM::Session& se = PPOM::g_block.session;
        u32 entries = *(u32*)FT_ENTRIES;
        u32 n = *(u32*)(FT_ENTRIES + 4);
        if (!isPtr(entries) || n > 9) return 0;
        u32 sides = 0;
        for (u32 k = 0; k < n; k++) {
            u8* e = (u8*)(entries + k * 0x244);
            u32 p = *(u32*)(e + 0x58);
            u32 owner = *(u32*)(e + 0x28);
            u32 data = isPtr(owner) ? *(u32*)owner : 0;
            if (p > 3 || !isPtr(data) || *(s32*)(data + 0x34) <= 0) continue;
            if (se.gone[p] || (((se.quitPorts | leaving) >> p) & 1)) continue;
            u8 team = se.players[p].team;
            sides |= se.teams && team < PPOM::TEAM_COUNT ? 0x10u << team : 1u << p;
        }
        int count = 0;
        for (; sides; sides &= sides - 1) count++;
        return count;
    }

    // P+'s "Allow Pausing When Set to Off" (NETPLAY.txt, a hook at 0x800505C8) writes 2 to
    // 0x80584084 in a match whose pause is off (Unranked, Ranked, Teams), and its "Hold Start to
    // Pause" (0x801E6D24) then takes START out of the pad reads until it has been held 50 frames,
    // in every scene while the word is 2. Only its "Match Win/Loss Clears Pause Address" hooks
    // put it back to 0, so a match that ends without a winner (the pause screen's L+R+A+START
    // after a held START, the DISCONNECTED and DESYNC DETECTED ends) left START dead in the menus:
    // START on the Direct CSS no longer opened the keypad. Cleared here once the match is left.
    static const u32 PPLUS_PAUSE_OFF = 0x80584084;

    // Ranked has no pause at all, as Slippi's (InitPause.asm: no pause in ranked, not even its
    // client pause; the user's, 2026-10-14: a player could pause a ranked game and never unpause,
    // and the opponent's only way out was to leave, which abandons the set). P+ has no pause that
    // is off: the game's pause rule 0 is P+'s hold, which the hook above turns into a pause after
    // START is held 50 frames. Its hold timers (u32 at 0x80584080 + 8 * pad, counted in the pad
    // read at 0x801E6D24 while the word is 2) are set back to 0 every ranked match frame, so START
    // never reaches the game and nothing pauses; with no pause there is no in-game quit either.
    // The timers are in the rollback set (gp-v23), and SESSION's mode is the same on both
    // machines, so a resimulated frame does the same.
    static const u32 PPLUS_PAUSE_HOLD = 0x80584080;
    static const u32 PPLUS_PAUSE_HOLD_FRAMES = 50;

    static void clientPause();

    // Unranked has no pause either (the user's, 2026-10-14), but keeps Slippi's client pause
    // (clientPause): its quit needs START in the session's inputs, so P+'s hold is switched off
    // instead (its word cleared, every hold timer at 50: START goes into the pad slots as it is)
    // and START is taken out of the game's pads after the quit check. The pad thread reads the
    // word and the timers on its own, but only this machine's controllers before the session
    // takes their input: at worst a START press reaches the inputs a frame late.
    static void noPause()
    {
        const u8 mode = PPOM::g_block.session.mode;
        if (mode == PPOM::MODE_RANKED) {
            for (u32 pad = 0; pad < 4; pad++) *(volatile u32*)(PPLUS_PAUSE_HOLD + 8 * pad) = 0;
        } else if (mode == PPOM::MODE_UNRANKED) {
            *(volatile u32*)PPLUS_PAUSE_OFF = 0;
            for (u32 pad = 0; pad < 4; pad++) {
                *(volatile u32*)(PPLUS_PAUSE_HOLD + 8 * pad) = PPLUS_PAUSE_HOLD_FRAMES;
            }
            clientPause();
        }
    }

    // The HOME Menu is banned for the whole online match (the user's crash, 2026-10-12: "i paused
    // and it crashed!"). sora_melee lifts its own ban (bit 1) and calls gfHomeMenu's preload
    // (0x800377D4) on every pause (text+0x256F24) and at the match's end (text+0x2574A8), and
    // bans it again with an unload (0x80037BE4) on the unpause: an asynchronous read of
    // /system/homebutton/home_eng.pac (0xD0260 bytes, about 40 frames) into the Network heap,
    // with a 0x1D000 buffer. gfHomeMenu (DOL .bss 0x80495510), the Network heap and the file
    // thread are outside the rollback set, while the pause that loads and unloads it is rolled
    // back, so rollbacks across a pause or an unpause left a loaded archive the menu no longer
    // knew of: the next pause's load found no room ("File Read (DVD): Alloc Buffer failed"), and
    // the game hung or crashed. Any ban bit makes preload return at once (and HOME shows the
    // game's "cannot be used" icon). Bit 6 is the collection viewer's (sora_menu_collect_viewer),
    // never set in a match; it is set here every match frame (gfHomeMenu is not rolled back, so
    // it holds) and cleared once when the match is left. The CSS after an online match no longer
    // keeps the archive in the Network heap either.
    static const u32 HOME_MENU = 0x805A0090;   // gfHomeMenu* g_gfHomeMenu
    static const u32 HOME_MENU_SET_BAN = 0x80037D44;
    static const u32 HOME_BAN_BIT = 6;
    typedef int (*SetBanFn)(void*, u32, u32);

    static void banHomeMenu(bool ban)
    {
        u32 hm = *(u32*)HOME_MENU;
        if (isPtr(hm)) ((SetBanFn)HOME_MENU_SET_BAN)((void*)hm, ban ? 1 : 0, HOME_BAN_BIT);
    }

    void offMatch()
    {
        s_seen = 0;
        Online::setRumblePorts(false, 0, 0);
        if (s_inMatch) {
            *(volatile u32*)PPLUS_PAUSE_OFF = 0;
            banHomeMenu(false);
            s_inMatch = false;
        }
    }

    // In a match (scMelee), every frame, also in resimulated frames: nothing here may depend on
    // anything that differs between the machines while the match runs under rollback. LOCAL's
    // `disconnected` only changes once Dolphin has ended the rollback session.
    // DEBUG cfg CFG_TEST_DISCONNECT (tests): act as if disconnected now, in any scMelee (also a
    // local Versus match); cleared when seen.
    void tickMatch()
    {
        PPOM::Debug& dbg = PPOM::g_block.debug;
        s_inMatch = true;
        // Rumble: only this player's port (its pad in the match), on their controller
        // (online.cpp, PADControlMotor / WPADControlMotor). It differs between the machines but
        // only reaches the controllers, never the game.
        const PPOM::Local& me = PPOM::g_block.local;
        const u8 mePort = OnlineMenu::nobodyPlaysHere() ? 0xFF : me.localPort;
        Online::setRumblePorts(g_onlineCss && g_onlineMatchGame, mePort < 4 ? s_portPad[mePort] : 0xFF,
                               me.lockPad);
        if (g_onlineCss && g_onlineMatchGame) {
            banHomeMenu(true);
            noPause();
        }
        if ((g_onlineCss && g_onlineMatchGame) || (dbg.cfg & PPOM::CFG_TEST_GONE)) tickPlayers();
        // This player quit a 3-4 player match (pponline_pauseResult) and Dolphin has ended the
        // session for this machine (LOCAL, set only after the session ended): the match ends here
        // as the quit's own end does (no "GAME!", no contest), then the CSS. The others play on.
        if (g_onlineCss && g_onlineMatchGame && PPOM::g_block.local.quitLeft) {
            u8* oi = operatorInfo();
            if (oi && !(oi[0x11B] & 0x40)) {
                oi[0x11B] |= 0x70;   // quit (0x30, as L+R+A+START) and stop now (0x40)
                dbg.scratch[10] |= 0x200000;
            }
            return;
        }
        // Unranked's quit (clientPause): once the session has ended on it (LOCAL, set only after
        // that, as for quitLeft), or after QUIT_MAX_FRAMES without, the match ends as the pause
        // screen's quit does (no "GAME!", no contest), then the CSS or the runback (afterMatch).
        if (g_onlineCss && g_onlineMatchGame && s_clientQuit) {
            u8* oi = operatorInfo();
            bool over = PPOM::g_block.local.state != PPOM::LS_IN_MATCH;
            if (oi && !(oi[0x11B] & 0x40) && (over || ++s_clientQuitFrames >= QUIT_MAX_FRAMES)) {
                oi[0x11B] |= 0x70;
                dbg.scratch[10] |= 0x80000;
            }
            return;
        }
        bool test = (dbg.cfg & PPOM::CFG_TEST_DISCONNECT) != 0;
        if (test && s_discFrames >= DISC_END_FRAMES) s_discFrames = -1;   // a new test
        if (s_discFrames >= 0) return;   // counted in onFrameDrawn
        const PPOM::Local& lo = PPOM::g_block.local;
        if (!test) {
            if (!g_onlineCss || !g_onlineMatchGame || !(lo.disconnected || lo.desynced)) return;
        }
        dbg.cfg &= ~(u32)PPOM::CFG_TEST_DISCONNECT;
        // Slippi (design 5.6): the error sound, DISCONNECTED in red at the top, the game ends as
        // an LRAS-type end (no "GAME!") after a 90-frame end screen, then the CSS (state 10,
        // afterMatch) without a results screen. A desync (Dolphin ended the session because the
        // machines' game states differ) ends the same way with DESYNC DETECTED, as Slippi's hard
        // desync does; the connection stays, so the CSS is ready for the next game.
        bool desync = !test && !lo.disconnected;
        s_discFrames = 0;
        playSE(3);
        MatchHud::show(true, desync);
        dbg.scratch[10] |= desync ? 0x100000 : 0x10000;
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

    // ---- The pause screen's quit (L+R+A+START) in an online match ----
    // While the game is paused, the operator's process (text+0x256B70) asks the pause screen
    // for its result every frame (text+0x256EE4) and switches on it at text+0x256EF0: 2 resume,
    // 3 quit (sound 0x2052, flags |= 0x30), 4 the other quit (sound 0x13, flags |= 0x20); both
    // quits jump to the operator's end (text+0x25748C) in the same frame, and the match is torn
    // down 4 frames later (ftManager::removeEntry). The rollback session cannot confirm the quit
    // that fast: the other machine learns of it up to the prediction window plus the input delay
    // later and has to roll back across the teardown, which hangs it (docs/game-code.md).
    // So online the quit is only marked when it is chosen: the quit bits are set (Dolphin ends
    // the session MAX_ROLLBACK_FRAMES + 12 frames later, as after a game set) and its sound
    // plays, while the game stays paused and every other pause result is ignored. The operator's
    // own end runs once LOCAL says the session is over, or after QUIT_MAX_FRAMES with an older
    // Dolphin. The pending quit lives in the plugin's .bss, which the session rolls back.
    //
    // A match of 3-4 players: the quit takes only the player who paused out (the others play on).
    // The operator's +0x124 is that player's port (set when START paused the game, text+0x257124:
    // fn_80110310 finds the controller that pressed START and the player it belongs to; -1 none),
    // and the pause screen reads only that controller. The quit sets the port's bit in SESSION's
    // quitPorts and resumes the game on the same frame; the next frame's tickPlayers takes the
    // fighter out as a dropped player's. Everything is the rolled-back state, driven by the
    // quitter's own input, so every machine does it on the same frame. Only one player holds the
    // pause, so two players can never quit on the same frame; quits after one another are each
    // their own. Dolphin ends the quitter's session once the frame is final and tells the others
    // it left (GameplaySession.cpp, the local quit); the game then ends its match (tickMatch,
    // LOCAL.quitLeft). A quit that leaves one player or team in ends the game by Brawl's own game
    // set: the quitter is in quitStays too and stays in the session until the game set ends it.
    // A paused game whose pauser has left the match (dropped or quit) resumes at once: nobody
    // else could unpause it (Slippi's PreventPauseStranding).
    static const int PAUSE_END_QUIT = 0x100;   // asm: the quit's flags and end, without its sound
    static const int PAUSE_RESUME = 2;
    static const u32 OP_PAUSER = 0x124;
    static const u32 OP_PAUSE_PAD = 0x120;   // the controller whose START paused (fn_80110310)
    // P+'s salty runback: L+R+Y held with the quit (the Code Menu header's 0x804E000A, 0x0860).
    // Online the Code Menu's own runback is off (OnlineMenu::runbackOff): it reads this machine's
    // pads. Here the buttons are the pauser's in the game's pad status, the session's inputs, so
    // every machine sees the same.
    static const u32 RUNBACK_BUTTONS = 0x0860;

    // The game's pause-screen quit per controller type (gfPadType: GameCube, Classic Controller,
    // Wii Remote, Nunchuk), its table at 0x80408C28 that the pause screen's quit check
    // (0x800F0A2C) compares the pauser's buttons (gfPadStatus +0x4) with: L+R+A+START on a
    // GameCube or Classic Controller (0x1160), B+1+2+PLUS on a Wii Remote alone (0x1340), and
    // 1+B+Z+PLUS with a Nunchuk (0x11210), in the game's button bits.
    static const u32 QUIT_MASKS = 0x80408C28;
    static const u32 BTN_PLUS = 0x1000;     // START, a Wii Remote's and a Classic's +
    static const u32 BTN_MINUS = 0x80000;   // a Wii Remote's -
    static u32 quitButtons(u32 type)
    {
        return ((const u32*)QUIT_MASKS)[type < 4 ? type : 0];
    }
    // The runback's: L+R+Y on a GameCube or Classic Controller; a Wii Remote has no Y, so its
    // quit's buttons (but PLUS) and MINUS.
    static u32 runbackButtons(u32 type)
    {
        if (type != 2 && type != 3) return RUNBACK_BUTTONS;
        return (quitButtons(type) & ~BTN_PLUS) | BTN_MINUS;
    }
    // A pad's controller type (gfPadStatus +0x3C) in gfPadSystem's first pad array.
    static u32 padType(u32 ps, int pad)
    {
        return *(volatile u32*)(ps + 0x40 + 0x40 * pad + 0x3C);
    }

    typedef void (*PlaySEArgsFn)(void*, int, int, int, int, int);

    static void playQuitSound(int result)
    {
        void* snd = *(void**)0x805A01D0;   // g_sndSystem, as the game's own call
        if (snd) ((PlaySEArgsFn)0x800742b0)(snd, result == 3 ? 0x2052 : 0x13, 0x10000, 0, 0, -1);
    }

    // The players of this match (SESSION, the same on every machine for the whole match).
    static int matchPlayers()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        int n = 0;
        for (int p = 0; p < PPOM::SESSION_PLAYERS; p++) n += se.players[p].present ? 1 : 0;
        return n;
    }

    // A room's set is being played (2v2: one player's quit is the whole game's, docs/design/
    // room-rules.md; SESSION's RoomPlay is the same on every machine during the match).
    static bool roomSetPlaying()
    {
        const PPOM::RoomPlay& rp = PPOM::g_block.session.play;
        return rp.valid && rp.setOn && !(rp.setFlags & PPOM::SET_FINISHED);
    }

    // The pauser's buttons: gfPadSystem's first pad array (+0x44, 0x40 a pad, +0 the buttons
    // down), where the session's inputs arrive (and P+'s Code Menu reads); found live, the same
    // on both machines at the quit (START+Y+A+L+R 0x1960). The sys pads (+0x244) are empty then.
    static u32 pauserHeld(u8* op)
    {
        u32 ps = *(u32*)0x805A0040;
        s32 pad = *(s32*)(op + OP_PAUSE_PAD);
        if (!isPtr(ps) || pad < 0 || pad >= 8) return 0;
        return *(volatile u32*)(ps + 0x44 + 0x40 * pad);
    }

    // The pauser held their controller's runback buttons.
    static bool pauserRunback(u8* op)
    {
        u32 ps = *(u32*)0x805A0040;
        s32 pad = *(s32*)(op + OP_PAUSE_PAD);
        if (!isPtr(ps) || pad < 0 || pad >= 8) return false;
        const u32 want = runbackButtons(padType(ps, pad));
        return (pauserHeld(op) & want) == want;
    }

    extern "C" int pponline_pauseResult(int result, u8* op)
    {
        if (!g_onlineCss || !g_onlineMatchGame) return result;
        PPOM::Debug& dbg = PPOM::g_block.debug;
        s32 pauser = *(s32*)(op + OP_PAUSER);
        if (s_quitOp != op && matchPlayers() >= 3 && !roomSetPlaying() && pauser >= 0 && pauser < 4) {
            PPOM::Session& se = PPOM::g_block.session;
            u8 bit = (u8)(1u << pauser);
            if (se.gone[pauser] || (se.quitPorts & bit)) {
                dbg.scratch[10] |= 0x800000;
                return PAUSE_RESUME;
            }
            if (result != 3 && result != 4) return result;
            // The salty runback (L+R+Y held) outside a set restarts the match for everyone (the
            // user's, 2026-10-13): it takes the quit below, which ends the match for all and is
            // marked as a runback. Only while every player is still in: with one gone, the same
            // match cannot start again, and the runback is this player's own quit.
            bool allIn = se.quitPorts == 0;
            for (int p = 0; p < PPOM::SESSION_PLAYERS; p++) allIn = allIn && !(se.players[p].present && se.gone[p]);
            if (!allIn || !pauserRunback(op)) {
                if (sidesLeftWithout(bit) <= 1) se.quitStays |= bit;
                se.quitPorts |= bit;
                s_matchQuitPorts = se.quitPorts;
                playQuitSound(result);
                dbg.scratch[10] |= 0x400000;
                return PAUSE_RESUME;
            }
        }
        if (s_quitOp != op) {
            if (result != 3 && result != 4) return result;
            s_quitOp = op;
            s_quitKind = (u8)result;
            s_quitFrames = 0;
            op[0x11B] |= result == 3 ? 0x30 : 0x20;
            PPOM::Session& se = PPOM::g_block.session;
            se.quitBy = pauser >= 0 && pauser < 4 ? (u8)(pauser + 1) : 0;
            se.quitRunback = pauserRunback(op) ? 1 : 0;
            s_matchQuitBy = se.quitBy;
            s_matchQuitRunback = se.quitRunback;
            if (se.quitRunback) dbg.scratch[10] |= 0x1000000;
            playQuitSound(result);
            dbg.scratch[10] |= 0x40000;
            return 0;   // stay paused
        }
        ++s_quitFrames;
        if (PPOM::g_block.local.state == PPOM::LS_IN_MATCH && s_quitFrames < QUIT_MAX_FRAMES) return 0;
        int kind = s_quitKind;
        s_quitOp = NULL;
        dbg.scratch[10] |= 0x80000;
        return PAUSE_END_QUIT | kind;
    }

    // text+0x256EF0 (0x80961904) `cmpwi r3,3`, right after the call that returned the pause
    // screen's result in r3 (nothing else volatile is live; the function saved LR). Quit kinds
    // go to their flag writes, past the sound: kind 3 text+0x256F80, kind 4 text+0x256FB0.
    __attribute__((naked)) void hookPauseResult()
    {
        asm volatile(
            "mr 4, 30\n\t"
            "lis 12, pponline_pauseResult@ha\n\t"
            "addi 12, 12, pponline_pauseResult@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "cmpwi 3, 0x103\n\t"
            "bne 1f\n\t"
            "lis 12, 0x8096\n\t"
            "ori 12, 12, 0x1994\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "1:\n\t"
            "cmpwi 3, 0x104\n\t"
            "bne 2f\n\t"
            "lis 12, 0x8096\n\t"
            "ori 12, 12, 0x19C4\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "2:\n\t"
            "cmpwi 3, 3\n\t"
            "lis 12, 0x8096\n\t"
            "ori 12, 12, 0x1908\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // ---- No snapshot online ----
    // Z on the pause screen takes Brawl's snapshot (bug report #53, 2026-10-18: CISS#598
    // "screenshot crashes the game"). IfPhoto's update (DOL 0x800F157C) plays its sound, logs
    // "SE Snapshot if_photo.cpp", starts the photo (0x800F0954) and goes to state 2, which reads
    // the NAND album (/title/00010000/52534245/data/collect.vff) and encodes the picture. The
    // album and the file system are outside the rollback set: the machine that rolled back across
    // that frame never reached another frame, and its peer dropped it ("peer timed out"). Online
    // the snapshot never starts: the update goes on as if Z were not pressed (no sound, the pause
    // stays as it is). Only SESSION's mode and the match decide it, the same on every machine and
    // in every resimulated frame. The rest of the pause (its camera, L+R+A+START) is unchanged.
    extern "C" int pponline_noSnapshot()
    {
        if (!g_onlineCss || !g_onlineMatchGame) return 0;
        PPOM::g_block.debug.scratch[10] |= 0x20000000;   // tests: a snapshot was refused
        return 1;
    }

    // 0x800F16DC `cmpwi r30,0`, once IfPhoto has decided to take the snapshot (P+'s Pause.asm
    // hooks 0x800F1680, before it). Only r29/r30 (non-volatile) are live and the function saved
    // LR. A refusal goes to 0x800F1728, the path of a frame without the press.
    __attribute__((naked)) void hookSnapshot()
    {
        asm volatile(
            "lis 12, pponline_noSnapshot@ha\n\t"
            "addi 12, 12, pponline_noSnapshot@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "cmpwi 3, 0\n\t"
            "lis 12, 0x800F\n\t"
            "beq 1f\n\t"
            "ori 12, 12, 0x1728\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "1:\n\t"
            "cmpwi 30, 0\n\t"
            "ori 12, 12, 0x16E0\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    // ---- Unranked: Slippi's client pause, without a pause ----
    // Slippi's Unranked never pauses (InitPause.asm ClientPause returns -1, "so the game doesn't
    // actually pause"); the user's, 2026-10-14: neither does ours, and nothing about the game
    // changes for a pause (Brawl's pause moves the camera, and screen KOs depend on the camera).
    // Every frame of an Unranked match, from the session's inputs in gfPadSystem's first pad
    // array (port p's input in its pad, s_portPad: the port's GameCube or Wii Remote pad, as the
    // session writes the slots), so every machine and every resimulation does the same:
    //  - any player holding their controller's quit buttons (the game's own, quitButtons:
    //    L+R+A+START on a GameCube or Classic Controller, B+1+2+PLUS on a Wii Remote alone,
    //    1+B+Z+PLUS with a Nunchuk) quits, as Slippi's LRAS in its client pause (no pause first).
    //    The operator's quit bits (0x30) are set without its stop: Dolphin ends the session on
    //    them as on the pause screen's quit (IsQuitRequested), with SESSION's quitBy and
    //    quitRunback (runbackButtons held too: P+'s salty runback, as on the pause screen), and
    //    the game plays on until then (MAX_ROLLBACK_FRAMES + 12 frames); tickMatch then ends
    //    the match.
    //  - a START press switches that port's quit hint (s_hintPorts; MatchHud draws the local
    //    player's), Slippi's client pause screen, drawn only, nothing in the game changes;
    //  - START is taken out of every pad the game reads, so nothing pauses.
    static const u32 BTN_START = 0x1000;
    static const u32 PAD_SYSTEM = 0x805A0040;

    static void clientPause()
    {
        PPOM::Session& se = PPOM::g_block.session;
        u32 ps = *(u32*)PAD_SYSTEM;
        if (!isPtr(ps)) return;
        u8* oi = operatorInfo();
        // Only while the session runs: its inputs are the same on every machine (neutral in the
        // countdown before it starts); after it ended, this machine's own controllers.
        bool live = PPOM::g_block.local.state == PPOM::LS_IN_MATCH;
        for (int p = 0; p < se.numPlayers && p < 4; p++) {
            if (!se.players[p].present) continue;
            const int padNo = s_portPad[p];
            const u32* pad = (const u32*)(ps + 0x44 + 0x40 * padNo);
            u32 held = pad[0];
            const u32 type = padType(ps, padNo);
            const u32 quit = quitButtons(type);
            const u32 runback = runbackButtons(type);
            u8 bit = (u8)(1u << p);
            // the press from the held buttons (this array's pressed words were never seen set)
            if ((held & BTN_START) && !(s_startPorts & bit)) s_hintPorts ^= bit;
            s_startPorts = (u8)((held & BTN_START) ? s_startPorts | bit : s_startPorts & ~bit);
            if (!live || s_clientQuit || !oi || (oi[0x11B] & 0x70) || (held & quit) != quit) continue;
            oi[0x11B] |= 0x30;
            s_clientQuit = 1;
            s_clientQuitFrames = 0;
            se.quitBy = (u8)(p + 1);
            se.quitRunback = (held & runback) == runback ? 1 : 0;
            s_matchQuitBy = se.quitBy;
            s_matchQuitRunback = se.quitRunback;
            if (se.quitRunback) PPOM::g_block.debug.scratch[10] |= 0x1000000;
            playQuitSound(3);
            PPOM::g_block.debug.scratch[10] |= 0x40000;
        }
        // START out of the game's pads: the first array (+0x44) and the sys pads (+0x244), six
        // words each (current, current2, held, pressed, released, pressed2).
        for (u32 off = 0x44; off < 0x944; off += 0x40) {
            u32* f = (u32*)(ps + off);
            for (int i = 0; i < 6; i++) f[i] &= ~BTN_START;
        }
    }

    const char* quitHintText()
    {
        u8 me = PPOM::g_block.local.localPort;
        u32 ps = *(u32*)PAD_SYSTEM;
        const u32 type = me < 4 && isPtr(ps) ? padType(ps, s_portPad[me]) : 0;
        switch (type) {
        case 1: return "L+R+A+PLUS to quit";    // Classic Controller
        case 2: return "B+1+2+PLUS to quit";    // Wii Remote alone
        case 3: return "1+B+Z+PLUS to quit";    // Wii Remote and Nunchuk
        default: return "L+R+A+START to quit";
        }
    }

    // Only for a player of the match: a replay (also a spectated game and a rendered clip) or a
    // watched room game replays the recorder's START presses, and the hint is for the player.
    bool quitHintShown()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        u8 me = PPOM::g_block.local.localPort;
        return g_onlineCss && g_onlineMatchGame && se.mode == PPOM::MODE_UNRANKED && !s_clientQuit &&
               !OnlineMenu::nobodyPlaysHere() && me < 4 && ((s_hintPorts >> me) & 1);
    }

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

    // ---- This player's own rumble setting ----
    // When a match is set up (sora_scene, the Versus setup's loop over the four players, then
    // fn_80110410), fn_80110368(gmPlayerInitData*) gives a player its rumble bit (+0x1B, 0x40): a
    // human with a controller (+0x3E bit 0x80, controller number +0x3D = pad + 1) gets its name
    // tag's Rumble (+0x18 the tag; fn_8004D9A8: the tag's +0x0C == 1) or, with no tag, the save's
    // Rumble option of that pad (*(GameGlobal+0x24) + 0x820 + pad); the others none. Then
    // fn_80110410(gmGlobalModeMelee*) masks the motors of every pad whose player has it off
    // (gfPadSystem::maskMotor). (fn_8011058C, other modes', does both in one; an online match
    // never calls it.) Online the match has no name tags, so a player's rumble was the save's
    // option of their in-game port, not the Rumble of the tag they picked, which the waiting
    // room's fighter has (training_room.cpp): a player whose tag rumbles, or who plays P2 with
    // only port 1's option on, felt it only in the waiting room (Sarah's board, 2026-10-25). Now,
    // after the game's own pass, this player's port gets their own setting, as offline: the
    // lock-in's tag's Rumble, or with no tag the save's option of the pad they locked in with
    // (lockPad), and the mask follows from the bits as before. Only this machine's motors follow
    // from it (online.cpp routes nobody else's port to a controller); the bit already came from
    // each machine's own save, and the setup key does not compare it.
    typedef void (*PlayerRumbleFn)(u8* player);
    static PlayerRumbleFn s_origPlayerRumble = NULL;
    static const u32 PI_FLAGS_1B = 0x1B;       // gmPlayerInitData: bit 0x40 rumble
    static const u32 PI_RUMBLE_PAD = 0x3D;     // gmPlayerInitData: the controller number rumble uses (pad + 1)
    static const u32 PI_HAS_PAD = 0x3E;        // gmPlayerInitData: bit 0x80 a controller plays it
    static const u8 PI_RUMBLE = 0x40;

    static bool ownRumble()
    {
        const PPOM::Local& me = PPOM::g_block.local;
        // As the game reads them: the tag's byte is on when 1, the save's option of pad p is
        // +0x820 + p for any pad 0-7 (a Wii Remote's past the four ports', as offline).
        if (me.own.flags & PPOM::PV_TAG) return me.own.rumble == 1;
        u8* save = gg(0x24);
        return save && me.lockPad < 8 && save[0x820 + me.lockPad] != 0;
    }

    static void hkPlayerRumble(u8* pl)
    {
        s_origPlayerRumble(pl);
        const u8 me = PPOM::g_block.local.localPort;
        if (!pl || !g_onlineCss || !g_onlineMatchGame || TrainingRoom::active() ||
            OnlineMenu::nobodyPlaysHere() || me >= 4) return;
        u8* mm = gg(0x08);
        if (!mm || pl != mm + MM_PLAYERS + me * PLAYER_SIZE) return;
        if (pl[0x01] != 0 || !(pl[PI_HAS_PAD] & 0x80) || pl[PI_RUMBLE_PAD] == 0) return;
        pl[PI_FLAGS_1B] = (u8)((pl[PI_FLAGS_1B] & ~PI_RUMBLE) | (ownRumble() ? PI_RUMBLE : 0));
        PPOM::g_block.debug.scratch[13] |= 0x100u << me;   // tests: this player's rumble applied
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
        TrainingRoom::install(api);
        api->sySimpleHookRel(0x00036F64, reinterpret_cast<void*>(hookState3), 1 /* SORA_SCENE */);
        api->sySimpleHookRel(0x00036F8C, reinterpret_cast<void*>(hookState4), 1);
        api->sySimpleHookRel(0x00036FDC, reinterpret_cast<void*>(hookState5), 1);
        api->sySimpleHookRel(0x00037580, reinterpret_cast<void*>(hookState10), 1);
        api->sySimpleHook(0x80110550, reinterpret_cast<void*>(hookPadConfigSet));
        api->syReplaceFunc(0x80110368, reinterpret_cast<void*>(hkPlayerRumble), (void**)&s_origPlayerRumble);
        api->sySimpleHook(0x800F16DC, reinterpret_cast<void*>(hookSnapshot));
        api->sySimpleHookRel(0x00256EF0, reinterpret_cast<void*>(hookPauseResult), 27 /* SORA_MELEE */);
    }
}
