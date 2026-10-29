// The room's rules menu as L / R page tabs (docs/design/special-rules.md §1-§5).
//
// P+'s rules menu (muRuleTask, room_rules.cpp) has RULES and MORE RULES; L / R turn the pages:
//   RULES --R--> MORE RULES --R--> MATCH SETUP --R--> SPECIAL, L back.
// R on RULES is P+'s own MORE RULES button (its cursor put there and A pressed for it), L on MORE
// RULES its B. MATCH SETUP and SPECIAL are not P+'s pages: P+'s MORE RULES page stays the open
// page under them.
//   - MATCH SETUP: MORE RULES' own rows relabelled (the RULESET / SET way, room_play.cpp), their
//     label textures and value strips pointed at copies with our texts drawn in the game's font:
//     row 0 (TIME LIMIT, whose value is P+'s digit models, rules task +0xA8..+0xBC) BANS as one
//     digit where the colon is; rows 1-4 (value strips) DSR, STAGE CHOICE, STRIKES, STOCK CONTROL (FRAME CONTROL
//     with P+'s regen, as P+'s RULES page names it); the STAGE SWITCH button ALT STAGES ON / OFF.
//     P+ keeps its cursor and highlight; left / right / A on the rows are ours.
//   - SPECIAL: P+'s Special Versus box (special_box.cpp) over the hidden MORE RULES rows.
// The copies are made in Fighter1Resoruce (free on the character select, measured: 5.4 MB) when
// a page first shows and freed when the menu closes, before any match loads (the menu is closed
// a frame before a game starts). The host's edits go to the room when the menu closes, as P+'s
// pages' do: ROOM_FORMAT for the ruleset's set and stage rows, ROOM_RULES (room_rules.cpp
// sendEdits, through rulesEdits) for stock control and the special rows.
#include "rules_pages.h"

#include <OS/OSCache.h>
#include <string.h>

#include "labels.h"
#include "online.h"
#include "ppom.h"
#include "page_hints.h"
#include "room_play.h"
#include "special_box.h"

extern "C" int sprintf(char* buf, const char* fmt, ...);

// Tests (harness, through PPOnlineMain.map).
struct RulesPagesDebug {
    u32 view;           // View
    u32 turns;          // pages turned
    u32 drawn;          // texts drawn
    u32 posts;          // ROOM_FORMAT sent
    u32 failed;         // copies that could not be made
    u8 bans, dsr, choice, alternates;
    u8 stockControl, strikeCount, strikes[6];
    char value[6][24];  // what each MATCH SETUP row shows
    u32 rt;             // the rules task
};
extern "C" {
    RulesPagesDebug g_rulesPages;
}

namespace RulesPages {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    static void playSE(int id)
    {
        typedef void (*PlaySEFn)(void*, int, int, int, int, int);
        void* snd = *(void**)0x805A01D0;   // g_sndSystem
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }
    static const int SE_CURSOR = 0, SE_DECIDE = 1, SE_ERROR = 3, SE_TOGGLE = 0x24;

    typedef void* (*AllocFn)(int heap, u32 size);
    typedef void (*FreeFn)(void* p);
    static const AllocFn HEAP_ALLOC = (AllocFn)0x800249E4;   // gfHeapManager::alloc(HeapType, size)
    static const FreeFn HEAP_FREE = (FreeFn)0x80024A4C;      // gfHeapManager::free
    static const int HEAP_FIGHTER1 = 18;                     // Fighter1Resoruce
    typedef void (*SetVisFn)(void* scnObj, bool vis);
    static const SetVisFn SET_VIS = (SetVisFn)0x80043D20;    // nwSMSetVisibility

    static const u32 RULES_PAGE2 = 0x26C;                    // muRuleTask: MORE RULES
    static const u32 PAGE_CURSOR = 0x42;                     // u16: the page's line
    static const u32 IN_MOVE = 0x1 | 0x2, IN_SIDE = 0x4 | 0x8, IN_LEFT = 0x4, IN_RIGHT = 0x8;
    static const u32 IN_A = 0x10 | 0x200, IN_B = 0x20, IN_L = 0x40, IN_R = 0x80;

    enum View { V_PPLUS = 0, V_SETUP = 1, V_SPECIAL = 2 };

    // MATCH SETUP's rows on MORE RULES' (rules task offsets of their MuObjects; the label texture,
    // the value strip; the label's box width, left-aligned as P+'s).
    enum { R_BANS = 0, R_DSR, R_CHOICE, R_STRIKES, R_CONTROL, R_ALTS, ROWS };
    struct Row { u32 obj; const char* labelTex; const char* valueTex; };
    static const Row ROW[ROWS] = {
        {0x90, "MenMainRule00font.02", NULL},                     // TIME LIMIT: P+'s digits
        {0x94, "MenMainAddRulefont02", "MenMainAddRuleSel02"},   // TEAM ATTACK
        {0x98, "MenMainAddRulefont03", "MenMainAddRuleSel05"},   // PAUSE
        {0x9C, "MenMainAddRulefont04", "MenMainAddRuleSel02"},   // AUTO L-CANCEL
        {0xA0, "MenMainAddRulefont06", "MenMainAddRuleSel02"},   // INPUT ASSIST
        {0xA4, "MenMainAddRulefont05", NULL},                     // STAGE SWITCH (a button)
    };
    // TIME LIMIT's digits (P+'s MenMainIcon01Num: PAT frame = the digit), its colon and infinity.
    static const u32 DIGITS[6] = {0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC};

