// Rendering highlights and clips to video (docs/design/highlights.md §5). A queue of jobs, one
// file each, rendered one after another by a render Dolphin (DolphinManager: the playback Dolphin
// started with `-b --render`, no window, its own User folder) through the comm file's `render`
// mode (Dolphin's ReplayPlayback.h). The Dolphin stays open between jobs that need the same one
// (the same Project+ release and music), so a next clip of the same replay goes on in its match.
import type { ClipResolution } from "@common/clip_resolution";
import type { HighlightPlayItem } from "@replays/highlights/types";
import electronLog from "electron-log";
import { mkdir, rename, stat, unlink } from "fs/promises";
import path from "path";

import { concatMp4 } from "./mp4_concat";
import type { RenderJobInfo, ReplayCommunication, ReplayPlaybackStatus } from "./types";

const log = electronLog.scope("dolphin/render");

/** How often the render Dolphin's status is read. */
const POLL_MS = 250;
/** A render whose Dolphin says nothing new for this long is given up (a hung boot, a replay that never starts). */
const STALL_MS = 3 * 60_000;
/**
 * What a fast-forwarded frame costs against a rendered one: the render Dolphin presents one in 30
 * of them and encodes none (measured 2026-10-21: ~600 a second against ~200 here).
 */
const SEEK_FRAME_COST = 1 / 3;
const MAX_NAME = 150;

/** What renders jobs: the render Dolphin for one key (DolphinManager.launchRenderDolphin), started by its first `play`. */
export type RenderDolphin = {
  play(comm: ReplayCommunication): Promise<void>;
  readStatus(): Promise<ReplayPlaybackStatus | null>;
  isOpen(): boolean;
  close(): Promise<void>;
};

/** What a render Dolphin is started with: one Project+ release's card, music, the video's height. */
export type RenderKey = { release: string | null; music: boolean; playAnyway: boolean; resolution: ClipResolution };

/**
 * One file: its clips in pieces, each piece the clips (in order) of one Project+ release (a Dolphin
 * plays one release's card); a file of several pieces is rendered piece by piece and joined into
 * one (mp4_concat.ts).
 */
export type RenderJob = {
  /** The file's name, without folder or extension (made safe and unique here). */
  name: string;
  music: boolean;
  playAnyway: boolean;
  resolution: ClipResolution;
  pieces: { release: string | null; items: HighlightPlayItem[] }[];
  /** A whole match (RenderRequest.wholeMatch). */
  wholeMatch?: boolean;
};

const jobItems = (job: RenderJob) => job.pieces.flatMap((p) => p.items);

export type RenderQueueDeps = {
  launch(key: RenderKey): Promise<RenderDolphin>;
  folder(): string;
  emit(job: RenderJobInfo): void;
  now?: () => number;
  sleep?: (ms: number) => Promise<void>;
  /** Joins a file's pieces (mp4_concat.ts; tests give another). */
  concat?: (pieces: string[], output: string) => Promise<void>;
};

