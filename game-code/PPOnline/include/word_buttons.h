#pragma once
#include <mu/mu_msg.h>
#include <sy_core.h>
#include <types.h>
#include "variant.h"

// The room's four buttons as words in the character select's bar, on a build whose character
// select has none of Brawl's online art (REX 0.5: no textures or rect on ITEM and STAGE, none of
// the online label textures; variant.h PPON_WORD_BUTTONS, source/variant/word_buttons.cpp).
// RULES, PUBLIC / FRIENDS / PRIVATE, SIT OUT / PLAY and FFA / TEAMS are printed in windows 4-7 of
// the rule line's text object, white, yellow under a hand, grey when disabled, at the places of
// the build's layout (layout.h g_cssTextButtons), and the hand finds them by those rects.
// rules_button.cpp, which makes the buttons as models everywhere else, asks here first; in a
// plugin built without this, on() is false and the rest does nothing.
namespace WordButtons {
    enum { RULES, PUBLIC, SIT_OUT, FFA, WORDS };   // (PUBLIC and FFA: Brawl's ITEM and STAGE)
#if PPON_WORD_BUTTONS
    // This character select has no online art: its buttons are words.
    bool on(u8* task);
    bool artless();                    // ... the character select that is up now
    // A word's rect for the hand: left, right, top, bottom, in hand units.
    const float* rect(int word);
    // RULES or SIT OUT as rules_button.cpp has it this frame (each frame, before tick): shown,
    // enabled, its second word (PLAY), and its counter of times made (the words first shown).
    void button(int word, u8* task, bool show, bool enabled, bool state, u32* made);
    // PUBLIC and FFA as the room has them (online_menu.cpp roomButtons): shown, and which word
    // (visibility 0 PRIVATE, 1 PUBLIC, 2 FRIENDS; teams 0 FFA, 1 rainbow TEAMS, 2 TEAMS).
    void header(bool show, int visibility, int teams);
    // The bar's text object this frame (online_menu.cpp, before the buttons' ticks): the rule
    // line's MuMsg, whether the character select has printed its rule line (the windows can be
    // attached), and that window's y1 / y2.
    void bar(MuMsg* msg, bool haveWindow, float y1, float y2);
    // The words, every frame after both buttons' ticks.
    void tick();
    // The hand button under pos (hand units), its rect into `rect` when given; 0 for none.
    int at(u8* task, const float* pos, float* rect);
    void install(CoreApi* api);        // the hand is on a word, not over the grid
    void taskGone(u8* task);           // muSelCharTask's destructor
#else
    static inline bool on(u8*) { return false; }
    static inline bool artless() { return false; }
    static inline const float* rect(int) { return 0; }
    static inline void button(int, u8*, bool, bool, bool, u32*) {}
    static inline void header(bool, int, int) {}
    static inline void bar(MuMsg*, bool, float, float) {}
    static inline void tick() {}
    static inline int at(u8*, const float*, float*) { return 0; }
    static inline void install(CoreApi*) {}
    static inline void taskGone(u8*) {}
#endif
}
