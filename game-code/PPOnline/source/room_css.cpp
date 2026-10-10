// The room CSS's panels: the other players of a room on Brawl's own Wi-Fi character select
// (docs/design/rooms.md #6, #11, #15; docs/game-code.md "Rooms: the 4-panel CSS").
//
// Brawl's Wi-Fi CSS (sel_char, sqNetAnyOkiraku) builds four player areas
// (muSelCharPlayerArea, 0x448 bytes, muSelCharTask+0x44 + 4 * i). In Wi-Fi mode it builds areas
// 1-3 as network panels that are only filled in when its WFC code reports a member, so their
// models are never placed (their world matrices stay zero). But it builds every area whose player
// record (gmSelCharData+0xB8 + 0x5C * i, +0x01 state) says human at that moment as a full local
// panel (sel_char text+0x2DC0, 0x80685A34: rec+1 == 0). So before a room's CSS is built the
// plugin marks records 1-3 human (prepareRecords): four full panels, PLAYER 1-4, and only area
// 0 has a controller (area+0x1DC = -1 for the others: no hand, no coin).
//
// Then each of areas 1-3 is set to area+0 = 2. area+0 is the area's kind of player: 0 local
// (sel_char text+0x6C84 moves the character with its coin every frame, text+0x12210 runs the
// local panel), 1 remote (text+0x12210 runs Brawl's network panel from its WFC member data);
// with 2 neither runs and the panel is the plugin's, drawn with the area's own functions:
//   0x80696F60 setChar(area, css id)         the character (0x28 none), as the coin does
//   0x8069742C setCharPic(area, css, kind, costume, isTeam, team, teamSet)
//   0x8069A4DC decide(area)                  the portrait's "picked" look (coin put down)
//   0x80698A1C showTeam(area)                team colour from area+0x1C0 when task+0x5C8 is set
//   0x806977FC showStars(area, count, kind)  Brawl's win stars above the name plate (kind 1,
//                                            1-5 stars, more as a star and "x N"): the host's mark,
//                                            one star (rooms.md #8: a reused Brawl icon)
//   area+0x40C MuMsg, window 0              the name plate ("PLAYER 2" -> the player's name)
//   area+0x41C panel state object (DOL 0x800FD630..): states 0 none, 1 "Seeking..",
//            2 "Selecting Character...", 3 "Selecting Stage...", 4 "Practice Stage...",
//            5 "Bye" (then 1), 6 "Brawling..."; set with 0x800FD96C(obj, state), animated by
//            0x800FD8E0(obj) every frame (which text+0x12210 no longer does for these areas).
// The "Seeking" texture (MenSelchrWifi03, in the state model MenSelchrState0000) is drawn again
// as "Searching" with the game's font (the model's own dots follow it), and "Selecting
// Character..." (WifiInfWait05_0, CMPR) as "Choosing...".
#include <mu/mu_msg.h>
#include <string.h>
#include "labels.h"
#include "online_menu.h"
#include "ppom.h"

namespace RoomCss {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    typedef void (*SetCharFn)(u8* area, int css);
    typedef void (*SetCharPicFn)(u8* area, int css, int kind, int costume, int isTeam, int team, int teamSet);
    typedef void (*AreaFn)(u8* area);
    typedef void (*StateSetFn)(u8* obj, int state);
    typedef void (*StateUpdateFn)(u8* obj);
    typedef void (*SetVisFn)(void* scnMdl, bool vis);
    typedef void (*StarsFn)(u8* area, int count, int kind);
    static const StarsFn SHOW_STARS = (StarsFn)0x806977FC;
    static const SetCharFn SET_CHAR = (SetCharFn)0x80696F60;
    static const SetCharPicFn SET_CHAR_PIC = (SetCharPicFn)0x8069742C;
    static const AreaFn DECIDE = (AreaFn)0x8069A4DC;
    static const AreaFn SHOW_TEAM = (AreaFn)0x80698A1C;
    static const StateSetFn STATE_SET = (StateSetFn)0x800FD96C;
    static const StateUpdateFn STATE_UPDATE = (StateUpdateFn)0x800FD8E0;
    static const SetVisFn SET_VIS = (SetVisFn)0x80043D20;   // nwSMSetVisibility

    static const int CSS_NONE = 0x28;
    enum PanelState { ST_NONE = 0, ST_SEEKING = 1, ST_SELECTING = 2, ST_BRAWLING = 6 };

    struct Shown {
        u8 kind;          // PanelView kind shown (0xFF: not set up yet)
        u8 css, costume, team, host;
        char name[NAME_CHARS + 1];
    };
    static Shown s_shown[4];
    static int s_meHostShown = -1;
    static u8* s_task = NULL;      // the CSS these panels belong to
    static bool s_teams = false;
    static bool s_seekingLabelled = false;
    static int s_myTeamShown = -1;
    static u32 s_shownMask[4];   // per area: models that were visible when it was hidden
    static bool s_hidden[4];

