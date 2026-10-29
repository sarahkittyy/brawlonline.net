// The room CSS's RULES button (docs/design/room-rules.md decision 4; docs/game-code.md §14
// "RULES button") and its SIT OUT / PLAY button (the user's, 2026-10-18; docs/design/rooms.md
// "Sitting out"): two more buttons in the bar, left of the room's PUBLIC / PRIVATE and FFA /
// TEAMS (Brawl's ITEM and STAGE buttons, room_css.cpp headerButtons), RULES level with PUBLIC
// and SIT OUT with FFA, made, labelled, lit and hit-tested the way Brawl does those two.
//
// How Brawl's CSS (sora_menu_sel_char, module 10, .text 0x806828C4) does its buttons:
//   - Its models are MuObjects in muSelCharTask+0x13C + 4 * index (0xBC slots), made from tables
//     of {name, first index, last index, draw priority} by text+0xC154 (0x8068EA18): for each,
//     MuObject::create(archive, name, priority, NULL, heap 0x2A) (0x800B2394), changeAnimN(name
//     + "__0") (0x800B534C) and the animation's update rate 0 (0x80028650). ITEM is
//     "MenSelchrState0005_TopN" (index 0xB9: +0x420), STAGE "MenSelchrState0006_TopN" (0xBA:
//     +0x424), priority 0x0A, from the table at .rodata 0x806A0AC0 (text+0xC138). Each one's
//     ScnMdl (MuObject+0x10) is appended to the CSS's scene group (task+0x6C, text+0x3A44).
//     The task's destructor (text+0x434) deletes every slot's object (the group is gone by then).
//   - The hand's hit test, text+0xDC78 (0x8069053C) hit(task, float rect[4] out, float pos[2]),
//     walks 24 buttons {button, object index, node, kind} (.rodata 0x806A0C70; ITEM 19 B9 00 04,
//     STAGE 1A BA 00 05): a button whose kind says it is on (by CSS mode) and whose object is
//     shown (task+0x510 + 12 * i, bit 0x80) is hit when the hand's position is inside the
//     model's rect between its nodes "colL" and "colR" (0x800B68F0 getRect(obj, rect, "colL",
//     "colR"): left, right, top, bottom, in the hand's units). It returns the button (else 0).
//     Each frame every hand stores it at muSelCharHand+0xAC (text+0x1B430); A runs
//     buttonProcInAllArea (text+0x7A94), which asks the hit test again and acts on the button.
//   - The highlight, text+0x9A2C (0x8068C2F0), per button model: while a hand's +0xAC is its
//     button, its colour animation (MuObject+0x14 -> +0x18, vtable +0x2C frame, +0x28 rate) plays
//     at rate 1 from frame 0 (the button pulses); otherwise setFrameMatCol(0) (0x800B7A18) and
//     rate 0.
//
// Each is a clone of ITEM's model, made the same way (same archive, name, priority, heap,
// animation), kept in the plugin (not in a task slot: those are Brawl's) and deleted in the
// task's destructor before Brawl deletes its own. Its labels are ITEM's label texture
// (MenSelchrWifi00Button11, CMPR 48x24) copied, the word drawn in the border as room_css.cpp
// draws PUBLIC / FFA ("RULES"; "SIT OUT" / "PLAY", the button's two states), and only that
// model's ScnMdl pointed at the copy (ITEM's own label is room_css.cpp's). They are moved left of
// the two (MuObject::setPos, 0x800B6838). Brawl's hit test is replaced by one that asks Brawl's
// first and, when nothing was hit, tests each one's colL / colR rect (BUTTON_RULES,
// BUTTON_SIT_OUT), so the hand's +0xAC, its look and the highlight work as for ITEM; and
// buttonProcInAllArea's dispatch (text+0x7AF0) takes them as ITEM's press is taken: noted for
// pressed(), then the function's exit (A on them does not reach the coin).
#include <OS/OSCache.h>
#include <gf/gf_heap_manager.h>
#include <memory.h>
#include <string.h>
#include "labels.h"
#include "online_menu.h"
#include "rules_button.h"

