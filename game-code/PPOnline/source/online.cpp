// PPOnline: Slippi-style online menus built from Brawl/P+'s own screens, fonts and messages.
//
// Hooks (all verified clear of P+ v3.2 codeset patches with tools/gamecode/pplus_hooks.py):
//   MuMsg::printIndex      0x800B91B8  replace  -> relabel message lines (Text::overrideFor)
//   gfPadSystem::updateSystem 0x8002A210 replace -> per-frame tick (menus only; mailbox polling)
//   MuMsg::beginPrint      0x800B8EE8  replace  -> debug log of MuMsg::printf callers
//   MuMsg::create          0x800B8930  replace  -> debug log of message objects
//   gfSceneManager::setNextSequence 0x8002D640 replace -> boot to the ONLINE page (boot_menu.cpp)
#include <gf/gf_scene.h>
#include <memory.h>
#include <mu/mu_msg.h>
#include <string.h>

#include "online.h"
#include "online_menu.h"
#include "netmenu.h"
#include "ppom.h"
#include "stage_legal.h"

namespace Online {

    typedef bool (*PrintIndexFn)(MuMsg*, u32, u32, void*);
    typedef void (*PadUpdateFn)(void*);
    typedef void (*BeginPrintFn)(MuMsg*, u32);
    typedef MuMsg* (*MsgCreateFn)(u32, u32, u32);

    static PrintIndexFn s_origPrintIndex = NULL;
    static PadUpdateFn s_origPadUpdate = NULL;
    static BeginPrintFn s_origBeginPrint = NULL;
    static MsgCreateFn s_origMsgCreate = NULL;

    static void logEntry(u32 lr, u32 msg, u32 window, s16 line, u32 data)
    {
        PPOM::Debug& dbg = PPOM::g_block.debug;
        if (!(dbg.cfg & PPOM::CFG_LOG)) return;
        PPOM::PrintLog& e = dbg.log[dbg.printCount % PPOM::DEBUG_LOG];
        e.lr = lr;
        e.msg = msg;
        e.window = (u16)window;
        e.line = line;
        e.data = data;
        dbg.printCount++;
    }

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    const char* currentSceneName()
    {
        u32 mgr = *(u32*)0x805A0060;
        if (!isPtr(mgr)) return "";
        u32 scene = *(u32*)(mgr + 4);
        if (!isPtr(scene)) return "";
        u32 name = *(u32*)scene;
        if (!isPtr(name)) return "";
        return (const char*)name;
    }

    bool inScene(const char* name) { return strcmp(currentSceneName(), name) == 0; }

    const char* msbinLine(const void* msbin, int line, int* outLen)
    {
        if (!msbin || line < 0) return NULL;
        const u32* offs = (const u32*)msbin;
        u32 n = offs[0] / 4;   // the offset table is followed directly by the text
        if ((u32)line + 1 >= n) return NULL;
        if (outLen) *outLen = (int)(offs[line + 1] - offs[line]);
        return (const char*)msbin + offs[line];
    }

    bool msbinLineContains(const void* msbin, int line, const char* needle)
    {
        int len = 0;
        const char* s = msbinLine(msbin, line, &len);
        if (!s) return false;
        int nl = strlen(needle);
        // (no memcmp in the DOL; a call to it would stay unresolved)
        for (int i = 0; i + nl <= len; i++) {
            int k = 0;
            while (k < nl && s[i + k] == needle[k]) k++;
            if (k == nl) return true;
        }
        return false;
    }

    static bool hkPrintIndex(MuMsg* self, u32 window, u32 line, void* msbin)
    {
        PPOM::Debug& dbg = PPOM::g_block.debug;
        u32 lr = (u32)__builtin_return_address(0);
        logEntry(lr, (u32)self, window, (s16)line, (u32)(msbin ? msbin : self->m_msgData));
        if (dbg.cfg & PPOM::CFG_TEXT) {
            // A NULL msbin means "the MuMsg's own data" (MuMsg::setMsgData).
            const void* data = msbin ? msbin : self->m_msgData;
            const char* text = Text::overrideFor(self, window, line, data, lr);
            if (text) {
                dbg.overrides++;
                Text::printStyled(self, window, data, line, text);
                return true;
            }
        }
        return s_origPrintIndex(self, window, line, msbin);
    }