    static u8* cssTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        return isPtr(task) ? (u8*)task : NULL;
    }

    static u8* area(u8* task, int i)
    {
        u32 a = *(u32*)(task + 0x44 + 4 * i);
        return isPtr(a) ? (u8*)a : NULL;
    }

    void prepareRecords()
    {
        u32 gg = *(u32*)0x805A00E0;
        if (!isPtr(gg)) return;
        u32 sel = *(u32*)(gg + 0x10);
        if (!isPtr(sel)) return;
        for (int i = 1; i < 4; i++) {
            u8* rec = (u8*)sel + 0xB8 + 0x5C * i;
            rec[0x01] = 0;                     // human: the CSS builds a full panel
            ((u8*)sel)[0x0A + 4 * i] = CSS_NONE;
        }
    }

    void reset()
    {
        s_task = NULL;
        s_seekingLabelled = false;
        s_myTeamShown = -1;
        s_meHostShown = -1;
        for (int i = 0; i < 4; i++) {
            s_shown[i].kind = 0xFF;
            s_hidden[i] = false;
        }
    }

    // A closed slot: the area's models hidden. nwSMSetVisibility (0x80043D20) is ScnObj's
    // SetScnObjOption(0x10001 "not gathered", 1 / 0) (vtable +0x20); GetScnObjOption (+0x24)
    // reads it back, so the models that were shown are noted and shown again when the slot opens
    // (the game's own update, which sets them, does not run for these areas).
    static const u16 MODEL_OBJS[] = {0xB0, 0xB4, 0xB8, 0xBC, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4,
                                     0xD8, 0xDC, 0xE0, 0xE4, 0xE8, 0x13C, 0x144, 0x148, 0x14C};
    static const int N_MODELS = sizeof(MODEL_OBJS) / sizeof(MODEL_OBJS[0]);

    static void* areaModel(u8* a, int i)
    {
        u32 obj = *(u32*)(a + MODEL_OBJS[i]);
        if (!isPtr(obj)) return NULL;
        u32 mdl = *(u32*)(obj + 0xC);
        return isPtr(mdl) ? (void*)mdl : NULL;
    }

    static bool modelHidden(void* mdl)
    {
        typedef bool (*GetOptFn)(void*, u32, u32*);
        u32 v = 0;
        ((GetOptFn)(*(u32*)(*(u32*)mdl + 0x24)))(mdl, 0x10001, &v);
        return v != 0;
    }

    static void hideArea(int idx, u8* a)
    {
        if (!s_hidden[idx]) {
            u32 mask = 0;
            for (int i = 0; i < N_MODELS; i++) {
                void* m = areaModel(a, i);
                if (m && !modelHidden(m)) mask |= 1u << i;
            }
            s_shownMask[idx] = mask;
            s_hidden[idx] = true;
        }
        for (int i = 0; i < N_MODELS; i++) {
            void* m = areaModel(a, i);
            if (m) SET_VIS(m, false);
        }
    }

    static void showArea(int idx, u8* a)
    {
        if (!s_hidden[idx]) return;
        for (int i = 0; i < N_MODELS; i++) {
            void* m = areaModel(a, i);
            if (m && (s_shownMask[idx] & (1u << i))) SET_VIS(m, true);
        }
        s_hidden[idx] = false;
    }

    static u8* stateObj(u8* a)
    {
        u32 o = *(u32*)(a + 0x41C);
        return isPtr(o) ? (u8*)o : NULL;
    }

    static void plate(u8* a, const char* name)
    {
        MuMsg* m = *(MuMsg**)(a + 0x40C);
        if (!isPtr((u32)m)) return;
        // (never print a lone " ": docs/game-code.md §6)
        m->printf(0, "%s", name[0] ? name : "");
    }

    // "Seeking" -> "Searching", in the state model's own texture (once per CSS).
    static void labelSeeking(u8* a)
    {
        if (s_seekingLabelled) return;
        u8* so = stateObj(a);
        u8* obj = so ? *(u8**)(so + 0xC) : NULL;
        if (!isPtr((u32)obj)) return;
        u8* mdl = *(u8**)(obj + 8);
        u8* tex = Labels::boundTexture(mdl, "MenSelchrWifi03");
        if (!tex || !Labels::render(tex, "Searching", 1, 1, 87, 19, 0)) return;
        // "Selecting Character..." (two lines, its own dots) -> "Choosing..." (rooms.md #3).
        tex = Labels::boundTexture(mdl, "WifiInfWait05_0");
        if (tex) Labels::render(tex, "Choosing...", 2, 8, 118, 34, 1);
        s_seekingLabelled = true;
    }

    static int stateOf(u8 kind)
    {
        switch (kind) {
        case PV_SEARCHING: return ST_SEEKING;
        case PV_CHOOSING: return ST_SELECTING;
        case PV_IN_GAME: return ST_BRAWLING;
        default: return ST_NONE;
        }
    }

    static void apply(u8* task, int i, const PanelView& v, bool teams)
    {
        u8* a = area(task, i);
        if (!a) return;
        Shown& sh = s_shown[i];
        if (sh.kind == 0xFF) {
            *(u32*)a = 2;   // the plugin's panel (see above)
        }
        u8* so = stateObj(a);
        bool changed = sh.kind != v.kind || sh.css != v.css || sh.costume != v.costume ||
                       sh.team != v.team || sh.host != v.host || strcmp(sh.name, v.name) != 0;
        if (changed) {
            int st = stateOf(v.kind);
            if (so && (sh.kind == 0xFF || stateOf(sh.kind) != st)) STATE_SET(so, st);
            *(u32*)(a + 0x1C0) = v.team < PPOM::TEAM_COUNT ? v.team : 0;
            if (v.kind == PV_READY && v.css != CSS_NONE) {
                bool fresh = sh.kind != PV_READY || sh.css != v.css;
                SET_CHAR(a, v.css);
                *(u32*)(a + 0x1BC) = v.costume;
                SET_CHAR_PIC(a, v.css, 1, v.costume, teams ? 1 : 0, *(u32*)(a + 0x1C0), 0);
                if (fresh) DECIDE(a);
            } else if (*(u32*)(a + 0x1B8) != (u32)CSS_NONE) {
                SET_CHAR(a, CSS_NONE);
            }
            if (sh.team != v.team || sh.kind == 0xFF) SHOW_TEAM(a);
            plate(a, v.kind == PV_CLOSED || v.kind == PV_SEARCHING ? "" : v.name);
            SHOW_STARS(a, v.host && v.kind != PV_CLOSED && v.kind != PV_SEARCHING ? 1 : 0, 1);
            sh.host = v.host;
            sh.kind = v.kind;
            sh.css = v.css;
            sh.costume = v.costume;
            sh.team = v.team;
            strncpy(sh.name, v.name, NAME_CHARS);
            sh.name[NAME_CHARS] = 0;
        }
        if (so) STATE_UPDATE(so);
        if (v.kind == PV_CLOSED) hideArea(i, a);
        else showArea(i, a);
    }

    void tick(const PanelView views[3], bool teams, int myTeam, bool meHost)
    {
        u8* task = cssTask();
        if (!task) return;
        if (task != s_task) {
            reset();
            s_task = task;
        }
        u8* a0 = area(task, 0);
        if (a0) labelSeeking(a0);
        // The team battle look: Brawl's own (task+0x5C8), with the flag on each panel; the local
        // player clicks their own flag to change team (myTeamClicked).
        bool wasTeams = task[0x5C8] != 0;
        if (wasTeams != teams || s_teams != teams) {
            task[0x5C8] = teams ? 1 : 0;
            s_teams = teams;
            for (int i = 0; i < 4; i++) {
                u8* a = area(task, i);
                if (a) SHOW_TEAM(a);
                if (i > 0) s_shown[i].kind = 0xFF;   // redraw the others' pictures for it
            }
            s_myTeamShown = -1;
        }
        if (teams && a0 && myTeam >= 0 && myTeam < PPOM::TEAM_COUNT && myTeam != s_myTeamShown) {
            *(u32*)(a0 + 0x1C0) = (u32)myTeam;
            SHOW_TEAM(a0);
            s_myTeamShown = myTeam;
        }
        if (a0 && (int)meHost != s_meHostShown) {
            SHOW_STARS(a0, meHost ? 1 : 0, 1);
            s_meHostShown = meHost ? 1 : 0;
        }
        for (int i = 0; i < 3; i++) apply(task, i + 1, views[i], teams);
    }

    // The team the local player has picked on their own panel (Brawl's flag), or -1 when it is
    // still the one the room gave (tick's myTeam).
    int myTeamClicked()
    {
        u8* task = cssTask();
        if (!task || task != s_task || !s_teams) return -1;
        u8* a0 = area(task, 0);
        if (!a0) return -1;
        int t = (int)*(u32*)(a0 + 0x1C0);
        if (t < 0 || t >= PPOM::TEAM_COUNT || t == s_myTeamShown) return -1;
        s_myTeamShown = t;
        return t;
    }

    // Which of the other players' panels (1-3) the local hand points at, -1 none. The hand's
    // position is muSelCharHand +0x90 (x) / +0x94 (y), in the CSS's own units (found live:
    // about 31 pixels a unit in a 2484-wide screenshot; x 0 is the screen's middle, y up); the four
    // panels are centred at about x -22, -7, 8, 23, from y -1.5 (their top) to -20 (the plates).
    static const u32 HAND_X = 0x90, HAND_Y = 0x94;
    static const float PANEL_X0 = -22.0f, PANEL_STEP = 15.0f, PANEL_HALF = 7.0f;
    static const float PANEL_TOP = -1.5f, PANEL_BOTTOM = -20.0f;
    int panelUnderHand()
    {
        u8* task = cssTask();
        if (!task) return -1;
        u8* a0 = area(task, 0);
        u8* hand = a0 ? *(u8**)(a0 + 0x1A8) : NULL;
        if (!isPtr((u32)hand)) return -1;
        float x = *(float*)(hand + HAND_X), y = *(float*)(hand + HAND_Y);
        if (y > PANEL_TOP || y < PANEL_BOTTOM) return -1;
        for (int i = 1; i < 4; i++) {
            float cx = PANEL_X0 + PANEL_STEP * i;
            if (x > cx - PANEL_HALF && x < cx + PANEL_HALF) return i;
        }
        return -1;
    }
}
