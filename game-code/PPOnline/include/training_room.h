#pragma once
#include <sy_core.h>
#include <types.h>

class MuMsg;

// The waiting room (docs/game-code.md, "The waiting room"): while Unranked or Ranked searches,
// the player plays in Brawl's own online waiting room, the Online Training stage that the Wi-Fi
// sequence (sqNetAnyOkiraku state 5) sets up for its "With Anyone" search, with a Sandbag in the
// middle and the search status in the room's scrolling banner. The room ends with its fade once
// the match is set up (Unranked: straight into the match), once the opponent is connected (Ranked:
// the CSS, for the game setup steps), or when the player cancels (START, or hold B: the CSS).
namespace TrainingRoom {
    enum End { END_NONE = 0, END_MATCH = 1, END_CSS = 2 };

    void install(CoreApi* api);

    // The online CSS leaves for the room (the search has started).
    void enter();
    // Between enter() and the room's end being handled by the sequence (the CSS has left, or the
    // room runs or fades out).
    bool active();
    // The game is somewhere the room cannot be (not its scMelee or a load screen): forget it.
    void abandon();
    // In the room's scMelee (set up, not ended yet): the room's own tick runs.
    bool running();
    // End the room with its fade: END_MATCH goes on to the match (OnlineMatch's setup, armed by
    // the caller), END_CSS back to the online CSS. The first call counts.
    void end(int how);
    // Every frame in the room, after OnlineMenu's own (the Sandbag, the banner, the room's end).
    void tick();
    // The banner's text; printed when it changes.
    void setBanner(const char* text, bool red);
    // Every drawn frame (MatchHud's frame hook): the alpha of the black over the screen that
    // covers the loads into and out of the room (0 none, 255 black).
    u8 fadeAlpha();
    // The fade out for the room is complete: the CSS may leave (its last frame is black).
    bool screenBlack();

    // sqNetAnyOkiraku state 3 (OnlineMatch::onSelStage): the CSS left for the room: the room's
    // setup (state 5) instead of the stage vote. 1: state set.
    int onSelStage(u8* seq);

    // The printIndex hook: the room's banner line; returns a replacement or NULL.
    const char* line(MuMsg* msg, u32 window, u32 line, const void* msbin);
}
