#pragma once
#include "BrawlbackTypes.h"
// Doesn't look like we use some of these anywhere, keeping commented out for archiving purposes.

/* ======== Rollback ======== */
const bs32 MAX_ROLLBACK_FRAMES = 5;
const bs32 MAX_SAVESTATES = MAX_ROLLBACK_FRAMES + 2;
const bs32 FRAME_DELAY = 1;
static_assert(FRAME_DELAY >= 1, "Frame Delay isn't large enough.");
static_assert(FRAME_DELAY + MAX_ROLLBACK_FRAMES >= 6, "Sum of frame delay and max rollback frames isn't large enough."); // minimum frames of "compensation"
const bool ROLLBACK_IMPL = true;

/* ======== General Netplay ======== */
// number of max FrameData's to keep in the (remote) queue
const bs32 FRAMEDATA_MAX_QUEUE_SIZE = 15; 
static_assert(FRAMEDATA_MAX_QUEUE_SIZE > MAX_ROLLBACK_FRAMES, "Framedata Max Queue Size isn't large enough (must be larger than max rollback frames).");
const bs32 GAME_START_FRAME = 0;
// before this frame we basically use delay-based netcode to ensure things are reasonably synced up before doing rollback stuff
const bs32 GAME_FULL_START_FRAME = 150;
// constexpr bs32 MAX_REMOTE_PLAYERS = 3;
const bs32 MAX_NUM_PLAYERS = 4;
// constexpr bs32 BRAWLBACK_PORT = 7779;

/* ======== Timesync ======== */
// update ping display every X frames
const bs32 PING_DISPLAY_INTERVAL = 30;
// check clock desynchronization every X frames
const bs32 ONLINE_LOCKSTEP_INTERVAL = 30;
const bs32 TIMESYNC_MAX_US_OFFSET = 10000; // 60% of a frame

/* ======== Debug Toggles ======== */
// #define SYNCLOG
// #define RANDOM_INPUTS

/* ======== Utils ======== */
// #define MS_IN_FRAME (1000 / 60)
// #define USEC_IN_FRAME (MS_IN_FRAME*1000)
// #define MS_TO_FRAMES(ms) (ms * 60 / 1000)
// #define FRAMES_TO_MS(frames) (1000 * frames / 60)