    struct Tex {
        u8* mdl;             // the row's MDL0 (the menu archive's: it outlives the menu)
        u8* orig;            // the game's TEX0
        u8* copy[2];         // ours (two: a changed text goes into the one not shown)
        int shown;
        char text[32];
    };
    static struct {
        View view;
        bool canEdit;
        bool opened;
        u8* rt;
        // The values shown (the room's, then the host's edits).
        u8 bestOf;
        bool setOn;          // a set is on: the set's rows wait for it
        u8 choice, bans, dsr, alternates, stockControl;
        u8 strikes[PPOM::MAX_STRIKE_TURNS];
        u8 strikeCount;
        u8 starters;         // the room's starters (the strike pattern leaves one)
        u8 stages;           // starters and counterpicks (the bans leave one)
        bool formatEdited, rulesEdited;
        // The textures (labels: one copy, values and the button's label: two).
        Tex label[ROWS], value[ROWS];
        u8* block;           // every copy, one Fighter1Resoruce block
        u32 blockUsed, blockSize;
        bool noCopies;       // the block could not be had: MATCH SETUP stays P+'s rows
        // TIME LIMIT's digits as P+ left them (PAT frames, shown).
        float digitFrame[6];
        bool digitShown[6];
        float digitX;        // the minutes digit's x (its scene model's local matrix)
        bool digitsSaved;
        int restoreFrames;   // frames left binding P+'s own textures back
        // Which of P+'s pages takes the input (notePage) and since when (ticks).
        int pageKind;
        u32 ticks, pageSeen;
        // Turning to MATCH SETUP or SPECIAL (or back to MORE RULES): the page's rows slide in from the
        // side it comes from (frames left, +1 from the right, -1 from the left), as P+'s own turn does.
        int slideLeft, slideDir;
        // P+'s own turn back to RULES (its B; our L): played mirrored (backTurn).
        bool backPending;
        int backT, backN;
        u8* backObj[48];
        u8* backAnim[48];
        u16 backLen[48];
        // The footer (P+'s MuMsg printing MORE RULES' descriptions).
        void* footMsg;
        const void* footMsbin;
        u32 footLine;
    } s;

    bool ours() { return s.opened && s.view != V_PPLUS; }

    static u8* page2() { return s.rt ? *(u8**)(s.rt + RULES_PAGE2) : NULL; }
    static int cursor()
    {
        u8* p = page2();
        return isPtr((u32)p) ? *(u16*)(p + PAGE_CURSOR) : 0;
    }
    static u8* rowObj(int r)
    {
        u8* o = s.rt ? *(u8**)(s.rt + ROW[r].obj) : NULL;
        return isPtr((u32)o) ? o : NULL;
    }
    static u8* rowMdl(int r)
    {
        u8* o = rowObj(r);
        u8* m = o ? *(u8**)(o + 8) : NULL;
        return isPtr((u32)m) ? m : NULL;
    }

    static int bitCount(const u8* mask)
    {
        int n = 0;
        for (int i = 0; i < 32; i++) {
            for (u8 b = mask[i]; b; b &= (u8)(b - 1)) n++;
        }
        return n;
    }

    static bool strikesUsed() { return s.bestOf > 0 || s.choice == PPOM::CHOICE_STRIKE; }
    static bool regen() { return SpecialBox::row(PPOM::SR_ATTACK) == 2; }

    // ---- The values as text ------------------------------------------------------------------
    static void strikeText(char* out)
    {
        if (!strikesUsed() || s.strikeCount == 0) {
            strcpy(out, "-");
            return;
        }
        int n = 0;
        for (int i = 0; i < s.strikeCount && n < 20; i++) n += sprintf(out + n, i ? "-%d" : "%d", s.strikes[i]);
    }

    static const char* valueText(int r, char* buf)
    {
        // (a value strip's cell is 96 texels: short words)
        static const char* const CHOICES[5] = {"LOSER", "STRIKE", "RANDOM", "LIST", "ANYONE"};
        switch (r) {
        case R_DSR: return s.bestOf ? (s.dsr ? "ON" : "OFF") : "-";
        case R_CHOICE: return s.bestOf ? "SET" : CHOICES[s.choice <= PPOM::CHOICE_ANYONE_PICKS ? s.choice : 0];
        case R_STRIKES: strikeText(buf); return buf;
        case R_CONTROL: return s.stockControl ? "ON" : "OFF";
        case R_ALTS: return s.alternates ? "ALT STAGES: ON" : "ALT STAGES: OFF";
        }
        return "";
    }

    static const char* labelText(int r)
    {
        switch (r) {
        case R_BANS: return "BANS";
        case R_DSR: return "DSR";
        case R_CHOICE: return "STAGE CHOICE";
        case R_STRIKES: return "STRIKES";
        case R_CONTROL: return regen() ? "FRAME CONTROL" : "STOCK CONTROL";
        }
        return "";
    }

