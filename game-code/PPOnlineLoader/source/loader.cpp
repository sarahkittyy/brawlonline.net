// PPOnline's loader: the Syriinge plugin that P+'s sy_core loads into its Syringe heap.
//
// The Syringe heap (P+ v3.2's Syringe.asm: heap 60, 0x10000 bytes at 0x817BA5A0) holds sy_core,
// P+'s four plugins and ours, and was full (docs/game-code.md section 2, "Heap budget"). So the
// plugin proper (PPOnlineMain, REL id 20560, which carries the PPOM block) is not loaded there.
// PPOnline.rel is this loader with the plugin appended after it (build.sh):
//
//     [loader REL][pad to 32][plugin REL][Trailer: "PPOL", offset, size, version]
//
// sy_core reads the file, links the loader (gfModule::create only keeps the REL's own sections and
// .bss: the appended bytes cost nothing here) and calls _prolog. We read the same file again and
// link the plugin into Brawl's Network heap (heap 6, about 1.3 MB in MEM2, empty: the WFC login it
// was made for never runs; not part of the rollback region set, and never rebuilt after boot).
//
// The plugin runs from MEM2, more than 32 MB away from the game's code in MEM1, so it makes no
// relative calls out of it (PPOnline/source/dol_calls.cpp), and Syriinge's hooks cannot branch to
// it directly (their stubs use relative branches). Every hook therefore goes through a thunk here
// in MEM1:
//     lis r12, target@h; ori r12, r12, target@l; mtctr r12; bctr
// which clobbers only r12 and CTR, volatile at every hook site the plugin uses (each of its naked
// hooks writes r12 and CTR itself before reading them). The plugin gets a CoreApi of ours (same
// vtable layout as sy_core's) that makes the thunk and passes it on to sy_core's.
//
// Inline hooks' thunks end in fixInlineStub instead of bctr. P+ v3.2.1's sy_core (still "0.6.0",
// file 18 of common2.pac) builds its inline hook stubs with two words swapped:
//     3.2.0:  bl hook; lwz r0,0x94(r1); mtlr r0; lwz r0,0x8(r1); lmw r3,0xC(r1); ...
//     3.2.1:  bl hook; lwz r0,0x8(r1);  mtlr r0; lwz r0,0x94(r1); lmw r3,0xC(r1); ...
// so every inline hook returned to the hooked code with r0 and LR exchanged. Ours rely on both
// (the title skip sets r0 through the stub's frame: on 3.2.1 the title's state got the caller's
// return address and the boot sat on a white screen). fixInlineStub looks at the stub words after
// its return address (the stub's bl) and, when they are 3.2.1's pair, writes 3.2.0's order back,
// once, before the hook runs. It uses r0 (here a copy of LR, already saved by the stub), r12, CTR
// and cr0, which the hook may clobber anyway. Stubs in 3.2.0's order are left alone.
//
// The same fix is also made as soon as sy_core has placed each stub (fixStub): for a hook in
// main.dol right after sy_core made it, for one in a module once sy_core has patched the module
// (our module load callback runs after sy_core's). Then no stub changes while it runs: code that
// was translated ahead of time from the stub's words (EmissaryRecomp's code variants) ran the
// rest of the stub as it was before the fix and gave the title's state LR (bug report #368: back
// to the title from the main menu, a black screen for good). fixInlineStub stays for a stub this
// does not reach (a hook in a module that was loaded before ours was made).
#include <OS/OSCache.h>
#include <OS/OSError.h>
#include <gf/gf_file_io_handle.h>
#include <gf/gf_heap_manager.h>
#include <gf/gf_module.h>
#include <memory.h>
#include <types.h>

class CoreApi;
struct Version {   // Syriinge's Version (lib/Syriinge/include/version.h)
    int major, minor, revision;
};
struct PluginMeta {
    char NAME[20];
    char AUTHOR[20];
    Version VERSION;
    Version SY_VERSION;
    void (*PLUGIN_MAIN)(CoreApi* api);
};

// sy_core's CoreApi, as the plugins see it (lib/Syriinge/include/sy_core.h): only the virtual
// functions, in that order. ThunkApi has the same layout, so the plugin can use it as a CoreApi.
class CoreApi {
public:
    virtual void syInlineHook(const u32 address, const void* replacement);
    virtual void syInlineHookRel(const u32 offset, const void* replacement, int moduleId);
    virtual void sySimpleHook(const u32 address, const void* replacement);
    virtual void sySimpleHookRel(const u32 offset, const void* replacement, int moduleId);
    virtual void syReplaceFunc(const u32 address, const void* replacement, void** original);
    virtual void syReplaceFuncRel(const u32 offset, const void* replacement, void** original, int moduleId);
    virtual void moduleLoadEventSubscribe(void (*cb)(void*));
};

