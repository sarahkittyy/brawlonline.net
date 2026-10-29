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
//                                            1-5 stars, more as a star and "x N"): during a set the
//                                            side's set wins, else the host's mark, one star
//                                            (rooms.md #8: a reused Brawl icon). One star display
//                                            per panel: each call takes the area's star models
//                                            out of the group (area+0xAC) and puts back those it
//                                            shows (one star model, visibility frame = count, or
//                                            a star and up to 3 digit models); 0 shows none.
//   area+0x40C MuMsg, window 0              the name plate ("PLAYER 2" -> the player's name)
//   area+0x41C panel state object (DOL 0x800FD630..): states 0 none, 1 "Seeking..",
//            2 "Selecting Character...", 3 "Selecting Stage...", 4 "Practice Stage...",
//            5 "Bye" (then 1), 6 "Brawling..."; set with 0x800FD96C(obj, state), animated by
//            0x800FD8E0(obj) every frame (which text+0x12210 no longer does for these areas).
// The "Seeking" texture (MenSelchrWifi03, in the state model MenSelchrState0000) is drawn again
// as "Searching" with the game's font (the model's own dots follow it), and "Selecting
// Character..." (WifiInfWait05_0, CMPR) as "Choosing...".
//
// Stock control (P+'s STOCK CONTROL, the rules' stockControl; docs/game-code.md §14 "Stock
// control on the room CSS"): every panel's STOCKS box (area+0xD4, P+'s look of Brawl's handicap
// box) is shown or taken away with
//   0x80697720 showStockBox(area, on)       takes the box's model out of the area's scene group
//                                            (area+0x404) and puts it back when on
// as the CSS does itself when P+'s rules menu closes with another handicap mode (sel_char
// text+0x10874: task+0x5CC the mode it shows, 2 = stock control). The local hand's A on its box
// (hand button 0x1E) opens Brawl's picker (0%, 10%, ... which P+ reads in tenths: 10% = 1
// stock), which leaves the pick in area+0x1D0 and the box showing it; the others' boxes show
// their picks the way the picker's close does (sel_char text+0x184D4): the box's frames set to
// the count (MuObject 0x800B7900, 0x800B7798, 0x800B7A18) and area+0x1D0.
// Stamina (the room's STAMINA row 2, docs/design/special-rules.md): the box is Brawl's HP box, as
// on its Special Brawl CSS, which sets task+0x5D0 when it starts with stamina (text+0x2AC); with
// it A opens Brawl's HP list (list kind 3, P+'s 100 entries), whose pick in area+0x1D0 is the
// HP / 10 (the CSS reads a record's +0x24 / 10 into it, text+0x1C44). The HP box's model is made
// here (hpBox): the area makes its box when it is made, before the room's stamina is known.
#include <OS/OSCache.h>
#include <gf/gf_heap_manager.h>
#include <memory.h>
#include <mu/mu_msg.h>
#include <string.h>
#include "labels.h"
#include "online_menu.h"
#include "ppom.h"