    static const char* rowHelp(int r)
    {
        switch (r) {
        case R_BANS:
            return s.bestOf ? "Games 2+ of a set: how many stages the winner bans."
                            : "Bans are for sets (RULES > SET).";
        case R_DSR:
            return s.bestOf ? "The loser can't pick a stage they already won on."
                            : "Dave's stupid rule is for sets (RULES > SET).";
        case R_CHOICE:
            if (s.bestOf) return "A set strikes, bans and picks in its own steps.";
            switch (s.choice) {
            case PPOM::CHOICE_LOSER_PICKS: return "LOSER: the last game's loser picks any stage.";
            case PPOM::CHOICE_LOSER_PICKS_LIST: return "LIST: the loser picks from the stage list.";
            case PPOM::CHOICE_STRIKE: return "STRIKE: strike from the starters every game.";
            case PPOM::CHOICE_ANYONE_PICKS: return "ANYONE: Everyone picks at once";
            default: return "RANDOM: a random stage from the list every game.";
            }
        case R_STRIKES:
            return strikesUsed() ? "Stages each side strikes in turn from the starters."
                                 : "Only sets and STRIKE strike stages.";
        case R_CONTROL:
            if (regen()) {
                return s.stockControl ? "Each player can set the amount of frames before their damage resets."
                                      : "Every player's damage resets after 90 frames.";
            }
            return s.stockControl ? "Each player can set an individual stock count."
                                  : "Players are all given the same stock count.";
        case R_ALTS:
            return "Hold L, R or Z on a stage pick for its alternate layout.";
        }
        return NULL;
    }

    // ---- Textures ----------------------------------------------------------------------------
    static u8* blockCopy(u8* tex0)
    {
        u32 n = Labels::textureSize(tex0);
        if (!n || !s.block || s.blockUsed + ((n + 31) & ~31u) > s.blockSize) return NULL;
        u8* d = Labels::copyTextureTo(tex0, s.block + s.blockUsed, s.blockSize - s.blockUsed);
        if (d) s.blockUsed += (n + 31) & ~31u;
        return d;
    }

    // The copies, once: labels (the button's twice), value strips twice.
    static bool makeCopies()
    {
        if (s.block || s.noCopies) return s.block != NULL;
        u32 need = 0;
        for (int r = 0; r < ROWS; r++) {
            u8* mdl = rowMdl(r);
            if (!mdl) return false;
            u8* lab = Labels::boundTexture(mdl, ROW[r].labelTex);
            u8* val = ROW[r].valueTex ? Labels::boundTexture(mdl, ROW[r].valueTex) : NULL;
            if (!lab || (ROW[r].valueTex && !val)) {
                s.noCopies = true;
                g_rulesPages.failed++;
                return false;
            }
            s.label[r].orig = lab;
            s.value[r].orig = val;
            s.label[r].mdl = mdl;
            s.value[r].mdl = val ? mdl : NULL;
            need += ((Labels::textureSize(lab) + 31) & ~31u) * (r == R_ALTS ? 2 : 1);
            if (val) need += ((Labels::textureSize(val) + 31) & ~31u) * 2;
        }
        s.block = (u8*)HEAP_ALLOC(HEAP_FIGHTER1, need + 32);
        if (!s.block) {
            s.noCopies = true;
            g_rulesPages.failed++;
            return false;
        }
        s.blockSize = need + 32;
        s.blockUsed = (u32)(((u32)s.block + 31) & ~31u) - (u32)s.block;
        for (int r = 0; r < ROWS; r++) {
            Tex& L = s.label[r];
            L.copy[0] = blockCopy(L.orig);
            L.copy[1] = r == R_ALTS ? blockCopy(L.orig) : NULL;
            L.shown = 0;
            L.text[0] = 1;
            Tex& V = s.value[r];
            if (V.orig) {
                V.copy[0] = blockCopy(V.orig);
                V.copy[1] = blockCopy(V.orig);
            }
            V.shown = 0;
            V.text[0] = 1;
        }
        return true;
    }

    // `text` into the copy not shown, and that one shown. A value strip (I4, P+'s value art side
    // by side, the value's cell scrolled into view: MORE RULES' 192 texels wide, two cells of 96;
    // RULES' 480 or 800, cells of 160) gets it in every cell, whichever P+'s animation shows: drawn
    // into the first (Labels::render writes the whole image), whose 8x8 blocks are then copied into
    // the others. A label is drawn from its left, as P+'s are, in a box as wide as the text needs.
    static void draw(Tex& T, const char* text, bool strip, int labelW)
    {
        if (!T.copy[0] || strcmp(T.text, text) == 0) return;
        int k = T.copy[1] ? T.shown ^ 1 : 0;
        u8* t = T.copy[k];
        int w = *(u16*)(t + 0x1C), h = *(u16*)(t + 0x1E);
        bool ok;
        if (strip) {
            const int cellW = w <= 192 ? 96 : 160;
            ok = Labels::render(t, text, 3, 2, cellW - 3, h - 2, 0);
            u8* img = t + 0x40;
            const int bpr = (w + 7) / 8, rows = (h + 7) / 8, cell = cellW / 8;
            for (int by = 0; ok && by < rows; by++) {
                for (int c = 1; (c + 1) * cell <= bpr; c++) {
                    memcpy(img + (by * bpr + c * cell) * 32, img + by * bpr * 32, cell * 32);
                }
            }
        } else {
            int need = 17 * (int)strlen(text) + 2;   // (P+'s label font: about 17 texels a letter)
            if (labelW > 0 && need > labelW) need = labelW;
            ok = Labels::render(t, text, 2, 1, need < w - 2 ? need : w - 2, h - 1, 0);
        }
        DCFlushRange(t, Labels::textureSize(t));
        if (!ok) return;
        T.shown = k;
        strncpy(T.text, text, sizeof(T.text) - 1);
        T.text[sizeof(T.text) - 1] = 0;
        g_rulesPages.drawn++;
    }