namespace Loader {
    // Enough for the plugin's hooks (62 on 2026-10-10) and the rooms work; 16 bytes each.
    const int THUNKS = 128;
    static u32 s_thunks[THUNKS][4] __attribute__((aligned(32)));
    static int s_used = 0;
    static CoreApi* s_api = 0;

    // Entered from an inline hook's thunk, with the hook in CTR and LR at the stub's word after its
    // bl (see the top of this file).
    extern "C" __attribute__((naked)) void fixInlineStub()
    {
        asm volatile(
            "mflr 12\n\t"
            "lwz 0, 0(12)\n\t"           // lwz r0,0x8(r1) where 3.2.0 has lwz r0,0x94(r1)?
            "xoris 0, 0, 0x8001\n\t"
            "cmplwi 0, 0x0008\n\t"
            "bnectr\n\t"
            "lwz 0, 8(12)\n\t"           // and lwz r0,0x94(r1) where 3.2.0 has lwz r0,0x8(r1)?
            "xoris 0, 0, 0x8001\n\t"
            "cmplwi 0, 0x0094\n\t"
            "bnectr\n\t"
            "lis 0, 0x8001\n\t"
            "ori 0, 0, 0x0094\n\t"
            "stw 0, 0(12)\n\t"           // lwz r0,0x94(r1): LR
            "lis 0, 0x8001\n\t"
            "ori 0, 0, 0x0008\n\t"
            "stw 0, 8(12)\n\t"           // lwz r0,0x8(r1): r0
            "li 0, 8\n\t"
            "dcbst 0, 12\n\t"
            "dcbst 12, 0\n\t"
            "sync\n\t"
            "icbi 0, 12\n\t"
            "icbi 12, 0\n\t"
            "isync\n\t"
            "bctr\n\t");
    }

    const u32 LWZ_R0_8 = 0x80010008;    // lwz r0,0x8(r1)
    const u32 LWZ_R0_94 = 0x80010094;   // lwz r0,0x94(r1)

    // fixInlineStub's fix, made where sy_core placed the stub: `target` holds sy_core's "b stub"
    // (to the stub's original instruction, then sy_core's InlineHook::instructions), and the stub's
    // bl to `thunk` is 0x18 bytes in. Anything else (not patched yet, not our stub) is left alone.
    static void fixStub(u32 target, const void* thunk)
    {
        if (!thunk) return;
        const u32 b = *(const u32*)target;
        if ((b & 0xFC000003) != 0x48000000) return;
        s32 disp = (s32)(b & 0x03FFFFFC);
        if (disp & 0x02000000) disp -= 0x04000000;
        u32* bl = (u32*)(target + disp) + 6;
        if (*bl != (0x48000001 | (((u32)thunk - (u32)bl) & 0x03FFFFFC))) return;
        if (bl[1] != LWZ_R0_8 || bl[3] != LWZ_R0_94) return;   // v3.2.0's order already
        bl[1] = LWZ_R0_94;
        bl[3] = LWZ_R0_8;
        DCFlushRange(bl, 16);
        ICInvalidateRange(bl, 16);
    }

    // Our inline hooks in modules: sy_core patches them in when the module loads.
    struct RelHook {
        u32 offset;
        int moduleId;
        const void* thunk;
    };
    static RelHook s_relHooks[THUNKS];
    static int s_relUsed = 0;

    // Subscribed after sy_core's own module load callback, so it runs once sy_core has patched the
    // module's hooks in (sy_core's doPatchesOnModule: a module hook's target is its offset + .text).
    static void onModuleLoaded(void* p)
    {
        gfModuleHeader* header = ((gfModuleInfo*)p)->m_module->header;
        const u32 text = header->getTextSectionAddr();
        for (int i = 0; i < s_relUsed; i++) {
            if (s_relHooks[i].moduleId == (int)header->id)
                fixStub(text + s_relHooks[i].offset, s_relHooks[i].thunk);
        }
    }

    static const void* thunk(const void* target, bool inlineHook = false)
    {
        u32 t = (u32)target;
        if (t < 0x90000000 && !inlineHook) return target;   // already in MEM1: no thunk needed
        if (s_used >= THUNKS) {
            OSReport("[PPOnline] out of hook thunks\n");
            return 0;
        }
        u32* w = s_thunks[s_used++];
        w[0] = 0x3D800000 | (t >> 16);       // lis r12, t@h
        w[1] = 0x618C0000 | (t & 0xFFFF);    // ori r12, r12, t@l
        w[2] = 0x7D8903A6;                   // mtctr r12
        w[3] = 0x4E800420;                   // bctr
        if (inlineHook) {
            // b fixInlineStub (both in MEM1, well within a relative branch)
            w[3] = 0x48000000 | (((u32)&fixInlineStub - (u32)&w[3]) & 0x03FFFFFC);
        }
        DCFlushRange(w, 16);
        ICInvalidateRange(w, 16);
        return w;
    }

