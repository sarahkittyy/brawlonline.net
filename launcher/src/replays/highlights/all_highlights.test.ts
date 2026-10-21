import { mkdir, mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { AllHighlights, findReplayFiles, replayDate } from "./all_highlights";
import type { AllHighlightsEvent, ReplayHighlights } from "./types";

const result = (n: number): ReplayHighlights => ({
  detectorVersion: 2,
  players: [],
  isTeams: false,
  highlights: Array.from({ length: n }, (_, i) => ({
    kind: "highlight" as const,
    attackerPort: 1,
    victimPort: 2,
    firstHitFrame: 100 * i,
    startFrame: 100 * i,
    endFrame: 100 * i + 60,
    kill: false,
    score: i,
  })),
  clips: [],
});

describe("all highlights", () => {
  let root: string;
  beforeEach(async () => {
    root = await mkdtemp(path.join(os.tmpdir(), "all-highlights-"));
    await mkdir(path.join(root, "2026-10"), { recursive: true });
    for (const f of [
      "2026-10/2026-10-20 1635 Room G1 - a (Mario) vs b (Link).brep",
      "2026-10/2026-10-19 0722 Room G6 - a (Mario) vs b (Link).brep",
      "old.brep",
      "notes.txt",
      "2026-10/clip.mp4",
    ]) {
      await writeFile(path.join(root, f), "x");
    }
  });
  afterEach(async () => {
    await rm(root, { recursive: true, force: true });
  });

  it("finds the .brep files in the folders and below", async () => {
    const files = (await findReplayFiles([root, root])).map((f) => path.relative(root, f)).sort();
    expect(files).toEqual([
      path.join("2026-10", "2026-10-19 0722 Room G6 - a (Mario) vs b (Link).brep"),
      path.join("2026-10", "2026-10-20 1635 Room G1 - a (Mario) vs b (Link).brep"),
      "old.brep",
    ]);
  });

  it("dates a game from its replay's name", async () => {
    const date = new Date(await replayDate(path.join(root, "2026-10", "2026-10-20 1635 Room G1 - x.brep")));
    expect([date.getFullYear(), date.getMonth() + 1, date.getDate(), date.getHours(), date.getMinutes()]).toEqual([
      2026, 10, 20, 16, 35,
    ]);
  });

  it("lists the cached ones at once and sends the others as they are found", async () => {
    const files = await findReplayFiles([root]);
    const cachedFile = files.find((f) => f.includes("1635"))!;
    const found = new Map<string, ReplayHighlights>([
      [files.find((f) => f.includes("0722"))!, result(2)],
      [files.find((f) => f.endsWith("old.brep"))!, result(0)],
    ]);
    const events: AllHighlightsEvent[] = [];
    let done!: () => void;
    const finished = new Promise<void>((resolve) => (done = resolve));
    const index = new AllHighlights(
      {
        cached: async (f) => (f === cachedFile ? result(1) : null),
        getHighlights: async (f) => found.get(f)!,
      },
      () => [root],
      (e) => {
        events.push(e);
        if (e.done === e.total) {
          done();
        }
      },
    );
    const now = await index.list();
    expect(now.games.map((g) => g.path)).toEqual([cachedFile]);
    expect([now.pending, now.total]).toEqual([2, 3]);
    await finished;
    // The one with highlights comes as a game; the one without, as progress only.
    expect(events.map((e) => [e.done, e.total, e.game?.highlights.highlights.length ?? null])).toEqual(
      expect.arrayContaining([
        [expect.any(Number), 3, 2],
        [expect.any(Number), 3, null],
      ]),
    );
    expect(events).toHaveLength(2);
  });
});