    static void bind(int r, Tex& T, const char* name, bool ours)
    {
        u8* obj = rowObj(r);
        if (!obj || !T.orig || !T.copy[0]) return;
        u32 from[3] = {Labels::imageWord(T.orig), Labels::imageWord(T.copy[0]),
                       T.copy[1] ? Labels::imageWord(T.copy[1]) : 0};
        Labels::bindTexture(obj, name, ours ? T.copy[T.shown] : T.orig, from, T.copy[1] ? 3 : 2);
    }

    static float slideOffset();

    // ---- TIME LIMIT's digits as BANS ----------------------------------------------------------
    static u8* digitObj(int i)
    {
        u8* o = s.rt ? *(u8**)(s.rt + DIGITS[i]) : NULL;
        return isPtr((u32)o) ? o : NULL;
    }
    static float* patFrame(u8* obj)
    {
        u8* anim = *(u8**)(obj + 0x14);
        if (!isPtr((u32)anim)) return NULL;
        u8* a = *(u8**)(anim + 0x10);
        return isPtr((u32)a) ? (float*)(a + 0x18) : NULL;
    }
    // The x of a digit's scene model (its local matrix's translation).
    static float* modelX(u8* obj)
    {
        u8* scn = *(u8**)(obj + 0x10);
        return isPtr((u32)scn) ? (float*)(scn + 0x48) : NULL;
    }
    static bool shown(u8* obj)
    {
        u8* scn = *(u8**)(obj + 0x10);
        return isPtr((u32)scn) && !(*(u32*)(scn + 0xCC) & 0x60);
    }
    static void setShown(u8* obj, bool on)
    {
        u8* scn = *(u8**)(obj + 0x10);
        if (isPtr((u32)scn)) SET_VIS(scn, on);
    }

    static void digitsOurs()
    {
        if (!s.digitsSaved) {
            for (int i = 0; i < 6; i++) {
                u8* o = digitObj(i);
                float* f = o ? patFrame(o) : NULL;
                s.digitFrame[i] = f ? *f : 0.0f;
                s.digitShown[i] = o && shown(o);
            }
            float* x = digitObj(1) ? modelX(digitObj(1)) : NULL;
            s.digitX = x ? *x : 0.0f;
            s.digitsSaved = true;
        }
        // The count in the minutes digit, moved to the colon (the value's middle); the rest hidden.
        u8* colon = digitObj(4);
        float* cx = colon ? modelX(colon) : NULL;
        for (int i = 0; i < 6; i++) {
            u8* o = digitObj(i);
            if (!o) continue;
            if (i == 1) {
                float* f = patFrame(o);
                float* x = modelX(o);
                if (f) *f = (float)s.bans;
                if (x && cx) *x = *cx + slideOffset();
            }
            setShown(o, i == 1);
        }
    }

    static void digitsBack()
    {
        if (!s.digitsSaved) return;
        for (int i = 0; i < 6; i++) {
            u8* o = digitObj(i);
            if (!o) continue;
            float* f = patFrame(o);
            if (f) *f = s.digitFrame[i];
            float* x = i == 1 ? modelX(o) : NULL;
            if (x) *x = s.digitX;
            setShown(o, s.digitShown[i]);
        }
        s.digitsSaved = false;
    }

    // MORE RULES' rows hidden under the Special Versus box, or shown again.
    static void moreRulesShown(bool on)
    {
        for (int r = 0; r < ROWS; r++) {
            u8* o = rowObj(r);
            if (o) setShown(o, on);
        }
        for (int i = 0; i < 6; i++) {
            u8* o = digitObj(i);
            if (o && (!on || !s.digitsSaved)) setShown(o, on ? (s.digitsSaved ? s.digitShown[i] : true) : false);
        }
    }

    // ---- Footer -------------------------------------------------------------------------------
    void reprintFooter()
    {
        typedef void (*PrintIndexFn)(void* msg, u32 window, u32 line, const void* msbin);
        if (isPtr((u32)s.footMsg) && s.footMsbin) ((PrintIndexFn)0x800B91B8)(s.footMsg, 0, s.footLine, s.footMsbin);
    }

    // MORE RULES' own descriptions (P+'s rules msbin, lines 17-26).
    static bool moreRulesLine(const void* msbin, u32 line)
    {
        static const char* const KEYS[] = {"one-minute", "teammates", "pause", "Pause", "L-cancel",
                                           "landing-lag", "input", "Adjust stages"};
        for (u32 i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
            if (Online::msbinLineContains(msbin, (int)line, KEYS[i])) return true;
        }
        return false;
    }

    const char* menuText(void* msg, const void* msbin, u32 line)
    {
        if (!s.opened || !moreRulesLine(msbin, line)) return NULL;
        s.footMsg = msg;
        s.footMsbin = msbin;
        s.footLine = line;
        if (s.view == V_SETUP) return rowHelp(cursor());
        if (s.view == V_SPECIAL) return SpecialBox::footer();
        return NULL;
    }