    class ThunkApi {
    public:
        virtual void syInlineHook(const u32 address, const void* replacement)
        {
            const void* t = thunk(replacement, true);
            s_api->syInlineHook(address, t);
            fixStub(address, t);   // sy_core patches a hook in main.dol at once
        }
        virtual void syInlineHookRel(const u32 offset, const void* replacement, int moduleId)
        {
            const void* t = thunk(replacement, true);
            s_api->syInlineHookRel(offset, t, moduleId);
            if (t && s_relUsed < THUNKS) {
                RelHook& h = s_relHooks[s_relUsed++];
                h.offset = offset;
                h.moduleId = moduleId;
                h.thunk = t;
            }
        }
        virtual void sySimpleHook(const u32 address, const void* replacement)
        {
            s_api->sySimpleHook(address, thunk(replacement));
        }
        virtual void sySimpleHookRel(const u32 offset, const void* replacement, int moduleId)
        {
            s_api->sySimpleHookRel(offset, thunk(replacement), moduleId);
        }
        virtual void syReplaceFunc(const u32 address, const void* replacement, void** original)
        {
            s_api->syReplaceFunc(address, thunk(replacement), original);
        }
        virtual void syReplaceFuncRel(const u32 offset, const void* replacement, void** original, int moduleId)
        {
            s_api->syReplaceFuncRel(offset, thunk(replacement), original, moduleId);
        }
        virtual void moduleLoadEventSubscribe(void (*cb)(void*))
        {
            s_api->moduleLoadEventSubscribe((void (*)(void*))thunk((const void*)cb));
        }
    };
    static ThunkApi s_thunkApi;

    struct Trailer {
        u32 magic;     // "PPOL"
        u32 offset;    // of the plugin REL in the file
        u32 size;
        u32 version;   // 1
    };
    const u32 TRAILER_MAGIC = 0x50504F4C;

    // gfModuleHeader: id +0, prologOffset +0x34 (absolute once linked).
    typedef const PluginMeta* (*PrologFn)(CoreApi*);

    // The plugin file, as sy_core reads it ("%s/%s" with "plugins"; P+'s file patch serves it from
    // /Project+/pf/plugins/ on the SD card).
    static const char PATH[] = "plugins/PPOnline.rel";

    static void loadPlugin()
    {
        gfFileIOHandle handle;
        handle.read(PATH, Heaps::MenuInstance, 0);
        u8* buf = (u8*)handle.getBuffer();
        u32 size = (u32)handle.getSize();
        if (!buf || size < sizeof(Trailer)) {
            OSReport("[PPOnline] cannot read %s\n", PATH);
            handle.release();
            return;
        }
        const Trailer* tr = (const Trailer*)(buf + size - sizeof(Trailer));
        void* heap = gfHeapManager::getHeap(Heaps::Network);
        if (tr->magic != TRAILER_MAGIC || tr->version != 1 || (tr->offset & 31) ||
            tr->offset + tr->size > size - sizeof(Trailer) || !heap) {
            OSReport("[PPOnline] no plugin in %s (or no Network heap)\n", PATH);
        } else {
            u8** module = (u8**)gfModule::create(heap, buf + tr->offset, tr->size);
            u8* header = module ? *module : 0;
            if (header) {
                PrologFn prolog = *(PrologFn*)(header + 0x34);
                const PluginMeta* meta = prolog((CoreApi*)&s_thunkApi);
                OSReport("[PPOnline] plugin %s linked at %08x (Network heap), %d hook thunks\n",
                         meta ? meta->NAME : "?", (u32)header, s_used);
            }
        }
        free(buf);
        handle.release();
    }
}

namespace Syringe {
    extern "C" {
        const PluginMeta* _prolog(CoreApi* api);
        void _epilog();
        void _unresolved();
    }
    static const PluginMeta META = {"PPOnline", "pm_rollback", {0, 2, 0}, {0, 6, 0}, 0};

    const PluginMeta* _prolog(CoreApi* api)
    {
        Loader::s_api = api;
        api->moduleLoadEventSubscribe(Loader::onModuleLoaded);   // after sy_core's (syInit)
        Loader::loadPlugin();
        return &META;
    }
    void _epilog() {}
    void _unresolved() {}
}