    // gfPadSystem::updateSystem runs once per game frame on the main thread: our tick.
    static void hkPadUpdate(void* padSystem)
    {
        s_origPadUpdate(padSystem);
        PPOM::g_block.debug.frames++;
        OnlineMenu::tick();
        StageLegal::tick();
    }

    // MuMsg::beginPrint: log MuMsg::printf calls (line = -2, data = printf's caller).
    static void hkBeginPrint(MuMsg* self, u32 window)
    {
        u32 lr = (u32)__builtin_return_address(0);
        if (lr >= 0x800B9230 && lr < 0x800B92D8) {
            u32 caller = *(u32*)(*(u32*)(*(u32*)__builtin_frame_address(0)) + 4);
            logEntry(caller, (u32)self, window, -2, 0);
            StageLegal::onSssPrint(caller);
        }
        s_origBeginPrint(self, window);
    }

    // MuMsg::create: log message objects (line = -3, window = their window count).
    static MuMsg* hkMsgCreate(u32 font, u32 heap, u32 windows)
    {
        MuMsg* m = s_origMsgCreate(font, heap, windows);
        logEntry((u32)__builtin_return_address(0), (u32)m, windows, -3, font);
        StageLegal::onMsgCreate(m, (u32)__builtin_return_address(0));
        return m;
    }

    void install(CoreApi* api)
    {
        PPOM::g_block.debug.cfg = PPOM::CFG_TEXT | PPOM::CFG_WIFI_HOOKS | PPOM::CFG_LOG;
        api->syReplaceFunc(0x800B91B8, reinterpret_cast<void*>(hkPrintIndex), (void**)&s_origPrintIndex);
        api->syReplaceFunc(0x8002A210, reinterpret_cast<void*>(hkPadUpdate), (void**)&s_origPadUpdate);
        api->syReplaceFunc(0x800B8EE8, reinterpret_cast<void*>(hkBeginPrint), (void**)&s_origBeginPrint);
        api->syReplaceFunc(0x800B8930, reinterpret_cast<void*>(hkMsgCreate), (void**)&s_origMsgCreate);
        NetMenu::install(api);
        OnlineMenu::install(api);
        StageLegal::install(api);
        AnyoneMenu::install(api);
        BootMenu::install(api);
    }
}

namespace Text {

    // A message line is "<style tags><text>\x13" (Brawl msbin: \x12 opens a style group whose
    // parameters, e.g. \x0c RGBA colour or \r size, may contain printable bytes, and \x13
    // closes it). The visible text is the last printable run that reaches the closing byte.
    static int textStart(const char* s, int len)
    {
        int end = len;
        for (int i = 0; i < len; i++) {
            if (s[i] == 0x13 || s[i] == 0) { end = i; break; }
        }
        int start = end;
        while (start > 0) {
            u8 c = (u8)s[start - 1];
            if (c < 0x20 || c >= 0x80) break;
            start--;
        }
        return start;
    }

    static char s_buf[256];

    void printStyled(MuMsg* msg, u32 window, const void* msbin, u32 line, const char* text)
    {
        int len = 0;
        const char* orig = Online::msbinLine(msbin, line, &len);
        // Only lines that open with a style group (0x12) carry a prefix to keep.
        int pre = (orig && len > 0 && orig[0] == 0x12) ? textStart(orig, len) : 0;
        if (pre > 64) pre = 0;
        int n = 0;
        for (int i = 0; i < pre; i++) s_buf[n++] = orig[i];
        for (int i = 0; text[i] && n < (int)sizeof(s_buf) - 2; i++) s_buf[n++] = text[i];
        if (pre > 0 && orig[0] == 0x12) s_buf[n++] = 0x13;
        s_buf[n] = 0;
        msg->printf(window, "%s", s_buf);
    }
}