    // ---- Pages --------------------------------------------------------------------------------
    static const int SLIDE_FRAMES = 9;
    static const float SLIDE_DISTANCE = 64.0f;   // menu units: from off the screen's edge

    // A MuObject's offset from where its animations put it (MuObject::setPos, 0x800B6838; the
    // offset kept at +0x3C): x set, y and z kept.
    static void setOffsetX(u8* obj, float x)
    {
        float* pos = (float*)(obj + 0x3C);
        if (pos[0] == x) return;
        float p[3] = {x, pos[1], pos[2]};
        typedef void (*SetPosFn)(u8*, float*);
        ((SetPosFn)0x800B6838)(obj, p);
    }

    // How far the sliding page is from its place now (0: in place).
    static float slideOffset()
    {
        if (s.slideLeft <= 0) return 0.0f;
        float t = (float)s.slideLeft / SLIDE_FRAMES;
        return s.slideDir * SLIDE_DISTANCE * t * t;
    }

    // ---- P+'s turn back --------------------------------------------------------------------------
    // P+'s rows turn with CHR animations of their own (MenMainIcon01xx_TopN__0..__5, slot +0x0C of
    // the MuObject's animations): __0 / __2 come in from the right, __1 / __3 go out to the left,
    // and there are none the other way, so its turn back (MORE RULES -> RULES) moves the same way
    // as its turn forward (Sarah's board, 2026-10-27: "pressing L to go back ALSO plays the same
    // swipe-right animation"). Mirrored here: right after P+ gave each model its __0 (coming in)
    // or __1 (going out), each gets the other, played backwards (frame from its last to 0, the
    // rate 0 and the frame ours): RULES comes in from the left, MORE RULES goes out to the right.
    typedef void (*ChangeAnimFn)(u8* obj, const char* name);
    static const ChangeAnimFn CHANGE_ANIM = (ChangeAnimFn)0x800B534C;   // MuObject::changeAnimN

    static u8* slotAnim(u8* obj)
    {
        u8* anims = *(u8**)(obj + 0x14);
        if (!isPtr((u32)anims)) return NULL;
        u8* a = *(u8**)(anims + 0x0C);
        return isPtr((u32)a) ? a : NULL;
    }

    // The CHR0 an animation object plays (a pointer to it in its first words, or one level down).
    static u8* chrOf(u8* anim)
    {
        for (int i = 0; i < 32; i++) {
            u32 w = *(u32*)(anim + 4 * i);
            if (!isPtr(w)) continue;
            if (*(u32*)w == 0x43485230) return (u8*)w;   // 'CHR0'
            for (int j = 0; j < 16; j++) {
                u32 w2 = *(u32*)(w + 4 * j);
                if (isPtr(w2) && *(u32*)w2 == 0x43485230) return (u8*)w2;
            }
        }
        return NULL;
    }

    static const u16 BACK_FRAMES = 10;
    extern "C" {
        u32 g_rulesBackHold = 0;   // tests: the mirrored turn held at this frame (0: not held)
    }

    static void startBack()
    {
        s.backN = 0;
        s.backT = 0;
        if (!s.rt) return;
        for (u32 off = 0x40; off < 0x2B0 && s.backN < 48; off += 4) {
            u8* obj = *(u8**)(s.rt + off);
            if (!isPtr((u32)obj) || obj == s.rt || !isPtr(*(u32*)(obj + 8)) || **(u32**)(obj + 8) != 0x4D444C30) continue;
            u8* a = slotAnim(obj);
            u8* chr = a ? chrOf(a) : NULL;
            if (!chr) continue;
            const char* name = (const char*)(chr + *(s32*)(chr + 0x14));
            int n = (int)strlen(name);
            if (n < 4 || n > 60 || name[n - 3] != '_' || name[n - 2] != '_' || (name[n - 1] != '0' && name[n - 1] != '1')) continue;
            char want[64];
            memcpy(want, name, n + 1);
            want[n - 1] = name[n - 1] == '0' ? '1' : '0';
            CHANGE_ANIM(obj, want);
            u8* a2 = slotAnim(obj);
            u8* chr2 = a2 ? chrOf(a2) : NULL;
            if (!chr2) continue;
            int i = s.backN++;
            s.backObj[i] = obj;
            s.backAnim[i] = a2;
            // (from frame 10 at most: P+'s turn takes 10 frames; __1 has the rows off the screen
            // from there to its end, 20)
            u16 len = *(u16*)(chr2 + 0x1C);
            s.backLen[i] = len < BACK_FRAMES ? len : BACK_FRAMES;
        }
    }

    static void backTurn()
    {
        if (s.backPending) {
            s.backPending = false;
            startBack();
        }
        if (s.backN == 0) return;
        bool more = false;
        for (int i = 0; i < s.backN; i++) {
            if (slotAnim(s.backObj[i]) != s.backAnim[i]) continue;   // P+ gave it another since
            int f = (int)s.backLen[i] - s.backT;
            if (f < 0) f = 0;
            if (f > 0) more = true;
            float* fr = (float*)(s.backAnim[i] + 0x18);
            fr[0] = (float)f;
            fr[1] = 0.0f;   // (the rate: the frame is ours)
        }
        if (!g_rulesBackHold || (u32)s.backT < g_rulesBackHold) s.backT++;
        if (!more) s.backN = 0;
    }

