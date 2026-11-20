import { describe, expect, it } from "vitest";

import { MAX_REPLAY_MARKS, replayMarks, requestMarks } from "./replay_marks";
import type { ManualClip, ReplayHighlights } from "./types";

const A = "C:/r/2026-11-20 1200 Ranked G1 - a (Fox) vs b (Wolf).brep";
const B = "C:/r/2026-11-20 1210 Ranked G2 - a (Fox) vs b (Wolf).brep";

const found = (highlights: [number, number][], clips: [number, number][]) =>
  ({
    highlights: highlights.map(([startFrame, endFrame]) => ({ startFrame, endFrame })),
    clips: clips.map(([startFrame, endFrame]) => ({ startFrame, endFrame })),
  } as unknown as ReplayHighlights);

const manual = (path: string, startFrame: number, endFrame: number) => ({ path, startFrame, endFrame } as ManualClip);

describe("replayMarks", () => {
  it("has every highlight, hotkey clip and manual clip of the replay, by start, each once", () => {
    const marks = replayMarks(
      A,
      found(
        [
          [3000, 3300],
          [900, 1100],
        ],
        [
          [2000, 3320],
          [900, 1100],
        ],
      ),
      [manual(A, 5000, 6200), manual(B, 100, 200), manual(A, 2000, 3320)],
    );
    expect(marks).toEqual([
      { path: A, startFrame: 900, endFrame: 1100, position: 0 },
      { path: A, startFrame: 2000, endFrame: 3320, position: 0 },
      { path: A, startFrame: 3000, endFrame: 3300, position: 0 },
      { path: A, startFrame: 5000, endFrame: 6200, position: 0 },
    ]);
  });

  it("leaves out frames that aren't a clip", () => {
    expect(
      replayMarks(
        A,
        found(
          [
            [300, 200],
            [-5, 10],
            [NaN, 4],
          ],
          [],
        ),
        [],
      ),
    ).toEqual([]);
  });

  it("has the manual clips of a replay whose highlights can't be read", async () => {
    const marks = await requestMarks(
      [A, B, A],
      async (p) => {
        if (p === B) {
          throw new Error("damaged");
        }
        return found([[10, 70]], []);
      },
      [manual(B, 400, 500)],
    );
    expect(marks).toEqual([
      { path: A, startFrame: 10, endFrame: 70, position: 0 },
      { path: B, startFrame: 400, endFrame: 500, position: 0 },
    ]);
  });

  it("stops at Dolphin's limit", async () => {
    const many = Array.from({ length: MAX_REPLAY_MARKS + 50 }, (_, i): [number, number] => [i * 10, i * 10 + 5]);
    expect(await requestMarks([A], async () => found(many, []), [])).toHaveLength(MAX_REPLAY_MARKS);
  });
});
