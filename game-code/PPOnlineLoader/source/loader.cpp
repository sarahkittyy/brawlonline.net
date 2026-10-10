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
// The plugin runs from MEM2, more than 32 MB away from the game's code in MEM1, so it is built with
// -mlongcall (no relative calls out of it), and Syriinge's hooks cannot branch to it directly
// (their stubs use relative branches). Every hook therefore goes through a thunk here in MEM1:
//     lis r12, target@h; ori r12, r12, target@l; mtctr r12; bctr
// which clobbers only r12 and CTR, volatile at every hook site the plugin uses (each of its naked
// hooks writes r12 and CTR itself before reading them). The plugin gets a CoreApi of ours (same
// vtable layout as sy_core's) that makes the thunk and passes it on to sy_core's.
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

    static const void* thunk(const void* target)
    {
        u32 t = (u32)target;
        if (t < 0x90000000) return target;   // already in MEM1: no thunk needed
        if (s_used >= THUNKS) {
            OSReport("[PPOnline] out of hook thunks\n");
            return 0;
        }
        u32* w = s_thunks[s_used++];
        w[0] = 0x3D800000 | (t >> 16);       // lis r12, t@h
        w[1] = 0x618C0000 | (t & 0xFFFF);    // ori r12, r12, t@l
        w[2] = 0x7D8903A6;                   // mtctr r12
        w[3] = 0x4E800420;                   // bctr
        DCFlushRange(w, 16);
        ICInvalidateRange(w, 16);
        return w;
    }

    class ThunkApi {
    public:
        virtual void syInlineHook(const u32 address, const void* replacement)
        {
            s_api->syInlineHook(address, thunk(replacement));
        }
        virtual void syInlineHookRel(const u32 offset, const void* replacement, int moduleId)
        {
            s_api->syInlineHookRel(offset, thunk(replacement), moduleId);
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
        Loader::loadPlugin();
        return &META;
    }
    void _epilog() {}
    void _unresolved() {}
}
