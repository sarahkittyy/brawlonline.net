
#include "TimeSync.h"
#include "VideoCommon/OnScreenDisplay.h"
#include <mutex>
#include <Common/Timer.h>
#include <Common/Logging/Log.h>

// pretty much all of this time sync stuff was taken from slippi
// Huge thanks to them <3
// if need be, this can be reworked so it doesn't resemble slippi so much
namespace Brawlback {

TimeSync::~TimeSync() {

}

TimeSync::TimeSync() {

}

bool TimeSync::shouldStallFrame(s32 currentFrame, s32 latestRemoteFrame, u8 numPlayers, s32 rollbackFrames)
{
    s32 frameDiff = currentFrame - latestRemoteFrame;

    std::stringstream dispStr;
    dispStr << "| Frame diff: " << frameDiff << " |\n";

    // SLIPPI LOGIC
    bool frameDiffCheck;
    if (ROLLBACK_IMPL && currentFrame > GAME_FULL_START_FRAME)
    {
      INFO_LOG_FMT(BRAWLBACK, "ROLLBACK IS ENABLED! FRAMEDIFF: {}", dispStr.str());
      frameDiffCheck = frameDiff >= rollbackFrames;
    }
    else
    {
      INFO_LOG_FMT(BRAWLBACK, "ROLLBACK IS NOT ENABLED! FRAMEDIFF: {}", dispStr.str());
      frameDiffCheck = frameDiff > FRAME_DELAY;
    }

    if (frameDiffCheck)
    {
        this->stallFrameCount += 1;
        if (this->stallFrameCount > 60 * 7) {
            ERROR_LOG_FMT(BRAWLBACK, "CONNECTION STALLED\n");
        }
        ERROR_LOG_FMT(BRAWLBACK, "Clients too far out of sync, stalling. Frame: {} Latest {} diff {}\n", currentFrame, latestRemoteFrame, frameDiff);
        return true;
    }
    this->stallFrameCount = 0;


    // Return true if we are over 60% of a frame ahead of our opponent. Currently limiting how
	// often this happens because I'm worried about jittery data causing a lot of unneccesary delays.
	// Only skip once for a given frame because our time detection method doesn't take into consideration
	// waiting for a frame. Also it's less jarring and it happens often enough that it will smoothly
	// get to the right place
	auto isTimeSyncFrame = currentFrame % ONLINE_LOCKSTEP_INTERVAL == 0; // Only time sync every few frames
	if (isTimeSyncFrame && !this->isSkipping)
	{
		s32 offsetUs = this->calcTimeOffsetUs(numPlayers);
		WARN_LOG_FMT(BRAWLBACK, "[Frame {}] Offset is: {} us", currentFrame, offsetUs);

    s32 frameTime = 16683;
    s32 t1 = 10000;
    s32 t2 = (2 * frameTime) + t1;
		// TODO: figure out a better solution here for doubles?
    if (offsetUs > (currentFrame <= 120 ? t1 : t2))
		{
			this->isSkipping = true;

			int maxSkipFrames = currentFrame <= 120 ? 5 : 1; // On early frames, support skipping more frames
			this->framesToSkip = ((offsetUs - TIMESYNC_MAX_US_OFFSET) / USEC_IN_FRAME) + 1;
      INFO_LOG_FMT(BRAWLBACK, "Unclamped framesToSkip {}", this->framesToSkip);
			this->framesToSkip = this->framesToSkip > maxSkipFrames ? maxSkipFrames : this->framesToSkip; // Only skip 5 frames max

			WARN_LOG_FMT(BRAWLBACK, "Halting on frame {} due to time sync. Offset: {} us. framesToSkip: {}", currentFrame,
			         offsetUs, this->framesToSkip);
		}
	}

	// Handle the skipped frames
	if (this->framesToSkip > 0)
	{
		// If ahead by 60{}of a frame, stall. I opted to use 60{}instead of half a frame
		// because I was worried about two systems continuously stalling for each other
		this->framesToSkip -= 1;
		return true;
	}

	this->isSkipping = false;

	return false;
}

void TimeSync::startGame(u8 numPlayers)
{
  for (int i = 0; i < numPlayers; i++)
  {
    FrameTiming timing;
    timing.frame = 0;
    timing.timeUs = Common::Timer::NowUs();
    this->lastFrameTimings[i] = timing;
    this->lastFrameAcked[i] = 0;

    // Reset ack timers
    this->ackTimers[i].clear();
  }
}

// called when sending inputs
void TimeSync::TimeSyncUpdate(u32 frame, u8 numPlayers) { // frame with delay
    u64 currentTime = Common::Timer::NowUs();
    {   // store the time that we sent framedata
        std::lock_guard<std::mutex> lock(this->ackTimersMutex);
        for (int i = 0; i < numPlayers; i++) {
            FrameTiming timing;
            timing.frame = frame;
            timing.timeUs = currentTime;

            this->lastFrameTimings[i] = timing;
            this->ackTimers[i].push_back(timing);
        }
    }
}


// getting frame with delay
void TimeSync::ReceivedRemoteFramedata(s32 frame, u8 localPlayerIdx, bool hasGameStarted)
{
  s64 curTime = (s64)Common::Timer::NowUs();
  // update frame timing/offsets for time sync logic

  // Pad received, try to guess what our local time was when the frame was sent by our opponent
  // before we initialized
  // We can compare this to when we sent a pad for last frame to figure out how far/behind we
  // are with respect to the opponent

  // SLIPPI LOGIC
  auto timing = this->lastFrameTimings[localPlayerIdx];
  if (!hasGameStarted)
  {
    // Handle case where opponent starts sending inputs before our game has reached frame 1. This
    // will continuously say frame 0 is now to prevent opp from getting too far ahead
    timing.frame = 0;
    timing.timeUs = curTime;
  }

  s64 opponentSendTimeUs = curTime - ((s64)this->pingUs[localPlayerIdx] / 2);
  s64 frameDiffOffsetUs = USEC_IN_FRAME * (timing.frame - frame);
  s64 timeOffsetUs = opponentSendTimeUs - timing.timeUs + frameDiffOffsetUs;

  if (hasGameStarted)
  {
    INFO_LOG_FMT(BRAWLBACK, "[Offset] Opp Frame: {}, My Frame: {}. Time offset: {} ms\n", frame,
                 timing.frame, (double)timeOffsetUs / 1000.0);
  }

  // Add this offset to circular buffer for use later

  // frameOffsetData is being treated as a "circular buffer". Hence this logic here
  if (this->frameOffsetData[localPlayerIdx].buf.size() < ONLINE_LOCKSTEP_INTERVAL)
  {
    this->frameOffsetData[localPlayerIdx].buf.push_back((s32)timeOffsetUs);
  }
  else
  {
    this->frameOffsetData[localPlayerIdx].buf[this->frameOffsetData[localPlayerIdx].idx] =
        (s32)timeOffsetUs;
  }

  this->frameOffsetData[localPlayerIdx].idx =
      (this->frameOffsetData[localPlayerIdx].idx + 1) & ONLINE_LOCKSTEP_INTERVAL;
}



// with delay
void TimeSync::ProcessFrameAck(FrameAck* frameAck)
{
  std::lock_guard<std::mutex> lock(this->ackTimersMutex);
  u8 localPlayerIdx = frameAck->playerIdx;  // local player idx
  int frame = frameAck->frame;              // this is with frame delay

  // SLIPPI LOGIC
  this->lastFrameAcked[localPlayerIdx] = frame;

  // remove old timings
  while (!this->ackTimers[localPlayerIdx].empty() &&
         this->ackTimers[localPlayerIdx].front().frame < frame)
  {
    this->ackTimers[localPlayerIdx].pop_front();
  }

  // don't get a ping if we don't have correct ack frame
  if (this->ackTimers[localPlayerIdx].empty())
  {
    INFO_LOG_FMT(BRAWLBACK, "Empty acktimers\n");
    return;
  }
  if (this->ackTimers[localPlayerIdx].front().frame != frame)
  {
    INFO_LOG_FMT(BRAWLBACK, "frontframe and acked frame not equal\n");
    return;
  }

  auto sendTime = this->ackTimers[localPlayerIdx].front().timeUs;
  this->ackTimers[localPlayerIdx].pop_front();

  // our ping is the current gametime - the time that the inputs were originally sent at
  // inputs go from client 1 -> client 2 -> client 2 acks & sends ack to client 1 -> client 1
  // receives ack here so this is full RTT (round trip time).
  u64 curTime = Common::Timer::NowUs();
  this->pingUs[localPlayerIdx] = curTime - sendTime;
  u64 rtt = this->pingUs[localPlayerIdx];
  double rtt_ms = (double)rtt / 1000.0;

  INFO_LOG_FMT(BRAWLBACK, "Received ack for frame {} (w/o delay: {})  [pIdx {} rtt {} ms]\n", frame,
               frame - FRAME_DELAY, (unsigned int)localPlayerIdx, rtt_ms);

  if (frame % PING_DISPLAY_INTERVAL == 0)
  {
    std::stringstream dispStr;
    dispStr << "Ping (rtt): " << (int)rtt_ms << " ms\n";
    // dispStr << "Time offset: " << (double)this->calcTimeOffsetUs(2) / 1000 << " ms\n";
    dispStr << "Frame delay: " << FRAME_DELAY << "\n";
  }
}


int TimeSync::getMinAckFrame(u8 numPlayers) {
    int minAckFrame = 0;
    for (int i = 0; i < numPlayers; i++) {
        //INFO_LOG_FMT(BRAWLBACK, "lastFrameAcked[{}]: {}", i, this->lastFrameAcked[i]);
        if (minAckFrame == 0 || (this->lastFrameAcked[i] < minAckFrame && this->lastFrameAcked[i] != 0))
            minAckFrame = this->lastFrameAcked[i];
    }
    INFO_LOG_FMT(BRAWLBACK, "MIN ACK FRAME: {}\n", minAckFrame);
    return minAckFrame;
}


// SLIPPI LOGIC
// discards highest and lowest offsets, then averages the rest
s32 TimeSync::calcTimeOffsetUs(u8 numPlayers) {
    bool empty = true;
	for (int i = 0; i < numPlayers; i++)
	{
		if (!frameOffsetData[i].buf.empty())
		{
			empty = false;
			break;
		}
	}
	if (empty)
	{
		return 0;
	}

	std::vector<int> offsets;
	for (int pIdx = 0; pIdx < numPlayers; pIdx++)
	{
		if (frameOffsetData[pIdx].buf.empty())
			continue;

		std::vector<s32> buf;
		std::copy(frameOffsetData[pIdx].buf.begin(), frameOffsetData[pIdx].buf.end(), std::back_inserter(buf));

		std::sort(buf.begin(), buf.end());

		int bufSize = (int)buf.size();
		int offset = (int)((1.0f / 3.0f) * bufSize); // takes the "middle third" of the offsets (discards higher-end vals and lower-end vals)
		int end = bufSize - offset;

		int sum = 0;
		for (int i = offset; i < end; i++)
		{
			sum += buf[i];
		}

		int count = end - offset;
		if (count <= 0)
		{
			return 0;
		}

		s32 result = sum / count;
		offsets.push_back(result);
	}

	s32 maxOffset = offsets.front();
	for (int i = 1; i < offsets.size(); i++)
	{
		if (offsets[i] > maxOffset)
			maxOffset = offsets[i];
	}


    INFO_LOG_FMT(BRAWLBACK, "Max time offset: {}\n", maxOffset);
	return maxOffset;
}

}
