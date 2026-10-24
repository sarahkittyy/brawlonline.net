/**
 * L+R+Z clips (Project Clippi's button-input highlights; DESIGN.md, 2026-10-20): a player pressing
 * L, R and Z together, for a frame or more, marks a clip of the CLIP_BEFORE frames before the hold and
 * CLIP_AFTER after it, and the next one counts only LOCKOUT frames later. Found after the game in
 * the recorded pads, so every replay's presses count, those made before this was written too:
 * nothing runs in the match, and the game shows nothing.
 *
 * A press meant as a clip is a tap: the user's 7 in 122 replays (2026-10-24) were down 6 to 16
 * frames, none 30 (half a second, the first threshold, which found none of them). Any press counts
 * (the user, 2026-10-24: "i can't imagine an actual game situation where you press all 3 and dont
 * intend to clip"); in those replays nothing else had the three down at all, and over the first
 * research's 167 replays they were down together in play 5 times, 4 frames at most.
 */
import type { Timeline } from "./replay_frames";
import { PAD_KEPT } from "./replay_frames";
import { CLIP_BEFORE_SECONDS } from "./types";

// gfPadStatus +0x0 (u32, big-endian): the low byte holds L, R and Z.
const L = 0x40;
const R = 0x20;
const Z = 0x10;
const LRZ = L | R | Z;
export const HOLD_FRAMES = 1;
// Project Clippi's defaults: 20 s before, 2 s after, a 5 s lockout.
export const CLIP_BEFORE = CLIP_BEFORE_SECONDS * 60;
export const CLIP_AFTER = 2 * 60;
export const LOCKOUT = 5 * 60;

export type LrzClip = {
  player: number; // timeline player index
  /** Timeline index of the frame the hold reached HOLD_FRAMES. */
  frame: number;
  start: number;
  end: number;
};

/** The longest L+R+Z hold of a player, in frames (for checking the threshold against real games). */
export function longestLrzHold(t: Timeline, player: number): number {
  const pads = t.pads[player];
  let run = 0;
  let best = 0;
  for (let f = 0; f < t.frames; f++) {
    run = (pads[f * PAD_KEPT + 3] & LRZ) === LRZ ? run + 1 : 0;
    best = Math.max(best, run);
  }
  return best;
}

/** The clips of the given players (timeline indices), in frame order. */
export function findLrzClips(t: Timeline, players: readonly number[]): LrzClip[] {
  const out: LrzClip[] = [];
  for (const player of players) {
    const pads = t.pads[player];
    let run = 0;
    let lockedUntil = -1;
    for (let f = 0; f < t.frames; f++) {
      run = (pads[f * PAD_KEPT + 3] & LRZ) === LRZ ? run + 1 : 0;
      if (run === HOLD_FRAMES && f >= lockedUntil) {
        out.push({
          player,
          frame: f,
          start: Math.max(f - HOLD_FRAMES + 1 - CLIP_BEFORE, 0),
          end: Math.min(f + CLIP_AFTER, t.frames - 1),
        });
        lockedUntil = f + LOCKOUT;
      }
    }
  }
  return out.sort((a, b) => a.frame - b.frame);
}
