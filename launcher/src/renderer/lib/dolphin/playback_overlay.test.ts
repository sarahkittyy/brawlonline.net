import type { ManualClip, ReplayHighlights } from "@replays/highlights/types";
import { describe, expect, it } from "vitest";

import { clipLength, cssColorToHex, findClip, findHighlight } from "./playback_overlay";

describe("playback_overlay", () => {
  it("turns the page's colour into #rrggbb", () => {
    expect(cssColorToHex("rgb(95, 180, 255)")).toBe("#5fb4ff");
    expect(cssColorToHex("rgba(208, 208, 208, 1)")).toBe("#d0d0d0");
    expect(cssColorToHex("#ABC")).toBe("#aabbcc");
    expect(cssColorToHex("#5FB4FF")).toBe("#5fb4ff");
    expect(cssColorToHex("var(--x)")).toBeNull();
  });

  it("says a clip's length as the lists do", () => {
    expect(clipLength(600, 1800)).toBe("0:20");
    expect(clipLength(0, 3900)).toBe("1:05");
  });

  it("finds clip mode's Manual Clip, a spectated game's by the stream's file", () => {
    const clips = [
      { path: "C:/r/a.brep", startFrame: 10, endFrame: 70 },
      { path: "C:/r/Spectated/b.brep", source: "C:/u/Spectate/x/1.brep", startFrame: 100, endFrame: 400 },
    ] as ManualClip[];
    const ask = { seq: 1, music: true, path: "C:/u/Spectate/x/1.brep", startFrame: 100, endFrame: 400 };
    expect(findClip(clips, ask)).toBe(clips[1]);
    expect(findClip(clips, { ...ask, path: "C:/r/a.brep", startFrame: 10, endFrame: 70 })).toBe(clips[0]);
    expect(findClip(clips, { ...ask, endFrame: 401 })).toBeNull();
  });

  it("finds the highlight or hotkey clip a star's render is of", () => {
    const found = {
      highlights: [{ kind: "highlight", startFrame: 900, endFrame: 1100 }],
      clips: [{ kind: "clip", startFrame: 2000, endFrame: 3320 }],
    } as unknown as ReplayHighlights;
    expect(findHighlight(found, { startFrame: 2000, endFrame: 3320 })?.kind).toBe("clip");
    expect(findHighlight(found, { startFrame: 900, endFrame: 1100 })?.kind).toBe("highlight");
    expect(findHighlight(found, { startFrame: 900, endFrame: 1101 })).toBeNull();
  });
});
