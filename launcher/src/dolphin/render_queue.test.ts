import { mkdtemp, readdir, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import type { RenderDolphin, RenderKey } from "./render_queue";
import { renderProgress, RenderQueue, safeFileName } from "./render_queue";
import type { RenderJobInfo, ReplayCommunication, ReplayPlaybackStatus } from "./types";

describe("safeFileName", () => {
  it("drops what file systems refuse and keeps the rest", () => {
    expect(safeFileName('2026-10-20 1635 a/b (Mr. Game & Watch) vs c:d "e"')).toBe(
      "2026-10-20 1635 ab (Mr. Game & Watch) vs cd e",
    );
    expect(safeFileName("  name...  ")).toBe("name");
    expect(safeFileName("???")).toBe("Highlight");
    expect(safeFileName("x".repeat(300))).toHaveLength(150);
  });
});

describe("renderProgress", () => {
  it("weighs each clip's fast-forward and the clip itself by what their frames cost", () => {
    // 900 frames to fast-forward (a third of a rendered frame each: 300) and a 300-frame clip.
    const seeking = { state: "seeking", index: 0, frame: 550, seekFrom: 100, clipStart: 1000, clipEnd: 1300 } as const;
    expect(renderProgress(seeking, 2)).toBeCloseTo((0.5 * 0.5) / 2);
    const playing = { ...seeking, state: "playing", frame: 1150 } as const;
    expect(renderProgress(playing, 2)).toBeCloseTo((0.5 + 0.5 * 0.5) / 2);
    // No fast-forward (straight on): the clip is all of it.
    const next = { state: "playing", index: 1, frame: 2150, seekFrom: 2000, clipStart: 2000, clipEnd: 2300 } as const;
    expect(renderProgress(next, 2)).toBeCloseTo((1 + 0.5) / 2);
    // A clip written while the next has not started yet.
    expect(renderProgress({ state: "playing", index: 0, rendered: 1, frame: 1300, clipStart: 1000 }, 2)).toBe(0.5);
    expect(renderProgress({ state: "ended" }, 3)).toBe(1);
    expect(renderProgress(null, 3)).toBe(0);
  });
});

/** A render Dolphin that plays each request through: loading, seeking, playing, then writes its file. */
class FakeDolphin implements RenderDolphin {
  open = false;
  closed = 0;
  requests: ReplayCommunication[] = [];
  private status: ReplayPlaybackStatus | null = null;
  private steps: ReplayPlaybackStatus[] = [];
  constructor(readonly key: RenderKey, private readonly fail?: string) {}
  async play(comm: ReplayCommunication) {
    this.open = true;
    this.requests.push(comm);
    const id = comm.commandId;
    const n = comm.queue?.length ?? 0;
    this.steps = [{ commandId: id, state: "loading" }];
    for (let i = 0; i < n; i++) {
      this.steps.push({
        commandId: id,
        state: "seeking",
        index: i,
        frame: 10,
        seekFrom: 0,
        clipStart: 20,
        rendered: i,
      });
      this.steps.push({
        commandId: id,
        state: "playing",
        index: i,
        frame: 25,
        clipStart: 20,
        clipEnd: 30,
        rendered: i,
      });
    }
    if (this.fail) {
      this.steps.push({ commandId: id, state: "error", error: this.fail });
    } else {
      this.steps.push({ commandId: id, state: "ended", rendered: n });
    }
  }
  async readStatus() {
    const next = this.steps.shift();
    if (next) {
      if (next.state === "ended") {
        const output = this.requests[this.requests.length - 1].output!;
        await writeFile(output, "mp4");
      }
      this.status = next;
    }
    return this.status;
  }
  isOpen() {
    return this.open;
  }
  async close() {
    this.open = false;
    this.closed++;
  }
}

describe("RenderQueue", () => {
  let folder: string;
  beforeEach(async () => {
    folder = await mkdtemp(path.join(os.tmpdir(), "render-queue-"));
  });
  afterEach(async () => {
    await rm(folder, { recursive: true, force: true });
  });

  const setup = (fail?: (key: RenderKey) => string | undefined) => {
    const dolphins: FakeDolphin[] = [];
    const events: RenderJobInfo[] = [];
    let idle!: () => void;
    let done = new Promise<void>((resolve) => (idle = resolve));
    const queue = new RenderQueue({
      launch: async (key) => {
        const d = new FakeDolphin(key, fail?.(key));
        dolphins.push(d);
        return d;
      },
      folder: () => folder,
      emit: (job) => {
        events.push(job);
        if (queue.jobs().every((j) => ["done", "error", "cancelled"].includes(j.state))) {
          idle();
          done = new Promise<void>((resolve) => (idle = resolve));
        }
      },
      sleep: async () => undefined,
    });
    return { queue, dolphins, events, finished: () => done };
  };
  const item = (startFrame: number) => ({
    path: "C:/r/2026-10-20 1635 Room G1.brep",
    startFrame,
    endFrame: startFrame + 60,
  });
  const key = (music: boolean, release: string | null = null): RenderKey => ({ release, music, playAnyway: false });

  it("renders jobs one after another in one Dolphin while they need the same one", async () => {
    const { queue, dolphins, finished } = setup();
    const wait = finished();
    queue.add([
      { items: [item(100)], name: "a vs b", key: key(true) },
      { items: [item(400)], name: "a vs b", key: key(true) },
      { items: [item(700), item(900)], name: "combo", key: key(false) },
    ]);
    await wait;
    const jobs = queue.jobs();
    expect(jobs.map((j) => j.state)).toEqual(["done", "done", "done"]);
    // Same name: the second gets " (2)"; every file is there.
    expect(jobs.map((j) => path.basename(j.output!))).toEqual(["a vs b.mp4", "a vs b (2).mp4", "combo.mp4"]);
    expect((await readdir(folder)).sort()).toEqual(["a vs b (2).mp4", "a vs b.mp4", "combo.mp4"]);
    // Music on twice in one Dolphin, then another for music off; closed once the queue is empty.
    expect(dolphins.map((d) => [d.key.music, d.requests.length])).toEqual([
      [true, 2],
      [false, 1],
    ]);
    expect(dolphins[1].requests[0]).toMatchObject({ mode: "render", queue: [item(700), item(900)] });
    expect(dolphins.every((d) => !d.open)).toBe(true);
  });

  it("reports progress, then an error, and deletes the failed file", async () => {
    const { queue, events, finished } = setup(() => "setup differs");
    const wait = finished();
    const [job] = queue.add([{ items: [item(100), item(300)], name: "combo", key: key(true) }]);
    await wait;
    const mine = events.filter((e) => e.jobId === job.jobId);
    expect(mine.map((e) => e.state)).toContain("rendering");
    expect(Math.max(...mine.map((e) => e.progress))).toBeGreaterThan(0.5);
    expect(queue.jobs()[0]).toMatchObject({ state: "error", error: "setup differs" });
    expect(await readdir(folder)).toEqual([]);
  });

  it("cancels a queued job before it starts", async () => {
    const { queue, finished } = setup();
    const wait = finished();
    const [, second] = queue.add([
      { items: [item(100)], name: "one", key: key(true) },
      { items: [item(200)], name: "two", key: key(true) },
    ]);
    await queue.cancel(second.jobId);
    await wait;
    expect(queue.jobs().map((j) => j.state)).toEqual(["done", "cancelled"]);
    expect(await readdir(folder)).toEqual(["one.mp4"]);
  });
});
