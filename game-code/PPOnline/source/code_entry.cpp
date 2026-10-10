// Connect-code entry for DIRECT and TEAMS (design 5.4 screen 3).
//
// Slippi reuses Melee's name-tag keyboard in "connect code mode", opened from the CSS with
// START: forced English layout, 8 characters including '#', a full-width '#' key, Confirm
// starts the search (TextEntryScreen/*, Allow8Characters.asm). We do the same with Brawl's
// name keypad (MuSelctChrNameEntry, resident module 16 sora_menu_name), opened exactly the way
// the character select opens it for a new name tag ("New entry"):
//
//   sel_char+0x18E1C  openNameEntry(area): area+0x400 = area+0x1DC; helper(area+0x370).open(NULL, NULL, 5)
//   sel_char+0x1A348  setHandMode(hand, mode, port): mode 8 = the hand drives the keypad
//   sel_char+0x18E38  updateNameEntry(...) calls helper.update(pad, out, allowRandom, se) each
//                     frame: 0 = OK (then writes `out` to the player's name tag), 1 = cancel,
//                     2 = editing
//
// While the keypad is ours (connect-code mode):
//   - it is opened with our own buffer and Slippi's 8-character limit;
//   - only the alphabet and digit pages exist (helper+0x24 page list, +0x38 count, +0x3C
//     current; restored on close), as Slippi forces the English layout;
//   - a code is letters, '#', digits (Slippi's connect codes; our server: 2-4 letters, 1-4
//     digits, 8 characters in all), and the keypad follows it (the user's design, 2026-10-08):
//     the alphabet page's first key is '#' (its "@()^:;" label hidden, a "＃" printed on it in
//     the game's font); typing '#' turns to the digits page at once, and erasing the '#' turns
//     back to the letters. The page key is not used. A key that cannot be valid where the
//     cursor is (a letter after four letters, '#' before two letters or a second time, a digit
//     after four digits, the symbol keys) plays the error sound; the symbol keys' labels are
//     hidden. A digit is committed at once (no multi-tap on single-character keys, so "11"
//     needs no move off the key and back). The keys' strings, labels and the page list are
//     shared with every other keypad: swapped in on open, restored on close;
//   - the empty field shows "PLYR#123" in the suggestion grey as a placeholder; a recent-code
//     suggestion takes its place;
//   - START confirms (Slippi: "Start = A on Confirm"); Confirm with an empty field plays the
//     error sound and stays (OnConfirmButtonHandler.asm:30-42); B deletes, and on an empty
//     field goes back to the CSS (vanilla, also Slippi);
//   - the Random button is off, and the text field's font narrows to fit 8 characters;
//   - our wrapper of the CSS's helper.update call gives the widget our own `out` and turns OK
//     into "cancel" for the CSS, so the CSS never writes the code into the player's name tag
//     (and never copies 8 characters into its 5-character stack buffer).
//
// Recent codes (Slippi TextEntryScreen/AutoComplete.s and friends, 0xBE FETCH_CODE_SUGGESTION):
// see the "Recent codes" block below.
//
// Room-code mode (Join Room, docs/design/rooms.md #4, #5): a room code is 4 characters. The
// server's codes are letters of BCDFGHJKLMNPQRSTVWXZ only, but the keypad takes every letter and
// digit it shows (both pages, the keys as printed: players found keys that show letters and then
// refuse them confusing, staging feedback 2026-10-10); a code no room has comes back "Room not
// found." from Dolphin (Rooms.cpp, NormalizeRoomCode). The symbol keys (the '#' and the
// punctuation keys; their labels hidden), a fifth character and OK before four are refused with
// the error sound; no recent codes (room codes are throwaway, rooms.md §2); the placeholder is
// "KFQB".
//
// The keypad's state (MuSelctChrNameEntry, 0x94 bytes, as far as we use it):
//   +0x00 active  +0x04 text buffer (UTF-8)  +0x08 max characters
//   +0x24 page list, +0x38 page count, +0x3C current page
//   +0x40 highlighted key (0 erase, 1-11 characters, 0xC page, 0xD OK)
//   +0x44 keypad model (key highlight = material frame key+1), +0x48.. page models,
//   +0x5C selector model, +0x60 underline model (frame = cursor+1), +0x64 text MuMsg,
//   +0x70 its window, +0x78 current page model
//   +0x7C last character committed (0 = multi-tap: the same key again cycles it)
//   +0x80 key UP from OK goes back to, +0x84 cursor (character index), +0x88 no-empty flag,
//   +0x8C hold-B counter (35 frames clears the text), +0x90 pad port
// update (text+0x998) = input decoder (+0x1670: A/B/START/held B -> action) + action (+0xE18:
// 1 OK, 2 cancel, 3 character key with multi-tap, 4 erase, 5 page, 6 clear) + cursor (+0xBBC:
// d-pad/stick repeat bits, START jumps to OK). Every edit reprints the field with
// MuMsg::printf(+0x64, +0x70, text).
#include <mu/mu_msg.h>
#include <string.h>
#include "online_menu.h"
#include "ppom.h"

namespace CodeEntry {

    // Resident P+ addresses (sora_menu_sel_char .text 0x806828C4, sora_menu_name .text 0x8067406C)
    typedef void (*SetHandModeFn)(void* hand, int mode, int port);
    typedef void (*KeypadOpenFn)(void* helper, const char* initial, char* buf, int maxChars);
    typedef int (*KeypadUpdateFn)(u8* helper, void* pad, char* out, int allowRandom, int se);
    static const u32 SEL_CHAR_TEXT = 0x806828C4;
    static const u32 NAME_TEXT = 0x8067406C;

