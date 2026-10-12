// The room's STAGE SWITCH (docs/design/room-rules.md §2, "The stage switch"; docs/game-code.md §14,
// "Stage select and stage switch layout" and "The STAGE SWITCH"). P+'s RANDOM STAGE SWITCH page of
// the rules menu (muProcStageSwitch, the rules task's +0x274), opened in a room, shows and edits
// the room's stages instead of the player's random switch:
//  - Opened (RoomRules::open): the room's stages (SESSION RoomStages) are P+'s switch for the menu
//    (RSS_EXDATA, which the page reads when it is entered; the player's own is put back when the
//    menu closes), so a listed stage's tile is ON.
//  - A tile shows OFF, ON (a counterpick) or STR (a starter): its CLR0 MenMainSwitch0105_TopN__0 at
//    frame 0 (off) or 1 (on; the CLR0 has 61 frames but only these two looks), set with
//    muProcMenu::setAnimFrame as the page itself does, and for a starter the ON label
//    (MenMainSwitch05On, one texture for every tile's model) swapped for a copy drawn "STR" in the
//    tile's own ScnMdl. (P+'s hazard switch passes "105_TopN__0", which names no CLR0: no look.)
//  - The labels are drawn in the game's font into copies of P+'s textures: the title "STAGE
//    SWITCH" (MenMainSwitch10Panel, which a PAT0 sets every frame: its resolved TEX0 pointers in
//    the menu's brres are pointed at the copy too, and back when the menu closes), the top bar
//    (P+'s TOGGLE PAGE, MenMainSwitch11Panel) "LOSER PICKS FROM LIST", and MORE RULES' row "STAGE
//    SWITCH". Under the grid (the room CSS's ruleset line, online_menu.cpp) a legend: what ON, STR
//    and OFF mean, and the room's hazards, one switch for every stage. The footer is the stage
//    hovered (P+'s own line), or on the top row what the bar or the page button does. P+'s hazard
//    switch button (the page's object 0x80) is never in the page's scene and is not usable: put in
//    the scene group, the next draw crashed.
//  - The host: A on a tile cycles Off -> Counterpick -> Starter, A on the bar switches the loser
//    between any stage and the list (only for a ruleset whose loser picks), Z the stage hazards;
//    P+'s presets (L/R) and its whole-page toggle do nothing. When the menu closes, the changes go
//    to the server (ROOM_STAGES; the hazards with the room's rules, RoomRules::sendEdits).
//  - Everyone else (and the host during a set) only looks: the directions, B, and A on the page
//    button.

#include "stage_switch.h"

#include <OS/OSCache.h>
#include <stdio.h>
#include <string.h>

#include "labels.h"
#include "online.h"
#include "online_menu.h"
#include "ppom.h"
#include "stage_legal.h"

namespace StageSwitch {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // Every word `from` in a resource file becomes `to`: the TEX0 pointers a PAT0 resolved when
    // it was bound (the title's texture is set from one every frame, so a rebound model alone
    // went back to the game's).
    static void repoint(u8* file, u32 size, u32 from, u32 to)
    {
        if (!isPtr((u32)file) || size < 0x10 || size > 0x400000 || !from || from == to) return;
        for (u32 off = 0; off + 4 <= size; off += 4) {
            if (*(u32*)(file + off) == from) *(u32*)(file + off) = to;
        }
        DCFlushRange(file, size);
    }
    static u32 fileSize(u8* bres) { return isPtr((u32)bres) && *(u32*)bres == 0x62726573 /* bres */ ? *(u32*)(bres + 8) : 0; }