// Tests (harness/tests/test_rooms_ui.py test_rules_button, test_room_sit_out_ui.py), read through
// PPOnlineMain.map.
struct ButtonDebug {
    u32 made;         // models made
    u32 presses;      // A presses taken
    u32 hits;         // frames a hand was on it
    float rect[4];    // its colL / colR rect last frame (left, right, top, bottom; hand units)
    float place[3];   // its offset from ITEM (MuObject::setPos; here in the hand's units)
};
// ITEM to STAGE is about 2.5 down in these units: RULES level with PUBLIC (ITEM), SIT OUT with
// FFA (STAGE), both one button's width to the left.
extern "C" ButtonDebug g_rulesButton = {0, 0, 0, {0, 0, 0, 0}, {-5.3f, 0.0f, 0.0f}};
extern "C" ButtonDebug g_sitOutButton = {0, 0, 0, {0, 0, 0, 0}, {-5.3f, -2.5f, 0.0f}};

namespace {

    bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // Hand button numbers Brawl does not use: its table has 2-0x1A, the panels 0x1B-0x26.
    const int BUTTON_RULES = 0x30, BUTTON_SIT_OUT = 0x31;
    const u32 ITEM_OBJ = 0x420;               // muSelCharTask: MenSelchrState0005 (ITEM)
    const char* const MODEL = "MenSelchrState0005_TopN";
    const char* const ANIM = "MenSelchrState0005_TopN__0";
    const char* const LABEL_TEX = "MenSelchrWifi00Button11";
    const int MODEL_PRIORITY = 0x0A, MODEL_HEAP = 0x2A;
    const u32 TASK_RULES_OPEN = 0x630;        // muSelCharTask: P+'s rules menu is open

    typedef u8* (*CreateFn)(void* modelRes, const char* name, int priority, void* texRes, int heap);
    typedef void (*NameFn)(u8* obj, const char* name);
    typedef void (*RateFn)(void* anim, float rate);
    typedef void (*ObjFloatFn)(u8* obj, float f);
    typedef void (*SetPosFn)(u8* obj, float* pos);
    typedef void (*RectFn)(u8* obj, float* rect, const char* left, const char* right);
    typedef void (*SetVisFn)(void* scnMdl, bool vis);
    typedef int (*HitFn)(u8* task, float* rect, float* pos);
    typedef void* (*DtorFn)(u8* task, int flags);
    const CreateFn MU_CREATE = (CreateFn)0x800B2394;          // MuObject::create(ResFile*, const char*, ...)
    const NameFn CHANGE_ANIM = (NameFn)0x800B534C;            // MuObject::changeAnimN
    const RateFn SET_UPDATE_RATE = (RateFn)0x80028650;        // gfModelAnimation::setUpdateRate
    const ObjFloatFn SET_FRAME_MATCOL = (ObjFloatFn)0x800B7A18;   // MuObject::setFrameMatCol
    const SetPosFn SET_POS = (SetPosFn)0x800B6838;            // MuObject::setPos
    const RectFn GET_RECT = (RectFn)0x800B68F0;
    const SetVisFn SET_VIS = (SetVisFn)0x80043D20;            // nwSMSetVisibility

    // One of the buttons: its two states' words (RULES has one), and per state a white and a grey
    // label (disabled: greyed, and the hand passes over it).
    struct Button {
        int id;
        const char* words[2];
        ButtonDebug* dbg;
        u8* task;          // the CSS the model belongs to
        u8* obj;           // our MuObject
        bool show, enabled, state;
        bool visible;      // what nwSMSetVisibility was last given
        bool pressed;
        bool lit;          // its highlight playing
        u8* label[2][2];   // our TEX0s (32-byte aligned): [state][grey]
        u32 fromWord;      // the label image the model had when it was made
    };
    Button s_buttons[2] = {
        {BUTTON_RULES, {"RULES", "RULES"}, &g_rulesButton, NULL, NULL, false, true, false, true, false, false,
         {{NULL, NULL}, {NULL, NULL}}, 0},
        {BUTTON_SIT_OUT, {"SIT OUT", "PLAY"}, &g_sitOutButton, NULL, NULL, false, true, false, true, false, false,
         {{NULL, NULL}, {NULL, NULL}}, 0},
    };
    HitFn s_origHit = NULL;
    DtorFn s_origDtor = NULL;

