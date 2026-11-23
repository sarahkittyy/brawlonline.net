/**
 * A replay's stars on the playback Dolphin's seek bar when it is played as a replay (not from a
 * highlights list): every highlight found in it, every hotkey (L+R+Z) clip and every manual clip
 * of it (Sarah's board, 2026-11-20: "all of the stars should be there for all highlights, hotkeys,
 * and manual clips"). A star plays its clip in the running match and pauses at its end
 * (Core/Rollback/ReplayPlayback.h, `marks`).
 */
import type { PlaybackMark } from "@dolphin/types";

import type { ManualClip, ReplayHighlights } from "./types";

/** As many as Dolphin takes (ReplayPlayback.cpp MAX_MARKS). */
export const MAX_REPLAY_MARKS = 500;

/** `path`'s stars: its highlights, hotkey clips and manual clips, by where they start, each once. */
export function replayMarks(
  path: string,
  highlights: Pick<ReplayHighlights, "highlights" | "clips"> | null,
  manualClips: readonly Pick<ManualClip, "path" | "startFrame" | "endFrame">[],
): PlaybackMark[] {
  const seen = new Set<string>();
  const out: PlaybackMark[] = [];
  const add = (startFrame: number, endFrame: number) => {
    if (!Number.isFinite(startFrame) || !Number.isFinite(endFrame) || endFrame < startFrame || startFrame < 0) {
      return;
    }
    const key = `${startFrame}|${endFrame}`;
    if (seen.has(key)) {
      return;
    }
    seen.add(key);
    out.push({ path, startFrame, endFrame, position: 0 });
  };
  for (const h of highlights?.highlights ?? []) {
    add(h.startFrame, h.endFrame);
  }
  for (const c of highlights?.clips ?? []) {
    add(c.startFrame, c.endFrame);
  }
  for (const c of manualClips) {
    if (c.path === path) {
      add(c.startFrame, c.endFrame);
    }
  }
  return out.sort((a, b) => a.startFrame - b.startFrame || a.endFrame - b.endFrame);
}

/** Every replay's of a request, up to Dolphin's limit (a replay whose highlights can't be read has its manual clips). */
export async function requestMarks(
  paths: readonly string[],
  getHighlights: (path: string) => Promise<ReplayHighlights>,
  manualClips: readonly ManualClip[],
): Promise<PlaybackMark[]> {
  const out: PlaybackMark[] = [];
  for (const path of [...new Set(paths)]) {
    const found = await getHighlights(path).catch(() => null);
    out.push(...replayMarks(path, found, manualClips));
    if (out.length >= MAX_REPLAY_MARKS) {
      break;
    }
  }
  return out.slice(0, MAX_REPLAY_MARKS);
}
