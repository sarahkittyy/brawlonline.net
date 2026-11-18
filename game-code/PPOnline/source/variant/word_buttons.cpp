// The room's buttons as words in the bar (word_buttons.h; variant.h PPON_WORD_BUTTONS: in the
// plugins built for a build whose character select has none of Brawl's online art, empty in the
// others).
//
// REX 0.5's sc_selcharacter.pac has ITEM and STAGE (MenSelchrState0005 / 0006) with no textures
// and no rect (Brawl's hit test never finds them), and none of the online textures
// (MenSelchrWifi*) to copy labels from, so rules_button.cpp's models cannot be made there. The
// hit test gives the same buttons from the layout's rects in hand units (PUBLIC and FFA as
// Brawl's ITEM and STAGE, 0x19 and 0x1A, so the game's dispatch and cssSwitchButtons take them as
// before), and the tests' counters say the same (made: the words shown).
#include "variant.h"
#if PPON_WORD_BUTTONS
#include <string.h>
#include "build.h"
#include "labels.h"
#include "layout.h"
#include "word_buttons.h"

namespace WordButtons {
    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // (as rules_button.cpp's)
    static const int HAND_BUTTON[WORDS] = {0x30 /* BUTTON_RULES */, 0x19, 0x31 /* BUTTON_SIT_OUT */, 0x1A};
    static const char* const LABEL_TEX = "MenSelchrWifi00Button11";
    static const u32 TASK_RULES_OPEN = 0x630;        // muSelCharTask: P+'s rules menu is open

