// PLAY ONLINE brought back on a main menu that took it off (restore_online.h; variant.h
// PPON_RESTORE_ONLINE: in the plugins built for such a build, empty in the others).
#include "variant.h"
#if PPON_RESTORE_ONLINE
#include <string.h>
#include <types.h>
#include "restore_online.h"

// The top page's ONLINE button on a build that took it off. muProcTop's six buttons are the
// first six MuObjects of its menu (page+0x64C): VERSUS, SOLO, ONLINE, VAULT, OPTIONS, DATA
// (MenMainIcon0001..0006_TopN), and the cursor goes between them by sora_menu_main's link
// table (.data+0x1DB0: per button up, down, left, right, each {u16 button, u8 wrap, pad}). Each
// button is placed by bone rotN in its six animations (<model>__0..__5: __0 / __2 enter, __1 /
// __3 leave, __4 chosen, __5 at rest). REX 0½ took ONLINE off its main menu: its animations put
// it at y -1000 and VAULT's and DATA's links skip it (VAULT's left is DATA, DATA's right VAULT),
// so once the cursor left it (the boot's ONLINE page, B) the way back was a new boot.
// A top page whose ONLINE rests off the screen gets it back at the start of the bottom row:
// every key of rotN's translation in its six animations moved by the same amount (its slides
// keep their shape) to VAULT's row, left of VAULT by 0.85 of the row's spacing (VAULT to
// OPTIONS: ONLINE's circle is the widest button), and the row (ONLINE to DATA) moved right as
// one to be centred again; then each button its own links name links back to it the other
// way (REX: VERSUS down and up, VAULT left, DATA right, as on P+). P+'s ONLINE rests on the
// screen and every link reaches it: nothing changes there.
namespace RestoreOnline {
    static bool menuPtr(u32 p) { return p >= 0x80000000 && p < 0x94000000; }

    const int ONLINE = 2, VAULT = 3, OPTIONS = 4, DATA = 5;
    const float OFF_SCREEN = 100.0f;   // rotN's units: the top page's buttons are within +-50

    // A brres ResDic's entry named `name` (exactly, or as a prefix with `prefix`): its data.
    static u8* dicFind(u8* dic, const char* name, bool prefix, int* from)
    {
        u32 n = *(u32*)(dic + 4);
        int len = (int)strlen(name);
        for (u32 k = from ? (u32)*from : 1; k <= n && k < 512; k++) {
            u8* e = dic + 8 + 16 * k;
            const char* nm = (const char*)(dic + *(s32*)(e + 8));
            if (prefix ? strncmp(nm, name, len) == 0 : strcmp(nm, name) == 0) {
                if (from) *from = (int)k + 1;
                return dic + *(s32*)(e + 0xC);
            }
        }
        return NULL;
    }

    // The animations' group of the brres a top-page button's MuObject was made from (+4).
    static u8* chrGroup(u8* obj)
    {
        u8* file = menuPtr((u32)obj) ? *(u8**)(obj + 4) : NULL;
        if (!menuPtr((u32)file) || *(u32*)file != 0x62726573 /* bres */) return NULL;
        u8* root = file + *(u16*)(file + 0xC);
        if (*(u32*)root != 0x726F6F74 /* root */) return NULL;
        return dicFind(root + 8, "AnmChr(NW4R)", false, NULL);
    }

    static const char* modelName(u8* obj)
    {
        u8* mdl = *(u8**)(obj + 8);
        if (!menuPtr((u32)mdl) || *(u32*)mdl != 0x4D444C30 /* MDL0 */) return NULL;
        u32 sections = *(u32*)(mdl + 8) >= 10 ? 14 : 11;
        return (const char*)(mdl + *(s32*)(mdl + 0x10 + 4 * sections));
    }

    // rotN's translation word for `axis` (0 x, 1 y) in a CHR0, its entry and format; NULL when
    // rotN has no translation of its own per axis.
    static u8* transWord(u8* chr, u8** entry, int* fmt)
    {
        u8* bones = chr + *(s32*)(chr + 0x10);
        u8* e = dicFind(bones, "rotN", false, NULL);
        if (!e) return NULL;
        u32 code = *(u32*)(e + 4);
        if (!(code >> 24 & 1) || (code >> 6 & 1)) return NULL;   // no translation / one value for all axes
        u8* p = e + 8;
        if (code >> 22 & 1) p += (code >> 4 & 1) ? 4 : 12;       // scale
        if (code >> 23 & 1) p += (code >> 5 & 1) ? 4 : 12;       // rotation
        *entry = e;
        *fmt = (int)(code >> 30);
        return p;   // x, y, z follow; fixed axes (bits 19..21) are floats, the others key offsets
    }