    void notePage(int kind)
    {
        s.pageKind = kind;
        s.pageSeen = s.ticks;
    }

    static void setView(View v)
    {
        if (v == s.view) return;
        View old = s.view;
        s.slideDir = v > old ? 1 : -1;
        s.slideLeft = SLIDE_FRAMES;
        s.view = v;
        g_rulesPages.view = v;
        g_rulesPages.turns++;
        if (old == V_SETUP) {
            s.restoreFrames = 3;
            digitsBack();
        }
        if (old == V_SPECIAL) {
            SpecialBox::show(false);
            moreRulesShown(true);
        }
        if (v == V_SETUP && !makeCopies()) {
            // Without the copies MATCH SETUP cannot be drawn: SPECIAL then.
            s.view = V_SPECIAL;
            v = V_SPECIAL;
        }
        if (v == V_SPECIAL) {
            moreRulesShown(false);
            SpecialBox::show(true);
        }
        playSE(SE_CURSOR);
        reprintFooter();
    }

    static bool pattern(int n, int k, u8* out, u8* count);

    // mm's default strike pattern for `n` starters (room_set.rs default_strikes, the launcher's
    // suggestStrikes): 1, then 2s, then what is left (5: 1-2-1, 7: 1-2-2-1, 3: 1-1).
    static void defaultStrikes(int n, u8* out, u8* count)
    {
        int left = n - 1, k = 0;
        if (left > 0) {
            out[k++] = 1;
            left--;
        }
        while (left > 2 && k < PPOM::MAX_STRIKE_TURNS) {
            out[k++] = 2;
            left -= 2;
        }
        if (left > 0 && k < PPOM::MAX_STRIKE_TURNS) out[k++] = (u8)left;
        *count = (u8)k;
    }

    // The room's values (SESSION): when the menu opens, and again when the room's ruleset changes
    // under it (the host picked another on RULESET).
    static void fromSession()
    {
        const PPOM::Session& se = PPOM::g_block.session;
        s.bestOf = se.play.bestOf;
        s.setOn = se.play.setOn && !(se.play.setFlags & PPOM::SET_FINISHED);
        s.choice = se.play.stageChoice <= PPOM::CHOICE_ANYONE_PICKS ? se.play.stageChoice : 0;
        s.bans = se.special.bans <= 3 ? se.special.bans : 0;
        s.dsr = se.special.dsr ? 1 : 0;
        s.alternates = se.special.alternates ? 1 : 0;
        s.strikeCount = se.special.strikeCount <= PPOM::MAX_STRIKE_TURNS ? se.special.strikeCount : 0;
        memcpy(s.strikes, se.special.strikes, sizeof(s.strikes));
        s.stockControl = se.rules.stockControl ? 1 : 0;
        s.starters = (u8)bitCount(se.stages.starters);
        u8 all[32];
        for (int i = 0; i < 32; i++) all[i] = se.stages.starters[i] | se.stages.counterpicks[i];
        s.stages = (u8)bitCount(all);
        s.formatEdited = s.rulesEdited = false;
    }

    void open(bool canEdit)
    {
        memset(&s, 0, sizeof(s));
        s.opened = true;
        s.canEdit = canEdit;
        fromSession();
        SpecialBox::open(canEdit);
        PageHints::open();
        g_rulesPages.view = V_PPLUS;
    }

    // The set length the page counts with: the one the host just picked on RULES' SET line (sent
    // when the menu closes, before this page's edits), else the room's. A set starting gets what
    // mm's room-best-of gives it (rooms.rs with_best_of): the default strike pattern if there is
    // none, two bans (fewer with few stages) and DSR.
    static void followBestOf()
    {
        int pend = RoomPlay::pendingBestOf();
        u8 want = pend >= 0 ? (u8)pend : PPOM::g_block.session.play.bestOf;
        if (want == s.bestOf) return;
        if (want > 0) {
            if (s.strikeCount == 0) defaultStrikes(s.starters, s.strikes, &s.strikeCount);
            if (s.bestOf == 0) {
                s.bans = s.stages > 2 ? (s.stages - 2 < 2 ? s.stages - 2 : 2) : 0;
                s.dsr = 1;
            }
        }
        s.bestOf = want;
        reprintFooter();
    }

    // The strike patterns over `n` starters (n - 1 struck), turns of 1-3, fewest turns first
    // (the default, 1 then 2s, first of its length): the k-th, or false past the last.
    static bool pattern(int n, int k, u8* out, u8* count)
    {
        int left = n - 1;
        if (left <= 0) return false;
        int found = 0;
        for (int turns = 1; turns <= left && turns <= PPOM::MAX_STRIKE_TURNS; turns++) {
            // Every composition of `left` into `turns` parts of 1-3, in lexicographic order.
            u8 p[PPOM::MAX_STRIKE_TURNS];
            for (int i = 0; i < turns; i++) p[i] = 1;
            while (true) {
                int sum = 0;
                for (int i = 0; i < turns; i++) sum += p[i];
                if (sum == left) {
                    if (found++ == k) {
                        memcpy(out, p, turns);
                        *count = (u8)turns;
                        return true;
                    }
                }
                int i = turns - 1;
                while (i >= 0 && p[i] == 3) p[i--] = 1;
                if (i < 0) break;
                p[i]++;
            }
            if (found > 64) break;
        }
        return false;
    }

