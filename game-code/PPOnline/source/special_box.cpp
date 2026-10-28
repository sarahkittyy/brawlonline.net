// P+'s Special Versus box on the room's rules menu (docs/design/special-rules.md §2, §5).
//
// The box is P+'s own: the models of Brawl's Special Brawl page (muProcSpecial, sora_menu_main),
// MenMainSpecial0001 (the frame and the OK button) to 0010, with their textures, from P+'s
// main-menu archive, which the character select never loads. Dolphin reads that archive from the
// player's card and copies the box's part (Online/SpecialAssets, about 330 KB) into a buffer of
// ours in Fighter1Resoruce (free on the character select) when the SPECIAL page first shows;
// everything is freed when the menu closes. The page is driven as muProcSpecial drives it
// (read off P+'s own page, 2026-10-27):
//   - the rows 0003..0007 (STAMINA..STATUS), 0010 (GRAVITY), 0008 (SPEED), 0009 (OTHER): CHR frame 9 (the end of their
//     entrance), CLR frame 3 + 4 * value (0 off, 1 its left value, 2 its right);
//   - the cursor 0002: VIS frame = the column (the value, 3 on OK), CHR frame = the row (8 on
//     OK), its CLR playing (the pulse);
//   - 0001: CHR 9, CLR 0, 1 with the cursor on OK.
// The descriptions are P+'s (its msbin, lines 34 + 3 * row + value), in the menu's footer.
#include "special_box.h"

#include <string.h>

#include <mu/mu_msg.h>

#include "online.h"
#include "ppom.h"
#include "rules_pages.h"

extern "C" int sprintf(char* buf, const char* fmt, ...);

// Tests (harness, through PPOnlineMain.map).
struct SpecialBoxDebug {
    u32 state;          // the assets: SpecialAssetsState as last answered
    u32 asks;           // CMD_SPECIAL_ASSETS sent
    u32 made;           // boxes made
    u32 objs;           // the box's MuObjects alive
    u32 buffer;         // the assets' buffer, 0 none
    u8 rows[8];         // the values shown
    u8 cursorRow, cursorCol, staminaType, edited;
    u32 failed;         // models that could not be made
    u32 msg;            // the description's MuMsg
};
extern "C" {
    SpecialBoxDebug g_specialBox;
}

namespace SpecialBox {

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
    static const AllocFn HEAP_ALLOC = (AllocFn)0x800249E4;
    static const FreeFn HEAP_FREE = (FreeFn)0x80024A4C;
    static const int HEAP_FIGHTER1 = 18, MENU_HEAP = 0x2A;
    typedef u8* (*CreateFn)(void* modelRes, const char* name, int priority, void* texRes, int heap);
    typedef void (*NameFn)(u8* obj, const char* name);
    typedef void (*FrameFn)(u8* obj, float frame);
    typedef void (*ResInitFn)(void* resFile);
    typedef void (*SetVisFn)(void* scnObj, bool vis);
    static const CreateFn MU_CREATE = (CreateFn)0x800B2394;   // MuObject::create(ResFile*, name, ...)
    static const NameFn CHANGE_ANIM = (NameFn)0x800B534C;     // MuObject::changeAnimN
    static const FrameFn SET_FRAME_VIS = (FrameFn)0x800B784C; // (anim +0x08)
    static const FrameFn SET_FRAME_NODE = (FrameFn)0x800B7798;// (anim +0x0C, CHR0)
    static const FrameFn SET_FRAME_COL = (FrameFn)0x800B7A18; // (anim +0x18, CLR0)
    static const ResInitFn RES_INIT = (ResInitFn)0x8018EA08;  // nw4r::g3d::ResFile::Init
    static const SetVisFn SET_VIS = (SetVisFn)0x80043D20;     // nwSMSetVisibility
    // The description in the box, as muProcSpecial prints it: a MuMsg attached to the frame
    // model's text node, P+'s own msbin line (sss_portraits.cpp's name tags for the calls).
    typedef MuMsg* (*MsgCreateFn)(u32, u32, u32);
    typedef void (*MsgAllocFn)(MuMsg*, u32 size, u32 count);
    typedef void (*MsgAttachFn)(MuMsg*, u32 window, void* scnMdl, u32 textNode, float size);
    typedef void (*MsgSetFaceFn)(MuMsg*, u32 window, u32 face);
    typedef void (*MsgDeleteFn)(MuMsg*, int);
    typedef void (*PrintIndexFn)(void* msg, u32 window, u32 line, const void* msbin);
    static const MsgCreateFn MSG_CREATE = (MsgCreateFn)0x800B8930;
    static const MsgAllocFn MSG_ALLOC = (MsgAllocFn)0x800B8B08;
    static const MsgAttachFn MSG_ATTACH = (MsgAttachFn)0x800B8C90;
    static const MsgSetFaceFn MSG_SET_FACE = (MsgSetFaceFn)0x800B9488;
    static const MsgDeleteFn MSG_DELETE = (MsgDeleteFn)0x800B8A0C;
    static const PrintIndexFn PRINT_INDEX = (PrintIndexFn)0x800B91B8;