    static const u32 RSS_EXDATA = 0x8042C4E8;   // P+'s random switch: page p at +6p {u16 31.., u32 0..30}
    static const int PAGES = 5, PAGE_BYTES = PAGES * 6;
    static const u32 RULES_SWITCH = 0x274;      // muRuleTask: the stage switch page
    static const u32 RULES_MORE_ROW = 0xA4;     // muRuleTask: MORE RULES' RANDOM STAGE SWITCH row
    // muProcStageSwitch (P+ Random.asm): the objects, the cursor, the page shown, its tile count.
    static const u32 PAGE_OBJS = 0x64C, PAGE_COL = 0x670, PAGE_ROW = 0x674, PAGE_TILES = 0x704;
    static const u32 PAGE_NUMBER = 0x763;
    static const int COLS = 5;                  // the page's tiles in rows of 5 (row 0: bar, page button)
    // The page's objects (P+'s muObjects indices).
    static const int OBJ_FRAME = 0x54, OBJ_BAR = 0x55, OBJ_TILE0 = 0x57, OBJ_CURSOR = 0x81;
    static const int MAX_TILES = 31;
    static const int ANIM_CLR = 0x10;
    // module 18 (sora_menu_rule) and the DOL
    typedef void (*SetAnimFn)(u8* page, int obj, int kinds, float value);
    static const SetAnimFn SET_ANIM_FRAME = (SetAnimFn)0x806A52D4;   // muProcMenu::setAnimFrame
    static const SetAnimFn SET_ANIM_RATE = (SetAnimFn)0x806A53F8;    // muProcMenu::setAnimUpdateRate
    typedef void (*ChangeClrFn)(u8* obj, const char* name);
    static const ChangeClrFn CHANGE_CLR = (ChangeClrFn)0x800B50CC;   // MuObject::changeClrAnimN
    static const char* const CLR_TILE = "MenMainSwitch0105_TopN__0";
    typedef void (*SetPosFn)(u8* obj, float* pos);
    static const SetPosFn SET_POS = (SetPosFn)0x800B6838;            // MuObject::setPos (an offset)
    typedef void (*SetCursorFn)(u8* page);
    static const SetCursorFn SET_CURSOR = (SetCursorFn)0x806ACD80;   // muProcStageSwitch::setCursor
    // The stage select's rows of 7 (a short last row centred), from P+'s rows of 5: the tiles'
    // spacing on the page (world units, measured from the tiles' ScnMdl matrices).
    static const int GRID = 7;
    static const float TILE_DX = 8.4f, TILE_DY = 4.4f;
    static const int SE_MOVE = 0;    static const int SE_TOGGLE = 0x24, SE_ERROR = 3;


    // The input bits (room_rules.cpp): directions, A, B, L, R.
    static const u32 IN_DIRS = 0xF, IN_A = 0x10 | 0x200, IN_B = 0x20;
    static const u32 PAD_Z = 0x10;

    enum Role { OFF = 0, COUNTERPICK = 1, STARTER = 2 };

    struct LabelTex {
        u8* copy[2];      // our TEX0 copies (one per text)
        u32 orig;         // the game's image word
        u8* origTex;      // ... and TEX0
        u8* mdl;          // the model it is bound in (the menu's brres, kept when the menu closes)
        u8* brres;        // the menu's brres (its PAT0s hold resolved TEX0 pointers)
        u8* shown;        // the copy the brres's pointers are set to, NULL none
        char text[2][28];
    };

    static struct {
        bool on;              // the menu shows the room's stages
        bool canEdit;
        u8 starters[32], counterpicks[32];          // the host's edits
        u8 roomStarters[32], roomCounterpicks[32];  // the room's, when the menu opened
        u8 choice, roomChoice;                      // CHOICE_LOSER_PICKS(_LIST), 0xFF: not the loser's
        u8 fixedChoice;                             // with 0xFF: CHOICE_STRIKE, CHOICE_RANDOM, or 0xFF a set
        bool hazards, roomHazards;
        u8 savedSwitch[PAGE_BYTES];
        bool saved;
        int activeFrames;     // the switch page's input was read lately: it is the page shown
        bool drawn;           // the page's tiles drawn since it was entered
        int shownPage;
        u8 tileShown[MAX_TILES];
        int barShown;
        int navCol;           // the column the cursor goes up and down in (rows of 7)
        u8* page;             // the switch page, while it is shown (its cursor: the footer's text)
        LabelTex title, bar, moreRow;
        // Starters: their tiles' ON label (MenMainSwitch05On, the same in every tile's model)
        // swapped for "STR" in each tile's own ScnMdl.
        u8* strTex;
        u32 onWord, strWord;
        u32* onSlot[MAX_TILES];
        // The footer (P+'s preset line on the top row) as last printed: printed again when A on
        // the bar changes what it says.
        void* footMsg;
        const void* footMsbin;
        u32 footLine;
    } s;