extern "C" {
    // Set while showTeamAt has another player number in an area: OnlineMenu's cssCharPic then
    // skips the picture that showTeam draws.
    u8 g_cssPicHold = 0;
}

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

    typedef void (*ShowBoxFn)(u8* area, int on);
    typedef void (*ObjFrameFn)(u8* obj, float frame);
    typedef u8* (*HandOfFn)(u8* task, int i);
    typedef void (*HandCloseFn)(u8* hand, int a, int b);
    static const ShowBoxFn SHOW_STOCK_BOX = (ShowBoxFn)0x80697720;
    static const ObjFrameFn BOX_FRAME[3] = {(ObjFrameFn)0x800B7900, (ObjFrameFn)0x800B7798, (ObjFrameFn)0x800B7A18};
    static const HandOfFn HAND_OF = (HandOfFn)0x806862A0;        // the CSS's hand i (task+0x648 hands)
    static const HandCloseFn HAND_CLOSE = (HandCloseFn)0x8069CC0C;
    static const AreaFn PICKER_CLOSE = (AreaFn)0x8069AED0;       // area+0x1FC: the list (picker) closed
    static const u32 TASK_HANDICAP_MODE = 0x5CC, TASK_HANDS = 0x648;
    static const u32 AREA_STOCK_BOX = 0xD4, AREA_STOCKS = 0x1D0;
    static const u32 HAND_MODE = 0xA4, HAND_AREA = 0xA8, HAND_BUTTON = 0xAC;
    static const int HAND_MODE_PICKER = 5, BUTTON_STOCK_BOX = 0x1E;
    static const u8 HANDICAP_STOCK_CONTROL = 2;   // set rule +5 (P+'s STOCK CONTROL on)
    // The box lower and further left than P+ has it (setPos units, as SLOT_POS): at P+'s place it
    // sits under the panel's state box ("Choosing...", "Ready"), which the CSS has only online, and
    // lower at P+'s x under the win stars (the host's star). There it is over the character
    // picture, above the name plate (screenshots, 2026-10-15); the hand's button follows the model.
    static const float STOCK_BOX_DX = -3.6f, STOCK_BOX_DY = -3.3f;
    // Stamina's box is Brawl's own HP box, MenSelchrChp4 (its count's animation has frames 0-100,
    // the STOCKS box's MenSelchrChandicap4 0-30: at 31-99, up to 990 HP, it showed 30). The area
    // makes one or the other once, when it is made (sel_char text+0x142A4: Chp4 with task+0x5D0
    // set, as on the Special Brawl CSS started in stamina); the room's CSS gets stamina later,
    // so each area's box is made again as the area makes it (text+0x14414: MuObject::create from
    // the CSS's archive, its draw priority, no texture file, the menu heap; changeAnimN(name +
    // "__0"), update rate 0) and stands in at area+0xD4 while stamina is on. The area's own is
    // kept out of its scene group meanwhile and comes back when stamina goes; the stand-in then
    // goes (it is out of the group too). Brawl's own deletes the one at area+0xD4 with the CSS,
    // as in its stamina CSS; ours deletes the one kept (taskGone, from muSelCharTask's destructor).
    typedef u8* (*CreateFn)(void* modelRes, const char* name, int priority, void* texRes, int heap);
    typedef void (*NameFn)(u8* obj, const char* name);
    typedef void (*RateFn)(void* anim, float rate);
    static const CreateFn MU_CREATE = (CreateFn)0x800B2394;     // MuObject::create(ResFile*, name, ...)
    static const NameFn CHANGE_ANIM = (NameFn)0x800B534C;       // MuObject::changeAnimN
    static const RateFn SET_UPDATE_RATE = (RateFn)0x80028650;   // gfModelAnimation::setUpdateRate
    static const ObjFrameFn SET_FRAME_VISIBLE = (ObjFrameFn)0x800B784C;   // MuObject::setFrameVisible
    static const int MENU_HEAP = 0x2A;
    static const char* const HP_BOX = "MenSelchrChp4_TopN";
    static const char* const HP_BOX_ANIM = "MenSelchrChp4_TopN__0";
    static u8* s_hpBoxTask = NULL;                          // the CSS whose areas have HP boxes
    static u8* s_ownBox[4] = {NULL, NULL, NULL, NULL};      // each area's own box while ours stands in

    static const int CSS_NONE = 0x28;
    enum PanelState { ST_NONE = 0, ST_SEEKING = 1, ST_SELECTING = 2, ST_CLOSED = 3, ST_READY = 4, ST_BRAWLING = 6 };
    // Team battle: the state models in the team's colour (red, blue, green).
    static const int TEAM_STATE_COLOR[PPOM::TEAM_COUNT] = {0, 1, 3};
    // The state model's draw priority (its ScnObj +0xD0 opaque, +0xD1 translucent): the game's
    // 0x0E puts it behind the panel's character picture; a ready player's "Ready" box is drawn in
    // front of it (the picture shows through the box's translucent card, as Brawl's stage-select
    // states). Every state is drawn there: at 0x0E the box and a team battle's flag (top left of
    // the panel, over the box's "P<n>") took turns in front of each other, frame to frame, as the
    // flag moved (staging feedback 2026-10-11); "Ready" was always in front of it.
    static const u8 STATE_PRIORITY_FRONT = 0x17;

    struct Shown {
        u8 kind;          // PanelView kind shown (0xFF: not set up yet)
        u8 css, costume, team, stars, inRoom, sittingOut;
        char name[NAME_SIZE];
    };

    // SIT OUT (the user's, 2026-10-18: "the sitting out player's portrait nameplate turns grey,
    // like as if they were a CPU"): the card in a CPU's grey, which Brawl's own showTeam
    // (0x80698A1C) draws for an area whose player kind (area+0x1B4, m_playerKind) is 2, CPU: the
    // backplate, the franchise icon and the name plate (a CPU's plate is more than a colour frame:
    // set by hand, colour, texture pattern and visibility frames left the plate in the port's
    // colour, probes 2026-10-18). showTeamAt gives showTeam that kind for a card that sits out.
    static const u32 AREA_PLAYER_KIND = 0x1B4;
    static const int KIND_CPU = 2;
    static bool s_grey[4];
    static const char* const SIT_OUT_PLATE = "SIT OUT";
    // Watching the room's game from the room CSS (2026-10-19): the same grey card.
    static const char* const WATCHING_PLATE = "WATCHING";
    static Shown s_shown[4];
    static int s_myStarsShown = -1;
    static u8* s_task = NULL;      // the CSS these panels belong to
    static bool s_teams = false;
    static bool s_seekingLabelled = false;
    static int s_myTeamShown = -1;
    static u32 s_shownMask[4];   // per area: models that were visible when it was hidden
    static bool s_hidden[4];
    // The local plate: the tag or the account name, printed again a few frames after Brawl may have
    // printed its own (the name tag or "PLAYER 1": a tag picked, the tag list closed).
    static char s_myPlate[NAME_SIZE];
    static int s_myTagShown = -2;
    static int s_myHandMode = -1;
    static int s_myPlateDelay = 0;
    static int s_myColourShown = -2;  // OnlineMenu::localColourKey when the plate was printed
    static int s_meReadyShown = -1;   // myReady's last state (ready | teams | team)
    static int s_meSitShown = -1;     // the local card's SIT OUT as last shown (1 / 0)
    static int s_localSlot = 0;       // the slot the local player's panel (area 0) is shown at
    static bool s_inRoom = false;
    static int s_colorShown[4] = {-1, -1, -1, -1};   // the slot colour each area was given
    static int s_boxShown[4] = {-1, -1, -1, -1};     // each area's STOCKS box: 1 shown, 0 not, -1 not set
    static int s_boxCount[4] = {-1, -1, -1, -1};     // the count shown on another player's box
    static bool s_stockControl = false;  // a box is shown on the local panel (STOCKS or HP)
    static int s_boxKind = BOX_NONE;     // the boxes' kind as last shown
    // The local player's pick, kept across the room's games (the CSS is built again after each
    // one, its picks at 0), as Slippi keeps the character.
    static u8 s_myStocks = 0;
    static u16 s_myHp = 0;
    static const u32 TASK_STAMINA = 0x5D0;   // muSelCharTask: Brawl's stamina flag (byte)
    static const int HP_DEFAULT_PICK = 15;   // the box's start in a stamina room: 150 HP
    // The box's label (P+'s "STOCKS", I4 80x16 in its model MenSelchrChandicap4) and a copy drawn
    // "HP" for stamina's boxes.
    static const char* const BOX_LABEL = "MenSelchrCardHandicap";
    static u8* s_boxLabel = NULL;
    static u8* s_hpLabel = NULL;

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

    // Records 1-3 as the Wi-Fi CSS had them before the first prepareRecords (releaseRecords puts
    // them back).
    static bool s_recordsSaved = false;
    static u8 s_savedState[4], s_savedCss[4];

    static u8* selCharData()
    {
        u32 gg = *(u32*)0x805A00E0;
        if (!isPtr(gg)) return NULL;
        u32 sel = *(u32*)(gg + 0x10);
        return isPtr(sel) ? (u8*)sel : NULL;
    }

    void prepareRecords()
    {
        u8* sel = selCharData();
        if (!sel) return;
        for (int i = 1; i < 4; i++) {
            u8* rec = sel + 0xB8 + 0x5C * i;
            if (!s_recordsSaved) {
                s_savedState[i] = rec[0x01];
                s_savedCss[i] = sel[0x0A + 4 * i];
            }
            rec[0x01] = 0;                     // human: the CSS builds a full panel
            sel[0x0A + 4 * i] = CSS_NONE;
        }
        s_recordsSaved = true;
    }

    // A room's CSS about to be built again as another online CSS (the launcher's Direct invite on
    // a room's CSS): records 1-3 back to what the Wi-Fi CSS had, so it builds its own panels.
    void releaseRecords()
    {
        u8* sel = selCharData();
        if (!sel || !s_recordsSaved) return;
        for (int i = 1; i < 4; i++) {
            u8* rec = sel + 0xB8 + 0x5C * i;
            rec[0x01] = s_savedState[i];
            sel[0x0A + 4 * i] = s_savedCss[i];
        }
        reset();
    }

    void reset()
    {
        s_task = NULL;
        s_seekingLabelled = false;
        s_myTeamShown = -1;
        s_myStarsShown = -1;
        s_myPlate[0] = 0;
        s_myTagShown = -2;
        s_myHandMode = -1;
        s_myPlateDelay = 0;
        s_myColourShown = -2;
        s_meReadyShown = -1;
        s_localSlot = 0;
        for (int i = 0; i < 4; i++) s_colorShown[i] = s_boxShown[i] = s_boxCount[i] = -1;
        s_stockControl = false;
        s_boxKind = BOX_NONE;
        s_boxLabel = NULL;   // (the CSS's archive goes with it)
        s_meSitShown = -1;
        for (int i = 0; i < 4; i++) {
            s_shown[i].kind = 0xFF;
            s_hidden[i] = false;
            s_grey[i] = false;
        }
    }

    static void showTeamAt(u8* ar, int a);
    // Area `ai`'s card grey (sitting out) or in its port's colours again. (showTeam prints the
    // plate too: the caller prints it again.)
    static void setGrey(u8* a, int ai, bool on)
    {
        if (on == s_grey[ai]) return;
        s_grey[ai] = on;
        showTeamAt(a, ai);
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
                s_labelCopy[k] = Labels::copyBlock(0x40 + bytes);
                if (!s_labelCopy[k]) continue;
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
    // "TEAMS" in rainbow letters (P+'s rainbow team battle: any costume, a team glow) / "TEAMS"
    // (the teams' costumes; the user's, 2026-10-21), and the game's own press on them (hand
    // buttons 0x19 / 0x1A, online_menu.cpp
    // cssSwitchButtons) switches them. Their label textures (MenSelchrWifi00Button11 / 12, CMPR
    // 48x24: a dark rounded border, white letters with a black outline, clear inside; the
    // button's colour is its material) are copied, once per label, with the new word drawn in
    // the border (texels 4..44 x 4..20, the border kept), and the button is pointed at the copy
    // for its state, as the state labels are (labelModel / labelWords).
    struct HeaderLabel {
        u32 obj;          // muSelCharTask offset
        const char* tex;
        const char* text[3];   // NULL: no such state
        int rainbow;           // the state whose letters are rainbow, -1 none
    };
    static const HeaderLabel HEADER[2] = {
        {0x420, "MenSelchrWifi00Button11", {"PRIVATE", "PUBLIC", NULL}, -1},
        {0x424, "MenSelchrWifi00Button12", {"FFA", "TEAMS", "TEAMS"}, 1},
    };
    static u8* s_hdrCopy[2][3];
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
        bool ours = cur == s_hdrCopy[b][0] || cur == s_hdrCopy[b][1] || cur == s_hdrCopy[b][2];
        if (!ours) {
            u32 bytes = texBytes(cur);
            if (!bytes || *(u32*)(cur + 0x10) != 0x40) return;
            s_hdrOrig[b] = imageWord(cur);
            for (int k = 0; k < 3; k++) {
                if (s_hdrCopy[b][k] || !H.text[k]) continue;
                s_hdrCopy[b][k] = Labels::copyBlock(0x40 + bytes);
                if (!s_hdrCopy[b][k]) return;
                memcpy(s_hdrCopy[b][k], cur, 0x40 + bytes);
                Labels::renderInBox(s_hdrCopy[b][k], H.text[k], 4, 4, 44, 20, 1);
                if (k == H.rainbow) Labels::rainbowInBox(s_hdrCopy[b][k], 4, 4, 44, 20);
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
        u32 from[4] = {s_hdrOrig[b], imageWord(s_hdrCopy[b][0]), s_hdrCopy[b][1] ? imageWord(s_hdrCopy[b][1]) : 0,
                       s_hdrCopy[b][2] ? imageWord(s_hdrCopy[b][2]) : 0};
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
                if (a != to && a && (a == from[0] || a == from[1] || a == from[2] || a == from[3]))
                    *(u32*)(p + off) = (w & 0xFF000000u) | to;
            }
        }
    }

    void forgetCopies()
    {
        s_hpLabel = NULL;
        for (int k = 0; k < 5; k++) {
            s_labelCopy[k] = NULL;
            s_labelFrom[k] = s_labelTo[k] = 0;
        }
        for (int b = 0; b < 2; b++) {
            s_hdrCopy[b][0] = s_hdrCopy[b][1] = s_hdrCopy[b][2] = NULL;
            s_hdrOrig[b] = 0;
        }
    }

    void headerButtons(bool isPublic, int teams)
    {
        u8* task = cssTask();
        if (!task) return;
        showHeaderLabel(task, 0, isPublic ? 1 : 0);
        showHeaderLabel(task, 1, teams >= 0 && teams <= 2 ? teams : 0);
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
        bool sitting = v.sittingOut && (v.kind == PV_CHOOSING || v.kind == PV_READY);
        u8 sitShown = sitting ? v.sittingOut : 0;   // 1 SIT OUT, 2 WATCHING
        bool changed = sh.kind != v.kind || sh.inRoom != (u8)s_inRoom || sh.css != v.css || sh.costume != v.costume ||
                       sh.team != v.team || sh.stars != v.stars || strcmp(sh.name, v.name) != 0 ||
                       sh.sittingOut != sitShown;
        if (changed) {
            // Sitting out: no state box ("Choosing..." is not what they do), the card grey.
            int st = (v.kind == PV_CLOSED && !s_inRoom) || sitting ? ST_NONE : stateOf(v.kind);
            if (so && (sh.kind == 0xFF || *(int*)(so + 8) != st)) STATE_SET(so, st);   // (so+8: the state shown)
            *(u32*)(a + 0x1C0) = v.team < PPOM::TEAM_COUNT ? v.team : 0;
            // The character: a ready player's lock-in, or the coin a choosing one has placed (the
            // user's, 2026-10-18: seen before they ready).
            if ((v.kind == PV_READY || v.kind == PV_CHOOSING) && v.css != CSS_NONE) {
                bool fresh = sh.kind == 0xFF || sh.css != v.css;
                SET_CHAR(a, v.css);
                *(u32*)(a + 0x1BC) = v.costume;
                // The player's own costume in a team battle too (not the team's: OnlineMenu's
                // cssCharPic makes every portrait on the online CSS the costume's).
                SET_CHAR_PIC(a, v.css, 1, v.costume, 0, *(u32*)(a + 0x1C0), 0);
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
                // The box in front of the character picture and the team flag.
                u8* mo = *(u8**)(so + 0xC);
                u8* scn = isPtr((u32)mo) ? *(u8**)(mo + 0xC) : NULL;
                if (isPtr((u32)scn)) scn[0xD0] = scn[0xD1] = STATE_PRIORITY_FRONT;
            }
            setGrey(a, i, sitting);
            plate(a, v.kind == PV_CLOSED || v.kind == PV_SEARCHING ? "" :
                     sitShown == 2                                ? WATCHING_PLATE :
                     sitting                                      ? SIT_OUT_PLATE :
                                                                    v.name);
            SHOW_STARS(a, v.kind != PV_CLOSED && v.kind != PV_SEARCHING ? v.stars : 0, 1);
            sh.sittingOut = sitShown;
            sh.stars = v.stars;
            sh.inRoom = (u8)s_inRoom;
            sh.kind = v.kind;
            sh.css = v.css;
            sh.costume = v.costume;
            sh.team = v.team;
            strncpy(sh.name, v.name, NAME_SIZE - 1);
            sh.name[NAME_SIZE - 1] = 0;
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
    // "P<n>" of its state box (state object +4) are the slot's. With P+'s slot colours, out of a
    // team battle, each area is drawn in its own line and the lines hold the slots' colours
    // (OnlineMenu's cssSlotColours): P+'s L / R / Z step the line of the area under the hand, area
    // 0's for the local panel. (A team battle's panels are drawn in the lines of the teams' ports,
    // red P1's, blue P2's, green P4's, which keep their own colours there.)
    static const float SLOT_POS = 16.0f / 0.99f;
    // Every model of an area (MuObjects at +0xB0..+0x14C: the panel, picture, plate, flag,
    // stars, ...; +0x204..+0x230: the name tag list and its rows; the keypad's at
    // +0x3B4..+0x3D0), but those that hang from one of them (slotChild).
    static const int SLOT_OBJ_COUNT = (0x150 - 0xB0) / 4 + (0x234 - 0x204) / 4 + (0x3D4 - 0x3B4) / 4;
    // Models that hang from another and move with it (found live: moving only the parent moved
    // them by the same amount): the character pictures and the name plate (+0xB4..+0xCC) and the
    // STOCKS box (+0xD4, 2026-10-15: given the slot's offset too it was drawn a slot further
    // away) from the panel (+0xB0), the name tag list's rows (+0x208..+0x230) from the list (+0x204).
    static bool slotChild(int off)
    {
        return (off >= 0xB4 && off <= 0xCC) || off == 0xD4 || (off >= 0x208 && off <= 0x230);
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

    static void placeObj(u8* obj, float dSlot, float dx = 0.0f)
    {
        if (!isPtr((u32)obj)) return;
        float want = dSlot * SLOT_POS + dx;
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
    // showTeam also draws the area's picture (setCharPic, at its end), and P+'s AsyncRSP loads
    // pictures per player number (+0x1B0): drawn with the slot's number, the load went to that
    // player's picture and, when it was done, AsyncRSP drew it on the area with that number
    // (bug #22, 2026-10-15: a joiner at P3 saw P1's Mario on their own panel, and their own
    // costume changes did not show after that). So that picture is skipped (cssCharPic); the
    // area's picture is the same before and after, as no character or costume changes here.
    static void showTeamAt(u8* ar, int a)
    {
        u32 own = *(u32*)(ar + 0x1B0);
        u32 colour = !s_teams && OnlineMenu::localColourKey() >= 0 ? own : (u32)areaSlot(a);
        *(u32*)(ar + 0x1B0) = colour;
        g_cssPicHold = own != colour ? 1 : 0;
        // Sitting out: showTeam draws a CPU's grey card (its kind for the call only).
        int kind = *(int*)(ar + AREA_PLAYER_KIND);
        if (s_grey[a]) *(int*)(ar + AREA_PLAYER_KIND) = KIND_CPU;
        SHOW_TEAM(ar);
        *(int*)(ar + AREA_PLAYER_KIND) = kind;
        g_cssPicHold = 0;
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
                placeObj(o, slotChild(off) ? 0.0f : (float)(slot - pa), off == (int)AREA_STOCK_BOX ? STOCK_BOX_DX : 0.0f);
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
    bool teamsShown() { return s_teams; }

    // The local panel's plate: `myName` is the name tag picked on it, else the account name
    // (Slippi shows the player's name; others see the account name on their panels). The tag's
    // controls go with the lock-in from area+0x1C8 (online_menu.cpp portValuesOf), and the tag
    // list (the plate's pencil) opens and picks as offline. Brawl prints the tag (or "PLAYER 1")
    // when a tag is picked or the list closes: ours goes back a few frames later.
    static void myPlate(u8* a0, const char* myName)
    {
        if (!myName || !myName[0]) return;
        u8* hand = *(u8**)(a0 + 0x1A8);
        int mode = isPtr((u32)hand) ? *(int*)(hand + 0xA4) : -1;
        // (showTeam prints the plate again too: the team is part of the key)
        int tag = (*(int*)(a0 + 0x1C8) & 0xFFFF) | ((*(int*)(a0 + 0x1C0) & 0xFF) << 16) | (s_teams ? 1 << 24 : 0) |
                  ((*(int*)(a0 + 0x1B0) & 0xF) << 25);
        // (P+'s L / R / Z on the player box print "PLAYER 1" too: after the colour, P+ presses the
        // panel's player kind button for it, which prints the plate)
        int colour = OnlineMenu::localColourKey();
        if (mode != s_myHandMode || tag != s_myTagShown || colour != s_myColourShown || strcmp(s_myPlate, myName) != 0) {
            s_myHandMode = mode;
            s_myTagShown = tag;
            s_myColourShown = colour;
            strncpy(s_myPlate, myName, NAME_SIZE - 1);
            s_myPlate[NAME_SIZE - 1] = 0;
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
            if (isPtr((u32)scn)) scn[0xD0] = scn[0xD1] = STATE_PRIORITY_FRONT;
            s_meReadyShown = key;
        }
        STATE_UPDATE(so);
    }

    // ---- Stock control ----------------------------------------------------------------------

    static u8* setRule()
    {
        u32 gg = *(u32*)0x805A00E0;
        if (!isPtr(gg)) return NULL;
        u32 r = *(u32*)(gg + 0x1C);
        return isPtr(r) ? (u8*)r : NULL;
    }

    // A box's count as the picker's close leaves it (sel_char text+0x184D4): its frames and area+0x1D0.
    static void showCount(u8* a, int n)
    {
        u8* box = *(u8**)(a + AREA_STOCK_BOX);
        if (!isPtr((u32)box)) return;
        for (int k = 0; k < 3; k++) BOX_FRAME[k](box, (float)n);
        *(int*)(a + AREA_STOCKS) = n;
    }

    static void deleteObj(u8* obj)
    {
        typedef void (*DelFn)(u8*, int);   // MuObject's virtual destructor (vtable +0x5C, entry +8)
        ((DelFn)(*(u32*)(*(u32*)(obj + 0x5C) + 8)))(obj, 1);
    }

    // Area i's box as the kind needs it: Brawl's HP box (made now) or the area's own (the HP box
    // deleted). Taken out of the area's scene group first (stockBoxes shows it again). Returns
    // whether the HP box is there.
    static bool hpBox(u8* task, int i, bool hp)
    {
        if (s_hpBoxTask != task) {   // (another CSS's: gone with it)
            for (int k = 0; k < 4; k++) s_ownBox[k] = NULL;
            s_hpBoxTask = task;
        }
        u8* a = area(task, i);
        u8* cur = a ? *(u8**)(a + AREA_STOCK_BOX) : NULL;
        if (!isPtr((u32)cur)) return false;
        if (hp == (s_ownBox[i] != NULL)) return hp;
        SHOW_STOCK_BOX(a, 0);
        if (!hp) {
            *(u8**)(a + AREA_STOCK_BOX) = s_ownBox[i];
            s_ownBox[i] = NULL;
            deleteObj(cur);
            return false;
        }
        u8* scn = *(u8**)(cur + 0x10);
        u8* obj = MU_CREATE(cur + 4, HP_BOX, isPtr((u32)scn) ? scn[0xD0] : 0x10, NULL, MENU_HEAP);
        if (!isPtr((u32)obj)) return false;
        CHANGE_ANIM(obj, HP_BOX_ANIM);
        void* anim = *(void**)(obj + 0x14);
        if (isPtr((u32)anim)) SET_UPDATE_RATE(anim, 0.0f);
        // As the area's set-up does (text+0x11CA0): its parts' visibility at frame 1 (0 hides them).
        SET_FRAME_VISIBLE(obj, 1.0f);
        // The area's calc-world callback the CSS gave its box (one of area+0x154's, text+0x14570;
        // ScnObj +0xD4, its timing and options +0xD8).
        u8* ours = *(u8**)(obj + 0x10);
        if (isPtr((u32)scn) && isPtr((u32)ours)) {
            *(u32*)(ours + 0xD4) = *(u32*)(scn + 0xD4);
            *(u32*)(ours + 0xD8) = *(u32*)(scn + 0xD8);
        }
        s_ownBox[i] = cur;
        *(u8**)(a + AREA_STOCK_BOX) = obj;
        return true;
    }

    void taskGone(u8* task)
    {
        if (task != s_hpBoxTask) return;
        for (int i = 0; i < 4; i++) {
            if (s_ownBox[i]) deleteObj(s_ownBox[i]);
            s_ownBox[i] = NULL;
        }
        s_hpBoxTask = NULL;
    }

    // A box's label: P+'s STOCKS, or "HP x10" for stamina's (a copy of it drawn with the game's
    // font) when Brawl's HP box could not be made (hpBox), the STOCKS box showing HP / 10.
    static void boxLabel(u8* a, bool hp)
    {
        u8* box = *(u8**)(a + AREA_STOCK_BOX);
        u8* mdl = isPtr((u32)box) ? *(u8**)(box + 8) : NULL;
        if (!isPtr((u32)mdl) || (!hp && !s_hpLabel)) return;
        if (!s_boxLabel) s_boxLabel = Labels::boundTexture(mdl, BOX_LABEL);
        if (!s_boxLabel) return;
        if (hp && !s_hpLabel) {
            s_hpLabel = Labels::copyTexture(s_boxLabel);
            if (s_hpLabel) {
                Labels::render(s_hpLabel, "HP x10", 2, 1, 78, 15, 0);   // (the box shows HP / 10)
                DCFlushRange(s_hpLabel, Labels::textureSize(s_hpLabel));
            }
        }
        if (hp && !s_hpLabel) return;
        u32 from[2] = {Labels::imageWord(s_boxLabel), s_hpLabel ? Labels::imageWord(s_hpLabel) : 0};
        Labels::bindTexture(box, BOX_LABEL, hp ? s_hpLabel : s_boxLabel, from, s_hpLabel ? 2 : 1);
    }

    // The local hand's picker closed, as the CSS closes it when the handicap mode goes off
    // (sel_char text+0x10874's loop).
    static void closePickers(u8* task)
    {
        int hands = *(int*)(task + TASK_HANDS);
        for (int i = 0; i < hands && i < 4; i++) {
            u8* h = HAND_OF(task, i);
            if (!isPtr((u32)h) || *(int*)(h + HAND_MODE) != HAND_MODE_PICKER) continue;
            int ai = *(int*)(h + HAND_AREA);
            HAND_CLOSE(h, 0, 0);
            u8* a = ai >= 0 && ai < 4 ? area(task, ai) : NULL;
            if (a) PICKER_CLOSE(a);
        }
    }

    // The boxes: the local panel's (the player picks there) and every taken slot's (the count that
    // player picked, once ready), with the room's stock control; the set rule's handicap mode and the
    // CSS's own (task+0x5CC) follow it, as P+'s rules menu leaves them.
    static void stockBoxes(u8* task, const PanelView views[3], int kind, int lockedStocks)
    {
        const bool on = kind != BOX_NONE;
        const bool hp = kind == BOX_HP;
        u8* rule = setRule();
        u8 mode = kind == BOX_STOCKS ? HANDICAP_STOCK_CONTROL : 0;
        if (rule && rule[5] != mode) rule[5] = mode;
        // The box's kind changed (stamina on or off): Brawl's stamina flag, the boxes shown again
        // and the local pick of the new kind back on its box.
        const bool kindChanged = kind != s_boxKind;
        if (kindChanged) {
            closePickers(task);
            task[TASK_STAMINA] = hp ? 1 : 0;
            for (int i = 0; i < 4; i++) {
                if (s_boxShown[i] == 1) {
                    u8* a = area(task, i);
                    if (a) SHOW_STOCK_BOX(a, 0);
                }
                hpBox(task, i, hp);
                s_boxShown[i] = -1;
            }
            s_boxKind = kind;
        }
        // (The CSS shows or takes away every box itself when P+'s rules menu closes with another
        // mode than task+0x5CC, e.g. the host's style change to time: then all of them again.)
        if (*(int*)(task + TASK_HANDICAP_MODE) != mode) {
            if (!on) closePickers(task);
            *(int*)(task + TASK_HANDICAP_MODE) = mode;
            for (int i = 0; i < 4; i++) s_boxShown[i] = -1;
        }
        s_stockControl = on;
        for (int i = 0; i < 4; i++) {
            u8* a = area(task, i);
            if (!a) continue;
            const PanelView* v = i > 0 ? &views[i - 1] : NULL;
            bool taken = v && (v->kind == PV_CHOOSING || v->kind == PV_READY || v->kind == PV_IN_GAME);
            int want = on && (i == 0 || taken) ? 1 : 0;
            if (s_boxShown[i] != want) {
                SHOW_STOCK_BOX(a, want);
                u8* box = *(u8**)(a + AREA_STOCK_BOX);
                if (want && isPtr((u32)box)) {
                    float* pos = (float*)(box + 0x3C);   // its offset (placeAreas keeps y)
                    if (pos[1] != STOCK_BOX_DY) {
                        float p[3] = {pos[0], STOCK_BOX_DY, pos[2]};
                        typedef void (*SetPosFn)(u8*, float*);
                        ((SetPosFn)0x800B6838)(box, p);
                    }
                }
                s_boxShown[i] = want;
                s_boxCount[i] = -1;
                // A new CSS (after a game) starts with no pick: the player's own comes back (an HP
                // box with none yet: 150, the match's own).
                int mine = hp ? (s_myHp ? s_myHp / 10 : HP_DEFAULT_PICK) : s_myStocks;
                if (want && i == 0 && mine && (kindChanged || *(int*)(a + AREA_STOCKS) == 0)) showCount(a, mine);
            }
            if (!want) continue;
            if (s_hpBoxTask != task || !s_ownBox[i]) boxLabel(a, hp);
            if (i == 0) {
                int n = *(int*)(a + AREA_STOCKS);
                // Locked in: the pick the lock-in carries (Brawl's picker still opened while ready).
                if (lockedStocks >= 0 && n != lockedStocks) showCount(a, n = lockedStocks);
                if (hp) {
                    if (n >= 1 && n * 10 <= PPOM::MAX_PICKED_HP) s_myHp = (u16)(n * 10);
                } else if (n >= 0 && n <= PPOM::MAX_PICKED_STOCKS) {
                    s_myStocks = (u8)n;
                }
            } else if (s_boxCount[i] != v->stocks) {
                showCount(a, v->stocks);
                // The HP box at 0 shows a "1" (its hundreds for 1000): hidden until they pick.
                if (s_hpBoxTask == task && s_ownBox[i])
                    SET_FRAME_VISIBLE(*(u8**)(a + AREA_STOCK_BOX), v->stocks ? 1.0f : 0.0f);
                s_boxCount[i] = v->stocks;
            }
        }
    }

    // Also off the CSS: the stage picker locks in again on the stage select with the pick (bug
    // #35, 2026-10-16), where reset() has turned s_stockControl off; with it here that lock-in
    // carried 0, the rules' stocks. The caller checks the rules (roomStockControl).
    u8 myStocks() { return s_myStocks; }
    u16 myHp() { return s_myHp; }

    bool handOnMyStocks()
    {
        u8* task = cssTask();
        u8* a0 = task ? area(task, 0) : NULL;
        u8* hand = a0 ? *(u8**)(a0 + 0x1A8) : NULL;
        return s_stockControl && isPtr((u32)hand) && *(int*)(hand + HAND_BUTTON) == BUTTON_STOCK_BOX;
    }

    void tick(const PanelView views[3], bool teams, int myTeam, int myStars, const char* myName, bool meReady,
              int boxKind, int lockedStocks, bool meSittingOut)
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
        // P+'s team battle with the players' own costumes (task+0x5CB = 2; 0 is Brawl's, every
        // player in the team's): P+'s setCharPic (0x80571350) and AsyncRSP (the picture it draws
        // when a load is done) give a team battle's pictures the team's costume unless it is 2.
        // Without it a team room's panels showed the team's costumes, and the local player's
        // costume changes (X / Y) not at all (bug #22, 2026-10-15).
        if (teams) task[0x5CB] = 2;
        placeAreas(task);
        // The team battle look: Brawl's own (task+0x5C8), with the flag on each panel; the local
        // player changes team with A on their own panel or its flag (OnlineMenu's tickRoom and
        // cssPanelClick), the costume with X / Y, as in P+'s rainbow TEAM BATTLE.
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
        if (teams && a0 && myTeam >= 0 && myTeam < PPOM::TEAM_COUNT && myTeam != s_myTeamShown) {
            *(u32*)(a0 + 0x1C0) = (u32)myTeam;
            showTeamAt(a0, 0);
            s_myTeamShown = myTeam;
        }
        if (a0 && myStars != s_myStarsShown) {
            SHOW_STARS(a0, myStars, 1);
            s_myStarsShown = myStars;
        }
        // Sitting out: the local card grey with "SIT OUT" on its plate, as the others see it.
        if (a0 && (int)meSittingOut != s_meSitShown) {
            setGrey(a0, 0, meSittingOut);
            s_meSitShown = meSittingOut ? 1 : 0;
        }
        if (a0) myPlate(a0, meSittingOut ? SIT_OUT_PLATE : myName);
        if (a0) myReady(a0, meReady && !meSittingOut, teams, myTeam);
        for (int i = 0; i < 3; i++) apply(task, i + 1, views[i], teams);
        stockBoxes(task, views, boxKind, lockedStocks);
    }

    // Which of the other players' panels (1-3) the local hand points at, -1 none. The hand's
    // position is muSelCharHand +0x90 (x) / +0x94 (y), in the CSS's own units (found live:
    // about 31 pixels a unit in a 2484-wide screenshot; x 0 is the screen's middle, y up); the four
    // panels are centred at about x -22, -7, 8, 23, from y -1.5 (their top) to -20 (the plates).
    static const u32 HAND_X = 0x90, HAND_Y = 0x94;
    static const float PANEL_X0 = -22.0f, PANEL_STEP = 15.0f, PANEL_HALF = 7.0f;
    static const float PANEL_TOP = -1.5f, PANEL_BOTTOM = -20.0f;

    void setMyCostume(int costume)
    {
        u8* task = cssTask();
        u8* a = task ? area(task, 0) : NULL;
        if (!a) return;
        int css = *(int*)(a + 0x1B8);
        if (css < 0 || css >= 0x40 || css == CSS_NONE || css == 0x29) return;
        *(u32*)(a + 0x1BC) = (u32)costume;
        SET_CHAR_PIC(a, css, 1, costume, 0, *(u32*)(a + 0x1C0), 0);
    }

    // The local player's own team flag (Brawl's, top left of the panel, drawn in a team battle):
    // A on it picks the next team. The flag is no hand target of Brawl's (the panel below it,
    // target 0x1B from y -6 down, picks the next team too: OnlineMenu's cssPanelClick), so
    // the plugin hit-tests the flag's upper part: found live, x -29.5..-23, y -6..-2.5.
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

    // A card's name plate under the hand. Offline the plate is Brawl's hand button 0x1D (a slot to
    // player / CPU / none) and the pencil's backdrop around it, left of it and below it, is 0x1C
    // (the name tags); on the Wi-Fi CSS the local card's plate and backdrop are both 0x1C and the
    // others' cards have no button there. So the plate is hit-tested here, where offline has 0x1D
    // (found live, harness probes 2026-10-23, a quarter unit apart: x -27.5..-15.5 on P1's card, a
    // slot 16 further on each, y -17.75..-21; the backdrop x -34..-27.75 beside it and from
    // y -21.25 down to the hand's lowest, -21.5, under it to x -16). The backdrop keeps opening
    // the name tags (the user's, 2026-10-23: the plate reached into the strip below, and they sat
    // out going to change their tag), and the hand on anything else of Brawl's (the tag list's
    // rows) is not on a plate.
    static const int BUTTON_TAGS = 0x1C;
    static const float PLATE_X0 = -21.5f, PLATE_STEP = 16.0f, PLATE_HALF = 6.0f;
    static const float PLATE_TOP = -17.75f, PLATE_BOTTOM = -21.1f;
    int plateUnderHand()
    {
        u8* task = cssTask();
        if (!task) return -1;
        u8* a0 = area(task, 0);
        u8* hand = a0 ? *(u8**)(a0 + 0x1A8) : NULL;
        if (!isPtr((u32)hand)) return -1;
        int button = *(int*)(hand + HAND_BUTTON);
        if (button != 0 && button != BUTTON_TAGS) return -1;
        float x = *(float*)(hand + HAND_X), y = *(float*)(hand + HAND_Y);
        if (y > PLATE_TOP || y < PLATE_BOTTOM) return -1;
        for (int i = 0; i < 4; i++) {
            float dx = x - (PLATE_X0 + PLATE_STEP * i);
            if (dx >= -PLATE_HALF && dx <= PLATE_HALF) return i;
        }
        return -1;
    }

    // The hand on the local card's player box (pencil and plate, Brawl's button 0x1C), where P+'s
    // slot colour hook (0x8068B168) steps the player's colour with L / R and resets it with Z.
    bool handOnMyPlayerBox()
    {
        u8* task = cssTask();
        u8* a0 = task ? area(task, 0) : NULL;
        u8* hand = a0 ? *(u8**)(a0 + 0x1A8) : NULL;
        return isPtr((u32)hand) && *(int*)(hand + HAND_BUTTON) == BUTTON_TAGS;
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