    // Dolphin's bundle (Online/SpecialAssets.h): "SPBX", size, count, 0, then per entry
    // {role u16, type u16, offset u32, size u32, 0}.
    static const u32 MAGIC = 0x53504258, CAPACITY = 0x80000;
    enum { ROLE_TEXTURES = 0, ROLE_MESSAGES = 11, ROLES = 12, MODELS = 10 };

    static const u32 TASK_GROUP = 0x6C;   // muSelCharTask: its scene group (the rules menu draws in it)
    static const u32 IN_UP = 0x1, IN_DOWN = 0x2, IN_LEFT = 0x4, IN_RIGHT = 0x8;
    static const u32 IN_A = 0x10 | 0x200, IN_Y = 0x800, IN_X = 0x400;
    static const int OK_ROW = 8;

    static struct {
        bool opened;
        bool canEdit;
        bool showing;
        u8 rows[8];
        u8 staminaType;
        bool edited;
        int cursorRow;
        // The assets.
        u8* raw;             // the Fighter1Resoruce block
        u8* buf;             // 32-byte aligned in it
        u32 nonce;
        bool asked;          // a request is out
        int askIn;           // frames until it is asked again (while Dolphin loads)
        u8 state;            // SpecialAssetsState
        bool written;
        u32 texRes;          // ResFiles (the brres' addresses, ResFile::Init'ed)
        u32 modelRes[MODELS];
        u8* obj[MODELS];     // MenMainSpecial0001..0010
        u8* group;           // the scene group they are in
        MuMsg* msg;          // the description
        u8* msgMdl;          // the ScnMdl it is attached to
        u32 msgOldCallback, msgCallback;
        int printed;         // the msbin line it shows, -1 none
        u32 titleKey;        // the rows the title was printed for (+1), 0 none
    } s;

    // The frame's text bones (attachScnMdlSimple's index: textN<i>, the window between textL<i> and
    // textR<i>): 0 the title band at the top (P+'s "300% Mini Versus"), 1 the description under the rows.
    static const u32 TEXT_TITLE = 0, TEXT_DESCRIPTION = 1;
    static const float TEXT_SIZE = 0.0625f;   // (as the CSS's name tags; P+'s own size there)

    u8 row(int i) { return i >= 0 && i < 8 ? s.rows[i] : 0; }

    void open(bool canEdit)
    {
        const PPOM::Special& sp = PPOM::g_block.session.special;
        bool asked = s.asked;
        memset(&s, 0, sizeof(s));
        s.asked = asked;   // (an answer still out finds no buffer: nothing is written, the magic is gone)
        s.opened = true;
        s.canEdit = canEdit;
        for (int i = 0; i < 8; i++) s.rows[i] = sp.rows[i] <= 2 ? sp.rows[i] : 0;
        s.staminaType = sp.staminaType >= 1 && sp.staminaType <= 5 ? sp.staminaType : 1;
    }

    // ---- The assets ---------------------------------------------------------------------------
    static void ask()
    {
        if (!s.buf) {
            s.raw = (u8*)HEAP_ALLOC(HEAP_FIGHTER1, CAPACITY + 32);
            if (!s.raw) {
                s.state = PPOM::SA_UNAVAILABLE;
                s.askIn = 120;
                return;
            }
            s.buf = (u8*)(((u32)s.raw + 31) & ~31u);
            g_specialBox.buffer = (u32)s.buf;
        }
        s.nonce = PPOM::g_block.debug.frames ^ 0x5B0C1A11;
        *(u32*)s.buf = MAGIC;
        *(u32*)(s.buf + 4) = s.nonce;
        *(u32*)(s.buf + 8) = 0;
        PPOM::SpecialAssetsRequest req = {(u32)s.buf, CAPACITY, s.nonce};
        PPOM::post(PPOM::CMD_SPECIAL_ASSETS, &req, sizeof(req));
        s.asked = true;
        g_specialBox.asks++;
    }

