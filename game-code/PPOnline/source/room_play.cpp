// The room's ruleset and set on the room CSS (docs/design/room-rules.md decisions 12, 16 and
// §2.1; docs/game-code.md §14 "The Ruleset line and the set").
//
// Dolphin fills SESSION RoomPlay from room-state: the ruleset's code and name, custom or not, its
// set length and stage choice, and the set (on, the game, the score by side, each side's in-game
// port, paused / can resume / finished). Two lines of P+'s own RULES page (the rules menu opened
// by the RULES button, room_rules.cpp) show it:
//   - RULESET: the STAGE CHOICE row (model MenMainIcon0126 at muRuleTask+0x58, line 4; not a room
//     rule, the session picks the stage). Its label texture MenMainRule00font.06 (I4 200x24) and
//     its value strip MenMainRule03Sel03 (I4 800x40, the values side by side; the value shown is
//     stage choice +0x670 = 0, the strip's first cell) are pointed at copies with "RULESET" and the
//     room's ruleset name drawn in the game's font (Labels::render). The host browses with left /
//     right through "P+ default", the player's recent and own published rulesets (CMD_RULESETS,
//     Dolphin's list) and "Enter code", and A picks: ROOM_RULESET with the code (empty: P+
//     default) and the menu closes; "Enter code" closes it and opens the keypad's 6-letter mode
//     (code_entry.cpp). The ruleset is locked during a set (rules.md §2).
//   - SET: the STOCK CONTROL row (MenMainIcon0124 at +0x50, line 2; handicap is left out,
//     decision 19): label MenMainRule00font.04, value strip MenMainRule03Sel02 (I4 480x40). It
//     shows the set ("No set", "Best of 3", "Bo3 1-0", "Paused 1-0"). During a set (not finished)
//     the host's left / right go to "End set" and A ends it with no result (ROOM_SET 0, the
//     user's of 2026-10-12; the server drops the set). With no set on (or after one
//     finished) the host changes its length there with left / right (No set, Best of 3, 5, 7), as
//     P+'s value lines change; it is sent as ROOM_BEST_OF when the menu closes (the server makes the
//     room's rules Custom and takes back every ready). A set paused because a player
//     left waits for them (rules.md §1.1 A: it resumes by itself when they are back in their held
//     slot); the host can only end it. A set paused because the machines' results differed, with
//     everyone there (SET_CAN_RESUME), offers Replay game (ROOM_SET 1) first, then End set.
// The page's own left / right / A on those two lines are taken here (room_rules.cpp's input
// filter calls rulesInput), so P+'s values never change.
// Value texts change: each row's value has two copies, the next text is drawn into the one not
// shown and the model is pointed at it (Dolphin's texture cache tells textures apart by address
// and a sampled hash).
#include <OS/OSCache.h>
#include <gf/gf_heap_manager.h>
#include <memory.h>
#include <string.h>
#include "labels.h"
#include "online.h"
#include "room_play.h"

extern "C" int sprintf(char* buf, const char* fmt, ...);

// Tests (harness/tests/test_rooms_ui.py test_room_ruleset_and_set), read through
// PPOnlineMain.map.
struct RoomPlayDebug {
    u32 posts;          // ROOM_RULESET / ROOM_SET sent
    u8 lastOp;          // the last one's op
    u8 lastArg;
    u8 browse;          // the RULESET line's entry (0xFF: the room's own shown)
    u8 setChoice;       // the SET line's choice (0 resume, 1 restart)
    char lastCode[8];   // ROOM_RULESET's code
    u32 drawn;          // value textures drawn
    char value[2][40];  // what the RULESET and SET values show
};
extern "C" {
    RoomPlayDebug g_roomPlay;
}

namespace RoomPlay {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    static void playSE(int id)
    {
        typedef void (*PlaySEFn)(void*, int, int, int, int, int);
        void* snd = *(void**)0x805A01D0;   // g_sndSystem
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }
    static const int SE_CURSOR = 0, SE_DECIDE = 1, SE_ERROR = 3;