/** A name usable as a file name on every OS: no reserved characters, no trailing dot or space, not too long. */
export function safeFileName(name: string): string {
  const cleaned = name
    // eslint-disable-next-line no-control-regex
    .replace(/[<>:"/\\|?*\u0000-\u001f]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, MAX_NAME)
    .replace(/[. ]+$/, "");
  return cleaned || "Highlight";
}

/**
 * How far a job is (0..1), from its Dolphin's status: each clip a share, split between the
 * fast-forward to it (from `seekFrom`, which stays while the clip plays) and the clip itself by what
 * their frames cost. (A job's first clip is the long one: its fast-forward runs from the match's
 * start; the next ones go on from there or from a keyframe.)
 */
export function renderProgress(status: ReplayPlaybackStatus | null, clips: number): number {
  if (!status || clips <= 0) {
    return 0;
  }
  if (status.state === "ended") {
    return 1;
  }
  const index = Math.min(Math.max(status.index ?? 0, 0), clips - 1);
  // (Rendered clips are done whatever the state says: the next one may not have started.)
  const rendered = Math.min(status.rendered ?? 0, clips);
  if (rendered > index) {
    return rendered / clips;
  }
  const frame = status.frame ?? -1;
  const start = status.clipStart ?? 0;
  const end = Math.max(status.clipEnd ?? start, start);
  const from = status.seekFrom != null && status.seekFrom >= 0 ? Math.min(status.seekFrom, start) : start;
  const seekCost = (start - from) * SEEK_FRAME_COST;
  const clipCost = Math.max(end - start, 1);
  const clamp = (x: number) => Math.min(1, Math.max(0, x));
  let part = 0;
  if (status.state === "seeking" && start > from) {
    part = (seekCost * clamp((frame - from) / (start - from))) / (seekCost + clipCost);
  } else if (status.state === "playing") {
    part = (seekCost + clipCost * clamp((frame - start) / clipCost)) / (seekCost + clipCost);
  }
  return Math.min(1, (index + part) / clips);
}

type Entry = { info: RenderJobInfo; job: RenderJob; cancelled: boolean };

/** A render given up: Dolphin writes `<output>.part` and names it `output` once complete (ClipRecorder.h). */
async function removeOutput(output: string): Promise<void> {
  await Promise.all([output, `${output}.part`].map((f) => unlink(f).catch(() => undefined)));
}

/** Where piece `i` of a file of several is written before they are joined. */
const pieceFile = (output: string, i: number) => `${output}.piece${i + 1}.mp4`;

const FINISHED: RenderJobInfo["state"][] = ["done", "error", "cancelled"];

export class RenderQueue {
  private entries: Entry[] = [];
  private running = false;
  private dolphin: { key: RenderKey; d: RenderDolphin } | null = null;
  private outputs = new Set<string>();
  private nextId = 1;

  constructor(private readonly deps: RenderQueueDeps) {}

  /** Queues `jobs`; their states come as `emit` calls (and from `jobs()`). */
  add(jobs: RenderJob[]): RenderJobInfo[] {
    const added = jobs.map((job) => {
      const items = jobItems(job);
      const info: RenderJobInfo = {
        jobId: `render-${Date.now().toString(36)}-${this.nextId++}`,
        name: safeFileName(job.name),
        output: null,
        paths: [...new Set(items.map((i) => i.path))],
        wholeMatch: job.wholeMatch === true,
        clips: items.length,
        clip: 0,
        music: job.music,
        resolution: job.resolution,
        release: job.pieces[0]?.release ?? null,
        state: "queued",
        progress: 0,
      };
      this.entries.push({ info, job, cancelled: false });
      return info;
    });
    added.forEach((info) => this.deps.emit({ ...info }));
    void this.run();
    return added.map((info) => ({ ...info }));
  }

  /** Every job of this launcher run, oldest first. */
  jobs(): RenderJobInfo[] {
    return this.entries.map((e) => ({ ...e.info }));
  }

  /** Stops a job: a queued one never starts; a running one's Dolphin is closed and its file deleted. */
  async cancel(jobId: string): Promise<void> {
    const entry = this.entries.find((e) => e.info.jobId === jobId);
    if (!entry || entry.cancelled || FINISHED.includes(entry.info.state)) {
      return;
    }
    entry.cancelled = true;
    if (entry.info.state === "queued") {
      this.update(entry, { state: "cancelled" });
    }
    // (A running one stops at its next poll.)
  }

  private update(entry: Entry, change: Partial<RenderJobInfo>) {
    Object.assign(entry.info, change);
    this.deps.emit({ ...entry.info });
  }

  private async closeDolphin() {
    const open = this.dolphin;
    this.dolphin = null;
    if (open) {
      await open.d.close().catch((err) => log.warn(err));
    }
  }

  private async run() {
    if (this.running) {
      return;
    }
    this.running = true;
    try {
      for (;;) {
        const entry = this.entries.find((e) => e.info.state === "queued" && !e.cancelled);
        if (!entry) {
          break;
        }
        await this.render(entry);
      }
    } finally {
      this.running = false;
      await this.closeDolphin();
    }
  }

  private async uniqueOutput(name: string): Promise<string> {
    const folder = this.deps.folder();
    await mkdir(folder, { recursive: true });
    for (let n = 1; ; n++) {
      const file = path.join(folder, `${name}${n > 1 ? ` (${n})` : ""}.mp4`);
      const exists = await stat(file).then(
        () => true,
        () => false,
      );
      if (!this.outputs.has(file) && !exists) {
        this.outputs.add(file);
        return file;
      }
    }
  }

  private async render(entry: Entry) {
    const { job, info } = entry;
    const items = jobItems(job);
    let output: string | null = null;
    const pieces: string[] = [];
    try {
      this.update(entry, { state: "starting" });
      output = await this.uniqueOutput(info.name);
      this.update(entry, { output });
      let before = 0;
      for (const [i, piece] of job.pieces.entries()) {
        const file = job.pieces.length > 1 ? pieceFile(output, i) : output;
        if (job.pieces.length > 1) {
          pieces.push(file);
        }
        const key: RenderKey = {
          release: piece.release,
          music: job.music,
          playAnyway: job.playAnyway,
          resolution: job.resolution,
        };
        const commandId = `${info.jobId}-${i + 1}`;
        if (!(await this.renderPiece(entry, key, piece.items, file, commandId, before, items.length))) {
          // Cancelled: its Dolphin is closed.
          await Promise.all([removeOutput(output), ...pieces.map(removeOutput)]);
          this.update(entry, { state: "cancelled" });
          return;
        }
        before += piece.items.length;
      }
      if (pieces.length) {
        // One movie: the pieces joined (written as .part, then named), then gone.
        await (this.deps.concat ?? concatMp4)(pieces, `${output}.part`);
        await rename(`${output}.part`, output);
        await Promise.all(pieces.map(removeOutput));
      }
      const size = await stat(output).then(
        (s) => s.size,
        () => 0,
      );
      if (size <= 0) {
        throw new Error("Dolphin finished without writing the video.");
      }
      this.update(entry, { state: "done", progress: 1, clip: items.length });
      log.info(`Rendered ${output} (${items.length} clips in ${job.pieces.length} pieces, ${size} bytes)`);
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      log.error(`Render ${info.jobId} failed: ${message}`);
      // A Dolphin that failed a render may be anywhere: the next job starts a new one.
      await this.closeDolphin();
      if (output) {
        await Promise.all([removeOutput(output), ...pieces.map(removeOutput)]);
      }
      this.update(entry, entry.cancelled ? { state: "cancelled" } : { state: "error", error: message });
    } finally {
      if (output) {
        this.outputs.delete(output);
      }
    }
  }

  /**
   * Renders `items` to `file` in the render Dolphin for `key` (`before` of the job's `total` clips
   * done already, for the progress). False when the job was cancelled meanwhile.
   */
  private async renderPiece(
    entry: Entry,
    key: RenderKey,
    items: HighlightPlayItem[],
    file: string,
    commandId: string,
    before: number,
    total: number,
  ): Promise<boolean> {
    const { info } = entry;
    const now = this.deps.now ?? Date.now;
    const sleep = this.deps.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
    const open = this.dolphin;
    const same =
      open != null &&
      open.d.isOpen() &&
      open.key.release === key.release &&
      open.key.music === key.music &&
      open.key.playAnyway === key.playAnyway &&
      open.key.resolution === key.resolution;
    if (!same) {
      await this.closeDolphin();
      this.dolphin = { key, d: await this.deps.launch(key) };
    }
    const dolphin = this.dolphin!.d;
    await dolphin.play({
      mode: "render",
      output: file,
      height: key.resolution,
      queue: items,
      commandId,
      playAnyway: key.playAnyway || undefined,
    });
    let lastText = "";
    let lastChange = now();
    for (;;) {
      if (entry.cancelled) {
        // A render is only given up with its Dolphin.
        await this.closeDolphin();
        return false;
      }
      if (!dolphin.isOpen()) {
        throw new Error("Dolphin closed while rendering.");
      }
      const status = await dolphin.readStatus();
      const text = JSON.stringify(status);
      if (text !== lastText) {
        lastText = text;
        lastChange = now();
      } else if (now() - lastChange > STALL_MS) {
        throw new Error("The render stopped making progress.");
      }
      if (status?.commandId === commandId) {
        if (status.state === "error") {
          throw new Error(status.error || "Dolphin could not render it.");
        }
        if (status.state === "ended") {
          return true;
        }
        const progress = (before + renderProgress(status, items.length) * items.length) / total;
        const clip = before + Math.min(items.length, Math.max(status.index ?? 0, status.rendered ?? 0) + 1);
        if (info.state !== "rendering" || Math.abs(progress - info.progress) >= 0.005 || clip !== info.clip) {
          this.update(entry, { state: "rendering", progress, clip });
        }
      }
      await sleep(POLL_MS);
    }
  }
}
