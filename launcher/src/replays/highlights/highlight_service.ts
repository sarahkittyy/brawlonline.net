/**
 * A replay's highlights and L+R+Z clips for its stats page (DESIGN.md §1): found on demand, in a
 * worker thread, one replay at a time, and kept in replays.sqlite (`highlight_cache`) by file
 * (path, size, modification time), detector version and what was looked for.
 */
import type { Database } from "@database/schema";
import electronLog from "electron-log";
import type { Kysely } from "kysely";
import { stat } from "node:fs/promises";

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
};

const modeKey = (o: AnalyzeOptions) => `${o.highlights ? "h" : ""}${o.clips ? `c-${o.clipPlayers}` : ""}`;

export class HighlightService {
  private worker: Promise<HighlightsWorker | null> | null = null;
  private idleTimer: ReturnType<typeof setTimeout> | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  private inFlight = new Map<string, Promise<ReplayHighlights>>();

  constructor(private readonly db: Promise<Kysely<Database>>, private readonly settings: () => HighlightSettings) {}

  async getHighlights(fullPath: string): Promise<ReplayHighlights> {
    const s = this.settings();
    const options: AnalyzeOptions = { highlights: s.detectHighlights, clips: s.clipOnLrz, clipPlayers: CLIP_PLAYERS };
    const key = `${fullPath}\n${modeKey(options)}`;
    let p = this.inFlight.get(key);
    if (!p) {
      p = this.load(fullPath, options).finally(() => this.inFlight.delete(key));
      this.inFlight.set(key, p);
    }
    return p;
  }

  private async load(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights> {
    const empty: ReplayHighlights = {
      detectorVersion: DETECTOR_VERSION,
      players: [],
      isTeams: false,
      highlights: [],
      clips: [],
    };
    if (!options.highlights && !options.clips) {
      return empty;
    }
    const { size, mtimeMs } = await stat(fullPath);
    const mtime = Math.floor(mtimeMs);
    const mode = modeKey(options);
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
        return JSON.parse(row.result) as ReplayHighlights;
      }
    } catch (err) {
      log.warn(`highlight cache: ${err}`);
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

  /** One replay at a time, in the worker (on this thread if no worker could start). */
  private analyze(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights> {
    const run = async () => {
      const t0 = Date.now();
      const worker = await this.getWorker();
      const result = worker ? await worker.analyzeReplay(fullPath, options) : await analyzeReplay(fullPath, options);
      log.info(
        `${result.highlights.length} highlights, ${result.clips.length} clips in ${Date.now() - t0} ms: ${fullPath}`,
      );
      return result;
    };
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
