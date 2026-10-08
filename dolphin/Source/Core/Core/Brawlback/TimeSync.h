#pragma once
#include "include/brawlback-common/BrawlbackConstants.h"
#include <deque>

constexpr float MS_IN_FRAME = 1000 / 60;
constexpr s32 USEC_IN_FRAME = MS_IN_FRAME * 1000;

namespace Brawlback {

struct FrameAck
{
  u8 playerIdx;
  u32 frame;
};
struct FrameOffsetData
{
  int idx;
  std::vector<s32> buf;
};
struct FrameTiming
{
  int frame;
  s64 timeUs;
};
class TimeSync {

public:

    TimeSync();
    ~TimeSync();


    // ---- Funcs that are called by game events

    // called right after sending local inputs over net
    void TimeSyncUpdate(u32 frame, u8 numPlayers);

    // called when we receive remote inputs
    void ReceivedRemoteFramedata(s32 frame, u8 playerIdx, bool hasGameStarted);

    // called when we receive an acknowledgement from opponent of our inputs
    void ProcessFrameAck(FrameAck* frameAck);

    // --------------------------------------------

    // this is the backbone of the time sync logic. Returns whether or not we should stall on *this* current frame.
    bool shouldStallFrame(s32 currentFrame, s32 latestRemoteFrame, u8 numPlayers,
                          s32 rollbackFrames);

    void startGame(u8 numPlayers);

    int getMinAckFrame(u8 numPlayers);

    bool getIsConnectionStalled() { return isConnectionStalled; }

    s32 calcTimeOffsetUs(u8 numPlayers);
private:

    FrameOffsetData frameOffsetData[MAX_NUM_PLAYERS];

    int stallFrameCount = 0;
    bool isConnectionStalled = false;

    bool isSkipping = false;
    int framesToSkip = 0;

    int lastFrameAcked[MAX_NUM_PLAYERS] = {};
    FrameTiming lastFrameTimings[MAX_NUM_PLAYERS] = {};
    std::array<std::deque<FrameTiming>, MAX_NUM_PLAYERS> ackTimers = {};
    u64 pingUs[MAX_NUM_PLAYERS] = {};
    
    std::mutex ackTimersMutex;

};
}
