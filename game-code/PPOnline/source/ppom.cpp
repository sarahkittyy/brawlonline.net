#include "ppom.h"
#include <memory.h>
#include <stddef.h>

namespace PPOM {

    // Aggregate-initialised so it lives in .data with its header already set: the harness can
    // find it before the plugin's prolog has run any code.
    Block g_block __attribute__((aligned(32))) = {
        MAGIC,
        VERSION,
        (u16)sizeof(Block),
        (u16)__builtin_offsetof(Block, mailbox), (u16)sizeof(Mailbox),
        0, 0,
        0, 0,
        (u16)__builtin_offsetof(Block, debug), (u16)sizeof(Debug),
        {0, 0, 0},
    };

    static u32 s_lastSeq = 0;
    static const Response* s_last = NULL;

    static void flushRange(void* p, u32 n)
    {
        // Dolphin's memory interface is coherent with the emulated CPU unless the (off by
        // default) write-back cache emulation is on; dcbst keeps us correct even then.
        u32 a = (u32)p & ~31;
        u32 end = (u32)p + n;
        for (; a < end; a += 32) {
            asm volatile("dcbst 0, %0" : : "r"(a) : "memory");
        }
        asm volatile("sync" ::: "memory");
    }

    u32 post(u8 cmd, const void* payload, u32 size)
    {
        Mailbox& mb = g_block.mailbox;
        u32 seq = ++s_lastSeq;
        Request& r = mb.req[(seq - 1) % REQ_SLOTS];
        if (size > REQ_PAYLOAD) {
            size = REQ_PAYLOAD;
        }
        memset(&r, 0, sizeof(r));
        r.cmd = cmd;
        if (payload && size) {
            memcpy(r.payload, (void*)payload, size);
        }
        r.seq = seq; // written last: a reader that sees seq sees the whole request
        mb.reqWrite = seq;
        flushRange(&mb, sizeof(mb));
        return seq;
    }

    const Response* pollResponse()
    {
        Mailbox& mb = g_block.mailbox;
        u32 n = *(volatile u32*)&mb.respCount;
        if (n == mb.respSeen) {
            return NULL;
        }
        mb.respSeen = n;
        flushRange(&mb.respSeen, 4);
        s_last = &mb.resp;
        return s_last;
    }

    const Response* lastResponse() { return s_last; }

    void asciiToU16(u16* dst, const char* src, int max)
    {
        int i = 0;
        for (; i < max - 1 && src[i]; i++) {
            dst[i] = (u8)src[i];
        }
        for (; i < max; i++) {
            dst[i] = 0;
        }
    }

    void u16ToAscii(char* dst, const u16* src, int max)
    {
        int i = 0;
        for (; i < max - 1 && src[i]; i++) {
            u16 c = src[i];
            if (c == 0xFF03) {
                c = '#'; // full-width number sign, as Slippi uses
            }
            dst[i] = (c < 0x80) ? (char)c : '?';
        }
        dst[i] = 0;
    }
}
