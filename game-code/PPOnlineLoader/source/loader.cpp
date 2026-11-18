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
// Nothing of this module is used once _prolog returns (2026-11-14). A mod may put the Syringe heap
// where a scene's heap also reaches: Cobalt's (P+Ex) is at 0x817AA5A0, 0x10000 below P+'s, and
// Brawl's Wi-Fi waiting room makes its MenuInstance heap up to 0x817AE860, over the lowest thing
// in the Syringe heap. That is this module, loaded last ("PPOnline" sorts after the mod's own
// plugins): the room overwrote its header and the start of its code, the next inline hook ran
// through what was left of fixInlineStub, and the game went off into low memory. So:
//  - the thunks and a copy of fixInlineStub live in a block from the default heap (operator new),
//    where sy_core keeps every hook it makes: they are safe wherever a build's hooks are;
//  - the module takes itself off the OS module list, so that its overwritten header is not a
//    link of the list the game (OSLink, for every module loaded later) and Dolphin (the PPOM
//    block's module) walk.
//
// Low aliases (2026-11-16). Some builds' codes call game functions by absolute branch to their
// physical address (`bla 0x0B9230`, MuMsg::printf, in REX's song title code that runs as every
// match starts; P+'s Code Menu and tag list have two such calls too). The function then runs at
// 0x000B9230, and every relative branch in it stays in that low alias, where the game's BATs map
// only MEM1's first 16 MB. A hook's branch at such a function's entry went to Syriinge's Hook
// record in the Tmp heap (0x8105xxxx in P+ and REX), so from the low alias to 0x0105xxxx, which
// is not mapped: REX hung at its first countdown. So the thunk block comes from the Thread heap
// (Brawl's first static heaps, 0x805CA260 in every build, almost all free; else the default heap
// as before), and the entry of each game function the plugin replaces or simple-hooks branches to
// its thunk directly, in place of Syriinge's record (whose only word is the same branch on):
// reachable from both aliases. Hooks inside modules keep Syriinge's records (it patches them as
// a module loads), and so does the one inline hook on the DOL.
#include <OS/OSCache.h>
#include <OS/OSError.h>
#include <gf/gf_file_io_handle.h>
#include <gf/gf_heap_manager.h>
#include <gf/gf_memory_pool.h>
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
    // Enough for the plugin's hooks (62 on 2026-10-10, 90 on 2026-11-14) and the rooms work; 16
    // bytes each. Outside this module (see the top of this file): s_thunks points into a block
    // from the default heap, with the copy of fixInlineStub (s_fixStub) after the thunks.
    const int THUNKS = 128;
    const u32 FIX_STUB_MAX = 0x80;   // bytes kept for the copy of fixInlineStub (it is 0x5C)
    // The part of MEM1 the game's BATs also map at its physical address (see the top of this file).
    const u32 LOW_ALIAS_END = 0x81000000;
    static u32 (*s_thunks)[4] = 0;
    static u32* s_fixStub = 0;
    static int s_used = 0;
    static CoreApi* s_api = 0;

    // Entered from an inline hook's thunk, with the hook in CTR and LR at the stub's word after its
    // bl (see the top of this file). Run from its copy in the thunk block (makeThunks), never from
    // here: it has no branch but those through CTR, so its words work anywhere, and its only
    // plain bctr is its last word (the copy ends there).
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

    // The thunk block: THUNKS thunks, then fixInlineStub's words up to its bctr.
    static bool makeThunks()
    {
        if (s_thunks) return true;
        const u32* stub = (const u32*)&fixInlineStub;
        u32 words = 0;
        while (words < FIX_STUB_MAX / 4 && stub[words] != 0x4E800420) words++;   // bctr
        if (words >= FIX_STUB_MAX / 4) {
            OSReport("[PPOnline] fixInlineStub has no end\n");
            return false;
        }
        words++;
        u32 size = THUNKS * 16 + FIX_STUB_MAX;
        u32* block = (u32*)gfHeapManager::alloc(Heaps::Thread, size);
        if (block && (u32)block + size > LOW_ALIAS_END) {
            free(block);
            block = 0;
        }
        if (!block) block = (u32*)operator new(size);
        if (!block) {
            OSReport("[PPOnline] no memory for the hook thunks\n");
            return false;
        }
        memset(block, 0, size);
        s_thunks = (u32 (*)[4])block;
        s_fixStub = block + THUNKS * 4;
        for (u32 i = 0; i < words; i++) s_fixStub[i] = stub[i];
        DCFlushRange(s_fixStub, words * 4);
        ICInvalidateRange(s_fixStub, words * 4);
        return true;
    }

    // A game function's entry that Syriinge just pointed at its Hook record: at the thunk itself,
    // when the thunk is reachable from both aliases (see the top of this file).
    static void pointAtThunk(u32 address, const void* thunkAddr)
    {
        u32 t = (u32)thunkAddr;
        if (address < 0x80000000 || address >= 0x81800000 || t + 16 > LOW_ALIAS_END) return;
        u32 w = *(u32*)address;
        if (w >> 26 != 18 || (w & 3) != 0) return;   // not Syriinge's `b` (to its record)
        s32 off = (s32)t - (s32)address;
        if (off < -0x02000000 || off >= 0x02000000) return;
        *(u32*)address = 0x48000000 | ((u32)off & 0x03FFFFFC);
        DCFlushRange((void*)address, 4);
        ICInvalidateRange((void*)address, 4);
    }

    static const void* thunk(const void* target, bool inlineHook = false)
    {
        u32 t = (u32)target;
        if (t < 0x90000000 && !inlineHook) return target;   // already in MEM1: no thunk needed
        if (!makeThunks()) return 0;
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
            // b to the copy of fixInlineStub (the same block: well within a relative branch)
            w[3] = 0x48000000 | (((u32)s_fixStub - (u32)&w[3]) & 0x03FFFFFC);
        }
        DCFlushRange(w, 16);
        ICInvalidateRange(w, 16);
        return w;
    }

    class ThunkApi {
    public:
        virtual void syInlineHook(const u32 address, const void* replacement)
        {
            s_api->syInlineHook(address, thunk(replacement, true));
        }
        virtual void syInlineHookRel(const u32 offset, const void* replacement, int moduleId)
        {
            s_api->syInlineHookRel(offset, thunk(replacement, true), moduleId);
        }
        virtual void sySimpleHook(const u32 address, const void* replacement)
        {
            const void* t = thunk(replacement);
            s_api->sySimpleHook(address, t);
            pointAtThunk(address, t);
        }
        virtual void sySimpleHookRel(const u32 offset, const void* replacement, int moduleId)
        {
            s_api->sySimpleHookRel(offset, thunk(replacement), moduleId);
        }
        virtual void syReplaceFunc(const u32 address, const void* replacement, void** original)
        {
            const void* t = thunk(replacement);
            s_api->syReplaceFunc(address, t, original);
            pointAtThunk(address, t);
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

    // A build without Brawl's Network heap (REX 0.5's RAM-EX.asm "Destroy Network Heap" gives its
    // 1.3 MB to the fighters) gets one in the MEM2 that real IOS keeps for itself: from mem2_end
    // (OS globals 0x80003120, 0x93600000 under Brawl's IOS) to the end of MEM2 (0x80003118, its
    // size). Dolphin's IOS runs on the host and never touches it (measured: all zero after boot);
    // the game's arena ends below it (0x80003128). It is registered as heap 6, so the plugin and
    // Brawl's own Wi-Fi screens find it as on P+. The first 0x20000 (published as "IOS reserved",
    // 0x80003148..0x8000314C) is left alone. Only under Dolphin: on a Wii, IOS lives there.
    const u32 NETWORK_HEAP_SIZE = 0x143800;   // P+'s (heap table 0x804218AC)
    const u32 IOS_RESERVED_SKIP = 0x20000;
    static void* networkHeap()
    {
        void* heap = gfHeapManager::getHeap(Heaps::Network);
        if (heap) return heap;
        u32 mem2Size = *(u32*)0x80003118;
        u32 mem2End = *(u32*)0x80003120;
        u32 arenaEnd = *(u32*)0x80003128;
        u32 start = mem2End + IOS_RESERVED_SKIP;
        if (mem2End < 0x90000000 || arenaEnd > mem2End || mem2Size > 0x10000000 ||
            start + NETWORK_HEAP_SIZE > 0x90000000 + mem2Size) {
            OSReport("[PPOnline] no room for a Network heap (MEM2 %08x, end %08x, arena end %08x)\n",
                     mem2Size, mem2End, arenaEnd);
            return 0;
        }
        static const char NAME[] = "Network";
        gfMemoryPool* pool = gfMemoryPool::create((void*)start, NETWORK_HEAP_SIZE, NAME);
        if (!pool) return 0;
        g_HeapInfos[Heaps::Network].m_name = NAME;
        g_HeapInfos[Heaps::Network].m_memoryPool = pool;
        g_HeapInfos[Heaps::Network].m_size = NETWORK_HEAP_SIZE;
        g_HeapInfos[Heaps::Network].m_arena = 1;
        OSReport("[PPOnline] no Network heap: made one at %08x, %x bytes\n", start, NETWORK_HEAP_SIZE);
        return pool;
    }

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
        void* heap = networkHeap();
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

namespace Loader {
    // This module off the OS module list (OSModuleInfo: id +0, next +4, prev +8; the list's head
    // at 0x800030C8 and tail at 0x800030CC): see the top of this file. Its memory stays allocated
    // (sy_core holds the module and its PluginMeta, and reads neither after the plugins are loaded).
    const u32 LOADER_REL_ID = 20561;   // the Makefile's RELID
    static void leaveModuleList()
    {
        u32* head = (u32*)0x800030C8;
        u32* tail = (u32*)0x800030CC;
        int n = 0;
        for (u32 m = *head; m >= 0x80000000 && n < 128; m = *(u32*)(m + 4), n++) {
            if (*(u32*)m != LOADER_REL_ID) continue;
            u32 next = *(u32*)(m + 4), prev = *(u32*)(m + 8);
            if (prev) *(u32*)(prev + 4) = next; else *head = next;
            if (next) *(u32*)(next + 8) = prev; else *tail = prev;
            *(u32*)(m + 4) = 0;
            *(u32*)(m + 8) = 0;
            return;
        }
        OSReport("[PPOnline] the loader is not on the module list\n");
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
        Loader::leaveModuleList();
        return &META;
    }
    void _epilog() {}
    void _unresolved() {}
}
