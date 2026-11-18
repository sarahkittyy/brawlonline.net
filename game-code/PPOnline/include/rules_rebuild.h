#pragma once
#include <types.h>
#include "ppom.h"
#include "variant.h"

// The room's rules menu on a build that does not open Brawl's from the character select (REX
// 0.5: its RULES button goes to the main menu's rules, and its character select's archive has no
// rules menu; variant.h PPON_REBUILD_RULES, source/variant/rebuild_rules.cpp). There the menu is
// made the way Brawl's openRules makes it, from the build's main-menu archive (loaded through
// Dolphin), with room borrowed for its models. room_rules.cpp calls these; in a plugin built
// without it they say "the character select has its own" and do nothing.

// room_rules.cpp: fn(a0, a1) with every non-volatile register kept around it.
extern "C" void pponline_callPreserving(u32 fn, u32 a0, u32 a1);

namespace RulesRebuild {
    // What RULES opens on a character select (prepare).
    enum Menu {
        MENU_OWN,       // its own archive has the menu: opened the build's own way
        MENU_REBUILT,   // ours is ready: opened with openFn()
        MENU_LOADING,   // ours is on its way: the press is kept, and tick() opens it once here
        MENU_MISSING,   // the build's card has nothing to make it from
        MENU_NONE,      // no menu, and no way to make one on this build
    };
#if PPON_REBUILD_RULES
    // Brawl's openRules made again (called as it: task, mode), or 0 on a build whose pieces of
    // it are not Brawl's.
    u32 openFn();
    Menu prepare(u8* task, bool setOnly);
    // fn(task, 0), with our archive for the menu's pages and the room its models need.
    void call(u32 fn, u8* task);
    // Every frame of the room's character select: the archive asked for as the room comes up,
    // and a RULES pressed meanwhile opened (RoomRules::open) once it is here, if it still may.
    void tick(u8* task, bool inRoom, bool mayOpen);
    void forget();                                   // the CSS is gone: no press is kept
    void onMenuArchive(const PPOM::Response& r);     // Dolphin's answer to CMD_MENU_ARCHIVE
    void taskGone(u8* task);                         // muSelCharTask's destructor: the archive is freed
#else
    static inline u32 openFn() { return 0; }
    static inline Menu prepare(u8*, bool) { return MENU_OWN; }
    static inline void call(u32 fn, u8* task) { pponline_callPreserving(fn, (u32)task, 0); }
    static inline void tick(u8*, bool, bool) {}
    static inline void forget() {}
    static inline void onMenuArchive(const PPOM::Response&) {}
    static inline void taskGone(u8*) {}
#endif
}
