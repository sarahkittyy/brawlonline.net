// Connect-code entry for DIRECT (design 5.4 screen 3), PROTOTYPE.
//
// Reuses Brawl's own name keypad (MuSelctChrNameEntry, resident module 16 sora_menu_name)
// exactly the way the character select opens it for a new name tag ("New entry"), which is
// also how Slippi does it (Melee's name-tag keyboard in "code mode", opened from the CSS):
//
//   sel_char+0x18E1C  openNameEntry(area): area+0x400 = area+0x1DC; helper(area+0x370).open(NULL, NULL, 5)
//   sel_char+0x1A348  setHandMode(hand, mode, port): mode 8 = the hand drives the keypad
//   sel_char+0x18E38  updateNameEntry(...) runs every frame while the hand is in mode 8
//
// We do the same two calls, but open the helper with our own buffer and a 7-character limit
// (module 16 text+0x5A8: open(this, const char* initial, char* extBuf, int maxChars)). The
// typed text is UTF-8 full-width ("ＡＢＣＤ１２３"); we read our buffer every frame and, when
// the keypad closes, turn it into "ABCD#123" (the '#' goes between the letters and digits,
// as Slippi pre-fills it; the keypad has no '#').
//
// Known limits of the prototype (docs/game-code.md): the key labels still show lower case
// and symbols, the underline stops at 5, OK also runs the CSS's own new-name path (reserved
// name check / name-tag list), and L/R history (mailbox 0xBE) is not wired.
#include <string.h>
#include "online_menu.h"
#include "ppom.h"

namespace CodeEntry {

    // Resident P+ addresses (sora_menu_sel_char .text 0x806828C4, sora_menu_name .text 0x8067406C)
    typedef void (*SetHandModeFn)(void* hand, int mode, int port);
    typedef void (*KeypadOpenFn)(void* helper, const char* initial, char* buf, int maxChars);
    static const u32 SEL_CHAR_TEXT = 0x806828C4;
    static const u32 NAME_TEXT = 0x8067406C;

    static char s_buf[64];          // the keypad's text (UTF-8), 7 chars x 3 bytes + NUL
    static bool s_active = false;
    static int s_port = 0;
    static char s_last[64];
    static int s_openTries = 0;     // frames left to see the hand enter mode 8

    static u8* cssArea(int port)
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!mgr) return NULL;
        u32 scene = *(u32*)(mgr + 4);
        if (!scene) return NULL;
        u32 task = *(u32*)(scene + 0x400);
        if (!task) return NULL;
        return *(u8**)(task + 0x44 + 4 * port);
    }

    static int decodeFullWidth(const char* in, char* out, int max)
    {
        int n = 0;
        const u8* p = (const u8*)in;
        while (*p && n < max) {
            u32 cp;
            if (p[0] < 0x80) { cp = p[0]; p += 1; }
            else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
            else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
                cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
            } else { p += 1; continue; }
            if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;  // full-width -> ASCII
            if (cp >= 'a' && cp <= 'z') cp -= 0x20;
            if ((cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '#') out[n++] = (char)cp;
        }
        out[n] = 0;
        return n;
    }

    // "ABCD123" -> "ABCD#123" (Slippi code shape: tag letters, '#', digits)
    static void toConnectCode(const char* raw, char* out)
    {
        int n = 0;
        bool hash = false;
        for (int i = 0; raw[i]; i++) hash |= raw[i] == '#';
        for (int i = 0; raw[i] && n < PPOM::CODE_LEN - 1; i++) {
            char c = raw[i];
            if (!hash && n > 0 && c >= '0' && c <= '9' && out[n - 1] >= 'A' && out[n - 1] <= 'Z') {
                out[n++] = '#';
                hash = true;
                if (n >= PPOM::CODE_LEN - 1) break;
            }
            out[n++] = c;
        }
        out[n] = 0;
    }

    void open()
    {
        u8* area = cssArea(s_port);
        if (!area) return;
        void* hand = *(void**)(area + 0x1A8);
        memset(s_buf, 0, sizeof(s_buf));
        *(u32*)(area + 0x400) = *(u32*)(area + 0x1DC);
        ((KeypadOpenFn)(NAME_TEXT + 0x5A8))(area + 0x370, NULL, s_buf, 7);
        ((SetHandModeFn)(SEL_CHAR_TEXT + 0x1A348))(hand, 8, s_port);
        s_active = true;
        s_openTries = 30;
        s_last[0] = 0;
        PPOM::g_block.debug.scratch[4] = 1;
    }

    // Called every frame while the online CSS is up.
    void tick()
    {
        if (!s_active) return;
        u8* area = cssArea(s_port);
        if (!area) { s_active = false; return; }
        u8* hand = *(u8**)(area + 0x1A8);
        int mode = hand ? *(int*)(hand + 0xA4) : 0;
        if (s_buf[0]) strcpy(s_last, s_buf);
        PPOM::g_block.debug.scratch[5] = (u32)mode;
        if (s_openTries > 0) {
            // The hand can refuse the mode change for a few frames (seen right after the
            // START press); ask again until it shows the keypad.
            s_openTries--;
            if (mode == 8) {
                s_openTries = 0;
            } else {
                if (hand) ((SetHandModeFn)(SEL_CHAR_TEXT + 0x1A348))(hand, 8, s_port);
                if (s_openTries == 0) s_active = false;
                return;
            }
        }
        if (mode != 8) {
            s_active = false;
            PPOM::g_block.debug.scratch[4] = 2;
            char raw[16], code[PPOM::CODE_LEN + 1];
            decodeFullWidth(s_last, raw, 8);
            if (raw[0]) {
                toConnectCode(raw, code);
                OnlineMenu::codeEntered(code);
            }
        }
    }

    bool active() { return s_active; }
}