    static int patternIndex()
    {
        u8 p[PPOM::MAX_STRIKE_TURNS], n;
        for (int k = 0; k < 64 && pattern(s.starters, k, p, &n); k++) {
            if (n != s.strikeCount) continue;
            int i = 0;
            while (i < n && p[i] == s.strikes[i]) i++;   // (the DOL has no memcmp)
            if (i == n) return k;
        }
        return -1;
    }

    // Left / right / A on a MATCH SETUP row.
    static void edit(int r, int dir)
    {
        if (!s.canEdit) return;
        if (s.setOn && r != R_CONTROL) {
            playSE(SE_ERROR);
            return;
        }
        switch (r) {
        case R_BANS: {
            int most = s.stages > 1 ? s.stages - 1 : 0;
            if (most > 3) most = 3;
            int b = (int)s.bans + dir;
            if (!s.bestOf || b < 0 || b > most) { playSE(SE_ERROR); return; }
            s.bans = (u8)b;
            s.formatEdited = true;
            break;
        }
        case R_DSR:
            if (!s.bestOf) { playSE(SE_ERROR); return; }
            s.dsr ^= 1;
            s.formatEdited = true;
            break;
        case R_CHOICE: {
            if (s.bestOf) { playSE(SE_ERROR); return; }
            // P+'s order on screen: loser picks, from the list, strike, random, anyone picks.
            static const u8 ORDER[5] = {PPOM::CHOICE_LOSER_PICKS, PPOM::CHOICE_LOSER_PICKS_LIST,
                                        PPOM::CHOICE_STRIKE, PPOM::CHOICE_RANDOM, PPOM::CHOICE_ANYONE_PICKS};
            int i = 0;
            while (i < 5 && ORDER[i] != s.choice) i++;
            i = (i + (dir < 0 ? 4 : 1)) % 5;
            s.choice = ORDER[i];
            if (s.choice == PPOM::CHOICE_STRIKE && s.strikeCount == 0) defaultStrikes(s.starters, s.strikes, &s.strikeCount);
            s.formatEdited = true;
            break;
        }
        case R_STRIKES: {
            if (!strikesUsed() || s.starters < 2) { playSE(SE_ERROR); return; }
            int k = patternIndex();
            u8 p[PPOM::MAX_STRIKE_TURNS], n;
            int next = k < 0 ? 0 : k + (dir < 0 ? -1 : 1);
            if (next < 0 || !pattern(s.starters, next, p, &n)) {
                // round: the last one from the first, the first from the last
                if (dir < 0) {
                    int last = 0;
                    while (last < 63 && pattern(s.starters, last + 1, p, &n)) last++;
                    pattern(s.starters, last, p, &n);
                } else {
                    pattern(s.starters, 0, p, &n);
                }
            }
            memcpy(s.strikes, p, n);
            s.strikeCount = n;
            s.formatEdited = true;
            break;
        }
        case R_CONTROL:
            if (s.setOn) { playSE(SE_ERROR); return; }
            s.stockControl ^= 1;
            s.rulesEdited = true;
            break;
        case R_ALTS:
            s.alternates ^= 1;
            s.formatEdited = true;
            break;
        }
        playSE(r == R_BANS || r == R_STRIKES || r == R_CHOICE ? SE_CURSOR : SE_TOGGLE);
        reprintFooter();
    }

    u32 input(u8* rt, u8* page, bool rulesPage, u32 bits)
    {
        s.rt = rt;
        if (!s.opened) return bits;
        if (rulesPage) {
            if (bits & IN_R) {
                // P+'s MORE RULES button: its line, and A for it.
                *(u16*)(page + PAGE_CURSOR) = 6;
                g_rulesPages.turns++;
                return IN_A;
            }
            return bits & ~(IN_L | IN_R);
        }
        // P+'s MORE RULES page (and our pages over it).
        if (bits & (IN_L | IN_R)) {
            bool right = (bits & IN_R) != 0;
            if (s.view == V_PPLUS) {
                if (!right) {
                    s.backPending = true;         // (P+'s turn back, mirrored: backTurn)
                    return IN_B;                  // back to RULES, as B
                }
                setView(V_SETUP);
            } else if (s.view == V_SETUP) {
                setView(right ? V_SPECIAL : V_PPLUS);
            } else if (!right) {
                setView(V_SETUP);
            }
            return 0;
        }
        if (s.view == V_PPLUS) {
            if (bits & IN_B) s.backPending = true;
            return bits;
        }
        if (bits & IN_B) {
            setView(V_PPLUS);
            s.backPending = true;
            return IN_B;
        }
        if (s.view == V_SPECIAL) {
            SpecialBox::input(bits, s.canEdit && !s.setOn);
            return 0;
        }
        int r = cursor();
        if (r >= 0 && r < ROWS) {
            if (bits & IN_LEFT) edit(r, -1);
            else if (bits & IN_RIGHT) edit(r, 1);
            else if ((bits & IN_A) && (r == R_ALTS || r == R_DSR || r == R_CONTROL)) edit(r, 1);
        }
        return bits & IN_MOVE;
    }