    static void playSE(int id)
    {
        typedef void (*PlaySEFn)(void*, int, int, int, int, int);
        void* snd = *(void**)0x805A01D0;   // g_sndSystem
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }

    static bool bit(const u8* m, int id) { return PPOM::stageBit(m, id); }
    static void setBit(u8* m, int id, bool on)
    {
        if (id <= 0 || id > 255) return;
        if (on) m[id >> 3] |= (u8)(1 << (id & 7));
        else m[id >> 3] &= (u8)~(1 << (id & 7));
    }

    static int roleOf(int id)
    {
        if (bit(s.starters, id)) return STARTER;
        return bit(s.counterpicks, id) ? COUNTERPICK : OFF;
    }

    // (the DOL has no memcmp: a call to it would go to the unresolved stub)
    static bool same(const u8* a, const u8* b, int n)
    {
        for (int i = 0; i < n; i++) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    static bool anyBit(const u8* m)
    {
        for (int i = 0; i < 32; i++) {
            if (m[i]) return true;
        }
        return false;
    }

    static u8** objects(u8* page)
    {
        u8** o = *(u8***)(page + PAGE_OBJS);
        return isPtr((u32)o) ? o : NULL;
    }

    void open(bool canEdit)
    {
        const PPOM::Session& se = PPOM::g_block.session;
        // (the texture copies are kept: the labels and STR)
        memset(&s, 0, (u32)((u8*)&s.title - (u8*)&s));
        memset(s.onSlot, 0, sizeof(s.onSlot));
        s.barShown = -1;
        memcpy(s.starters, se.stages.starters, 32);
        memcpy(s.counterpicks, se.stages.counterpicks, 32);
        // An older mm sends no stages: P+'s switch as it is, read-only.
        s.on = se.play.valid && (anyBit(s.starters) || anyBit(s.counterpicks));
        if (!s.on) return;
        memcpy(s.roomStarters, s.starters, 32);
        memcpy(s.roomCounterpicks, s.counterpicks, 32);
        u8 c = se.play.stageChoice;
        s.choice = se.play.bestOf == 0 && (c == PPOM::CHOICE_LOSER_PICKS || c == PPOM::CHOICE_LOSER_PICKS_LIST) ? c : 0xFF;
        s.roomChoice = s.choice;
        s.fixedChoice = se.play.bestOf == 0 && (c == PPOM::CHOICE_STRIKE || c == PPOM::CHOICE_RANDOM) ? c : 0xFF;
        s.hazards = s.roomHazards = se.rules.hazards != 0;
        s.canEdit = canEdit;
        // P+'s switch for the menu: the room's stages on.
        memcpy(s.savedSwitch, (const void*)RSS_EXDATA, PAGE_BYTES);
        s.saved = true;
        for (int p = 0; p < PAGES; p++) {
            u32 main = 0;
            u16 ext = 0;
            for (int pos = 0; pos < 39; pos++) {
                int kind = StageLegal::kindAt(p, pos);
                if (kind <= 0 || roleOf(kind) == OFF) continue;
                if (pos < 31) main |= 1u << pos;
                else ext |= (u16)(1u << (pos - 31));
            }
            u8* sw = (u8*)RSS_EXDATA + p * 6;   // (2-aligned: halfword stores)
            *(u16*)sw = ext;
            *(u16*)(sw + 2) = (u16)(main >> 16);
            *(u16*)(sw + 4) = (u16)main;
        }
    }

    bool hazards() { return s.on ? s.hazards : PPOM::g_block.session.rules.hazards != 0; }

    static void unlabel(LabelTex& L, const char* name);

    bool close()
    {
        unlabel(s.title, "MenMainSwitch10Panel");
        unlabel(s.bar, "MenMainSwitch11Panel");
        unlabel(s.moreRow, "MenMainAddRulefont05");
        if (s.saved) memcpy((void*)RSS_EXDATA, s.savedSwitch, PAGE_BYTES);
        s.saved = false;
        bool changed = s.on && s.canEdit &&
                       (!same(s.starters, s.roomStarters, 32) || !same(s.counterpicks, s.roomCounterpicks, 32) ||
                        s.choice != s.roomChoice);
        s.on = false;
        s.activeFrames = 0;
        return changed;
    }

    void send()
    {
        PPOM::RoomStagesRequest req;
        memset(&req, 0, sizeof(req));
        req.op = PPOM::ROOM_STAGES;
        req.choice = s.choice;
        memcpy(req.stages.starters, s.starters, 32);
        memcpy(req.stages.counterpicks, s.counterpicks, 32);
        PPOM::post(PPOM::CMD_ROOM, &req, sizeof(req));
    }

    // ------------------------------------------------------------------------------------------
    // Labels.

    // Draw `text` into a copy of the object's `name` texture (two copies, one per text: `which`)
    // and show it.
    static void label(u8* obj, const char* name, LabelTex& L, int which, const char* text, int box[4])
    {
        if (!isPtr((u32)obj)) return;
        u8* mdl = *(u8**)(obj + 8);
        if (!isPtr((u32)mdl)) return;
        if (!L.copy[which]) {
            u8* cur = Labels::boundTexture(mdl, name);
            if (!cur) return;
            if (!L.orig) {
                L.orig = Labels::imageWord(cur);
                L.origTex = cur;
            }
            // (a copy of ours that is bound now is the same texture: copy the game's from it)
            L.copy[which] = Labels::copyTexture(cur);
            if (!L.copy[which]) return;
            L.text[which][0] = 1;
        }
        if (strcmp(L.text[which], text) != 0) {
            int w = *(u16*)(L.copy[which] + 0x1C), h = *(u16*)(L.copy[which] + 0x1E);
            int x0 = box ? box[0] : 1, y0 = box ? box[1] : 1, x1 = box ? box[2] : w - 1, y1 = box ? box[3] : h - 1;
            u32 fmt = *(u32*)(L.copy[which] + 0x20);
            if (!Labels::render(L.copy[which], text, x0, y0, x1, y1, fmt == 2 ? 3 : 0)) return;
            strncpy(L.text[which], text, sizeof(L.text[which]) - 1);
            L.text[which][sizeof(L.text[which]) - 1] = 0;
        }
        u32 from[3] = {L.orig, L.copy[0] ? Labels::imageWord(L.copy[0]) : 0, L.copy[1] ? Labels::imageWord(L.copy[1]) : 0};
        Labels::bindTexture(obj, name, L.copy[which], from, 3);
        L.mdl = mdl;
        if (L.shown != L.copy[which]) {
            u8* bres = *(u8**)(obj + 4);
            u32 size = fileSize(bres);
            if (size) {
                repoint(bres, size, (u32)(L.shown ? L.shown : L.origTex), (u32)L.copy[which]);
                L.brres = bres;
                L.shown = L.copy[which];
            }
        }
    }

    // The game's texture again (the menu is closed: its brres stays with the CSS).
    static void unlabel(LabelTex& L, const char* name)
    {
        if (L.shown && L.brres && fileSize(L.brres)) repoint(L.brres, fileSize(L.brres), (u32)L.shown, (u32)L.origTex);
        L.shown = NULL;
        if (L.mdl && L.origTex && *(u32*)L.mdl == 0x4D444C30 /* MDL0 */) Labels::bindModel(L.mdl, name, L.origTex);
        L.mdl = NULL;
    }

    // ------------------------------------------------------------------------------------------

    // The tile's ON label word in its own ScnMdl (found once a tile shows it), or NULL.
    static u32* onSlot(u8* obj, int i)
    {
        if (s.onSlot[i]) return s.onSlot[i];
        u8* scn = *(u8**)(obj + 0xC);
        if (!isPtr((u32)scn) || !s.onWord) return NULL;
        u8* bufs[3] = {scn, *(u8**)(scn + 0x14C), *(u8**)(scn + 0x140)};
        for (int b = 0; b < 3; b++) {
            u8* p = bufs[b];
            if (!isPtr((u32)p)) continue;
            u32 len = b == 0 ? 0x2000 : 0x1000;
            for (u32 off = 0; off < len; off += 4) {
                u32 w = *(u32*)(p + off), r = w >> 24, a = w & 0xFFFFFF;
                if (!((r >= 0x94 && r <= 0x97) || (r >= 0xB4 && r <= 0xB7))) continue;
                if (a == s.onWord || a == s.strWord) return s.onSlot[i] = (u32*)(p + off);
            }
        }
        return NULL;
    }

    static void starterLabel(u8* obj, int i, bool starter)
    {
        if (!s.strTex) {
            u8* mdl = *(u8**)(obj + 8);
            u8* on = isPtr((u32)mdl) ? Labels::boundTexture(mdl, "MenMainSwitch05On") : NULL;
            if (!on) return;
            u8* t = Labels::copyTexture(on);
            if (!t) return;
            int w = *(u16*)(t + 0x1C), h = *(u16*)(t + 0x1E);
            if (!Labels::render(t, "STR", 0, 1, w, h - 1, 0)) return;
            s.strTex = t;
            s.onWord = Labels::imageWord(on);
            s.strWord = Labels::imageWord(t);
        }
        u32* slot = onSlot(obj, i);
        if (!slot) return;
        u32 want = (*slot & 0xFF000000u) | (starter ? s.strWord : s.onWord);
        if (*slot != want) *slot = want;
    }

    // ------------------------------------------------------------------------------------------
    // Rows of 7.

    // The column a tile is drawn in (a short last row centred: half columns).
    static float gridCol(int i, int n)
    {
        int row = i / GRID, last = (n - 1) / GRID, inLast = n - last * GRID;
        float shift = row == last ? (GRID - inLast) * 0.5f : 0.0f;
        return (i % GRID) + shift;
    }

    // P+ draws tile i at column i % 5, row i / 5: the offset to its column and row of 7.
    static void tileOffset(int i, int n, float* pos)
    {
        pos[0] = (gridCol(i, n) - (i % 5)) * TILE_DX;
        pos[1] = -((i / GRID) - (i / 5)) * TILE_DY;
        pos[2] = 0.0f;
    }

    // The cursor's frame (its own object, at P+'s place for the tile) with its tile.
    static void placeCursor(u8* page, u8** objs, int n)
    {
        u8* cur = objs[OBJ_CURSOR];
        if (!isPtr((u32)cur)) return;
        int col = *(int*)(page + PAGE_COL), row = *(int*)(page + PAGE_ROW);
        int tile = row >= 1 ? (row - 1) * COLS + col : -1;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        if (tile >= 0 && tile < n) tileOffset(tile, n, pos);
        SET_POS(cur, pos);
    }

    static void placeTiles(u8* page, u8** objs, int n)
    {
        for (int i = 0; i < n; i++) {
            u8* obj = objs[OBJ_TILE0 + i];
            if (!isPtr((u32)obj)) continue;
            float pos[3];
            tileOffset(i, n, pos);
            SET_POS(obj, pos);
        }
        placeCursor(page, objs, n);
    }

    // The tile in row `row` (rows of 7) nearest column `col`.
    static int tileIn(int row, float col, int n)
    {
        int first = row * GRID, count = n - first < GRID ? n - first : GRID;
        int best = first;
        float bestD = 99.0f;
        for (int k = 0; k < count; k++) {
            float d = gridCol(first + k, n) - col;
            if (d < 0) d = -d;
            if (d < bestD) { bestD = d; best = first + k; }
        }
        return best;
    }

    // A D-pad move on the page, in rows of 7: P+'s cursor (rows of 5) set to the tile, and its
    // setCursor (the highlight, the footer) as P+'s own move does. Row 0 is the bar (left) and the
    // page button (right, over the last two columns).
    static void navigate(u8* page, u32 dirs)
    {
        int n = *(int*)(page + PAGE_TILES);
        if (n <= 0) return;
        if (n > MAX_TILES) n = MAX_TILES;
        int col = *(int*)(page + PAGE_COL), row = *(int*)(page + PAGE_ROW);
        int rows = (n + GRID - 1) / GRID;
        int tile = row >= 1 ? (row - 1) * COLS + col : -1;   // -1: row 0
        if (tile >= n) tile = n - 1;
        float vcol = tile >= 0 ? gridCol(tile, n) : (col >= COLS ? GRID - 1 : (s.navCol < GRID - 2 ? s.navCol : 0));
        int target = tile;                 // -1: row 0
        bool pageButton = row == 0 && col >= COLS;
        if (dirs & 1) {                    // up
            if (tile < 0) target = tileIn(rows - 1, vcol, n);
            else target = tile / GRID == 0 ? -1 : tileIn(tile / GRID - 1, vcol, n);
            if (target < 0) pageButton = vcol >= GRID - 2;
        } else if (dirs & 2) {             // down
            if (tile < 0) target = tileIn(0, vcol, n);
            else target = tile / GRID + 1 >= rows ? -1 : tileIn(tile / GRID + 1, vcol, n);
            if (target < 0) pageButton = vcol >= GRID - 2;
        } else if (dirs & (4 | 8)) {       // left / right: along the row (round), or bar <-> page
            if (tile < 0) {
                pageButton = !pageButton;
            } else {
                int first = (tile / GRID) * GRID, count = n - first < GRID ? n - first : GRID;
                int k = tile - first + ((dirs & 8) ? 1 : count - 1);
                target = first + k % count;
                vcol = gridCol(target, n);
            }
        }
        if (target >= 0) {
            *(int*)(page + PAGE_COL) = target % COLS;
            *(int*)(page + PAGE_ROW) = target / COLS + 1;
            s.navCol = (int)(gridCol(target, n) + 0.5f);
        } else {
            *(int*)(page + PAGE_COL) = pageButton ? COLS : 0;
            *(int*)(page + PAGE_ROW) = 0;
        }
        if (*(int*)(page + PAGE_COL) != col || *(int*)(page + PAGE_ROW) != row) {
            playSE(SE_MOVE);
            SET_CURSOR(page);
            // The cursor's offset for its new tile now: P+'s place for it changed this frame, and
            // with the old tile's offset until the next tick it showed for a frame over another
            // stage (staging feedback 2026-10-12, "the highlight flickers on other stages").
            u8** objs = objects(page);
            if (objs) placeCursor(page, objs, n);
        }
    }

    static void showTile(u8* page, u8** objs, int i, int role)
    {
        u8* obj = objs[OBJ_TILE0 + i];
        if (!isPtr((u32)obj)) return;
        CHANGE_CLR(obj, CLR_TILE);
        SET_ANIM_RATE(page, OBJ_TILE0 + i, ANIM_CLR, 0.0f);
        SET_ANIM_FRAME(page, OBJ_TILE0 + i, ANIM_CLR, role == OFF ? 0.0f : 1.0f);
        s.tileShown[i] = (u8)role;
    }

    void tick(u8* rt)
    {
        u8* page = *(u8**)(rt + RULES_SWITCH);
        // MORE RULES' row, whenever the menu is open in a room.
        u8* more = *(u8**)(rt + RULES_MORE_ROW);
        label(more, "MenMainAddRulefont05", s.moreRow, 0, "STAGE SWITCH", NULL);
        if (!isPtr((u32)page)) return;
        bool active = s.activeFrames > 0;
        if (s.activeFrames > 0) s.activeFrames--;
        if (!active) {
            s.drawn = false;
            return;
        }
        u8** objs = objects(page);
        if (!objs) return;
        label(objs[OBJ_FRAME], "MenMainSwitch10Panel", s.title, 0, "STAGE SWITCH", NULL);
        if (!s.on) return;
        label(objs[OBJ_BAR], "MenMainSwitch11Panel", s.bar, 0, "LOSER PICKS FROM LIST", NULL);
        int pnum = *(u8*)(page + PAGE_NUMBER);
        if (!s.drawn || pnum != s.shownPage) {
            // Entered, or another page: P+ drew its tiles from the switch; ours over them.
            memset(s.tileShown, 0xFF, sizeof(s.tileShown));
            s.barShown = -1;
            s.shownPage = pnum;
            s.drawn = true;
        }
        int n = *(int*)(page + PAGE_TILES);
        if (n > MAX_TILES) n = MAX_TILES;
        placeTiles(page, objs, n);
        for (int i = 0; i < n; i++) {
            int kind = StageLegal::kindAt(pnum, i);
            int role = kind > 0 ? roleOf(kind) : OFF;
            if (s.tileShown[i] != role) showTile(page, objs, i, role);
            if (isPtr((u32)objs[OBJ_TILE0 + i])) starterLabel(objs[OBJ_TILE0 + i], i, role == STARTER);
        }
        // ON: the loser picks from the list (or a set, strikes or random, which always use it).
        int bar = s.choice != PPOM::CHOICE_LOSER_PICKS ? 1 : 0;
        if (s.barShown != bar) {
            SET_ANIM_FRAME(page, OBJ_BAR, ANIM_CLR, bar ? 1.0f : 0.0f);
            s.barShown = bar;
        }
    }

    static u32 padPressed()
    {
        u32 ps = *(u32*)0x805A0040;   // g_gfPadSystem: the sys pads' pressed field (+0xC)
        if (!isPtr(ps)) return 0;
        u32 b = 0;
        for (int p = 0; p < 4; p++) b |= *(volatile u32*)(ps + 0x244 + 0x40 * p + 0xC);
        return b;
    }

    u32 input(u8* page, u32 bits)
    {
        s.activeFrames = 2;
        s.page = page;
        u32 keep = bits & (IN_DIRS | IN_B);
        if (s.on && (bits & IN_DIRS)) {
            navigate(page, bits & IN_DIRS);   // rows of 7 (P+'s own move would go by rows of 5)
            keep &= ~IN_DIRS;
        }
        int col = *(int*)(page + PAGE_COL), row = *(int*)(page + PAGE_ROW);
        bool pageButton = row == 0 && col >= COLS;
        if ((bits & IN_A) && pageButton) keep |= bits & IN_A;   // P+ turns the page, for everyone
        if (!s.on || !s.canEdit) return keep;
        if (bits & IN_A) {
            int tile = row >= 1 ? (row - 1) * COLS + col : -1;
            int n = *(int*)(page + PAGE_TILES);
            if (row == 0 && !pageButton) {
                if (s.choice == 0xFF) {
                    playSE(SE_ERROR);   // strike, random, a set: the launcher's ruleset decides
                } else {
                    s.choice = s.choice == PPOM::CHOICE_LOSER_PICKS ? PPOM::CHOICE_LOSER_PICKS_LIST : PPOM::CHOICE_LOSER_PICKS;
                    playSE(SE_TOGGLE);
                    // The footer says what A does now: printed again (MuMsg::printIndex, through
                    // Text's hook).
                    typedef void (*PrintIndexFn)(void* msg, u32 window, u32 line, const void* msbin);
                    if (isPtr((u32)s.footMsg)) ((PrintIndexFn)0x800B91B8)(s.footMsg, 0, s.footLine, s.footMsbin);
                }
            } else if (tile >= 0 && tile < n && tile < MAX_TILES) {
                int kind = StageLegal::kindAt(*(u8*)(page + PAGE_NUMBER), tile);
                if (kind > 0) {
                    int role = (roleOf(kind) + 1) % 3;   // Off -> Counterpick -> Starter -> Off
                    setBit(s.counterpicks, kind, role == COUNTERPICK);
                    setBit(s.starters, kind, role == STARTER);
                    playSE(SE_TOGGLE);
                }
            }
        }
        if (padPressed() & PAD_Z) {   // (the legend under the grid shows it)
            s.hazards = !s.hazards;
            playSE(SE_TOGGLE);
        }
        return keep;
    }

    static bool shown() { return s.on && s.activeFrames > 0; }

    bool legend(char* buf, int size)
    {
        if (!shown() || size < 96) return false;
        sprintf(buf, s.canEdit ? "ON = counterpick    STR = starter    OFF = not played    Hazards (Z): %s, every stage"
                               : "ON = counterpick    STR = starter    OFF = not played    Hazards: %s, every stage",
                s.hazards ? "ON" : "OFF");
        return true;
    }

    // The footer on the top row: what the bar or the page button does.
    static const char* rowText()
    {
        // (the page's first footer is printed before its first input: the cursor on the bar)
        u8* page = s.page;
        if (isPtr((u32)page)) {
            if (*(int*)(page + PAGE_ROW) != 0) return NULL;
            if (*(int*)(page + PAGE_COL) >= COLS) return "A: the next page of stages.";
        }
        if (s.choice == 0xFF) {
            if (s.fixedChoice == PPOM::CHOICE_STRIKE) return "Every game's stage is struck from the list.";
            if (s.fixedChoice == PPOM::CHOICE_RANDOM) return "Every game's stage is drawn from the list.";
            return "In a set the loser always picks from the list.";
        }
        if (!s.canEdit) {
            return s.choice == PPOM::CHOICE_LOSER_PICKS_LIST ? "The loser picks only from the list."
                                                             : "The loser picks any stage, variants too.";
        }
        return s.choice == PPOM::CHOICE_LOSER_PICKS_LIST ? "A: the loser picks any stage instead."
                                                         : "A: the loser picks only from the list.";
    }

    const char* menuText(void* msg, const void* msbin, u32 line)
    {
        if (!s.on) return NULL;
        // The footer's msbin: P+'s presets (line 0 "Default (L/R to swap preset)", up to 8), then
        // every stage's name at its icon + 8 (P+'s own), printed as the cursor moves.
        if (Online::msbinLineContains(msbin, 0, "swap preset")) {
            if (line >= 8) return NULL;
            s.footMsg = msg;
            s.footMsbin = msbin;
            s.footLine = line;
            const char* row = rowText();
            if (row) return row;
            return s.canEdit ? "A: off / counterpick / starter." : "The room's stages.";
        }
        if (Online::msbinLineContains(msbin, (int)line, "open/clos")) return " ";
        if (Online::msbinLineContains(msbin, (int)line, "Adjust stages in random selection")) {
            return s.canEdit ? "The room's stages: starters, counterpicks and hazards." : "The room's stages and hazards.";
        }
        return NULL;
    }
}