    void onAssets(const PPOM::Response& r)
    {
        s.asked = false;
        if (!s.opened || !s.buf) return;
        s.state = r.payload[0] >= PPOM::SA_LOADING && r.payload[0] <= PPOM::SA_UNAVAILABLE ? r.payload[0]
                                                                                    : PPOM::SA_UNAVAILABLE;
        g_specialBox.state = s.state;
        if (s.state == PPOM::SA_READY && r.payload[1]) {
            u32 size = *(u32*)(r.payload + 4);
            s.written = *(u32*)s.buf == MAGIC && *(u32*)(s.buf + 4) == size && size <= CAPACITY;
        } else if (s.state == PPOM::SA_LOADING || s.state == PPOM::SA_READY) {
            s.askIn = 20;
        } else {
            s.askIn = 120;   // Dolphin builds it again a few seconds after a failure (the card busy)
        }
    }

    // The bundle's entry for `role`, or NULL.
    static u8* entry(u32 role, u32* size)
    {
        u32 count = *(u32*)(s.buf + 8);
        for (u32 i = 0; i < count && i < ROLES; i++) {
            const u8* e = s.buf + 0x10 + 0x10 * i;
            u32 off = *(u32*)(e + 4), n = *(u32*)(e + 8);
            if (*(u16*)e != role || off + n > CAPACITY || (off & 31)) continue;
            if (size) *size = n;
            return s.buf + off;
        }
        return NULL;
    }