    static u8* cssTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        return isPtr(task) ? (u8*)task : NULL;
    }

    static const u32 TASK_RULES = 0x62C, TASK_RULES_OPEN = 0x630;   // muSelCharTask
    static const u32 RULES_CLOSE = 0x288, RULES_PAGE1 = 0x268;      // muRuleTask

    static u8* rulesTask()
    {
        u8* task = cssTask();
        if (!task || *(u32*)(task + TASK_RULES_OPEN) == 0) return NULL;
        u32 rt = *(u32*)(task + TASK_RULES);
        return isPtr(rt) ? (u8*)rt : NULL;
    }

    static void closeMenu()
    {
        u8* rt = rulesTask();
        if (rt) *(u8*)(rt + RULES_CLOSE) = 1;   // a byte (isEnd: lbz); closes next frame, writing nothing back
    }

    // ------------------------------------------------------------------------------------------
    // The two rows.
    struct Row {
        u32 obj;            // muRuleTask offset of the row's MuObject
        const char* label;
        const char* labelTex;
        const char* valueTex;
        s16 box[4];         // the value cell in the strip (texels: the strips' cells are 160 wide)
        s16 labelW;         // the label's box width (left-aligned, as P+'s labels)
    };
    enum { ROW_RULESET = 0, ROW_SET = 1 };
    static const Row ROWS[2] = {
        {0x58, "RULESET", "MenMainRule00font.06", "MenMainRule03Sel03", {4, 2, 156, 38}, 118},
        {0x50, "SET", "MenMainRule00font.04", "MenMainRule03Sel02", {4, 2, 156, 38}, 52},
    };

    struct RowTex {
        u8* label;          // our TEX0 copies (32-byte aligned, Network heap, kept)
        u8* value[2];
        int shown;          // the value copy the model shows
        char text[40];      // ... and its text
        u32 origLabel, origValue;   // the game's image words
    };
    static RowTex s_rows[2];

    static u32 imageWord(const u8* tex) { return ((u32)(tex + 0x40) & 0x1FFFFFFF) >> 5; }

    static u8* copyTex(const u8* tex)
    {
        if (*(u32*)(tex + 0x10) != 0x40 || *(u32*)(tex + 0x20) != 0) return NULL;   // I4 only
        int w = *(u16*)(tex + 0x1C), h = *(u16*)(tex + 0x1E);
        u32 bytes = (u32)(((w + 7) / 8) * ((h + 7) / 8) * 32);
        u8* m = (u8*)gfHeapManager::alloc(Heaps::Network, 0x40 + bytes + 32);
        if (!m) return NULL;
        u8* c = (u8*)(((u32)m + 31) & ~31u);
        memcpy(c, tex, 0x40 + bytes);
        return c;
    }

    static void retarget(u8* p, u32 size, u32 from, u32 to)
    {
        // As room_css.cpp: the GXTexObj image words and the display lists' texture image
        // registers (BP 0x61, 0x94-0x97 / 0xB4-0xB7).
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

    // Point the model's `name` at `want` (the MDL0's texture links and lists), and its ScnMdl's own
    // copies of the texture objects (ScnMdl and its material buffers +0x14C / +0x140, every
    // frame) from any of `from` to it.
    static void bind(u8* obj, const char* name, u8* want, const u32* from, int nFrom)
    {
        u8* mdl = *(u8**)(obj + 8);
        if (!isPtr((u32)mdl) || *(u32*)mdl != 0x4D444C30 /* MDL0 */) return;
        u8* cur = Labels::boundTexture(mdl, name);
        if (!cur) return;
        u32 to = imageWord(want);
        if (cur != want) {
            Labels::rebindTexture(mdl, name, want);
            u32 size = *(u32*)(mdl + 4);
            if (size > 0x40 && size < 0x100000) {
                retarget(mdl, size, imageWord(cur), to);
                DCFlushRange(mdl, size);
            }
        }
        u8* scn = *(u8**)(obj + 0xC);
        if (!isPtr((u32)scn)) return;
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
                if (a == to || !a) continue;
                for (int k = 0; k < nFrom; k++) {
                    if (from[k] && a == from[k]) {
                        *(u32*)(p + off) = (w & 0xFF000000u) | to;
                        break;
                    }
                }
            }
        }
    }

    static void showRow(u8* rt, int r, const char* value)
    {
        const Row& R = ROWS[r];
        RowTex& T = s_rows[r];
        u8* obj = *(u8**)(rt + R.obj);
        if (!isPtr((u32)obj)) return;
        u8* mdl = *(u8**)(obj + 8);
        if (!isPtr((u32)mdl)) return;
        if (!T.label || !T.value[0] || !T.value[1]) {
            u8* lab = Labels::boundTexture(mdl, R.labelTex);
            u8* val = Labels::boundTexture(mdl, R.valueTex);
            if (!lab || !val) return;
            if (!T.label) {
                T.origLabel = imageWord(lab);
                T.label = copyTex(lab);
                if (!T.label) return;
                int h = *(u16*)(T.label + 0x1E);
                Labels::render(T.label, R.label, 2, 1, R.labelW, h - 1, 0);
            }
            for (int k = 0; k < 2; k++) {
                if (T.value[k]) continue;
                if (k == 0) T.origValue = imageWord(val);
                T.value[k] = copyTex(val);
                if (!T.value[k]) return;
            }
            T.text[0] = 1;   // (no text drawn yet)
            T.shown = 1;
        }
        if (strcmp(T.text, value) != 0) {
            int next = T.shown ^ 1;
            Labels::render(T.value[next], value, R.box[0], R.box[1], R.box[2], R.box[3], 0);
            T.shown = next;
            strncpy(T.text, value, sizeof(T.text) - 1);
            T.text[sizeof(T.text) - 1] = 0;
            g_roomPlay.drawn++;
        }
        u32 fromLabel[1] = {T.origLabel};
        bind(obj, R.labelTex, T.label, fromLabel, 1);
        u32 fromValue[3] = {T.origValue, imageWord(T.value[0]), imageWord(T.value[1])};
        bind(obj, R.valueTex, T.value[T.shown], fromValue, 3);
        strncpy(g_roomPlay.value[r], value, sizeof(g_roomPlay.value[r]) - 1);
    }

    // ------------------------------------------------------------------------------------------
    static struct {
        PPOM::RulesetList list;
        bool haveList;
        int listWait;        // frames until the list is asked for again (while it loads)
        bool asked;          // asked since the menu opened
        int browse;          // RULESET: -1 the room's own, else 0 P+ default, 1..n the list, n+1 code
        int setChoice;       // SET: 0 replay the game, 1 end the set
        bool keypad;         // "Enter code": open the keypad once the menu has closed
        bool wasOpen;
        bool host;
        int bestOf;          // SET: the length the host picked in the menu (-1: none), sent on close
        u32 playKey;         // the ruleset as last seen (changedNow), 0 none
        bool changed;
    } s;

    static const PPOM::RoomPlay& play() { return PPOM::g_block.session.play; }

    static int listCount() { return s.haveList && s.list.state == PPOM::LIST_READY ? (s.list.count > 15 ? 15 : s.list.count) : 0; }
    static int entryCount() { return listCount() + 2; }   // P+ default, the list, Enter code

    static void copyName(char* out, int size, const char* in, int max)
    {
        int n = 0;
        for (; n < max && n < size - 1 && in[n]; n++) out[n] = in[n] >= 0x20 && in[n] < 0x7F ? in[n] : '?';
        out[n] = 0;
    }

    static void entryText(int i, char* out, int size)
    {
        if (i <= 0) {
            strcpy(out, "P+ default");
        } else if (i <= listCount()) {
            copyName(out, size, s.list.entries[i - 1].name, sizeof(s.list.entries[0].name));
            if (!out[0]) copyName(out, size, s.list.entries[i - 1].code, 6);
        } else {
            strcpy(out, "Enter code");
        }
    }

    static bool codeIs(const char* a, const char* b)
    {
        for (int k = 0; k < 6; k++) {
            if (a[k] != b[k]) return false;
            if (!a[k]) return true;
        }
        return true;
    }

    // The entry the room plays, or -1 (custom rules, or a ruleset not in the list).
    static int currentEntry()
    {
        const PPOM::RoomPlay& p = play();
        if (p.custom) return -1;
        if (!p.code[0]) return 0;
        for (int i = 0; i < listCount(); i++) {
            if (codeIs(p.code, s.list.entries[i].code)) return i + 1;
        }
        return -1;
    }

    // The ruleset is locked while a set is on (rules.md §2: "During a set").
    static bool rulesetLocked()
    {
        const PPOM::RoomPlay& p = play();
        return p.setOn && !(p.setFlags & PPOM::SET_FINISHED);
    }

    static bool setPaused() { return play().setOn && (play().setFlags & PPOM::SET_PAUSED); }
    // Paused because the results differed, everyone there: the host replays the game or ends the set.
    static bool hostChoice() { return setPaused() && (play().setFlags & PPOM::SET_CAN_RESUME) != 0; }
    // The set's length can change while no set is on, or once it is finished (the next all-ready
    // starts a new one).
    static bool lengthEditable() { return !play().setOn || (play().setFlags & PPOM::SET_FINISHED); }
    static const u8 LENGTHS[4] = {0, 3, 5, 7};

    static void post(u8 op, u8 arg, const char* code)
    {
        PPOM::RoomRequest req;
        memset(&req, 0, sizeof(req));
        req.op = op;
        req.arg = arg;
        if (code) PPOM::asciiToU16(req.code, code, PPOM::CODE_LEN);
        PPOM::post(PPOM::CMD_ROOM, &req, sizeof(req));
        g_roomPlay.posts++;
        g_roomPlay.lastOp = op;
        g_roomPlay.lastArg = arg;
        memset(g_roomPlay.lastCode, 0, sizeof(g_roomPlay.lastCode));
        if (code) strncpy(g_roomPlay.lastCode, code, 6);
    }

    static void pickRuleset(int i)
    {
        s.bestOf = -1;   // a ruleset brings its own set length
        if (i > listCount()) {
            s.keypad = true;                   // Enter code: the keypad once the menu is gone
        } else {
            char code[8];
            memset(code, 0, sizeof(code));
            if (i > 0) memcpy(code, s.list.entries[i - 1].code, 6);
            post(PPOM::ROOM_RULESET, 0, code);
        }
        playSE(SE_DECIDE);
        closeMenu();
    }

    static const u32 IN_MOVE = 0x1 | 0x2, IN_SIDE = 0x4 | 0x8, IN_LEFT = 0x4, IN_A = 0x10 | 0x200, IN_B = 0x20;

    u32 rulesInput(u32 line, u32 bits, bool host)
    {
        u32 keep = bits & (IN_MOVE | IN_B);
        if (!host || !play().valid) return keep;
        if (line == 4) {
            if (!(bits & (IN_SIDE | IN_A))) return keep;
            if (rulesetLocked()) {
                playSE(SE_ERROR);
                return keep;
            }
            int n = entryCount();
            if (bits & IN_SIDE) {
                int from = s.browse >= 0 ? s.browse : currentEntry();
                if (from < 0) from = (bits & IN_LEFT) ? 0 : n - 1;   // not in the list: from the ends
                s.browse = (from + ((bits & IN_LEFT) ? n - 1 : 1)) % n;
                playSE(SE_CURSOR);
            } else if (s.browse >= 0 && s.browse != currentEntry()) {
                pickRuleset(s.browse);
            } else if (s.browse < 0 && currentEntry() < 0) {
                playSE(SE_ERROR);   // custom rules: browse first
            }
            return keep;
        }
        if (line == 2) {
            if (!(bits & (IN_SIDE | IN_A))) return keep;
            if (!hostChoice() && lengthEditable()) {
                // The set's length: left / right through No set, Best of 3, 5, 7 (A does nothing)
                if (bits & IN_SIDE) {
                    int cur = s.bestOf >= 0 ? s.bestOf : play().bestOf, k = 0;
                    while (k < 4 && LENGTHS[k] != cur) k++;
                    if (k == 4) k = 0;
                    k = (k + ((bits & IN_LEFT) ? 3 : 1)) % 4;
                    s.bestOf = LENGTHS[k];
                    playSE(SE_CURSOR);
                }
                return keep;
            }
            // A set on (playing, paused, or the results differed): left / right to End set (the
            // user's, 2026-10-12: it ends with no result), A confirms; after differing results
            // Replay game comes first.
            if (bits & IN_SIDE) {
                s.setChoice ^= 1;
                playSE(SE_CURSOR);
            } else if (s.setChoice == 1 || hostChoice()) {
                post(PPOM::ROOM_SET, s.setChoice == 0 ? 1 : 0, NULL);   // 1 replay, 0 end the set
                playSE(SE_DECIDE);
                closeMenu();
            }
            return keep;
        }
        return bits;
    }

    // ------------------------------------------------------------------------------------------
    static void setText(char* out, bool host)
    {
        const PPOM::RoomPlay& p = play();
        if (host && s.bestOf >= 0 && lengthEditable()) {
            // the length the host is picking (sent when the menu closes)
            if (s.bestOf) sprintf(out, "Best of %d", s.bestOf);
            else strcpy(out, "No set");
        } else if (!p.bestOf) {
            strcpy(out, "No set");
        } else if (!p.setOn) {
            sprintf(out, "Best of %d", p.bestOf);
        } else if (host && !(p.setFlags & PPOM::SET_FINISHED) && s.setChoice == 1) {
            strcpy(out, "End set");                    // the host's choice during a set
        } else if (p.setFlags & PPOM::SET_PAUSED) {
            if (host && hostChoice()) strcpy(out, "Replay game");
            else sprintf(out, "Paused %d-%d", p.score[0], p.score[1]);
        } else if (p.setFlags & PPOM::SET_FINISHED) {
            sprintf(out, "Bo%d %d-%d final", p.bestOf, p.score[0], p.score[1]);
        } else {
            sprintf(out, "Bo%d %d-%d", p.bestOf, p.score[0], p.score[1]);
        }
    }

    // What the room plays, as one number: a change of any of it took back every ready.
    static u32 playKey()
    {
        const PPOM::RoomPlay& p = play();
        if (!p.valid) return 0;
        u32 h = 0x80000000u | ((u32)p.bestOf << 16) | ((u32)p.stageChoice << 8) | p.custom;
        for (int k = 0; k < 8 && p.code[k]; k++) h = h * 31 + (u8)p.code[k];
        for (int k = 0; k < 32 && p.name[k]; k++) h = h * 31 + (u8)p.name[k];
        return h | 1u;
    }

    bool changedNow() { return s.changed; }

    void tick(bool inRoom, bool host)
    {
        s.host = host;
        u32 key = inRoom ? playKey() : 0;
        s.changed = key && s.playKey && key != s.playKey;
        s.playKey = key;
        u8* rt = inRoom && play().valid ? rulesTask() : NULL;
        if (!rt) {
            // The menu closed: the set length the host picked goes to the room (ROOM_BEST_OF).
            if (s.wasOpen && s.bestOf >= 0 && host && inRoom && play().valid && s.bestOf != play().bestOf &&
                lengthEditable()) {
                post(PPOM::ROOM_BEST_OF, (u8)s.bestOf, NULL);
            }
            s.bestOf = -1;
            s.browse = -1;
            s.asked = false;
            s.wasOpen = false;
            g_roomPlay.browse = 0xFF;
            return;
        }
        if (!s.wasOpen) {
            s.wasOpen = true;
            s.browse = -1;
            s.setChoice = 0;
            s.bestOf = -1;
        }
        // The list: asked for when the host opens the menu, and again while Dolphin loads it.
        if (host && (!s.asked || (s.listWait > 0 && --s.listWait == 0))) {
            PPOM::post(PPOM::CMD_RULESETS, NULL, 0);
            s.asked = true;
            s.listWait = 0;
        }
        u8* page = *(u8**)(rt + RULES_PAGE1);
        if (isPtr((u32)page) && *(u16*)(page + 0x42) != 4) s.browse = -1;   // the cursor left it
        if (lengthEditable()) s.setChoice = 0;   // no set on: the score (or Replay game) first next time
        g_roomPlay.browse = s.browse < 0 ? 0xFF : (u8)s.browse;
        g_roomPlay.setChoice = (u8)s.setChoice;
        char text[40];
        if (s.browse >= 0) {
            entryText(s.browse, text, sizeof(text));
        } else {
            copyName(text, sizeof(text), play().name, sizeof(play().name));
            if (!text[0]) strcpy(text, play().custom ? "Custom" : "P+ default");
        }
        showRow(rt, ROW_RULESET, text);
        setText(text, host);
        showRow(rt, ROW_SET, text);
    }

    void onRulesets(const PPOM::Response& r)
    {
        const PPOM::RulesetList& l = *(const PPOM::RulesetList*)r.payload;
        if (l.state == PPOM::LIST_LOADING || l.state == PPOM::LIST_NONE) {
            s.listWait = 30;   // ask again in half a second
            if (!s.haveList) return;
        }
        if (l.state == PPOM::LIST_READY || !s.haveList) {
            memcpy(&s.list, &l, sizeof(s.list));
            s.haveList = true;
        }
    }

    bool takeKeypadRequest()
    {
        if (!s.keypad || rulesTask()) return false;
        s.keypad = false;
        return true;
    }

    void cssLine(char* out, int size, const char* name0, const char* name1)
    {
        out[0] = 0;
        const PPOM::RoomPlay& p = play();
        if (!p.valid || size < 96) return;
        char name[40];
        copyName(name, sizeof(name), p.name, sizeof(p.name));
        if (!name[0]) strcpy(name, p.custom ? "Custom" : "P+ default");
        int n = sprintf(out, "Ruleset: %s", name);
        if (p.setOn) {
            n += sprintf(out + n, "\nBo%d: %s %d - %d %s", p.bestOf, name0, p.score[0], p.score[1], name1);
            if (p.setFlags & PPOM::SET_PAUSED) sprintf(out + n, " (paused)");
            else if (p.setFlags & PPOM::SET_FINISHED) sprintf(out + n, " (final)");
        } else if (p.bestOf) {
            sprintf(out + n, "\nBest of %d", p.bestOf);
        } else {
            static const char* const CHOICE[3] = {"Loser picks the stage", "Stages struck every game",
                                                  "Random stage"};
            if (p.stageChoice < 3) sprintf(out + n, "\n%s", CHOICE[p.stageChoice]);
        }
    }

    const char* menuText(const void* msbin, u32 line)
    {
        if (!s.wasOpen || !rulesTask()) return NULL;   // the rules menu, opened in a room
        // P+'s descriptions for the values these rows keep: stock control OFF, stage choice CHOOSE
        if (Online::msbinLineContains(msbin, (int)line, "same stock count")) {
            return s.host ? "Left/right: the set's length; during a set, End set (A)."
                          : "The room's set and its score.";
        }
        if (Online::msbinLineContains(msbin, (int)line, "complete freedom")) {
            return s.host ? "Left/right: your recent and own rulesets. A: play it."
                          : "The ruleset the room plays (the host picks it).";
        }
        return NULL;
    }

    void reset()
    {
        s.playKey = 0;
        s.changed = false;
        s.bestOf = -1;
        s.browse = -1;
        s.asked = false;
        s.wasOpen = false;
        s.keypad = false;
    }
}