    // rotN's resting place (<model>__5, the first key if keyed).
    static bool rest(u8* obj, float* x, float* y)
    {
        u8* grp = chrGroup(obj);
        const char* name = grp ? modelName(obj) : NULL;
        if (!name) return false;
        char want[48];
        int n = (int)strlen(name);
        if (n > 40) return false;
        memcpy(want, name, n);
        memcpy(want + n, "__5", 4);
        u8* chr = dicFind(grp, want, false, NULL);
        u8* e;
        int fmt;
        u8* t = chr ? transWord(chr, &e, &fmt) : NULL;
        if (!t) return false;
        u32 code = *(u32*)(e + 4);
        float* out[2] = {x, y};
        for (int a = 0; a < 2; a++) {
            if (code >> (19 + a) & 1) {
                *out[a] = *(float*)(t + 4 * a);
                continue;
            }
            u8* k = e + *(s32*)(t + 4 * a);
            if (fmt == 3) *out[a] = *(float*)(k + 8 + 4);
            else return false;   // (quantised keys at rest: not seen)
        }
        return true;
    }

    // Move every key of rotN's x / y translation in one CHR0 by (dx, dy).
    static void shift(u8* chr, float dx, float dy)
    {
        u8* e;
        int fmt;
        u8* t = transWord(chr, &e, &fmt);
        if (!t) return;
        u32 code = *(u32*)(e + 4);
        float d[2] = {dx, dy};
        for (int a = 0; a < 2; a++) {
            if (code >> (19 + a) & 1) {
                *(float*)(t + 4 * a) += d[a];
                continue;
            }
            u8* k = e + *(s32*)(t + 4 * a);
            u16 n = *(u16*)k;
            if (fmt == 3) {
                for (u16 i = 0; i < n && i < 256; i++) *(float*)(k + 8 + 12 * i + 4) += d[a];
            } else if (fmt == 1 || fmt == 2) {
                *(float*)(k + 0xC) += d[a];   // the keys are base + step * value
            }
        }
    }

    // Move a button: rotN's translation in all its animations (<model>__0 .. __5).
    static void shiftModel(u8* obj, float dx, float dy)
    {
        u8* grp = chrGroup(obj);
        const char* name = grp ? modelName(obj) : NULL;
        if (!name) return;
        int from = 1;
        for (int i = 0; i < 8; i++) {
            u8* chr = dicFind(grp, name, true, &from);
            if (!chr) break;
            shift(chr, dx, dy);
        }
    }

    static u32 linkTable()
    {
        u32 m = *(u32*)0x800030C8;
        for (int n = 0; menuPtr(m) && n < 64; n++) {
            if (*(u32*)m == 2 /* sora_menu_main */) {
                u32 sec = *(u32*)(m + 0x10);
                u32 data = menuPtr(sec) && *(u32*)(m + 0xC) > 5 ? (*(u32*)(sec + 8 * 5) & ~1u) : 0;
                return data ? data + 0x1DB0 : 0;
            }
            m = *(u32*)(m + 4);
        }
        return 0;
    }

    // Every frame of the top page (a few name lookups): the menu loads its archive and module
    // again after a match, often at the same addresses. Once done, both checks find it done.
    void fix(u8* page)
    {
        u8** objs = *(u8***)(page + 0x64C);
        if (!menuPtr((u32)objs)) return;
        for (int b = ONLINE; b <= DATA; b++) {   // Brawl's buttons, in its order
            static const char NAME[] = "MenMainIcon000?_TopN";
            const char* n = chrGroup(objs[b]) ? modelName(objs[b]) : NULL;
            if (!n || strncmp(n, NAME, 14) != 0 || n[14] != '1' + b || strcmp(n + 15, NAME + 15) != 0) return;
        }
        u8* online = objs[ONLINE];
        float ox, oy, vx, vy, px, py, dx, dy;
        if (!rest(online, &ox, &oy) || !rest(objs[VAULT], &vx, &vy) || !rest(objs[OPTIONS], &px, &py) ||
            !rest(objs[DATA], &dx, &dy))
            return;
        if (oy > OFF_SCREEN || oy < -OFF_SCREEN) {
            float tx = vx - 0.85f * (px - vx), ty = vy;
            float centre = -(tx + dx) * 0.5f;   // the row's ends either side of the middle
            shiftModel(online, tx + centre - ox, ty - oy);
            for (int b = VAULT; b <= DATA; b++) shiftModel(objs[b], centre, 0.0f);
            if (!rest(online, &ox, &oy) || oy > OFF_SCREEN || oy < -OFF_SCREEN) return;
        }
        u32 links = linkTable();
        if (!links) return;
        u8* L = (u8*)links;   // button b, direction d (up, down, left, right): L + 16 b + 4 d
        for (int i = 0; i < 24; i++) {   // (a build whose table is elsewhere: left alone)
            if (*(u16*)(L + 4 * i) >= 6 || L[4 * i + 2] > 1 || L[4 * i + 3]) return;
        }
        for (int b = 0; b < 6; b++) {
            for (int d = 0; d < 4; d++) {
                if (b != ONLINE && *(u16*)(L + 16 * b + 4 * d) == ONLINE) return;   // reachable
            }
        }
        for (int d = 0; d < 4; d++) {   // each button it names links back the other way
            u8* mine = L + 16 * ONLINE + 4 * d;
            u16 other = *(u16*)mine;
            if (other >= 6 || other == ONLINE) continue;
            u8* back = L + 16 * other + 4 * (d ^ 1);   // up <-> down, left <-> right
            *(u16*)back = ONLINE;
            back[2] = mine[2];   // wrapping as its own link
        }
    }
}
#endif