    // ---- The box ------------------------------------------------------------------------------
    static u8* cssTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        u32 scene = isPtr(mgr) ? *(u32*)(mgr + 4) : 0;
        u32 t = isPtr(scene) ? *(u32*)(scene + 0x400) : 0;
        return isPtr(t) ? (u8*)t : NULL;
    }

    static void setFrame(u8* obj, u32 slot, FrameFn set, float frame, float rate)
    {
        u8* anim = *(u8**)(obj + 0x14);
        if (!isPtr((u32)anim)) return;
        u8* a = *(u8**)(anim + slot);
        if (!isPtr((u32)a)) return;
        set(obj, frame);
        *(float*)(a + 0x1C) = rate;
    }

    // P+'s OK button under the box hides nothing but the footer, where the stamina type is (Sarah's
    // board, 2026-10-27: "remove the OK button, as it serves no purpose and hides the stamina mode
    // blast zone setting beneath"): its nodes (okN, ok, okbase1, okbase2) in our copy of the frame's
    // MDL0 made invisible (ResNodeData +0x14 flags, 0x100 visible) and scaled to nothing (+0x20),
    // before the model is made. The cursor never goes to it (input).
    static void hideOkButton(u8* brres, u32 size)
    {
        u8* mdl = NULL;
        for (u32 o = 0; o + 0x50 < size; o += 4) {
            if (*(u32*)(brres + o) == 0x4D444C30) {   // 'MDL0'
                mdl = brres + o;
                break;
            }
        }
        if (!mdl) return;
        s32 nodes = *(s32*)(mdl + 0x14);   // the node group (section 1, version 9)
        if (nodes <= 0 || (u32)nodes > size) return;
        u8* grp = mdl + nodes;
        u32 count = *(u32*)(grp + 4);
        for (u32 i = 1; i <= count && i < 64; i++) {
            u8* e = grp + 8 + 16 * i;
            const char* name = (const char*)(grp + *(s32*)(e + 8));
            u8* node = grp + *(s32*)(e + 12);
            if (name[0] != 'o' || name[1] != 'k') continue;   // okN, ok, okbase1, okbase2
            *(u32*)(node + 0x14) &= ~0x100u;
            float* scale = (float*)(node + 0x20);
            scale[0] = scale[1] = scale[2] = 0.0f;
        }
    }

    static void makeBox(u8* rt)
    {
        u8* task = cssTask();
        u8* group = task ? *(u8**)(task + TASK_GROUP) : NULL;
        u8* tex = entry(ROLE_TEXTURES, NULL);
        if (!isPtr((u32)group) || !tex) return;
        s.texRes = (u32)tex;
        RES_INIT(&s.texRes);
        // The rules menu's own rows' draw priority (their ScnMdl +0xD0): the box over the menu.
        u8 prio = 0x14;
        u8* rowObj = rt ? *(u8**)(rt + 0x90) : NULL;
        u8* rowScn = isPtr((u32)rowObj) ? *(u8**)(rowObj + 0x10) : NULL;
        if (isPtr((u32)rowScn)) prio = rowScn[0xD0];
        for (int m = 0; m < MODELS; m++) {
            u8* mdl = entry((u32)(m + 1), NULL);
            if (!mdl) {
                g_specialBox.failed++;
                continue;
            }
            if (m == 0) {
                u32 size = 0;
                entry(1, &size);
                hideOkButton(mdl, size);
            }
            s.modelRes[m] = (u32)mdl;
            RES_INIT(&s.modelRes[m]);
            char name[32];
            sprintf(name, "MenMainSpecial%04d_TopN", m + 1);
            // P+'s own order (read off its page): the frame, the rows 9 over it, the cursor on top.
            int p = prio + (m == 0 ? 0 : m == 1 ? 0x17 : 9);
            u8* obj = MU_CREATE(&s.modelRes[m], name, p > 0xFF ? 0xFF : p, &s.texRes, MENU_HEAP);
            if (!isPtr((u32)obj)) {
                g_specialBox.failed++;
                continue;
            }
            char anim[40];
            sprintf(anim, "%s__0", name);
            CHANGE_ANIM(obj, anim);
            s.obj[m] = obj;
            g_specialBox.objs++;
            u8* scn = *(u8**)(obj + 0x10);
            if (isPtr((u32)scn)) {
                typedef bool (*InsertFn)(u8*, u32, u8*);
                ((InsertFn)(*(u32*)(*(u32*)group + 0x34)))(group, *(u32*)(group + 0xE4), scn);
            }
        }
        s.group = group;
        g_specialBox.made++;
        // The description.
        u8* frameScn = s.obj[0] ? *(u8**)(s.obj[0] + 0x10) : NULL;
        if (isPtr((u32)frameScn) && entry(ROLE_MESSAGES, NULL)) {
            s.msg = MSG_CREATE(3, MENU_HEAP, 0x2B);
            if (s.msg) {
                // Window 0 the description (P+'s dark red), 1 the title (P+'s grey, bf c7 c4).
                MSG_ALLOC(s.msg, 0x200, 2);
                s.msgMdl = frameScn;
                s.msgOldCallback = *(u32*)(frameScn + 0xD4);
                MSG_ATTACH(s.msg, 0, frameScn, TEXT_DESCRIPTION, TEXT_SIZE);
                MSG_ATTACH(s.msg, 1, frameScn, TEXT_TITLE, TEXT_SIZE);
                MSG_SET_FACE(s.msg, 0, 0);
                MSG_SET_FACE(s.msg, 1, 0);
                s.msgCallback = *(u32*)(frameScn + 0xD4);
                s.msg->setFontColor(0, 0x8C, 0x10, 0x10, 0xFF);
                s.msg->setFontColor(1, 0xBF, 0xC7, 0xC4, 0xFF);
                s.msg->setAlignMode(0, MuMsg::Align_Center);
                s.msg->setAlignMode(1, MuMsg::Align_Center);
                s.printed = -1;
                s.titleKey = 0;
                g_specialBox.msg = (u32)s.msg;
            }
        }
    }

    static void deleteBox()
    {
        if (s.msg) {
            if (s.msgMdl && *(u32*)(s.msgMdl + 0xD4) == s.msgCallback) *(u32*)(s.msgMdl + 0xD4) = s.msgOldCallback;
            MSG_DELETE(s.msg, 1);
            s.msg = NULL;
            s.msgMdl = NULL;
        }
        u8* task = cssTask();
        bool groupThere = task && s.group && *(u8**)(task + TASK_GROUP) == s.group;
        for (int m = 0; m < MODELS; m++) {
            u8* obj = s.obj[m];
            if (!obj) continue;
            u8* scn = *(u8**)(obj + 0x10);
            if (groupThere && isPtr((u32)scn)) {
                typedef bool (*RemoveFn)(u8*, u8*);
                ((RemoveFn)(*(u32*)(*(u32*)s.group + 0x3C)))(s.group, scn);
            }
            typedef void (*DelFn)(u8*, int);
            ((DelFn)(*(u32*)(*(u32*)(obj + 0x5C) + 8)))(obj, 1);
            s.obj[m] = NULL;
            g_specialBox.objs--;
        }
        s.group = NULL;
    }

    static void visible(bool on)
    {
        for (int m = 0; m < MODELS; m++) {
            u8* obj = s.obj[m];
            u8* scn = obj ? *(u8**)(obj + 0x10) : NULL;
            if (isPtr((u32)scn)) SET_VIS(scn, on);
        }
    }

    // The box as the values and the cursor are (§ above).
    static void frames()
    {
        // The rows' models: STAMINA..STATUS 0003..0007, then GRAVITY 0010, SPEED 0008, OTHER 0009
        // (P+'s box has them in that order on screen; Sarah's board, 2026-10-27: "the gravity setting
        // controls speed, speed controls other, and other controls gravity" with 0008-0010 in order).
        static const int ROW_MODEL[8] = {2, 3, 4, 5, 6, 9, 7, 8};
        for (int r = 0; r < 8; r++) {
            u8* o = s.obj[ROW_MODEL[r]];
            if (!o) continue;
            setFrame(o, 0x0C, SET_FRAME_NODE, 9.0f, 0.0f);
            setFrame(o, 0x18, SET_FRAME_COL, (float)(3 + 4 * s.rows[r]), 0.0f);
        }
        if (u8* frame = s.obj[0]) {
            setFrame(frame, 0x0C, SET_FRAME_NODE, 9.0f, 0.0f);
            setFrame(frame, 0x18, SET_FRAME_COL, s.cursorRow == OK_ROW ? 1.0f : 0.0f, 0.0f);
        }
        // P+'s description of the row under the cursor and its value (msbin lines 34 + 3 * row + value).
        int line = s.cursorRow == OK_ROW ? -1 : 34 + 3 * s.cursorRow + s.rows[s.cursorRow];
        u8* msbin = entry(ROLE_MESSAGES, NULL);
        if (s.msg && msbin && line != s.printed) {
            if (line >= 0) PRINT_INDEX(s.msg, 0, (u32)line, msbin);
            else s.msg->printf(0, " ");
            s.printed = line;
        }
        // The title as P+ makes it: each row that is on's word, then "Versus" ("Normal Versus").
        u32 key = 1;
        for (int r = 0; r < 8; r++) key = key * 3 + s.rows[r];
        if (s.msg && key != s.titleKey) {
            static const char* const WORDS[8][2] = {
                {"300%", "Stamina"}, {"Mega", "Mini"}, {"Random Element", "Regen"}, {"Metal", "Clear"},
                {"Turbo Mode", "Bunny"}, {"Light", "Heavy"}, {"Wild", "Fast"}, {"Meter", "All-Star"}};
            char title[128];
            int n = 0;
            for (int r = 0; r < 8; r++) {
                if (s.rows[r] == 1 || s.rows[r] == 2) n += sprintf(title + n, "%s ", WORDS[r][s.rows[r] - 1]);
            }
            if (n == 0) n = sprintf(title, "Normal ");
            sprintf(title + n, "Versus");
            s.msg->printf(1, "%s", title);
            s.titleKey = key;
        }
        if (u8* cur = s.obj[1]) {
            bool ok = s.cursorRow == OK_ROW;
            setFrame(cur, 0x08, SET_FRAME_VIS, ok ? 3.0f : (float)s.rows[s.cursorRow], 0.0f);
            setFrame(cur, 0x0C, SET_FRAME_NODE, (float)s.cursorRow, 0.0f);
        }
    }

    u8* object(int i) { return i >= 0 && i < MODELS ? s.obj[i] : NULL; }

    void slide(float dx)
    {
        // (MuObject::setPos, 0x800B6838: an offset from where the models' animations put them)
        typedef void (*SetPosFn)(u8*, float*);
        for (int i = 0; i < MODELS; i++) {
            u8* o = s.obj[i];
            if (!o) continue;
            float* pos = (float*)(o + 0x3C);
            if (pos[0] == dx) continue;
            float p[3] = {dx, pos[1], pos[2]};
            ((SetPosFn)0x800B6838)(o, p);
        }
    }

    void show(bool on)
    {
        s.showing = on;
        if (!on) {
            visible(false);
            return;
        }
        if (!s.buf && !s.asked) ask();
        visible(true);
    }

    void tick(u8* rt, bool showing)
    {
        if (!s.opened) return;
        s.showing = showing;
        if (s.askIn > 0 && --s.askIn == 0 && !s.asked && !s.written) ask();
        if (showing && s.written && !s.obj[0] && !s.group) {
            makeBox(rt);
            visible(true);
        }
        if (s.obj[0]) frames();
        memcpy(g_specialBox.rows, s.rows, 8);
        g_specialBox.cursorRow = (u8)s.cursorRow;
        g_specialBox.cursorCol = s.cursorRow == OK_ROW ? 3 : s.rows[s.cursorRow];
        g_specialBox.staminaType = s.staminaType;
        g_specialBox.edited = s.edited;
    }

    static void reprint();

    void input(u32 bits, bool canEdit)
    {
        int r = s.cursorRow;
        // (the eight rows round: P+'s OK button below them is hidden, hideOkButton)
        if (bits & IN_UP) {
            s.cursorRow = r <= 0 ? 7 : r - 1;
            playSE(SE_CURSOR);
        } else if (bits & IN_DOWN) {
            s.cursorRow = r >= 7 ? 0 : r + 1;
            playSE(SE_CURSOR);
        } else if (r != OK_ROW && (bits & (IN_LEFT | IN_RIGHT | IN_A))) {
            if (!canEdit) {
                playSE(SE_ERROR);
            } else {
                int v = s.rows[r];
                v = (bits & IN_LEFT) ? (v + 2) % 3 : (v + 1) % 3;
                s.rows[r] = (u8)v;
                if (r == PPOM::SR_STAMINA && v != 2) s.staminaType = 1;
                s.edited = true;
                playSE(SE_TOGGLE);
            }
        } else if (r == PPOM::SR_STAMINA && s.rows[r] == 2 && (bits & (IN_Y | IN_X))) {
            if (!canEdit) {
                playSE(SE_ERROR);
            } else {
                s.staminaType = (u8)(s.staminaType % 5 + 1);
                s.edited = true;
                playSE(SE_TOGGLE);
            }
        } else if (r == OK_ROW && (bits & IN_A)) {
            playSE(SE_DECIDE);
        }
        reprint();
    }

    bool edits(u8 special[8], u8* staminaType)
    {
        if (!s.edited) return false;
        memcpy(special, s.rows, 8);
        *staminaType = s.rows[PPOM::SR_STAMINA] == 2 ? s.staminaType : 1;
        return true;
    }

    void close()
    {
        deleteBox();
        // (an answer still out: the buffer's magic is gone, so Dolphin writes nothing into it)
        if (s.buf) *(u32*)s.buf = 0;
        if (s.raw) HEAP_FREE(s.raw);
        bool asked = s.asked;
        memset(&s, 0, sizeof(s));
        s.asked = asked;
        g_specialBox.buffer = 0;
    }

    // ---- The footer: P+'s descriptions ------------------------------------------------------
    static const char* const DESCRIPTIONS[8][3] = {
        {"Start the match with 0% damage.", "Start with 300% damage! One-hit KOs!",
         "When your stamina runs out, you're KO'd!"},
        {"Fight a match at normal size.", "Fight a huge battle while giant sized!",
         "Fight a tiny battle while miniaturized!"},
        {"Fight with normal attack effects.", "Fight with random hit effects!",
         "Your damage resets if you can avoid being hit for a second."},
        {"Play with standard body types.", "Play in metal form, making you flinch less.",
         "Play with invisible characters!"},
        {"Play with characters in normal states.", "Cancel moves on-hit for maximum combo potential!",
         "Wear a Bunny Hood, boosting performance."},
        {"Play with characters in normal gravity.", "Jump higher and fall more slowly.",
         "Jump lower and fall faster."},
        {"Play at normal speed.", "No! This isn't how you're supposed to play the game!",
         "Play at a bewildering speed!"},
        {"Play without extra gimmicks.", "Charge a Final Smash while fighting!",
         "Fight with a different character per stock!"},
    };
    static const char* const STAMINA_TYPES[5] = {
        "Set knockback with blastzones.", "Set knockback without blastzones.",
        "Normal knockback without blastzones.", "Normal knockback with blastzones.",
        "Normal knockback, no blastzones, and stabilized camera.",
    };

    const char* footer()
    {
        static char buf[120];
        if (!s.written) return "Loading Special Versus...";
        if (s.cursorRow == OK_ROW) return s.canEdit ? "B: back to the rules." : "The room's special rules (the host sets them).";
        if (s.cursorRow == PPOM::SR_STAMINA && s.rows[0] == 2) {
            sprintf(buf, "Type %d: %s (Y)", s.staminaType, STAMINA_TYPES[s.staminaType - 1]);
            return buf;
        }
        // (the description is in the box, P+'s own line; the footer is behind the OK button)
        return s.msg ? " " : DESCRIPTIONS[s.cursorRow][s.rows[s.cursorRow]];
    }

    static void reprint() { RulesPages::reprintFooter(); }
}
