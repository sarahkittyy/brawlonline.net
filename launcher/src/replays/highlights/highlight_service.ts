/**
 * A replay's highlights and L+R+Z clips for its stats page (DESIGN.md §1): found on demand, in a
 * worker thread, one replay at a time, and kept in replays.sqlite (`highlight_cache`) by file
 * (path, size, modification time), detector version and what was looked for (the clip hotkey of
 * the game's time included, @common/clip_hotkey).
 */
import type { ClipHotkey } from "@common/clip_hotkey";
import { clipButtonsAt, DEFAULT_CLIP_BUTTONS } from "@common/clip_hotkey";
import type { Database } from "@database/schema";
import { readReplayHeader } from "@replays/replay_format";
import type { StatsType } from "@slippi/slippi-js";
import electronLog from "electron-log";
import type { Kysely } from "kysely";
import { stat } from "node:fs/promises";

import { readStats } from "../stats/brawl_stats";
import type { AnalyzeOptions } from "./analyze_replay";
import { analyzeReplay } from "./analyze_replay";
import { DETECTOR_VERSION } from "./detect_highlights";
import type { HighlightsWorker } from "./highlights.worker.interface";
import { createHighlightsWorker } from "./highlights.worker.interface";
import type { ReplayHighlights } from "./types";

const log = electronLog.scope("highlights");

/**
 * Whose L+R+Z presses make clips: only the recording player's (this machine's), so an opponent
 * can't put clips in someone else's replays (the user's decision, 2026-10-20).
 */
export const CLIP_PLAYERS: AnalyzeOptions["clipPlayers"] = "local";

/** The worker goes after this long without a request. */
const WORKER_IDLE_MS = 2 * 60_000;

export type HighlightSettings = {
  detectHighlights: boolean;
  clipOnLrz: boolean;
  clipHotkeys: ClipHotkey[];
};

// L+R+Z's key is the one from before the hotkey could change: those rows stay.
const modeKey = (o: AnalyzeOptions) =>
  `${o.highlights ? "h" : ""}${o.clips ? `c-${o.clipPlayers}` : ""}${
    o.clips && o.clipButtons !== undefined && o.clipButtons !== DEFAULT_CLIP_BUTTONS
      ? `-${o.clipButtons.toString(16)}`
      : ""
  }`;

export class HighlightService {
  private worker: Promise<HighlightsWorker | null> | null = null;
  private idleTimer: ReturnType<typeof setTimeout> | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  private inFlight = new Map<string, Promise<ReplayHighlights>>();
  /** Replays' start times (ms), by path, size and modification time: which clip hotkey was theirs. */
  private startTimes = new Map<string, number | null>();

  constructor(private readonly db: Promise<Kysely<Database>>, private readonly settings: () => HighlightSettings) {}

  /** What to look for in `fullPath`: the clip hotkey is the one of the game's start (else of the file's last write). */
  private async options(fullPath: string, size: number, mtime: number): Promise<AnalyzeOptions> {
    const s = this.settings();
    const options: AnalyzeOptions = { highlights: s.detectHighlights, clips: s.clipOnLrz, clipPlayers: CLIP_PLAYERS };
    if (s.clipOnLrz && s.clipHotkeys.length > 0) {
      const startTime = await this.startTime(fullPath, size, mtime);
      options.clipButtons = clipButtonsAt(s.clipHotkeys, startTime ?? mtime);
    }
    return options;
  }

  private async startTime(fullPath: string, size: number, mtime: number): Promise<number | null> {
    const key = `${fullPath}\n${size}\n${mtime}`;
    if (this.startTimes.has(key)) {
      return this.startTimes.get(key) ?? null;
    }
    const startTime = await readReplayHeader(fullPath).then(
      (h) => (h.meta.startTime ? Date.parse(h.meta.startTime) : null),
      () => null,
    );
    this.startTimes.set(key, startTime);
    return startTime;
  }

  /** What is cached for `fullPath` (as this file, detector and settings are now), or null: nothing is found here. */
  async cached(fullPath: string): Promise<ReplayHighlights | null> {
    const s = this.settings();
    if (!s.detectHighlights && !s.clipOnLrz) {
      return null;
    }
    try {
      const { size, mtimeMs } = await stat(fullPath);
      const mtime = Math.floor(mtimeMs);
      const options = await this.options(fullPath, size, mtime);
      return await this.fromCache(fullPath, size, mtime, modeKey(options));
    } catch {
      return null;
    }
  }

