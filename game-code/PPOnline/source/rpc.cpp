// Reverse-engineering aid: the harness writes a call (function address and arguments) into
// g_pponlineRpc and the next tick runs it on the game's main thread, keeping the result. Only
// while DEBUG cfg has CFG_TEST_RPC; tools/gamecode/rpc.py drives it (g_pponlineRpc's address comes
// from the plugin's map file and the OS module list).
#include "ppom.h"
#include "online_menu.h"

extern "C" {
    struct PPOnlineRpc {
        u32 seq;          // harness: bumped to ask for a call
        u32 done;         // plugin: the seq of the last call made
        u32 fn;
        u32 args[8];      // r3..r10
        float fargs[4];   // f1..f4
        u32 ret;          // r3 after the call
        u32 poke[4][2];   // {address, byte}: written every tick while set (address 0 = unused)
    };
    PPOnlineRpc g_pponlineRpc = {0, 0, 0, {0}, {0}, 0, {{0}}};
}

namespace Rpc {
    typedef u32 (*Fn)(u32, u32, u32, u32, u32, u32, u32, u32, float, float, float, float);
    // DEBUG cfg bit (the harness's; not part of the Dolphin contract).
    const u32 CFG_TEST_RPC = 1u << 8;

    void tick()
    {
        PPOnlineRpc& r = g_pponlineRpc;
        if (!(PPOM::g_block.debug.cfg & CFG_TEST_RPC)) return;
        for (int i = 0; i < 4; i++) {
            u32 a = r.poke[i][0];
            if ((a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000)) *(u8*)a = (u8)r.poke[i][1];
        }
        if (r.seq == r.done) return;
        u32 fn = r.fn;
        if ((fn >= 0x80000000 && fn < 0x81800000) || (fn >= 0x90000000 && fn < 0x94000000)) {
            const u32* a = r.args;
            const float* f = r.fargs;
            r.ret = ((Fn)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], f[0], f[1], f[2], f[3]);
        }
        r.done = r.seq;
    }
}
