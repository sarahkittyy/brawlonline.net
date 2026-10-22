import { readReplayHeader } from "../replay_format";
import { readTimeline } from "./replay_frames";
import type { HighlightPlayItem } from "./types";

/**
 * A whole match as one clip (the replays list's Render to video): from its first frame recorded to
 * its last, the frames playback seeks. From ENDG, else (a game that crashed has none) from the
 * frames themselves.
 */
export async function matchFrames(fullPath: string): Promise<HighlightPlayItem> {
  const { end } = await readReplayHeader(fullPath);
  if (end && end.firstFrame >= 0 && end.lastFrame >= end.firstFrame) {
    return { path: fullPath, startFrame: end.firstFrame, endFrame: end.lastFrame };
  }
  const t = await readTimeline(fullPath);
  if (t.frames <= 0) {
    throw new Error("This replay has no frames to render.");
  }
  return { path: fullPath, startFrame: t.firstFrame, endFrame: t.firstFrame + t.frames - 1 };
}