  private async fromCache(fullPath: string, size: number, mtime: number, mode: string) {
    const db = await this.db;
    try {
      const row = await db.selectFrom("highlight_cache").selectAll().where("path", "=", fullPath).executeTakeFirst();
      if (
        row &&
        row.size_bytes === size &&
        row.mtime_ms === mtime &&
        row.detector_version === DETECTOR_VERSION &&
        row.mode === mode
      ) {
        const result = JSON.parse(row.result) as ReplayHighlights;
        // Cached before the recorder was kept: from the replay's header, once.
        if (result.recorderPort === undefined && !result.error) {
          result.recorderPort = await readReplayHeader(fullPath).then(
            (h) => h.meta.recorderPort,
            () => null,
          );
          await db
            .updateTable("highlight_cache")
            .set({ result: JSON.stringify(result) })
            .where("path", "=", fullPath)
            .execute();
        }
        return result;
      }
    } catch (err) {
      log.warn(`highlight cache: ${err}`);
    }
    return null;
  }

  async getHighlights(fullPath: string): Promise<ReplayHighlights> {
    const s = this.settings();
    const key = `${fullPath}\n${s.detectHighlights}\n${s.clipOnLrz}\n${JSON.stringify(s.clipHotkeys)}`;
    let p = this.inFlight.get(key);
    if (!p) {
      p = this.load(fullPath).finally(() => this.inFlight.delete(key));
      this.inFlight.set(key, p);
    }
    return p;
  }

  private async load(fullPath: string): Promise<ReplayHighlights> {
    const empty: ReplayHighlights = {
      detectorVersion: DETECTOR_VERSION,
      players: [],
      isTeams: false,
      highlights: [],
      clips: [],
    };
    const s = this.settings();
    if (!s.detectHighlights && !s.clipOnLrz) {
      return empty;
    }
    const { size, mtimeMs } = await stat(fullPath);
    const mtime = Math.floor(mtimeMs);
    const options = await this.options(fullPath, size, mtime);
    const mode = modeKey(options);
    const db = await this.db;
    const hit = await this.fromCache(fullPath, size, mtime, mode);
    if (hit) {
      return hit;
    }
    let result: ReplayHighlights;
    try {
      result = await this.analyze(fullPath, options);
    } catch (err) {
      // An older replay (no fighter memory) or a damaged one: nothing to list, and not asked again.
      result = { ...empty, error: err instanceof Error ? err.message : String(err) };
    }
    try {
      const values = {
        path: fullPath,
        size_bytes: size,
        mtime_ms: mtime,
        detector_version: DETECTOR_VERSION,
        mode,
        result: JSON.stringify(result),
      };
      await db
        .insertInto("highlight_cache")
        .values(values)
        .onConflict((oc) => oc.column("path").doUpdateSet(values))
        .execute();
    } catch (err) {
      log.warn(`highlight cache: ${err}`);
    }
    return result;
  }

  /**
   * A replay's stats for its stats page (stats/brawl_stats.ts), in the same worker as the highlights.
   * Not cached: a long game takes a fifth of a second.
   */
  async getStats(fullPath: string): Promise<StatsType> {
    return this.inWorker(async (worker) => {
      const t0 = Date.now();
      const stats = worker ? await worker.calculateStats(fullPath) : await readStats(fullPath);
      log.info(`stats in ${Date.now() - t0} ms: ${fullPath}`);
      return stats;
    });
  }

  /** One replay at a time, in the worker (on this thread if no worker could start). */
  private analyze(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights> {
    return this.inWorker(async (worker) => {
      const t0 = Date.now();
      const result = worker ? await worker.analyzeReplay(fullPath, options) : await analyzeReplay(fullPath, options);
      log.info(
        `${result.highlights.length} highlights, ${result.clips.length} clips in ${Date.now() - t0} ms: ${fullPath}`,
      );
      return result;
    });
  }

  /** Runs `job` after the jobs before it, with the worker (null: none could start, so on this thread). */
  private inWorker<T>(job: (worker: HighlightsWorker | null) => Promise<T>): Promise<T> {
    const run = async () => job(await this.getWorker());
    const next = this.queue.then(run, run);
    this.queue = next.catch(() => undefined);
    return next.finally(() => this.armIdle());
  }

  private getWorker(): Promise<HighlightsWorker | null> {
    if (this.idleTimer) {
      clearTimeout(this.idleTimer);
      this.idleTimer = null;
    }
    if (!this.worker) {
      this.worker = createHighlightsWorker().catch((err) => {
        log.error(`highlights worker: ${err}; finding them on the main thread`);
        return null;
      });
    }
    return this.worker;
  }

  private armIdle() {
    if (this.idleTimer) {
      clearTimeout(this.idleTimer);
    }
    this.idleTimer = setTimeout(() => {
      const w = this.worker;
      this.worker = null;
      this.idleTimer = null;
      void w?.then((x) => x?.terminate());
    }, WORKER_IDLE_MS);
  }
}
