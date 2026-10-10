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
        (u16)__builtin_offsetof(Block, session), (u16)sizeof(Session),
        (u16)__builtin_offsetof(Block, local), (u16)sizeof(Local),
        (u16)__builtin_offsetof(Block, debug), (u16)sizeof(Debug),
        {0, 0, 0},
    };

    // The contract with Dolphin (GameBridge.cpp): sizes and the mailbox/LOCAL adjacency.
    static_assert(sizeof(Mailbox) == 0x410, "mailbox");
    static_assert(sizeof(LockIn) == 0x0C, "lock-in");
    static_assert(sizeof(PortValues) == 0x3C, "port values");
    static_assert(sizeof(Local) == 0x80, "local");
    static_assert(__builtin_offsetof(Local, lockPad) == 0x35, "lock pad");
    static_assert(sizeof(SessionPlayer) == 0x80, "session player");
    static_assert(sizeof(Session) == 0x220, "session");
    static_assert(__builtin_offsetof(Session, gone) == 0x20C, "session gone flags");
    static_assert(__builtin_offsetof(SessionPlayer, picksStage) == 0x36, "session player stage pick");
    static_assert(__builtin_offsetof(Local, lockTeam) == 0x34, "lock-in team");
    // v5: rooms (docs/rooms-game-interface.md), all in v4's spare bytes.
    static_assert(__builtin_offsetof(Local, screen) == 0x36 && __builtin_offsetof(Local, roomJoin) == 0x37 &&
                  __builtin_offsetof(Local, room) == 0x38 && __builtin_offsetof(Local, own) == 0x40, "local rooms");
    static_assert(__builtin_offsetof(SessionPlayer, roomSlot) == 0x38 && __builtin_offsetof(SessionPlayer, roomTeam) == 0x39 &&
                  __builtin_offsetof(SessionPlayer, roomChar) == 0x3A && __builtin_offsetof(SessionPlayer, roomCostume) == 0x3B &&
                  __builtin_offsetof(SessionPlayer, pv) == 0x40, "session player rooms");
    static_assert(__builtin_offsetof(Session, roomFlags) == 0x213 && __builtin_offsetof(Session, roomCode) == 0x214 &&
                  __builtin_offsetof(Session, roomHost) == 0x218 && __builtin_offsetof(Session, roomStatus) == 0x219 &&
                  __builtin_offsetof(Session, roomMode) == 0x21A, "session rooms");
    static_assert(sizeof(RoomRequest) == 0x18 && sizeof(RoomRequest) <= REQ_PAYLOAD, "room request");
    static_assert(sizeof(RoomStatus) == 0x88 && sizeof(RoomStatus) <= RESP_PAYLOAD, "room status");
    static_assert(__builtin_offsetof(Block, local) == __builtin_offsetof(Block, mailbox) + sizeof(Mailbox), "local after mailbox");

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
        asm volatile("" ::: "memory");   // (built with -Os: keep the stores in this order)
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

    static bool samePv(const PortValues& a, const PortValues* b)
    {
        const u8* x = (const u8*)&a;
        const u8* y = (const u8*)b;
        for (u32 i = 0; i < sizeof(PortValues); i++) {
            if (x[i] != (b ? y[i] : 0)) return false;
        }
        return true;
    }

    void writeLockIn(bool ready, u8 cssChar, u8 charKind, u8 costume, u16 stagePick, u8 asl, u8 game,
                     const PortValues* pv, u8 pad)
    {
        LockIn& l = g_block.local.lockIn;
        PortValues& own = g_block.local.own;
        if (l.seq && l.ready == (ready ? 1 : 0) && l.cssChar == cssChar && l.charKind == charKind &&
            l.costume == costume && l.stagePick == stagePick && l.asl == asl && l.game == game &&
            samePv(own, pv) && g_block.local.lockPad == pad) {
            return;
        }
        g_block.local.lockPad = pad;
        flushRange(&g_block.local.lockPad, 1);
        if (pv) memcpy(&own, (void*)pv, sizeof(own));
        else memset(&own, 0, sizeof(own));
        flushRange(&own, sizeof(own));
        l.ready = ready ? 1 : 0;
        l.cssChar = cssChar;
        l.charKind = charKind;
        l.costume = costume;
        l.stagePick = stagePick;
        l.asl = asl;
        l.game = game;
        asm volatile("" ::: "memory");
        l.seq++;     // written last
        flushRange(&l, sizeof(l));
    }

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