    void tick(u8* rt)
    {
        s.rt = rt;
        g_rulesPages.rt = (u32)rt;
        if (!s.opened) return;
        s.ticks++;
        backTurn();
        if (RoomPlay::changedNow()) {
            fromSession();
            reprintFooter();
        }
        followBestOf();
        // The page sliding in: MORE RULES' rows (MATCH SETUP's too) and the box, from the side.
        if (s.slideLeft > 0) {
            s.slideLeft--;
            float dx = slideOffset();
            for (int r = 0; r < ROWS; r++) {
                u8* o = rowObj(r);
                if (o) setOffsetX(o, s.view == V_SPECIAL ? 0.0f : dx);
            }
            SpecialBox::slide(s.view == V_SPECIAL ? dx : 0.0f);
        }
        // L / R: which ways a page is, while P+'s RULES or MORE RULES (with ours) takes the input.
        bool onPages = s.ticks - s.pageSeen <= 2;
        bool rulesPage = onPages && s.pageKind == 0, morePage = onPages && s.pageKind == 1;
        PageHints::tick(rt, morePage, rulesPage || (morePage && s.view != V_SPECIAL));
        if (s.view == V_SETUP && s.block) {
            char buf[32];
            for (int r = 0; r < ROWS; r++) {
                draw(s.label[r], r == R_ALTS ? valueText(r, buf) : labelText(r), false, r == R_ALTS ? 300 : 200);
                bind(r, s.label[r], ROW[r].labelTex, true);
                if (s.value[r].orig) {
                    draw(s.value[r], valueText(r, buf), true, 0);
                    bind(r, s.value[r], ROW[r].valueTex, true);
                }
                strncpy(g_rulesPages.value[r], r == R_BANS ? "" : valueText(r, buf), sizeof(g_rulesPages.value[r]) - 1);
            }
            sprintf(g_rulesPages.value[R_BANS], "%d", s.bans);
            digitsOurs();
        } else if (s.restoreFrames > 0 && s.block) {
            s.restoreFrames--;
            for (int r = 0; r < ROWS; r++) {
                bind(r, s.label[r], ROW[r].labelTex, false);
                if (s.value[r].orig) bind(r, s.value[r], ROW[r].valueTex, false);
            }
        }
        if (s.view == V_SPECIAL) moreRulesShown(false);
        SpecialBox::tick(rt, s.view == V_SPECIAL);
        g_rulesPages.bans = s.bans;
        g_rulesPages.dsr = s.dsr;
        g_rulesPages.choice = s.choice;
        g_rulesPages.alternates = s.alternates;
        g_rulesPages.stockControl = s.stockControl;
        g_rulesPages.strikeCount = s.strikeCount;
        memcpy(g_rulesPages.strikes, s.strikes, sizeof(g_rulesPages.strikes));
    }

    bool rulesEdits(PPOM::Rules& ru, u8 special[8], u8* staminaType)
    {
        bool any = false;
        if (s.rulesEdited) {
            ru.stockControl = s.stockControl;
            any = true;
        }
        if (SpecialBox::edits(special, staminaType)) any = true;
        return any;
    }

    void close(bool send)
    {
        if (!s.opened) return;
        // P+'s own textures, digits and rows back (the menu is still there when the host closes
        // it with B; closed from outside it is gone, and nothing of it is touched).
        u8* task = NULL;
        {
            u32 mgr = *(u32*)0x805A0060;
            u32 scene = isPtr(mgr) ? *(u32*)(mgr + 4) : 0;
            u32 t = isPtr(scene) ? *(u32*)(scene + 0x400) : 0;
            task = isPtr(t) ? (u8*)t : NULL;
        }
        bool menuThere = task && *(u32*)(task + 0x630) != 0 && *(u8**)(task + 0x62C) == s.rt;
        PageHints::close(menuThere);
        if (menuThere && s.block) {
            for (int r = 0; r < ROWS; r++) {
                bind(r, s.label[r], ROW[r].labelTex, false);
                if (s.value[r].orig) bind(r, s.value[r], ROW[r].valueTex, false);
            }
            digitsBack();
            moreRulesShown(true);
        } else if (s.block) {
            // Closed from outside: the task and its models are gone, but their MDL0s are the
            // menu archive's (the CSS keeps it) and still point at our copies: back to P+'s.
            for (int r = 0; r < ROWS; r++) {
                if (s.label[r].mdl) Labels::bindModel(s.label[r].mdl, ROW[r].labelTex, s.label[r].orig);
                if (s.value[r].mdl) Labels::bindModel(s.value[r].mdl, ROW[r].valueTex, s.value[r].orig);
            }
        }
        if (send && s.canEdit && s.formatEdited && !s.setOn) {
            PPOM::RoomFormatRequest req;
            memset(&req, 0, sizeof(req));
            req.op = PPOM::ROOM_FORMAT;
            req.choice = s.choice;
            req.bans = s.bestOf ? s.bans : 0;
            req.dsr = s.bestOf ? s.dsr : 0;
            req.alternates = s.alternates;
            req.strikeCount = strikesUsed() ? s.strikeCount : 0;
            memcpy(req.strikes, s.strikes, sizeof(req.strikes));
            PPOM::post(PPOM::CMD_ROOM, &req, sizeof(req));
            g_rulesPages.posts++;
        }
        SpecialBox::close();
        if (s.block) HEAP_FREE(s.block);
        s.block = NULL;
        s.opened = false;
        s.view = V_PPLUS;
        g_rulesPages.view = V_PPLUS;
    }
}
