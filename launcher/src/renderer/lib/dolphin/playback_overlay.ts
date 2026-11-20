/**
 * Pure helpers of use_playback_overlay.ts (the playback Dolphin's seek bar, the launcher's side), kept apart so they
 * can be tested without the app.
 */
import type { ReplayPlaybackStatus } from "@dolphin/types";
import type { ManualClip, ReplayClip, ReplayHighlight, ReplayHighlights } from "@replays/highlights/types";

export type RenderAsk = NonNullable<ReplayPlaybackStatus["renderRequests"]>[number];

/** "#rrggbb" of a CSS colour (as the page computes it), or null. */
export function cssColorToHex(css: string): string | null {
  const m = /^rgba?\(\s*(\d+)[\s,]+(\d+)[\s,]+(\d+)/i.exec(css.trim());
  if (m) {
    return `#${[m[1], m[2], m[3]].map((v) => Math.min(255, Number(v)).toString(16).padStart(2, "0")).join("")}`;
  }
  const hex = /^#([0-9a-f]{6})$/i.exec(css.trim());
  if (hex) {
    return `#${hex[1].toLowerCase()}`;
  }
  const short = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/i.exec(css.trim());
  return short ? `#${short[1]}${short[1]}${short[2]}${short[2]}${short[3]}${short[3]}`.toLowerCase() : null;
}

/** "0:20" */
export function clipLength(startFrame: number, endFrame: number): string {
  const seconds = Math.max(0, Math.round((endFrame - startFrame) / 60));
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
}

/** The Manual Clip Dolphin's render request is of (a spectated game's: its copy, `source` the stream's file). */
export function findClip(clips: readonly ManualClip[], ask: RenderAsk): ManualClip | null {
  return (
    clips.find(
      (c) =>
        c.startFrame === ask.startFrame &&
        c.endFrame === ask.endFrame &&
        (c.path === ask.path || c.source === ask.path),
    ) ?? null
  );
}

/** The replay's highlight or hotkey clip a star's render request is of. */
export function findHighlight(
  found: Pick<ReplayHighlights, "highlights" | "clips">,
  ask: Pick<RenderAsk, "startFrame" | "endFrame">,
): ReplayHighlight | ReplayClip | null {
  return (
    [...found.highlights, ...found.clips].find((r) => r.startFrame === ask.startFrame && r.endFrame === ask.endFrame) ??
    null
  );
}