    const int CODE_CHARS = 8;           // Slippi: CONNECT_CODE_LENGTH 8, '#' included

    static char s_buf[64];              // the keypad's text (UTF-8 full-width, 8 x 3 bytes + NUL)
    static char s_out[64];              // what the game would copy to the name tag
    static bool s_active = false;
    static bool s_room = false;         // room-code mode (Join Room)
    static bool s_confirmed = false;
    static bool s_startPressed = false;   // START went down this frame (the game no longer sees it)
    static int s_port = 0;
    static int s_openTries = 0;         // frames left to see the hand enter mode 8
    static u8* s_helper = NULL;
    static const KeypadUpdateFn s_origUpdate = (KeypadUpdateFn)(NAME_TEXT + 0x998);

    // Key table: 5 pages x 11 keys of {isDakuten, utf8 characters} (sora_menu_name .data).
    struct Key {
        u32 isDakuten;
        const char* chars;
    };
    static Key* const KEYS = (Key*)0x8067BEB0;
    const int PAGE_ALPHA = 2;
    const int PAGE_DIGITS = 3;
    static Key s_savedKeys[11];
    static u32 s_savedPages[6];         // helper+0x24 .. +0x3C

    // Alphabet page, upper case only; key 1 (the symbols key) is the '#'.
    static const char* const ALPHA_KEYS[11] = {
        "\xEF\xBC\x83",                                  // ＃
        "\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3",          // ＡＢＣ
        "\xEF\xBC\xA4\xEF\xBC\xA5\xEF\xBC\xA6",          // ＤＥＦ
        "\xEF\xBC\xA7\xEF\xBC\xA8\xEF\xBC\xA9",          // ＧＨＩ
        "\xEF\xBC\xAA\xEF\xBC\xAB\xEF\xBC\xAC",          // ＪＫＬ
        "\xEF\xBC\xAD\xEF\xBC\xAE\xEF\xBC\xAF",          // ＭＮＯ
        "\xEF\xBC\xB0\xEF\xBC\xB1\xEF\xBC\xB2\xEF\xBC\xB3",  // ＰＱＲＳ
        "\xEF\xBC\xB4\xEF\xBC\xB5\xEF\xBC\xB6",          // ＴＵＶ
        "\xEF\xBC\xB7\xEF\xBC\xB8\xEF\xBC\xB9\xEF\xBC\xBA",  // ＷＸＹＺ
        NULL,                                             // !?&%$  (disabled, unchanged)
        NULL,                                             // .,/~   (disabled, unchanged)
    };
    const int ROOM_CODE_CHARS = 4;

    // helper+0x40 is the highlighted key: 0 delete, 1-11 character keys, 0xC page, 0xD OK.
    // Page index (+0x3C) 0 = letters, 1 = digits (our page list).
    const int KEY_ERASE = 0, KEY_HASH = 1, KEY_PAGE = 0xC, KEY_OK = 0xD;
    const int MIN_LETTERS = 2, MAX_LETTERS = 4, MAX_DIGITS = 4;   // server/crates/common codes.rs
    static int s_lastKey = -1;          // the last character key that typed (multi-tap)

    // What has been typed: letters before the '#', whether there is one, digits after it.
    struct CodeShape {
        int len, letters, digits;
        bool hash;
    };
    static int decodeFullWidth(const char* in, char* out, int max);
    static CodeShape shapeOf(const char* buf)
    {
        char in[16];
        CodeShape c;
        c.len = decodeFullWidth(buf, in, 15);
        c.letters = c.digits = 0;
        c.hash = false;
        for (int i = 0; i < c.len; i++) {
            if (in[i] == '#') c.hash = true;
            else if (c.hash) c.digits++;
            else c.letters++;
        }
        return c;
    }

    // May `key` be pressed on page `page` with the text as it is? `cycles`: the press only
    // cycles the last character (multi-tap on the key that typed it), it adds nothing.
    static bool keyAllowed(int page, int key, const CodeShape& c, bool cycles)
    {
        if (key == KEY_ERASE) return true;
        if (s_room) {
            if (key == KEY_OK) return c.len == ROOM_CODE_CHARS && !c.hash;
            if (key == KEY_PAGE) return true;
            if (page == PAGE_ALPHA && (key < 2 || key > 9)) return false;   // '#', !?&%$, .,/~
            if (page == PAGE_DIGITS && (key < 1 || key > 11 || key == 10)) return false;   // -+x=
            if (page != PAGE_ALPHA && page != PAGE_DIGITS) return false;
            return cycles || c.len < ROOM_CODE_CHARS;
        }
        if (key == KEY_OK) return c.len > 0;
        if (key == KEY_PAGE || key < 1 || key > 11) return false;
        if (page == PAGE_ALPHA) {
            if (key == 10 || key == 11) return false;   // !?&%$ and .,/~
            if (key == KEY_HASH) return !c.hash && c.letters >= MIN_LETTERS && c.len < CODE_CHARS;
            if (cycles) return true;
            return !c.hash && c.letters < MAX_LETTERS && c.len < CODE_CHARS;
        }
        if (page == PAGE_DIGITS) {
            if (key == 10) return false;                // -+x=
            return c.hash && c.digits < MAX_DIGITS && c.len < CODE_CHARS;
        }
        return false;
    }

