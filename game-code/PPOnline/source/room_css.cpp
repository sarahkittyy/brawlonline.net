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
#include <OS/OSCache.h>
#include <gf/gf_heap_manager.h>
#include <memory.h>
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
    typedef void (*StateColorFn)(u8* obj, int color);
    static const StarsFn SHOW_STARS = (StarsFn)0x806977FC;
    static const SetCharFn SET_CHAR = (SetCharFn)0x80696F60;
    static const SetCharPicFn SET_CHAR_PIC = (SetCharPicFn)0x8069742C;
    static const AreaFn DECIDE = (AreaFn)0x8069A4DC;
    static const AreaFn SHOW_TEAM = (AreaFn)0x80698A1C;
    static const StateSetFn STATE_SET = (StateSetFn)0x800FD96C;
    static const StateUpdateFn STATE_UPDATE = (StateUpdateFn)0x800FD8E0;
    // The state model's colour (its CLR0 frame: 0 red, 1 blue, 2 yellow, 3 green), as the state
    // object is made with its panel's (so+4) and as showTeam sets it from area+0x1B0.
    static const StateColorFn STATE_COLOR = (StateColorFn)0x800FD878;
    static const SetVisFn SET_VIS = (SetVisFn)0x80043D20;   // nwSMSetVisibility

    static const int CSS_NONE = 0x28;
    enum PanelState { ST_NONE = 0, ST_SEEKING = 1, ST_SELECTING = 2, ST_CLOSED = 3, ST_READY = 4, ST_BRAWLING = 6 };
    // Team battle: the state models in the team's colour (red, blue, green).
    static const int TEAM_STATE_COLOR[PPOM::TEAM_COUNT] = {0, 1, 3};
    // The state model's draw priority (its ScnObj +0xD0 opaque, +0xD1 translucent): 0x0E puts it
    // behind the panel's character picture; a ready player's "Ready" box is drawn in front of it
    // (the picture shows through the box's translucent card, as Brawl's stage-select states).
    static const u8 STATE_PRIORITY_FRONT = 0x17;

    struct Shown {
        u8 kind;          // PanelView kind shown (0xFF: not set up yet)
        u8 css, costume, team, host, inRoom;
        u8 prio;          // the state model's own draw priority (0: not read yet)
        char name[NAME_CHARS + 1];
    };
    static Shown s_shown[4];
    static int s_meHostShown = -1;
    static u8* s_task = NULL;      // the CSS these panels belong to
    static bool s_teams = false;
    static bool s_seekingLabelled = false;
    static int s_myTeamShown = -1;
    // A team the player just picked on their own panel (Brawl's click, which plays the flag's
    // animation itself): the room's view still has the old one for a few frames, and showing
    // that and then the new one played the flag twice (staging feedback 2026-10-10). The room's
    // team is not shown again until it agrees or this runs out.
    static int s_myTeamPicked = -1;
    static int s_myTeamPickedFrames = 0;
    static u32 s_shownMask[4];   // per area: models that were visible when it was hidden
    static bool s_hidden[4];
    // The local plate: the account name, printed again a few frames after Brawl may have printed
    // its own (the name tag or "PLAYER 1": a tag picked, the tag list closed).
    static char s_myPlate[NAME_CHARS + 1];
    static int s_myTagShown = -2;
    static int s_myHandMode = -1;
    static int s_myPlateDelay = 0;
    static int s_meReadyShown = -1;   // myReady's last state (ready | teams | team)
    static int s_localSlot = 0;       // the slot the local player's panel (area 0) is shown at
    static bool s_inRoom = false;
    static int s_colorShown[4] = {-1, -1, -1, -1};   // the slot colour each area was given
    static u8 s_mePrio = 0;

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
        s_myTeamPicked = -1;
        s_myTeamPickedFrames = 0;
        s_meHostShown = -1;
        s_myPlate[0] = 0;
        s_myTagShown = -2;
        s_myHandMode = -1;
        s_myPlateDelay = 0;
        s_meReadyShown = -1;
        s_mePrio = 0;
        s_localSlot = 0;
        for (int i = 0; i < 4; i++) s_colorShown[i] = -1;
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

    static int areaSlot(int a);
    static void showTeamAt(u8* ar, int a);
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
    // The states' labels, drawn as large as their textures allow, white letters with a
    // two-texel outline (the panel's colour shows through it), as Brawl's own:
    //   Seeking (I4)                  -> "Open"     an open slot nobody has taken
    //   Selecting Character... (CMPR) -> "Choosing..."
    //   Selecting Stage... (CMPR)     -> "Closed"   a slot the host has closed
    //   Practice Stage... (CMPR)      -> "Ready"    a player locked in for the next game
    //   Brawling... (CMPR)            -> "Brawling..." again, in the same style
    // (staging feedback 2026-10-10: the open and closed slots must say so; the state text was
    // hard to read). They are drawn into copies of the textures (in the Network heap, made
    // once, kept for every CSS), and the state models are pointed at the copies, not into the
    // game's own textures: back from a match the CSS's archive is loaded again at the same
    // address, and Dolphin's texture cache (which samples a texture to tell it changed) kept
    // showing the reloaded original, partly overdrawn (found live).
    struct StateLabel {
        const char* tex;
        const char* text;
        s16 box[4];
        s16 rim;
    };
    static const StateLabel STATE_LABELS[5] = {
        {"MenSelchrWifi03", "Open", {1, 1, 87, 19}, 0},
        {"WifiInfWait05_0", "Choosing...", {1, 4, 119, 37}, 2},
        {"WifiInfWait05_1", "Closed", {2, 4, 102, 37}, 2},
        {"WifiInfWait05_2", "Ready", {2, 4, 94, 37}, 2},
        {"WifiInfWait07_0", "Brawling...", {1, 1, 111, 23}, 2},
    };
    static u8* s_labelCopy[5];        // our TEX0s (header + image), 32-byte aligned
    static u32 s_labelFrom[5], s_labelTo[5];   // image words (physical >> 5): the game's, ours

    static u32 texBytes(const u8* tex)
    {
        int w = *(u16*)(tex + 0x1C), h = *(u16*)(tex + 0x1E);
        u32 fmt = *(u32*)(tex + 0x20);
        if (fmt != 0 && fmt != 14) return 0;   // I4 and CMPR: 8x8 blocks of 32 bytes
        return (u32)(((w + 7) / 8) * ((h + 7) / 8) * 32);
    }

    static void retarget(u8* p, u32 size, u32 from, u32 to)
    {
        // GXTexObj image words (MDL0 texture infos) and the display lists' texture image
        // registers (BP 0x61, 0x94-0x97 / 0xB4-0xB7, 24-bit address >> 5).
        for (u32 off = 0; off + 4 <= size; off += 4) {
            if (*(u32*)(p + off) == from) *(u32*)(p + off) = to;
        }
        for (u32 off = 0; off + 5 <= size; off++) {
            u8* q = p + off;
            if (q[0] != 0x61 || !((q[1] >= 0x94 && q[1] <= 0x97) || (q[1] >= 0xB4 && q[1] <= 0xB7))) continue;
            if ((((u32)q[2] << 16) | ((u32)q[3] << 8) | q[4]) != (from & 0xFFFFFF)) continue;
            q[2] = (u8)(to >> 16);
            q[3] = (u8)(to >> 8);
            q[4] = (u8)to;
        }
    }

    static void labelModel(u8* mdl)
    {
        for (int k = 0; k < 5; k++) {
            const StateLabel& L = STATE_LABELS[k];
            u8* tex = Labels::boundTexture(mdl, L.tex);
            if (!tex || tex == s_labelCopy[k]) continue;   // not there, or already ours
            u32 bytes = texBytes(tex);
            if (!bytes || *(u32*)(tex + 0x10) != 0x40) continue;
            if (!s_labelCopy[k]) {
                u8* m = (u8*)gfHeapManager::alloc(Heaps::Network, 0x40 + bytes + 32);
                if (!m) continue;
                s_labelCopy[k] = (u8*)(((u32)m + 31) & ~31u);
                memcpy(s_labelCopy[k], tex, 0x40 + bytes);
                Labels::render(s_labelCopy[k], L.text, L.box[0], L.box[1], L.box[2], L.box[3], L.rim);
            }
            s_labelFrom[k] = ((u32)(tex + 0x40) & 0x1FFFFFFF) >> 5;
            s_labelTo[k] = ((u32)(s_labelCopy[k] + 0x40) & 0x1FFFFFFF) >> 5;
            Labels::rebindTexture(mdl, L.tex, s_labelCopy[k]);
            u32 size = *(u32*)(mdl + 4);   // the MDL0's own size
            if (size > 0x40 && size < 0x100000) retarget(mdl, size, s_labelFrom[k], s_labelTo[k]);
            DCFlushRange(mdl, size);
        }
    }

    // A state model's ScnMdl keeps its own copies of the materials' texture objects (GX words
    // 0x94.. << 24 | image >> 5, as the WITH FRIENDS pill, netmenu.cpp), here in the material
    // buffer at ScnMdl+0x14C (found live, e.g. "Ready" at +0xE48): they are pointed at our copies
    // every frame (only the game's five images are matched).
    static void labelWords(u8* p, u32 size)
    {
        if (!isPtr((u32)p)) return;
        for (u32 off = 0; off < size; off += 4) {
            u32 w = *(u32*)(p + off);
            u32 r = w >> 24;
            if (!((r >= 0x94 && r <= 0x97) || (r >= 0xB4 && r <= 0xB7))) continue;
            for (int k = 0; k < 5; k++) {
                if (s_labelFrom[k] && (w & 0xFFFFFF) == s_labelFrom[k]) {
                    *(u32*)(p + off) = (w & 0xFF000000u) | s_labelTo[k];
                    break;
                }
            }
        }
    }

    static void labelScnMdl(u8* obj)
    {
        u8* scn = *(u8**)(obj + 0xC);
        if (!isPtr((u32)scn)) return;
        labelWords(*(u8**)(scn + 0x14C), 0x1000);
        labelWords(*(u8**)(scn + 0x140), 0x1000);
    }


    // ------------------------------------------------------------------------------------------
    // "Open"'s dots: the state model's own three dots (bones dot, dot1, dot2) follow the label
    // where "Seeking" ended; "Open" is shorter, so they are moved left by about a character
    // (staging feedback 2026-10-10). Their places are constant translations in the model's CHR0
    // (node entry +8: x; P+'s values 3.3, 4.2, 5.1, 0.9 apart), shared by the four areas and
    // loaded again with the CSS: an entry still at P+'s value is moved (other values are left
    // alone: moved already, or another file).
    static const float DOT_SHIFT = 1.0f;
    static const float DOT_X[3] = {3.3f, 4.2f, 5.1f};

    static void moveDots(u8* obj)
    {
        u8* anim = *(u8**)(obj + 0x14);
        u8* chr = isPtr((u32)anim) ? *(u8**)(anim + 0xC) : NULL;
        u8* res = isPtr((u32)chr) ? *(u8**)(chr + 0x2C) : NULL;
        if (!isPtr((u32)res) || *(u32*)res != 0x43485230 /* CHR0 */ || *(u32*)(res + 8) != 4) return;
        u8* dic = res + *(s32*)(res + 0x10);
        u32 n = *(u32*)(dic + 4);
        static const char* const NAMES[3] = {"dot", "dot1", "dot2"};
        for (u32 i = 1; i <= n && i < 128; i++) {
            u8* e = dic + 8 + 16 * i;
            const char* name = (const char*)(dic + *(s32*)(e + 8));
            for (int k = 0; k < 3; k++) {
                if (strcmp(name, NAMES[k]) != 0) continue;
                u8* node = dic + *(s32*)(e + 12);
                if (*(u32*)(node + 4) != 0x013FE039) break;   // constant translation (as found)
                float* x = (float*)(node + 8);
                float d = *x - DOT_X[k];
                if (d > -0.05f && d < 0.05f) *x = DOT_X[k] - DOT_SHIFT;
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    // The room's two settings on the CSS's own ITEM and STAGE buttons (right end of the bar,
    // MenSelchrState0005 / 0006 at muSelCharTask +0x420 / +0x424; hidden on the other online
    // CSSs, online_menu.cpp hideHeaderArt): ITEM shows "PUBLIC" / "PRIVATE", STAGE "FFA" /
    // "TEAMS", and the game's own press on them (hand buttons 0x19 / 0x1A, online_menu.cpp
    // cssSwitchButtons) switches them. Their label textures (MenSelchrWifi00Button11 / 12, CMPR
    // 48x24: a dark rounded border, white letters with a black outline, clear inside; the
    // button's colour is its material) are copied, once per label, with the new word drawn in
    // the border (texels 4..44 x 4..20, the border kept), and the button is pointed at the copy
    // for its state, as the state labels are (labelModel / labelWords).
    struct HeaderLabel {
        u32 obj;          // muSelCharTask offset
        const char* tex;
        const char* text[2];
    };
    static const HeaderLabel HEADER[2] = {
        {0x420, "MenSelchrWifi00Button11", {"PRIVATE", "PUBLIC"}},
        {0x424, "MenSelchrWifi00Button12", {"FFA", "TEAMS"}},
    };
    static u8* s_hdrCopy[2][2];
    static u32 s_hdrOrig[2];      // the game's image word of each

    static u32 imageWord(const u8* tex) { return ((u32)(tex + 0x40) & 0x1FFFFFFF) >> 5; }

    static void showHeaderLabel(u8* task, int b, int which)
    {
        const HeaderLabel& H = HEADER[b];
        u8* obj = *(u8**)(task + H.obj);
        if (!isPtr((u32)obj)) return;
        u8* mdl = *(u8**)(obj + 8);
        if (!isPtr((u32)mdl) || *(u32*)mdl != 0x4D444C30) return;
        u8* cur = Labels::boundTexture(mdl, H.tex);
        if (!cur) return;
        bool ours = cur == s_hdrCopy[b][0] || cur == s_hdrCopy[b][1];
        if (!ours) {
            u32 bytes = texBytes(cur);
            if (!bytes || *(u32*)(cur + 0x10) != 0x40) return;
            s_hdrOrig[b] = imageWord(cur);
            for (int k = 0; k < 2; k++) {
                if (s_hdrCopy[b][k]) continue;
                u8* m = (u8*)gfHeapManager::alloc(Heaps::Network, 0x40 + bytes + 32);
                if (!m) return;
                s_hdrCopy[b][k] = (u8*)(((u32)m + 31) & ~31u);
                memcpy(s_hdrCopy[b][k], cur, 0x40 + bytes);
                Labels::renderInBox(s_hdrCopy[b][k], H.text[k], 4, 4, 44, 20, 1);
            }
        }
        u8* want = s_hdrCopy[b][which];
        if (!want) return;
        if (cur != want) {
            Labels::rebindTexture(mdl, H.tex, want);
            u32 size = *(u32*)(mdl + 4);
            if (size > 0x40 && size < 0x100000) {
                retarget(mdl, size, imageWord(cur), imageWord(want));
                DCFlushRange(mdl, size);
            }
        }
        // the ScnMdl's own copies of the texture objects (every frame, as labelScnMdl)
        u8* scn = *(u8**)(obj + 0xC);
        if (!isPtr((u32)scn)) return;
        u32 to = imageWord(want);
        u32 from[3] = {s_hdrOrig[b], imageWord(s_hdrCopy[b][0]), s_hdrCopy[b][1] ? imageWord(s_hdrCopy[b][1]) : 0};
        u8* bufs[3] = {scn, *(u8**)(scn + 0x14C), *(u8**)(scn + 0x140)};
        for (int i = 0; i < 3; i++) {
            u8* p = bufs[i];
            if (!isPtr((u32)p)) continue;
            u32 len = i == 0 ? 0x2000 : 0x1000;
            for (u32 off = 0; off < len; off += 4) {
                u32 w = *(u32*)(p + off);
                u32 r = w >> 24;
                if (!((r >= 0x94 && r <= 0x97) || (r >= 0xB4 && r <= 0xB7))) continue;
                u32 a = w & 0xFFFFFF;
                if (a != to && (a == from[0] || a == from[1] || a == from[2])) *(u32*)(p + off) = (w & 0xFF000000u) | to;
            }
        }
    }

    void headerButtons(bool isPublic, bool teams)
    {
        u8* task = cssTask();
        if (!task) return;
        showHeaderLabel(task, 0, isPublic ? 1 : 0);
        showHeaderLabel(task, 1, teams ? 1 : 0);
    }

    static void labelSeeking(u8* a, int ai)
    {
        (void)ai;
        u8* so = stateObj(a);
        u8* obj = so ? *(u8**)(so + 0xC) : NULL;
        if (!isPtr((u32)obj)) return;
        u8* mdl = *(u8**)(obj + 8);
        if (!isPtr((u32)mdl) || *(u32*)mdl != 0x4D444C30) return;
        labelModel(mdl);
        labelScnMdl(obj);
        moveDots(obj);
        s_seekingLabelled = true;
    }

    static int stateOf(u8 kind)
    {
        switch (kind) {
        case PV_CLOSED: return ST_CLOSED;
        case PV_SEARCHING: return ST_SEEKING;
        case PV_CHOOSING: return ST_SELECTING;
        case PV_READY: return ST_READY;
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
        bool changed = sh.kind != v.kind || sh.inRoom != (u8)s_inRoom || sh.css != v.css || sh.costume != v.costume ||
                       sh.team != v.team || sh.host != v.host || strcmp(sh.name, v.name) != 0;
        if (changed) {
            int st = (v.kind == PV_CLOSED && !s_inRoom) ? ST_NONE : stateOf(v.kind);
            if (so && (sh.kind == 0xFF || *(int*)(so + 8) != st)) STATE_SET(so, st);   // (so+8: the state shown)
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
            if (sh.team != v.team || sh.kind == 0xFF) showTeamAt(a, i);
            if (so) {
                // Colour: the team's in a team battle, else the panel's own; after showTeam,
                // which sets the panel's.
                int color = teams && v.team < PPOM::TEAM_COUNT ? TEAM_STATE_COLOR[v.team] : so[4] & 3;
                STATE_COLOR(so, color);
                // Ready: the box in front of the character picture.
                u8* mo = *(u8**)(so + 0xC);
                u8* scn = isPtr((u32)mo) ? *(u8**)(mo + 0xC) : NULL;
                if (isPtr((u32)scn)) {
                    if (!sh.prio) sh.prio = scn[0xD0];
                    u8 pr = v.kind == PV_READY ? STATE_PRIORITY_FRONT : sh.prio;
                    scn[0xD0] = scn[0xD1] = pr;
                }
            }
            plate(a, v.kind == PV_CLOSED || v.kind == PV_SEARCHING ? "" : v.name);
            SHOW_STARS(a, v.host && v.kind != PV_CLOSED && v.kind != PV_SEARCHING ? 1 : 0, 1);
            sh.host = v.host;
            sh.inRoom = (u8)s_inRoom;
            sh.kind = v.kind;
            sh.css = v.css;
            sh.costume = v.costume;
            sh.team = v.team;
            strncpy(sh.name, v.name, NAME_CHARS);
            sh.name[NAME_CHARS] = 0;
        }
        if (so) STATE_UPDATE(so);
        // In a room a closed slot stays on screen with "Closed" on it, so that the host sees
        // where to open one; not in a room (Join Room before the code) the others are hidden.
        if (v.kind == PV_CLOSED && !s_inRoom) hideArea(i, a);
        else showArea(i, a);
    }


    // ------------------------------------------------------------------------------------------
    // Panels in slot order (staging feedback 2026-10-10: a joiner saw the players in other
    // panels than the host did): the panel at slot p shows the room's slot p (P1-P4) on every
    // screen, the local player's own too. Only area 0 has the controller (its hand, coin, name
    // tag), so area 0 stays the local player's and is shown at the local slot L, and area L
    // (which would be there) is shown at slot 0; the other areas stay. Each area's models are
    // MuObjects placed by their own animations; MuObject::setPos (0x800B6838) adds an offset,
    // scaled by the CSS's own scale (0.99 world units a unit, measured on every model kind),
    // and a slot is 16 world units (measured on all four areas), so a model moves d slots with
    // the offset d * 16 / 0.99. Brawl's own hit tests (the panel under the hand, the name
    // tag's pencil) go by the models, so they follow.
    // The panel's colour (area+0x1B0, applied by showTeam: the panel, its hand and coin) and the
    // "P<n>" of its state box (state object +4) are the slot's.
    static const float SLOT_POS = 16.0f / 0.99f;
    // Every model of an area (MuObjects at +0xB0..+0x14C: the panel, picture, plate, flag,
    // stars, ...; +0x204..+0x230: the name tag list and its rows; the keypad's at
    // +0x3B4..+0x3D0), but those that hang from one of them (slotChild).
    static const int SLOT_OBJ_COUNT = (0x150 - 0xB0) / 4 + (0x234 - 0x204) / 4 + (0x3D4 - 0x3B4) / 4;
    // Models that hang from another and move with it (found live: moving only the parent moved
    // them by the same amount): the character pictures and the name plate (+0xB4..+0xCC) from
    // the panel (+0xB0), the name tag list's rows (+0x208..+0x230) from the list (+0x204).
    static bool slotChild(int off)
    {
        return (off >= 0xB4 && off <= 0xCC) || (off >= 0x208 && off <= 0x230);
    }

    static int slotObjOffset(int i)
    {
        if (i < (0x150 - 0xB0) / 4) return 0xB0 + 4 * i;
        i -= (0x150 - 0xB0) / 4;
        if (i < (0x234 - 0x204) / 4) return 0x204 + 4 * i;
        i -= (0x234 - 0x204) / 4;
        return 0x3B4 + 4 * i;
    }

    static int areaSlot(int a)
    {
        int L = s_localSlot;
        if (L <= 0 || L > 3) return a;
        if (a == 0) return L;
        if (a == L) return 0;
        return a;
    }

    static void placeObj(u8* obj, float dSlot)
    {
        if (!isPtr((u32)obj)) return;
        float want = dSlot * SLOT_POS;
        float* pos = (float*)(obj + 0x3C);
        if (pos[0] == want) return;
        float p[3] = {want, pos[1], pos[2]};
        typedef void (*SetPosFn)(u8*, float*);
        ((SetPosFn)0x800B6838)(obj, p);
    }

    // area+0x1B0 is the area's player number: the panel's colour when showTeam runs, but also
    // the buffer its character picture is loaded into (setCharPic), which its picture model
    // reads (found live: with the numbers of two areas swapped, neither showed its character).
    // So it stays the area's own, and is the slot's only while showTeam colours the panel.
    static void showTeamAt(u8* ar, int a)
    {
        u32 own = *(u32*)(ar + 0x1B0);
        *(u32*)(ar + 0x1B0) = (u32)areaSlot(a);
        SHOW_TEAM(ar);
        *(u32*)(ar + 0x1B0) = own;
        if (a == 0) s_myTagShown = -2;   // showTeam printed the plate: the name again (myPlate)
    }

    static void placeAreas(u8* task)
    {
        for (int a = 0; a < 4; a++) {
            u8* ar = area(task, a);
            if (!ar) continue;
            int slot = areaSlot(a);
            int pa = a;   // its own place
            for (int i = 0; i < SLOT_OBJ_COUNT; i++) {
                int off = slotObjOffset(i);
                u8* o = *(u8**)(ar + off);
                // a MuObject: its +0xC is a g3d ScnMdl (vtable 0x804663C8)
                if (!isPtr((u32)o) || !isPtr(*(u32*)(o + 0xC)) || **(u32**)(o + 0xC) != 0x804663C8) continue;
                // A child moves with its parent, so its own offset stays 0. (The name tag list's
                // rows are given the list's offset by the game when the list opens: put back.)
                placeObj(o, slotChild(off) ? 0.0f : (float)(slot - pa));
            }
            u8* so = stateObj(ar);
            if (so) placeObj(*(u8**)(so + 0xC), (float)(slot - pa));
            // the slot's colour (showTeam with the slot as the panel's colour, showTeamAt)
            if (s_colorShown[a] != slot) {
                s_colorShown[a] = slot;
                showTeamAt(ar, a);
                if (a > 0) s_shown[a].kind = 0xFF;
            }
            if (so && so[4] != slot) {
                so[4] = (u8)slot;
                if (a > 0) s_shown[a].kind = 0xFF;   // the state again, with its "P<n>"
                else s_meReadyShown = -1;
            }
        }
    }

    void setLocalSlot(int slot, bool inRoom)
    {
        s_localSlot = slot >= 0 && slot < 4 ? slot : 0;
        s_inRoom = inRoom;
    }
    int portOfArea(int a) { return areaSlot(a); }

    // The local panel's plate: the account name (Slippi shows the player's name; others see it
    // on their panels too). The name tag stays the player's choice: its controls go with the
    // lock-in from area+0x1C8 (online_menu.cpp portValuesOf), whatever the plate says, and the
    // tag list (the plate's pencil) still opens and picks. Brawl prints the tag (or "PLAYER 1")
    // when a tag is picked or the list closes: the name goes back a few frames later.
    static void myPlate(u8* a0, const char* myName)
    {
        if (!myName || !myName[0]) return;
        u8* hand = *(u8**)(a0 + 0x1A8);
        int mode = isPtr((u32)hand) ? *(int*)(hand + 0xA4) : -1;
        // (showTeam prints the plate again too: the team is part of the key)
        int tag = (*(int*)(a0 + 0x1C8) & 0xFFFF) | ((*(int*)(a0 + 0x1C0) & 0xFF) << 16) | (s_teams ? 1 << 24 : 0) |
                  ((*(int*)(a0 + 0x1B0) & 0xF) << 25);
        if (mode != s_myHandMode || tag != s_myTagShown || strcmp(s_myPlate, myName) != 0) {
            s_myHandMode = mode;
            s_myTagShown = tag;
            strncpy(s_myPlate, myName, NAME_CHARS);
            s_myPlate[NAME_CHARS] = 0;
            s_myPlateDelay = 3;
        }
        if (s_myPlateDelay > 0 && --s_myPlateDelay == 0) plate(a0, s_myPlate);
    }

    // The local player's own panel: "Ready" while locked in (the others see the same on theirs),
    // with the state object Brawl gives every area (area 0's is otherwise unused online).
    static void myReady(u8* a0, bool ready, bool teams, int myTeam)
    {
        u8* so = stateObj(a0);
        if (!so) return;
        int key = (ready ? 1 : 0) | (teams ? 2 : 0) | ((myTeam & 0xF) << 4) | (so[4] << 8);
        if (key != s_meReadyShown) {
            STATE_SET(so, ready ? ST_READY : ST_NONE);
            STATE_COLOR(so, teams && myTeam >= 0 && myTeam < PPOM::TEAM_COUNT ? TEAM_STATE_COLOR[myTeam] : so[4] & 3);
            u8* mo = *(u8**)(so + 0xC);
            u8* scn = isPtr((u32)mo) ? *(u8**)(mo + 0xC) : NULL;
            if (isPtr((u32)scn)) {
                if (!s_mePrio) s_mePrio = scn[0xD0];
                scn[0xD0] = scn[0xD1] = ready ? STATE_PRIORITY_FRONT : s_mePrio;
            }
            s_meReadyShown = key;
        }
        STATE_UPDATE(so);
    }

    void tick(const PanelView views[3], bool teams, int myTeam, bool meHost, const char* myName, bool meReady)
    {
        u8* task = cssTask();
        if (!task) return;
        if (task != s_task) {
            reset();
            s_task = task;
        }
        u8* a0 = area(task, 0);
        // Each area's state model (they share one MDL0; each has its own ScnMdl).
        for (int i = 0; i < 4; i++) {
            u8* ai = area(task, i);
            if (ai) labelSeeking(ai, i);
        }
        placeAreas(task);
        // The team battle look: Brawl's own (task+0x5C8), with the flag on each panel; the local
        // player clicks their own flag to change team (myTeamClicked).
        bool wasTeams = task[0x5C8] != 0;
        if (wasTeams != teams || s_teams != teams) {
            task[0x5C8] = teams ? 1 : 0;
            s_teams = teams;
            for (int i = 0; i < 4; i++) {
                u8* a = area(task, i);
                if (a) showTeamAt(a, i);
                if (i > 0) s_shown[i].kind = 0xFF;   // redraw the others' pictures for it
            }
            s_myTeamShown = -1;
        }
        if (s_myTeamPickedFrames > 0 && (myTeam == s_myTeamPicked || --s_myTeamPickedFrames == 0)) {
            s_myTeamPickedFrames = 0;
        }
        if (teams && a0 && myTeam >= 0 && myTeam < PPOM::TEAM_COUNT && myTeam != s_myTeamShown &&
            s_myTeamPickedFrames == 0) {
            *(u32*)(a0 + 0x1C0) = (u32)myTeam;
            showTeamAt(a0, 0);
            s_myTeamShown = myTeam;
        }
        if (a0 && (int)meHost != s_meHostShown) {
            SHOW_STARS(a0, meHost ? 1 : 0, 1);
            s_meHostShown = meHost ? 1 : 0;
        }
        if (a0) myPlate(a0, myName);
        if (a0) myReady(a0, meReady, teams, myTeam);
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
        s_myTeamPicked = t;
        s_myTeamPickedFrames = 90;
        return t;
    }

    // Which of the other players' panels (1-3) the local hand points at, -1 none. The hand's
    // position is muSelCharHand +0x90 (x) / +0x94 (y), in the CSS's own units (found live:
    // about 31 pixels a unit in a 2484-wide screenshot; x 0 is the screen's middle, y up); the four
    // panels are centred at about x -22, -7, 8, 23, from y -1.5 (their top) to -20 (the plates).
    static const u32 HAND_X = 0x90, HAND_Y = 0x94;
    static const float PANEL_X0 = -22.0f, PANEL_STEP = 15.0f, PANEL_HALF = 7.0f;
    static const float PANEL_TOP = -1.5f, PANEL_BOTTOM = -20.0f;
    // The local player's own team flag (Brawl's, top left of the panel, drawn in a team battle):
    // A on it picks the next team. Brawl's own team click works on the panel itself (its hand
    // target 0x1B, from y -6 down: myTeamClicked) but not on the flag above it (no target
    // there), so the plugin hit-tests the flag's upper part: found live, x -29.5..-23,
    // y -6..-2.5. (Both on the panel played the flag's animation twice.)
    bool handOverMyFlag()
    {
        u8* task = cssTask();
        if (!task || !s_teams) return false;
        u8* a0 = area(task, 0);
        u8* hand = a0 ? *(u8**)(a0 + 0x1A8) : NULL;
        if (!isPtr((u32)hand)) return false;
        float x = *(float*)(hand + HAND_X) - PANEL_STEP * s_localSlot, y = *(float*)(hand + HAND_Y);
        return x >= PANEL_X0 - 7.5f && x <= PANEL_X0 - 1.0f && y > -6.0f && y <= -2.5f;
    }

    int panelUnderHand()
    {
        u8* task = cssTask();
        if (!task) return -1;
        u8* a0 = area(task, 0);
        u8* hand = a0 ? *(u8**)(a0 + 0x1A8) : NULL;
        if (!isPtr((u32)hand)) return -1;
        float x = *(float*)(hand + HAND_X), y = *(float*)(hand + HAND_Y);
        if (y > PANEL_TOP || y < PANEL_BOTTOM) return -1;
        for (int i = 0; i < 4; i++) {
            if (i == s_localSlot) continue;   // the player's own
            float cx = PANEL_X0 + PANEL_STEP * i;
            if (x > cx - PANEL_HALF && x < cx + PANEL_HALF) return i;
        }
        return -1;
    }
}