    u8* cssTask()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        return isPtr(task) ? (u8*)task : NULL;
    }

    u32 imageWord(const u8* tex) { return ((u32)(tex + 0x40) & 0x1FFFFFFF) >> 5; }

    // ------------------------------------------------------------------------------------------
    // The labels: ITEM's label texture copied, the word drawn in the border (Labels::renderInBox,
    // the game's font, as room_css.cpp's PUBLIC / FFA); in the grey ones the letters' white is
    // made grey (CMPR: inside the box every 4x4 sub-block is c0 black, c1 white, in 3-colour mode;
    // a grey c1 keeps that mode).
    const int BOX[4] = {4, 4, 44, 20};
    const u16 GREY_565 = 0x8C51;   // (140, 138, 140)

    void greyLetters(u8* tex)
    {
        int w = *(u16*)(tex + 0x1C), h = *(u16*)(tex + 0x1E);
        u8* data = tex + *(u32*)(tex + 0x10);
        int bpr = (w + 7) / 8, bpc = (h + 7) / 8;
        for (int by = 0; by < bpc; by++) {
            for (int bx = 0; bx < bpr; bx++) {
                for (int sb = 0; sb < 4; sb++) {
                    int sx = bx * 8 + (sb & 1) * 4, sy = by * 8 + (sb >> 1) * 4;
                    if (sx < BOX[0] || sy < BOX[1] || sx + 4 > BOX[2] || sy + 4 > BOX[3]) continue;
                    u8* e = data + (by * bpr + bx) * 32 + sb * 8;
                    if (*(u16*)(e + 2) == 0xFFFF) *(u16*)(e + 2) = GREY_565;
                }
            }
        }
        DCFlushRange(data, (u32)(bpr * bpc * 32));
    }

    bool makeLabels(Button& b, u8* mdl)
    {
        u8* cur = Labels::boundTexture(mdl, LABEL_TEX);
        if (!cur || *(u32*)(cur + 0x20) != 14 || *(u32*)(cur + 0x10) != 0x40) return false;   // CMPR
        int w = *(u16*)(cur + 0x1C), h = *(u16*)(cur + 0x1E);
        u32 bytes = (u32)(((w + 7) / 8) * ((h + 7) / 8) * 32);
        for (int st = 0; st < 2; st++) {
            for (int k = 0; k < 2; k++) {
                if (b.label[st][k]) continue;
                b.label[st][k] = Labels::copyBlock(0x40 + bytes);
                if (!b.label[st][k]) return false;
                memcpy(b.label[st][k], cur, 0x40 + bytes);
                Labels::renderInBox(b.label[st][k], b.words[st], BOX[0], BOX[1], BOX[2], BOX[3], 1);
                if (k == 1) greyLetters(b.label[st][k]);
            }
        }
        return true;
    }

    // The ScnMdl keeps its own copies of the materials' texture objects (GX words 0x94.. / 0xB4..
    // << 24 | image >> 5: in the ScnMdl and its material buffers +0x14C / +0x140, as room_css.cpp
    // showHeaderLabel); ours are pointed at the label for the state. The model's MDL0 is ITEM's
    // and stays as room_css.cpp has it.
    void pointLabel(Button& b, u8* mdl)
    {
        u8* scn = *(u8**)(b.obj + 0xC);
        u8* want = b.label[b.state ? 1 : 0][b.enabled ? 0 : 1];
        if (!isPtr((u32)scn) || !want) return;
        u8* bound = Labels::boundTexture(mdl, LABEL_TEX);
        u32 to = imageWord(want);
        // What the model may point at now: the game's label, or any of ours.
        u32 from[6] = {b.fromWord, bound ? imageWord(bound) : 0, 0, 0, 0, 0};
        int n = 2;
        for (int st = 0; st < 2; st++) {
            for (int k = 0; k < 2; k++) {
                if (b.label[st][k]) from[n++] = imageWord(b.label[st][k]);
            }
        }
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
                for (int k = 0; k < n; k++) {
                    if (a == from[k]) {
                        *(u32*)(p + off) = (w & 0xFF000000u) | to;
                        break;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    u8* make(Button& b, u8* task)
    {
        u8* item = *(u8**)(task + ITEM_OBJ);
        if (!isPtr((u32)item)) return NULL;
        u8* itemMdl = *(u8**)(item + 8);
        if (!isPtr((u32)itemMdl) || *(u32*)itemMdl != 0x4D444C30 /* MDL0 */) return NULL;
        if (!makeLabels(b, itemMdl)) return NULL;
        u8* bound = Labels::boundTexture(itemMdl, LABEL_TEX);
        b.fromWord = bound ? imageWord(bound) : 0;
        // As text+0xC154 makes ITEM: its archive (MuObject+4, m_resFile), its name and priority,
        // no separate texture file, the menu's heap; its animation, stopped.
        u8* obj = MU_CREATE(item + 4, MODEL, MODEL_PRIORITY, NULL, MODEL_HEAP);
        if (!isPtr((u32)obj)) return NULL;
        CHANGE_ANIM(obj, ANIM);
        void* anim = *(void**)(obj + 0x14);
        if (isPtr((u32)anim)) SET_UPDATE_RATE(anim, 0.0f);
        // Into the CSS's scene group (task+0x6C), at its end, as the task adds its models
        // (text+0x3A44: the group's vtable +0x34 insert(group, count +0xE4, MuObject+0x10)).
        u8* group = *(u8**)(task + 0x6C);
        u8* scn = *(u8**)(obj + 0x10);
        if (isPtr((u32)group) && isPtr((u32)scn)) {
            typedef bool (*InsertFn)(u8*, u32, u8*);
            ((InsertFn)(*(u32*)(*(u32*)group + 0x34)))(group, *(u32*)(group + 0xE4), scn);
        }
        b.dbg->made++;
        b.visible = true;
        b.lit = false;
        return obj;
    }

    void place(Button& b)
    {
        float* pos = (float*)(b.obj + 0x3C);
        const float* want = b.dbg->place;
        if (pos[0] == want[0] && pos[1] == want[1] && pos[2] == want[2]) return;
        float p[3] = {want[0], want[1], want[2]};
        SET_POS(b.obj, p);
    }

    // The colour animation's frame controller (MuObject+0x14 -> +0x18), or NULL.
    u8* clrAnim(u8* obj)
    {
        u8* anim = *(u8**)(obj + 0x14);
        if (!isPtr((u32)anim)) return NULL;
        u8* ctl = *(u8**)(anim + 0x18);
        return isPtr((u32)ctl) && isPtr(*(u32*)ctl) ? ctl : NULL;
    }

    // As text+0x9A2C does for ITEM and STAGE.
    void light(Button& b, bool on)
    {
        u8* ctl = clrAnim(b.obj);
        if (!ctl) return;
        typedef float (*GetFn)(u8*);
        typedef void (*SetFn)(u8*, float);
        u32 vt = *(u32*)ctl;
        float frame = ((GetFn)(*(u32*)(vt + 0x2C)))(ctl);
        if (on) {
            if (frame == 0.0f) ((SetFn)(*(u32*)(vt + 0x28)))(ctl, 1.0f);
        } else if (frame > 0.0f) {
            SET_FRAME_MATCOL(b.obj, 0.0f);
            ctl = clrAnim(b.obj);
            if (ctl) ((SetFn)(*(u32*)(*(u32*)ctl + 0x28)))(ctl, 0.0f);
        }
        b.lit = on;
    }

    bool handOnIt(u8* task, int id)
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

    bool live(const Button& b, u8* task)
    {
        return b.obj && task == b.task && b.show && b.enabled && *(u32*)(task + TASK_RULES_OPEN) == 0;
    }

    void tickButton(Button& b, bool show, bool enabled, bool state)
    {
        u8* task = cssTask();
        if (task != b.task) {
            b.task = task;
            b.obj = NULL;   // (the old one went with its task: hkTaskDtor)
            b.pressed = false;
        }
        b.show = show;
        b.enabled = enabled;
        b.state = state;
        if (!task) return;
        if (!b.obj && show) b.obj = make(b, task);
        if (!b.obj) return;
        u8* scn = *(u8**)(b.obj + 0xC);
        if (isPtr((u32)scn) && show != b.visible) {
            SET_VIS(scn, show);
            b.visible = show;
        }
        if (!show) {
            if (b.lit) light(b, false);
            b.pressed = false;
            return;
        }
        place(b);
        u8* item = *(u8**)(task + ITEM_OBJ);
        if (isPtr((u32)item)) pointLabel(b, *(u8**)(item + 8));
        GET_RECT(b.obj, b.dbg->rect, "colL", "colR");
        bool on = live(b, task) && handOnIt(task, b.id);
        if (on) b.dbg->hits++;
        if (on || b.lit) light(b, on);
    }

    bool takePressed(Button& b)
    {
        bool p = b.pressed;
        b.pressed = false;
        return p;
    }

    // ------------------------------------------------------------------------------------------
    // The hand's hit test (sel_char text+0xDC78): Brawl's buttons first, then ours.
    int hkHit(u8* task, float* rect, float* pos)
    {
        int hit = s_origHit(task, rect, pos);
        if (hit) return hit;
        for (int i = 0; i < 2; i++) {
            Button& b = s_buttons[i];
            if (!live(b, task)) continue;
            float r[4];
            GET_RECT(b.obj, r, "colL", "colR");
            if (r[0] <= pos[0] && r[1] >= pos[0] && r[3] <= pos[1] && r[2] >= pos[1]) {
                for (int k = 0; k < 4; k++) rect[k] = r[k];
                return b.id;
            }
        }
        return 0;
    }

    // muSelCharTask's destructor (text+0x434): ours first, as the task deletes its own (each
    // slot's MuObject: vtable at +0x5C, entry +8, with 1 = delete).
    void* hkTaskDtor(u8* task, int flags)
    {
        RoomCss::taskGone(task);   // (stamina's HP boxes: the areas' own STOCKS boxes they stood in for)
        for (int i = 0; i < 2; i++) {
            Button& b = s_buttons[i];
            if (task && task == b.task && b.obj) {
                // Deleted as the task deletes its own: the scene group (task+0x6C) is already
                // gone by now (taking ours out of it first jumped through its freed vtable).
                typedef void (*DelFn)(u8*, int);
                ((DelFn)(*(u32*)(*(u32*)(b.obj + 0x5C) + 8)))(b.obj, 1);
                b.obj = NULL;
                b.task = NULL;
                b.pressed = false;
            }
        }
        return s_origDtor(task, flags);
    }
}

extern "C" void pponline_rulesPressed(u8* task, int id)
{
    for (int i = 0; i < 2; i++) {
        Button& b = s_buttons[i];
        if (b.id == id && live(b, task)) {
            b.pressed = true;
            b.dbg->presses++;
        }
    }
}

namespace RulesButton {

    // buttonProcInAllArea, sel_char text+0x7AF0 (0x8068A3B4) `cmpwi r29,2`, reached when A was
    // pressed (r29 = the hit test's button, r31 = the task): BUTTON_RULES and BUTTON_SIT_OUT are
    // noted and go to the function's exit (+0x8334), as ITEM's press does (online_menu.cpp
    // cssSwitchButtons); left to the rest, an unknown button went on as A on nothing (the coin's
    // handling).
    __attribute__((naked)) void rulesButtonPress()
    {
        asm volatile(
            "cmpwi 29, 0x30\n\t"   // BUTTON_RULES
            "beq 2f\n\t"
            "cmpwi 29, 0x31\n\t"   // BUTTON_SIT_OUT
            "beq 2f\n\t"
            "cmpwi 29, 2\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0xA3B8\n\t"
            "mtctr 12\n\t"
            "bctr\n\t"
            "2:\n\t"
            "mr 3, 31\n\t"
            "mr 4, 29\n\t"
            "lis 12, pponline_rulesPressed@ha\n\t"
            "addi 12, 12, pponline_rulesPressed@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x8068\n\t"
            "ori 12, 12, 0xABF8\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    void tick(bool show, bool enabled) { tickButton(s_buttons[0], show, enabled, false); }
    bool pressed() { return takePressed(s_buttons[0]); }

    void forgetCopies()
    {
        for (int i = 0; i < 2; i++) {
            for (int st = 0; st < 2; st++) s_buttons[i].label[st][0] = s_buttons[i].label[st][1] = NULL;
        }
    }

    void install(CoreApi* api)
    {
        api->syReplaceFuncRel(0xDC78, reinterpret_cast<void*>(hkHit), (void**)&s_origHit, 10 /* sora_menu_sel_char */);
        api->syReplaceFuncRel(0x434, reinterpret_cast<void*>(hkTaskDtor), (void**)&s_origDtor, 10);
        api->sySimpleHookRel(0x7AF0, reinterpret_cast<void*>(rulesButtonPress), 10);
    }
}

namespace SitOutButton {
    void tick(bool show, bool enabled, bool sittingOut) { tickButton(s_buttons[1], show, enabled, sittingOut); }
    bool pressed() { return takePressed(s_buttons[1]); }
}