    static void playSE(int id)
    {
        typedef void (*PlaySEFn)(void*, int, int, int, int, int);
        void* snd = *(void**)0x805A01D0;   // g_sndSystem
        if (snd) ((PlaySEFn)0x800742b0)(snd, id, -1, 0, 0, -1);
    }

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

    static void patchKeys(u8* helper)
    {
        const char* const* keys = ALPHA_KEYS;
        for (int i = 0; i < 11; i++) {
            s_savedKeys[i] = KEYS[PAGE_ALPHA * 11 + i];
            if (keys[i]) KEYS[PAGE_ALPHA * 11 + i].chars = keys[i];
        }
        memcpy(s_savedPages, helper + 0x24, sizeof(s_savedPages));
        u32* pages = (u32*)(helper + 0x24);
        pages[0] = PAGE_ALPHA;
        pages[1] = PAGE_DIGITS;
        *(u32*)(helper + 0x38) = 2;    // page count
        *(u32*)(helper + 0x3C) = 0;    // current page index
    }

    static void restoreKeys(u8* helper)
    {
        for (int i = 0; i < 11; i++) KEYS[PAGE_ALPHA * 11 + i] = s_savedKeys[i];
        if (helper) memcpy(helper + 0x24, s_savedPages, sizeof(s_savedPages));
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

    // ----------------------------------------------------------------------------------------
    // Recent codes (Slippi: TextEntryScreen/AutoComplete.s, NameEntryThinkOneShot.asm,
    // OnEnterText.asm, OnBPressAutoComplete.asm, OnLPress/OnRPress.asm, CheckTriggersAndZ.asm,
    // HandleAutocompleteText.asm, OnConfirmButtonHandler.asm; Dolphin handleNameEntryLoad).
    //
    //   - opened, a character typed, cycled (multi-tap) or deleted: FETCH_CODE_SUGGESTION
    //     "reset" with the new text: the newest recent code that starts with it;
    //   - L / R: "older" / "newer" from the current suggestion's index. Brawl's keypad does not
    //     use L, R or Z at all (its input decoder, MuSelctChrNameEntry text+0x1670, reads A, B,
    //     START and held B; its cursor, +0xBBC, reads the d-pad/stick repeat bits and START),
    //     so these are added rather than replaced; they are taken out of the pad copy the
    //     keypad gets;
    //   - Z: the suggestion becomes the text (success sound) and the highlight moves to OK, as
    //     Slippi moves it to Confirm; no suggestion (or nothing left to complete): error sound;
    //   - the rest of the suggestion is drawn after the typed characters in the field's own
    //     window, in Slippi's grey (0x8E9196, HandleAutocompleteText.asm): beginPrint, the typed
    //     text in the window's colour, then the Message's colour commands and the remainder;
    //   - the keypad's buffer only ever holds the typed characters, so Confirm sends exactly
    //     those (Slippi truncates the suggestion, OnConfirmButtonHandler.asm), and B deletes the
    //     last typed character (or leaves on an empty field) as before.
    // One 0xBE in flight at a time; what is asked meanwhile waits in a small queue and goes out
    // when the answer arrives (with the then-current text and index).
    typedef void (*MsgBeginPrintFn)(MuMsg*, u32);
    typedef void (*MessagePrintfFn)(void* message, const char* fmt, ...);
    typedef void (*MessageColorFn)(void* message, const u8* rgba);
    typedef void (*ObjFrameFn)(void* obj, float frame);
    static const MsgBeginPrintFn s_beginPrint = (MsgBeginPrintFn)0x800B8EE8;   // MuMsg::beginPrint
    static const MessagePrintfFn s_msgPrintf = (MessagePrintfFn)0x80069D40;    // Message::printf
    static const MessageColorFn s_msgColorTop = (MessageColorFn)0x8006A170;    // colour command 0xC
    static const MessageColorFn s_msgColorBottom = (MessageColorFn)0x8006A28C; // colour command 0x4
    static const ObjFrameFn s_setFrameMatCol = (ObjFrameFn)0x800B7A18;         // MuObject::setFrameMatCol
    static const ObjFrameFn s_setFrame = (ObjFrameFn)0x800B7798;               // MuObject frame (selector, underline)
    static const u8 SUGGESTION_GREY[4] = {0x8E, 0x91, 0x96, 0xFF};
    static const char* const PLACEHOLDER = "PLYR#123";   // the empty field's placeholder
    static const char* const ROOM_PLACEHOLDER = "KFQB";   // room-code mode's
    // The placeholder: the suggestion's grey at half opacity (a hint, not something Z takes).
    static const u8 PLACEHOLDER_GREY[4] = {0x8E, 0x91, 0x96, 0x80};

    static char s_sugCode[PPOM::CODE_LEN + 1];   // last answer (ASCII), valid if s_sugFound
    static bool s_sugFound = false;
    static u32 s_sugIndex = 0;
    static u32 s_sugSeq = 0;            // the 0xBE in flight (0 = none)
    static u32 s_sugIgnore = 0;         // in flight from an earlier keypad: drop its payload
    static u32 s_sugWait = 0;
    static u8 s_sugQueue[8];
    static int s_sugQueued = 0;
    static char s_lastBuf[64];          // the keypad's text when we last looked
    static char s_drawn[64];            // the field as we last drew it ("\x01" = must redraw)

    static int utf8Chars(const char* s)
    {
        int n = 0;
        for (; *s; s++) {
            if (((u8)*s & 0xC0) != 0x80) n++;
        }
        return n;
    }

    // "AB#1" -> full-width UTF-8, the way the keypad's keys type it.
    static void toFullWidth(const char* in, char* out)
    {
        int o = 0;
        for (; *in; in++) {
            u32 x = (u8)*in - 0x20;   // U+FF00 + x
            out[o++] = (char)0xEF;
            out[o++] = (char)(0xBC + (x >> 6));
            out[o++] = (char)(0x80 + (x & 0x3F));
        }
        out[o] = 0;
    }

    static bool startsWith(const char* code, const char* prefix, int n)
    {
        for (int i = 0; i < n; i++) {
            char c = code[i];
            if (c >= 'a' && c <= 'z') c -= 0x20;
            if (c != prefix[i]) return false;
        }
        return true;
    }

    static void sugSend(u8 scroll)
    {
        PPOM::CodeSuggestionRequest req;
        memset(&req, 0, sizeof(req));
        char in[16];
        int n = decodeFullWidth(s_buf, in, CODE_CHARS);
        req.mode = (u8)OnlineMenu::currentMode();
        req.scroll = scroll;
        req.inputLen = (u8)n;
        req.index = s_sugIndex;
        PPOM::asciiToU16(req.input, in, PPOM::CODE_LEN);
        s_sugSeq = PPOM::post(PPOM::CMD_FETCH_CODE_SUGGESTION, &req, sizeof(req));
        s_sugWait = 0;
        PPOM::g_block.debug.scratch[0]++;   // 0xBE requests sent
    }

    static void sugRequest(u8 scroll)
    {
        if (s_room) return;   // no recent room codes
        if (!s_sugSeq) {
            sugSend(scroll);
            return;
        }
        if (scroll == PPOM::SCROLL_RESET) s_sugQueued = 0;   // a reset supersedes queued scrolls
        if (s_sugQueued < (int)sizeof(s_sugQueue)) s_sugQueue[s_sugQueued++] = scroll;
    }

    void onSuggestion(const PPOM::Response& r)
    {
        if (!s_sugSeq || r.seq != s_sugSeq) return;
        u32 seq = s_sugSeq;
        s_sugSeq = 0;
        if (seq != s_sugIgnore && s_active) {
            if (r.status == 0) {
                const PPOM::CodeSuggestion& c = *(const PPOM::CodeSuggestion*)r.payload;
                s_sugIndex = c.index;
                s_sugFound = c.found != 0;
                PPOM::u16ToAscii(s_sugCode, c.code, sizeof(s_sugCode));
            } else {
                s_sugFound = false;   // a Dolphin without recent codes: no suggestions
            }
            // scratch[2]: Z accepts << 24 | found << 16 | index (harness/tests)
            PPOM::g_block.debug.scratch[2] = (PPOM::g_block.debug.scratch[2] & 0xFF000000u) |
                                             (s_sugFound ? 0x10000u : 0) | (s_sugIndex & 0xFFFF);
        }
        s_sugIgnore = 0;
        if (s_active && s_sugQueued > 0) {
            u8 next = s_sugQueue[0];
            for (int i = 1; i < s_sugQueued; i++) s_sugQueue[i - 1] = s_sugQueue[i];
            s_sugQueued--;
            sugSend(next);
        } else {
            s_sugQueued = 0;
        }
    }

    // The suggestion's characters after the typed ones ("" if there is none that fits).
    static const char* suggestionTail()
    {
        char in[16];
        int n = decodeFullWidth(s_buf, in, CODE_CHARS);
        if (!s_sugFound || !startsWith(s_sugCode, in, n) || (int)strlen(s_sugCode) <= n) return "";
        return s_sugCode + n;
    }

    // The text field (helper+0x64 MuMsg, window +0x70): typed text in the window's colour, then
    // the rest of the suggestion in grey. Only when it changed (the keypad prints the plain text
    // itself whenever its text changes; s_drawn is reset then).
    static void drawField(u8* helper)
    {
        const char* tail = suggestionTail();
        // The empty field without a suggestion shows the placeholder, in the same grey.
        if (!s_buf[0] && !*tail) tail = s_room ? ROOM_PLACEHOLDER : PLACEHOLDER;
        char want[64];
        int n = 0;
        for (const char* p = s_buf; *p && n < 40; p++) want[n++] = *p;
        want[n++] = '|';
        for (const char* p = tail; *p && n < 60; p++) want[n++] = *p;
        want[n] = 0;
        if (strcmp(want, s_drawn) == 0) return;
        strcpy(s_drawn, want);
        MuMsg* msg = *(MuMsg**)(helper + 0x64);
        if (!msg) return;
        u32 win = *(u32*)(helper + 0x70);
        s_beginPrint(msg, win);
        void* m = msg->m_message;
        if (s_buf[0]) s_msgPrintf(m, "%s", s_buf);
        if (*tail) {
            char fw[40];
            toFullWidth(tail, fw);
            const u8* grey = (tail == PLACEHOLDER || tail == ROOM_PLACEHOLDER) ? PLACEHOLDER_GREY : SUGGESTION_GREY;
            s_msgColorTop(m, grey);
            s_msgColorBottom(m, grey);
            s_msgPrintf(m, "%s", fw);
        }
    }

    // Highlight a key the way the keypad's cursor code does (text+0xBBC): the key's material
    // frame on the keypad and page models, the selector model, and +0x80 (where UP from OK
    // goes back to; START sets 0xC there too).
    static void selectKey(u8* helper, int key)
    {
        float f = (float)(key + 1);
        void* keypad = *(void**)(helper + 0x44);
        void* page = *(void**)(helper + 0x78);
        void* selector = *(void**)(helper + 0x5C);
        if (keypad) s_setFrameMatCol(keypad, f);
        if (page) s_setFrameMatCol(page, f);
        if (selector) s_setFrame(selector, f);
        *(int*)(helper + 0x80) = 0xC;
        *(int*)(helper + 0x40) = key;
    }

    // Replace the text the way the keypad does after an edit: last character committed
    // (+0x7C = 1, no multi-tap pending), cursor (+0x84) and its underline (+0x60) after the text.
    static void commitText(u8* helper);
    static void setText(u8* helper, const char* utf8)
    {
        strncpy(s_buf, utf8, sizeof(s_buf) - 1);
        s_buf[sizeof(s_buf) - 1] = 0;
        commitText(helper);
    }

    // The text as it is, committed: no multi-tap pending, the cursor after it.
    static void commitText(u8* helper)
    {
        *(u8*)(helper + 0x7C) = 1;
        int max = *(int*)(helper + 8);
        int cur = utf8Chars(s_buf);
        if (cur > max - 1) cur = max - 1;
        if (cur < 0) cur = 0;
        void* underline = *(void**)(helper + 0x60);
        if (underline) s_setFrame(underline, (float)(cur + 1));
        *(int*)(helper + 0x84) = cur;
        strcpy(s_lastBuf, s_buf);
        s_drawn[0] = 1;
        s_drawn[1] = 0;
    }

    // Z (CheckTriggersAndZ.asm): take the suggestion.
    static void acceptSuggestion(u8* helper)
    {
        const char* tail = suggestionTail();
        if (!*tail) {
            playSE(3);
            return;
        }
        playSE(1);
        char fw[40];
        toFullWidth(s_sugCode, fw);
        setText(helper, fw);
        selectKey(helper, 0xD);
        PPOM::g_block.debug.scratch[2] += 0x1000000u;
    }

    // ----------------------------------------------------------------------------------------
    // Pages. The keypad's own page key (action 5, text+0xE18 case 5) does: next index (+0x3C),
    // take the current page model (+0x78) off the keypad's scene group (+0x6C, vtable +0x3C with
    // the model's ScnMdlSimple at MuObject+0x10), put the new one on its node "cpos<n>" (+0x74 + 1)
    // with 0x801B4D94, commit the multi-tap (+0x7C = 1) and play SE 0x23. setPage does the same
    // for a given index, and keeps the highlighted key's frame on the new page.
    typedef void (*GroupRemoveFn)(void* group, void* scnMdl);
    typedef void (*GroupAttachFn)(void* group, void* scnMdl, const char* node);
    extern "C" int sprintf(char* buf, const char* fmt, ...);

    static int pageIndex(u8* helper) { return *(int*)(helper + 0x3C); }
    static int pageOf(u8* helper) { return ((int*)(helper + 0x24))[pageIndex(helper) & 7]; }

    static void setPage(u8* helper, int idx)
    {
        if (pageIndex(helper) == idx || idx < 0 || idx >= *(int*)(helper + 0x38)) return;
        u8* group = *(u8**)(helper + 0x6C);
        u8* cur = *(u8**)(helper + 0x78);
        int page = ((int*)(helper + 0x24))[idx];
        u8* next = *(u8**)(helper + 0x48 + 4 * page);
        if (!group || !next) return;
        *(int*)(helper + 0x3C) = idx;
        if (cur) {
            GroupRemoveFn remove = *(GroupRemoveFn*)(*(u32*)group + 0x3C);
            remove(group, *(void**)(cur + 0x10));
            *(u8**)(helper + 0x78) = NULL;
        }
        char node[16];
        sprintf(node, "cpos%d", *(int*)(helper + 0x74) + 1);
        ((GroupAttachFn)0x801B4D94)(group, *(void**)(next + 0x10), node);
        *(u8**)(helper + 0x78) = next;
        *(u8*)(helper + 0x7C) = 1;
        s_setFrameMatCol(next, (float)(*(int*)(helper + 0x40) + 1));
        playSE(0x23);   // the page key's own sound
        s_lastKey = -1;
    }

    // ----------------------------------------------------------------------------------------
    // Key labels. Each page model (MenSelchrWalphabet / MenSelchrWnumber) has one bone per key
    // label with a visibility (VIS0) entry; found live by hiding them one by one: pPlane54 ABC,
    // pPlane55 DEF, pPlane65 the symbol key (key 1), pPlane66-71 GHI..WXYZ, pPlane72 key 10,
    // pPlane73 key 11 (the same bones on the digits page). An entry's flags word made constant
    // off (2) hides the label; the key itself stays. The VIS0 data is the loaded resource, shared
    // by the four players' keypads, so the flags are put back on close (as anyone_menu.cpp).
    struct VisSave {
        u32* flags;
        u32 old;
    };
    static VisSave s_vis[20];
    static int s_visCount = 0;

    static bool nameIs(const char* s, const char* want)
    {
        int i = 0;
        for (; want[i]; i++) if (s[i] != want[i]) return false;
        return s[i] == 0;
    }

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    // The VIS0 entry flags of `bone` in the MuObject's visibility animation
    // (+0x14 gfModelAnimation -> +0x08 AnmObjVisRes -> +0x2C ResAnmVis data).
    static u32* visFlags(u8* obj, const char* bone)
    {
        if (!obj) return NULL;
        u32 anim = *(u32*)(obj + 0x14);
        if (!isPtr(anim)) return NULL;
        u32 visRes = *(u32*)(anim + 0x8);
        if (!isPtr(visRes)) return NULL;
        u8* vis0 = *(u8**)(visRes + 0x2C);
        if (!isPtr((u32)vis0) || *(u32*)vis0 != 0x56495330 /* "VIS0" */) return NULL;
        u8* grp = vis0 + *(s32*)(vis0 + 0x10);
        u32 n = *(u32*)(grp + 4);
        for (u32 i = 1; i <= n && i < 64; i++) {
            u8* e = grp + 8 + 16 * i;
            if (nameIs((const char*)(grp + *(s32*)(e + 8)), bone)) return (u32*)(grp + *(s32*)(e + 12) + 4);
        }
        return NULL;
    }

    static void hideLabel(u8* obj, const char* bone)
    {
        u32* f = visFlags(obj, bone);
        if (!f || s_visCount >= (int)(sizeof(s_vis) / sizeof(s_vis[0]))) return;
        s_vis[s_visCount].flags = f;
        s_vis[s_visCount].old = *f;
        s_visCount++;
        *f = 2;   // constant, invisible
    }

    static void restoreLabels()
    {
        while (s_visCount > 0) {
            s_visCount--;
            *s_vis[s_visCount].flags = s_vis[s_visCount].old;
        }
    }

    // The '#' label: a MuMsg window of our own on the alphabet page's key-1 bone (pPlane65), in
    // the game's font, made the way the character select makes its windows (MuMsg::create(3,
    // 0x2A, 0x2B), allocMsgBuf, then Message::attachMsgBuf to the model node, which is what
    // MuMsg::attachScnMdlSimple does for "textN<n>" nodes). It lives in the CSS's heap (0x2A),
    // so it exists only while the keypad is open: made on open, taken off the model and deleted
    // on close (a block left in MenuInstance when the CSS ends stops the scene change: "Heap
    // (MenuInstance) has allocated block", the game hangs in scMemoryChange). attachMsgBuf hooks
    // the message buffer into the model's draw callback (ScnMdlSimple+0xD4, chaining the one
    // that was there), so that pointer is put back before the delete.
    typedef MuMsg* (*MsgCreateFn)(u32, u32, u32);
    typedef void (*MsgAllocFn)(MuMsg*, u32 size, u32 count);
    typedef void (*MsgInitWsFn)(MuMsg*, void* ws);
    typedef void (*MsgSetFaceFn)(MuMsg*, u32 window, u32 face);
    typedef void (*MsgDeleteFn)(MuMsg*, int);   // MuMsg's destructor (0x800B8A0C), 1 = free it
    typedef void (*MessageAttachFn)(void* message, u32 idx, void* scnMdl, const char* node, u8, int, float);
    // Our labels (the '#' key's): each a MuMsg of one window on its key's label bone. attachMsgBuf chains each message into the page
    // model's draw callback (+0xD4): the callback from before the first is put back when they
    // go.
    static const int MAX_LABELS = 9;
    static MuMsg* s_lblMsg[MAX_LABELS];
    static int s_lblCount = 0;
    static u8* s_lblMdl = NULL;
    static u32 s_lblOldCallback = 0;
    static u32 s_lblCallback = 0;
    static const char* const HASH_LABEL = "\xEF\xBC\x83";   // ＃

    void forgetLabels()
    {
        s_lblCount = 0;   // the CSS (and its heap) is gone
        s_lblMdl = NULL;
    }

    static void hideOurLabels()
    {
        if (!s_lblCount) return;
        if (s_lblMdl && *(u32*)(s_lblMdl + 0xD4) == s_lblCallback) *(u32*)(s_lblMdl + 0xD4) = s_lblOldCallback;
        for (int i = 0; i < s_lblCount; i++) ((MsgDeleteFn)0x800B8A0C)(s_lblMsg[i], 1);
        s_lblCount = 0;
        s_lblMdl = NULL;
    }

    // A label on `bone` of the page model `mdl`, `text` in the game's font, `scale` the size
    // (1.3: the size of Brawl's own printed one-character labels).
    static void addLabel(u8* mdl, const char* bone, const char* text, float scale, float halfWidth)
    {
        if (s_lblCount >= MAX_LABELS) return;
        if (s_lblCount == 0) {
            s_lblMdl = mdl;
            s_lblOldCallback = *(u32*)(mdl + 0xD4);
        }
        MuMsg* m = ((MsgCreateFn)0x800B8930)(3, 0x2A, 0x2B);
        if (!m) return;
        ((MsgAllocFn)0x800B8B08)(m, 0x40, 1);
        ((MessageAttachFn)0x8006B518)(m->m_message, 0, mdl, bone, 0, 3, *(float*)0x806A0D64);
        s_lblCallback = *(u32*)(mdl + 0xD4);
        u8* ws = *(u8**)((u8*)m + 0xC);
        ((MsgInitWsFn)0x800B8BE0)(m, ws);
        float* rect = (float*)(ws + 4);   // the character select's one-character windows
        rect[0] = -4.0f - halfWidth;      // (-16..16, 4 to the left: centred on the key)
        rect[1] = 12.8f;
        rect[2] = -4.0f + halfWidth;
        rect[3] = -16.0f;
        float* sc = (float*)(ws + 0x30);
        sc[0] = sc[1] = scale;
        ((MsgSetFaceFn)0x800B9488)(m, 0, 1);   // the face the CSS gives its windows
        m->setAlignMode(0, MuMsg::Align_Center);
        m->setFontColor(0, 0x30, 0x30, 0x30, 0xFF);   // as the keys' printed labels
        m->printf(0, "%s", text);
        s_lblMsg[s_lblCount++] = m;
    }

    static u8* alphaModel(u8* helper)
    {
        u8* alpha = *(u8**)(helper + 0x48 + 4 * PAGE_ALPHA);
        u8* mdl = alpha ? *(u8**)(alpha + 0x10) : NULL;
        return isPtr((u32)mdl) ? mdl : NULL;
    }

    static void showHashLabel(u8* helper)
    {
        u8* mdl = alphaModel(helper);
        if (!mdl) return;
        hideOurLabels();
        addLabel(mdl, "pPlane65", HASH_LABEL, 1.3f, 16.0f);
    }

    static void patchLabels(u8* helper)
    {
        restoreLabels();
        u8* alpha = *(u8**)(helper + 0x48 + 4 * PAGE_ALPHA);
        u8* digits = *(u8**)(helper + 0x48 + 4 * PAGE_DIGITS);
        hideLabel(alpha, "pPlane65");   // "@()^:;" -> our '#'
        hideLabel(alpha, "pPlane72");   // !?&%$
        hideLabel(alpha, "pPlane73");   // .,/~
        hideLabel(digits, "pPlane72");  // -+x=
        if (s_room) {
            // Room codes: no '#' (its key stays blank); the page key works (letters, digits),
            // so its arrows stay; the Random tab goes as below.
            u8* b = *(u8**)(helper + 0x44);
            hideLabel(b, "randam");
            hideLabel(b, "zz_Gc");
            return;
        }
        // The keypad base (MenSelchrWbase): the page key's arrows (the pages follow the '#') and
        // the "Z RANDOM" tab (Random is off; Z takes a recent code here).
        u8* base = *(u8**)(helper + 0x44);
        hideLabel(base, "kirikae");
        hideLabel(base, "randam");
        hideLabel(base, "zz_Gc");
        showHashLabel(helper);
    }

    void open(int port, bool room)
    {
        s_room = room;
        s_port = port;
        u8* area = cssArea(s_port);
        if (!area) return;
        void* hand = *(void**)(area + 0x1A8);
        u8* helper = area + 0x370;
        memset(s_buf, 0, sizeof(s_buf));
        *(u32*)(area + 0x400) = *(u32*)(area + 0x1DC);
        patchKeys(helper);
        patchLabels(helper);
        s_lastKey = -1;
        ((KeypadOpenFn)(NAME_TEXT + 0x5A8))(helper, NULL, s_buf, s_room ? ROOM_CODE_CHARS : CODE_CHARS);
        // The text field is sized for Brawl's 5-character names: let the font narrow to fit 8.
        MuMsg* msg = *(MuMsg**)(helper + 0x64);
        if (msg) msg->setFontWidthModeAuto(*(u32*)(helper + 0x70));
        ((SetHandModeFn)(SEL_CHAR_TEXT + 0x1A348))(hand, 8, s_port);
        s_helper = helper;
        s_active = true;
        s_confirmed = false;
        s_openTries = 30;
        PPOM::g_block.debug.scratch[4] = 1;
        // Slippi NameEntryThinkOneShot.asm: the first suggestion as the keypad appears.
        s_sugFound = false;
        s_sugIndex = 0;
        s_sugQueued = 0;
        s_sugIgnore = s_sugSeq;   // an answer for an earlier keypad may still come
        s_lastBuf[0] = 0;
        s_drawn[0] = 1;
        s_drawn[1] = 0;
        sugRequest(PPOM::SCROLL_RESET);
    }

    static void finish()
    {
        u8* area = cssArea(s_port);
        if (s_helper && area && area + 0x370 == s_helper) {
            // The CSS is still there: its resources and our '#' window too.
            restoreLabels();
            hideOurLabels();
        } else {
            s_visCount = 0;
            forgetLabels();
        }
        restoreKeys(s_helper);
        s_active = false;
        s_helper = NULL;
    }

    // Called every frame while the online CSS is up, with our own START edge: START is masked
    // out of the game's pad statuses on the online CSS (online_menu.cpp maskStart).
    void tick(bool startPressed)
    {
        s_startPressed = startPressed;
        if (!s_active) return;
        if (s_sugSeq && ++s_sugWait > 120) {
            // no answer for 2 s (lost): ask again for the current text
            s_sugSeq = 0;
            sugRequest(PPOM::SCROLL_NONE);
        }
        u8* area = cssArea(s_port);
        if (!area) { finish(); return; }
        u8* hand = *(u8**)(area + 0x1A8);
        int mode = hand ? *(int*)(hand + 0xA4) : 0;
        PPOM::g_block.debug.scratch[5] = (u32)mode;
        if (s_openTries > 0) {
            // The hand can refuse the mode change for a few frames (seen right after the
            // START press); ask again until it shows the keypad.
            s_openTries--;
            if (mode == 8) {
                s_openTries = 0;
            } else {
                if (hand) ((SetHandModeFn)(SEL_CHAR_TEXT + 0x1A348))(hand, 8, s_port);
                if (s_openTries == 0) finish();
                return;
            }
        }
        if (mode != 8) {
            bool ok = s_confirmed;
            finish();
            PPOM::g_block.debug.scratch[4] = ok ? 2 : 3;
            char code[16];
            decodeFullWidth(s_buf, code, CODE_CHARS);
            if (ok && code[0]) OnlineMenu::codeEntered(code);
        }
    }

    bool active() { return s_active; }

    // The CSS's call of MuSelctChrNameEntry::update (sora_menu_name text+0x998) at
    // sel_char+0x18F34 comes here instead (keypadUpdateCall below); the widget itself and its
    // other users (Vs names, Stage Builder, ...) are untouched.
    static int hkUpdate(u8* helper, void* pad, char* out, int allowRandom, int se)
    {
        if (!s_active || helper != s_helper || !pad) {
            return s_origUpdate(helper, pad, out, allowRandom, se);
        }
        // `pad` is the CSS's own per-frame copy of the pad status (sel_char+0x18E50..+0x18EE8),
        // so the buttons can be filtered in place.
        u8* ps = (u8*)pad;
        u32* pressed = (u32*)(ps + 0xC);     // gfPadStatus::m_buttonsPressedThisFrame
        u32* pressed2 = (u32*)(ps + 0x14);
        const u32 A = 0x100, Z = 0x10, R = 0x20, L = 0x40;
        u32 trig = *pressed;
        if (trig & (L | R | Z)) {
            // Recent codes: L older, R newer, Z take it (see above). The keypad has no use
            // for these buttons; take them out of its pad copy all the same.
            *pressed &= ~(L | R | Z);
            *pressed2 &= ~(L | R | Z);
            if (trig & L) {
                sugRequest(PPOM::SCROLL_OLDER);
            } else if (trig & R) {
                sugRequest(PPOM::SCROLL_NEWER);
            }
            if (trig & Z) acceptSuggestion(helper);
        }
        int page = pageOf(helper);
        int key = *(int*)(helper + 0x40);
        bool empty = s_buf[0] == 0;
        if (s_startPressed) {
            s_startPressed = false;
            if (empty) {
                playSE(3);
            } else {
                *(int*)(helper + 0x40) = 0xD;   // Slippi: START = Confirm
                key = 0xD;
                *pressed |= A;
            }
        }
        int typedKey = -1;
        if (*pressed & A) {
            CodeShape shape = shapeOf(s_buf);
            bool cycles = *(u8*)(helper + 0x7C) == 0 && key == s_lastKey;
            if (!keyAllowed(page, key, shape, cycles)) {
                playSE(3);
                *pressed &= ~A;
                *pressed2 &= ~A;
                u32& sc = PPOM::g_block.debug.scratch[15];
                sc = (sc & ~0xFF0000u) | ((sc + 0x10000u) & 0xFF0000u);   // refused keys (tests)
            } else if (key >= 1 && key <= 11) {
                typedKey = key;
            }
        }
        int r = s_origUpdate(helper, pad, s_out, 0 /* no Random */, se);
        if (r == 0) {
            // OK: we take the code; the CSS takes its cancel path (keeps the name tag).
            s_confirmed = true;
            return 1;
        }
        if (r == 2) {
            // Typed, cycled or deleted (B, the erase key, hold B): the keypad has printed its
            // plain text; ask for the newest code that starts with the new text (Slippi
            // OnEnterText.asm / OnBPressAutoComplete.asm: scroll reset).
            if (strcmp(s_buf, s_lastBuf) != 0) {
                if (typedKey >= 0) {
                    s_lastKey = typedKey;
                    // Single-character keys ('#', the digits) commit at once: pressing the same
                    // key again types another character instead of cycling this one.
                    if (typedKey == KEY_HASH || page == PAGE_DIGITS) {
                        commitText(helper);
                        s_lastKey = -1;
                    }
                } else {
                    s_lastKey = -1;
                }
                strcpy(s_lastBuf, s_buf);
                s_drawn[0] = 1;
                s_drawn[1] = 0;
                sugRequest(PPOM::SCROLL_RESET);
            }
            // The page follows the text: digits once there is a '#' (typed, or a recent code
            // taken with Z), letters when there is none (the '#' erased, the field cleared).
            if (!s_room) setPage(helper, shapeOf(s_buf).hash ? 1 : 0);
            u32& sc = PPOM::g_block.debug.scratch[15];
            sc = (sc & ~0xFF00u) | ((u32)(pageIndex(helper) & 0xFF) << 8);   // tests: the page
            drawField(helper);
        }
        return r;
    }

    extern "C" int pponline_keypadUpdate(u8* helper, void* pad, char* out, int allowRandom, int se)
    {
        return hkUpdate(helper, pad, out, allowRandom, se);
    }

    // sel_char+0x18F30 `addi r3,r29,0x370`, followed by +0x18F34 `bl MuSelctChrNameEntry::update`
    // (r4-r7 are set): do the addi, call ours with the same arguments and go back to +0x18F38
    // with its result in r3. The CSS function saved LR in its prologue, so using LR here is
    // fine. (The hook cannot sit on the `bl` itself: it carries a relocation against module 16,
    // which the loader applies after Syriinge's patch and which turned the patch into a `b`.)
    __attribute__((naked)) void keypadUpdateCall()
    {
        asm volatile(
            "addi 3, 29, 0x370\n\t"
            "lis 12, pponline_keypadUpdate@ha\n\t"
            "addi 12, 12, pponline_keypadUpdate@l\n\t"
            "mtctr 12\n\t"
            "bctrl\n\t"
            "lis 12, 0x8069\n\t"
            "ori 12, 12, 0xB7FC\n\t"
            "mtctr 12\n\t"
            "bctr\n\t");
    }

    void install(CoreApi* api)
    {
        api->sySimpleHookRel(0x18F30, reinterpret_cast<void*>(keypadUpdateCall), 10 /* sora_menu_sel_char */);
    }
}