    static u8* cssTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        return isPtr(task) ? (u8*)task : NULL;
    }

    static bool handOnIt(u8* task, int id)
    {
        int n = *(int*)(task + 0x648);   // the task's areas
        for (int i = 0; i < n && i < 4; i++) {
            u8* a = *(u8**)(task + 0x44 + 4 * i);
            if (!isPtr((u32)a)) continue;
            u8* hand = *(u8**)(a + 0x1A8);
            if (isPtr((u32)hand) && *(int*)(hand + 0xAC) == id) return true;
        }
        return false;
    }

    // ---- Is this such a character select -------------------------------------------------------
    static u8* s_textTask = NULL;
    static bool s_text = false;
    bool on(u8* task)
    {
        if (task != s_textTask) {
            s_textTask = task;
            s_text = false;
            u8* item = task ? Build::cssModel(task, Build::CSS_ITEM) : NULL;
            u8* mdl = isPtr((u32)item) ? *(u8**)(item + 8) : NULL;
            if (isPtr((u32)mdl) && *(u32*)mdl == 0x4D444C30 /* MDL0 */) s_text = !Labels::boundTexture(mdl, LABEL_TEX);
        }
        return task && s_text;
    }
    bool artless() { return on(cssTask()); }

    // The words' places (layout.h g_cssTextButtons; all 0 on a character select with Brawl's
    // online art, which has the buttons themselves): per word the window's x1, x2 and y (window
    // units, y from the bar's middle, growing down), then the hand's rect: left, right, top,
    // bottom. A build whose character select has no such art has them in its entry of mm's games
    // table (`layout.cssButtons`). REX's: laid out from the bar's right end for the longest words
    // (FRIENDS, SIT OUT, TEAMS) with a gap of about a letter; RULES then starts where the room's
    // status line ends at the most (g_cssPlaces [3]); a window unit is about 0.62 px (the windows
    // hang from the bar's rule model) and a hand unit 9.8 px at 828 wide.
    static const float* place(int word) { return g_cssTextButtons[word]; }
    const float* rect(int word) { return place(word) + 3; }

    static bool inRect(const float* r, const float* pos) { return r[0] <= pos[0] && r[1] >= pos[0] && r[3] <= pos[1] && r[2] >= pos[1]; }

    // ---- What the room shows -------------------------------------------------------------------
    static struct {
        u8* task;          // the character select it was last ticked on
        bool show, enabled, state;
        u32* made;
    } s_btn[2];            // RULES, SIT OUT
    static bool s_hdrShow = false;
    static int s_hdrVis = 0, s_hdrTeams = 0;

    void button(int word, u8* task, bool show, bool enabled, bool state, u32* made)
    {
        s_btn[word == RULES ? 0 : 1].task = task;
        s_btn[word == RULES ? 0 : 1].show = show;
        s_btn[word == RULES ? 0 : 1].enabled = enabled;
        s_btn[word == RULES ? 0 : 1].state = state;
        s_btn[word == RULES ? 0 : 1].made = made;
    }

    void header(bool show, int visibility, int teams)
    {
        s_hdrShow = show;
        s_hdrVis = visibility;
        s_hdrTeams = teams;
    }

    // (rules_button.cpp's live(): shown, enabled, on this character select, no rules menu open)
    static bool live(int b, u8* task)
    {
        return task == s_btn[b].task && s_btn[b].show && s_btn[b].enabled && *(u32*)(task + TASK_RULES_OPEN) == 0;
    }

    int at(u8* task, const float* pos, float* rect)
    {
        if (*(u32*)(task + TASK_RULES_OPEN) != 0) return 0;
        for (int i = 0; i < WORDS; i++) {
            bool on = i == RULES ? live(0, task) : i == SIT_OUT ? live(1, task) : s_hdrShow;
            const float* r = place(i) + 3;
            if (!on || !inRect(r, pos)) continue;
            if (rect)
                for (int k = 0; k < 4; k++) rect[k] = r[k];
            return HAND_BUTTON[i];
        }
        return 0;
    }

    // Is the hand over the character grid (sel_char text+0x1B750, muSelCharHand at +0x90 / +0x94
    // its position): the CSS's A goes to the grid's handling when it is (sel_char+0x75A4), else
    // to buttonProcInAllArea, and the hand's target is the grid's. REX's grid answers yes in the
    // bar too (everywhere tried), so A on a word holding the coin dropped the coin there.
    // On a word the hand is on that button, not over the grid.
    typedef int (*OverGridFn)(u8* hand);
    static OverGridFn s_origOverGrid = NULL;
    static int hkOverGrid(u8* hand)
    {
        int over = s_origOverGrid(hand);
        if (over) {
            u8* task = cssTask();
            if (task && on(task) && at(task, (const float*)(hand + 0x90), NULL)) return 0;
        }
        return over;
    }

    void install(CoreApi* api)
    {
        api->syReplaceFuncRel(0x1B750, reinterpret_cast<void*>(hkOverGrid), (void**)&s_origOverGrid, 10 /* sora_menu_sel_char */);
    }

    // ---- The bar's windows ---------------------------------------------------------------------
    // Windows of the rule line's MuMsg (muSelCharTask+0x5C4: 8 windows, the character select uses
    // window 0 and online_menu.cpp 1-3), attached as its own-code window is: to the rule model's
    // node, its rect in window units, y from the bar's middle (y grows down), centred.
    struct WindowSetting {
        u32 flags;
        float x1, y1, x2, y2;
        u8 _14[0x2C - 0x14];
        float lineSpace;
        float scaleX, scaleY;
        u8 _38[0x48 - 0x38];
    };
    static struct {
        MuMsg* msg;
        bool haveWindow;
        float y1, y2;
    } s_bar;

    void bar(MuMsg* msg, bool haveWindow, float y1, float y2)
    {
        s_bar.msg = msg;
        s_bar.haveWindow = haveWindow;
        s_bar.y1 = y1;
        s_bar.y2 = y2;
    }

    // False until the CSS has printed its rule line, or for a window the MuMsg does not have.
    static bool barWindow(u8* task, u32 window, float x1, float x2, float y, float half, float scale)
    {
        MuMsg* m = s_bar.msg;
        if (!m || !s_bar.haveWindow) return false;
        u32 obj = (u32)Build::cssModel(task, Build::CSS_RULE);
        u32 mdl = isPtr(obj) ? *(u32*)(obj + 0x10) : 0;
        u8* message = (u8*)m->m_message;
        u32 bufs = isPtr((u32)message) ? *(u32*)(message + 0x1D8) : 0;
        if (!isPtr(mdl) || !isPtr(bufs) || *(u32*)((u8*)m + 0x10) <= window) return false;
        float size = *(float*)(*(u32*)bufs + 0x28);
        typedef void (*AttachFn)(MuMsg*, u32, u32 scnMdl, u32 node, float size);
        ((AttachFn)0x800B8C90)(m, window, mdl, 0, size);   // MuMsg::attachScnMdlSimple
        WindowSetting* ws = (WindowSetting*)(*(u32*)((u8*)m + 0xC) + 0x48 * window);
        float mid = (s_bar.y1 + s_bar.y2) * 0.5f;
        ws->x1 = x1;
        ws->x2 = x2;
        ws->y1 = mid + half + y;
        ws->y2 = mid - half + y;
        ws->scaleX = ws->scaleY = scale;
        m->setAlignMode(window, MuMsg::Align_Center);
        return true;
    }

    static void barPrint(u32 window, const char* text, u8 r, u8 g, u8 b)
    {
        MuMsg* m = s_bar.msg;
        if (!m || !s_bar.haveWindow || *(u32*)((u8*)m + 0x10) <= window) return;
        m->setFontColor(window, r, g, b, 0xFF);
        m->printf(window, "%s", text);
    }

    // ---- The words -----------------------------------------------------------------------------
    static u8* s_wordsTask = NULL;
    // What the windows were last laid out for: every word's place, the bar's text size and its
    // middle (tests move a place and see the windows follow).
    static float s_laidOut[WORDS * 3 + 3];
    static char s_wordsShown[WORDS][12];
    static u32 s_wordsColour[WORDS];
    static bool s_wordsMade[2];
    static const float TEXT_HALF = 26.0f, TEXT_SCALE = 0.8f;

    // The CSS is gone: the next one (often at the same address) lays the words out and makes them
    // again.
    void taskGone(u8* task)
    {
        if (task && task == s_wordsTask) s_wordsTask = NULL;
    }

    void tick()
    {
        u8* task = cssTask();
        if (!task || !on(task) || !s_bar.msg || !s_bar.haveWindow) return;
        float want[WORDS * 3 + 3];
        for (int i = 0; i < WORDS; i++) {
            for (int k = 0; k < 3; k++) want[i * 3 + k] = place(i)[k];
        }
        want[WORDS * 3] = g_cssPlaces[13];   // (the build's text size there)
        want[WORDS * 3 + 1] = s_bar.y1;
        want[WORDS * 3 + 2] = s_bar.y2;
        bool attach = task != s_wordsTask;
        for (u32 k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
            if (want[k] != s_laidOut[k]) attach = true;
        }
        if (attach) {
            for (int i = 0; i < WORDS; i++) {
                const float* p = place(i);
                float k = g_cssPlaces[13];
                if (!barWindow(task, 4 + i, p[0], p[1], p[2], TEXT_HALF * k, TEXT_SCALE * k)) return;
                s_wordsShown[i][0] = 1;   // printed again
                s_wordsColour[i] = 0;
            }
            if (task != s_wordsTask) s_wordsMade[0] = s_wordsMade[1] = false;
            s_wordsTask = task;
            memcpy(s_laidOut, want, sizeof(want));
        }
        for (int i = 0; i < 2; i++) {   // made: the words first shown on this CSS (as the model made)
            if (s_btn[i].show && !s_wordsMade[i]) {
                s_wordsMade[i] = true;
                if (s_btn[i].made) (*s_btn[i].made)++;
            }
        }
        static const char* const VIS[3] = {"PRIVATE", "PUBLIC", "FRIENDS"};
        static const char* const TEAMS[3] = {"FFA", "TEAMS", "TEAMS"};
        const bool menuOpen = *(u32*)(task + TASK_RULES_OPEN) != 0;
        for (int i = 0; i < WORDS; i++) {
            const int b = i == RULES ? 0 : i == SIT_OUT ? 1 : -1;
            bool show = !menuOpen && (b >= 0 ? s_btn[b].show && s_btn[b].task == task : s_hdrShow);
            bool enabled = b >= 0 ? s_btn[b].enabled : true;
            const char* word = !show ? "" : i == RULES ? "RULES" : i == SIT_OUT ? (s_btn[1].state ? "PLAY" : "SIT OUT")
                             : i == PUBLIC ? VIS[s_hdrVis >= 0 && s_hdrVis <= 2 ? s_hdrVis : 0]
                                           : TEAMS[s_hdrTeams >= 0 && s_hdrTeams <= 2 ? s_hdrTeams : 0];
            bool hovered = show && enabled && handOnIt(task, HAND_BUTTON[i]);
            // white; under a hand yellow; disabled grey; rainbow TEAMS (P+'s rainbow letters) pink
            u32 colour = !enabled ? 0x8C8C8Cu : hovered ? 0xFFE040u : (i == FFA && s_hdrTeams == 1) ? 0xFF8CE6u : 0xFFFFFFu;
            if (strcmp(word, s_wordsShown[i]) == 0 && colour == s_wordsColour[i]) continue;
            barPrint(4 + i, word, (u8)(colour >> 16), (u8)(colour >> 8), (u8)colour);
            strncpy(s_wordsShown[i], word, sizeof(s_wordsShown[i]) - 1);
            s_wordsShown[i][sizeof(s_wordsShown[i]) - 1] = 0;
            s_wordsColour[i] = colour;
        }
    }
}
#endif
