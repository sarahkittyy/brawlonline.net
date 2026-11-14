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
 *
 * A press after the match's last frame, while the game still shows its end (GAME!, before it goes
 * back to the CSS), clips the match's last CLIP_BEFORE frames (Sarah's board, 2026-11-14: "a
 * hotkey press in between the game ending and returning to css clips the last N seconds of the
 * match like normal"). Those buttons are not in the match's pads: Dolphin keeps the recording
 * player's after the end and adds them to the file (Timeline.after, the AFTR chunk). The hold
 * and the lockout run on across the end, so a press held over it or right after a clip near the
 * end makes no second clip.
 */
import { DEFAULT_CLIP_BUTTONS } from "@common/clip_hotkey";

import type { Timeline } from "./replay_frames";
import { PAD_KEPT } from "./replay_frames";
import { CLIP_BEFORE_SECONDS } from "./types";

// gfPadStatus +0x0 (u32, big-endian): L, R and Z are in the low byte. Since 2026-11-03 the player
// sets the buttons (Settings > Clips > Manual Clip Hotkey, @common/clip_hotkey): any of the low 13
// bits, so the low two bytes.
const LRZ = DEFAULT_CLIP_BUTTONS;
const held = (pads: Uint8Array, f: number) => (pads[f * PAD_KEPT + 2] << 8) | pads[f * PAD_KEPT + 3];
export const HOLD_FRAMES = 1;
// Project Clippi's defaults: 20 s before, 2 s after, a 5 s lockout.
export const CLIP_BEFORE = CLIP_BEFORE_SECONDS * 60;
export const CLIP_AFTER = 2 * 60;
export const LOCKOUT = 5 * 60;

export type LrzClip = {
  player: number; // timeline player index
  /** Timeline index of the frame the hold reached HOLD_FRAMES (the last frame for a press after the end). */
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

/** The clips of the given players (timeline indices), in frame order: presses of `buttons` (all held together). */
export function findLrzClips(t: Timeline, players: readonly number[], buttons: number = LRZ): LrzClip[] {
  const out: LrzClip[] = [];
  if (buttons === 0) {
    return out;
  }
  const last = t.frames - 1;
  for (const player of players) {
    const pads = t.pads[player];
    const after = t.after && t.after.port === t.players[player]?.port ? t.after.buttons : null;
    let run = 0;
    let lockedUntil = -1;
    for (let f = 0; f < t.frames + (after?.length ?? 0); f++) {
      const down = f < t.frames ? held(pads, f) : (after as Uint16Array)[f - t.frames];
      run = (down & buttons) === buttons ? run + 1 : 0;
      if (run === HOLD_FRAMES && f >= lockedUntil) {
        if (f <= last) {
          out.push({
            player,
            frame: f,
            start: Math.max(f - HOLD_FRAMES + 1 - CLIP_BEFORE, 0),
            end: Math.min(f + CLIP_AFTER, last),
          });
        } else if (last >= 0) {
          // After the end: the match's last frames, as a press on its last frame.
          out.push({ player, frame: last, start: Math.max(last - CLIP_BEFORE, 0), end: last });
          break;
        }
        lockedUntil = f + LOCKOUT;
      }
    }
  }
  return out.sort((a, b) => a.frame - b.frame);
}
