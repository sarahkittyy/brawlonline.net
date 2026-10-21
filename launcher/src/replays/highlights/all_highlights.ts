/**
 * Every replay's highlights, for the Replays > Highlights tab (docs/design/highlights.md §6): the
 * .brep files in the replay folders (and below), those already in the highlight cache at once, the
 * others found afterwards, one replay at a time in the highlights worker (a first run with a
 * thousand replays must never bog the PC down), each one sent as it is found.
 */
import electronLog from "electron-log";
import type { Dirent } from "node:fs";
import { readdir, stat } from "node:fs/promises";
import path from "node:path";

import type { HighlightService } from "./highlight_service";
import type { AllHighlightsEvent, GameHighlights, ReplayHighlights } from "./types";

const log = electronLog.scope("highlights");

/** A pause between two replays found in the background, so other work (playing a replay, a game) goes first. */
const BETWEEN_REPLAYS_MS = 50;
/** Folders this deep below a replay folder are not looked in. */
const MAX_DEPTH = 4;

/** The .brep files in `folders` and their subfolders. */
export async function findReplayFiles(folders: readonly string[]): Promise<string[]> {
  const found = new Set<string>();
  const walk = async (dir: string, depth: number) => {
    let entries: Dirent[];
    try {
      entries = await readdir(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const full = path.join(dir, e.name);
      if (e.isDirectory() && depth < MAX_DEPTH) {
        await walk(full, depth + 1);
      } else if (e.isFile() && e.name.toLowerCase().endsWith(".brep")) {
        found.add(full);
      }
    }
  };
  for (const folder of new Set(folders.filter(Boolean))) {
    await walk(folder, 0);
  }
  return [...found];
}

/** When the game was played: from our replays' names ("2026-10-20 1635 ..."), else the file's time. */
export async function replayDate(file: string): Promise<string> {
  const m = /^(\d{4})-(\d{2})-(\d{2}) (\d{2})(\d{2})/.exec(path.basename(file));
  if (m) {
    return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5]).toISOString();
  }
  try {
    return new Date((await stat(file)).mtimeMs).toISOString();
  } catch {
    return new Date(0).toISOString();
  }
}

const hasAny = (h: ReplayHighlights) => !h.error && (h.highlights.length > 0 || h.clips.length > 0);

export class AllHighlights {
  /** Bumped by every list(): a running fill stops once a newer one starts. */
  private generation = 0;

  constructor(
    private readonly service: Pick<HighlightService, "cached" | "getHighlights">,
    private readonly folders: () => readonly string[],
    private readonly emit: (event: AllHighlightsEvent) => void,
  ) {}

  /** Every replay's highlights that are known now; the others come as events (`pending` of them). */
  async list(): Promise<{ games: GameHighlights[]; pending: number; total: number }> {
    const generation = ++this.generation;
    const files = await findReplayFiles(this.folders());
    const games: GameHighlights[] = [];
    const pending: string[] = [];
    for (const file of files) {
      const highlights = await this.service.cached(file);
      if (!highlights) {
        pending.push(file);
      } else if (hasAny(highlights)) {
        games.push({ path: file, date: await replayDate(file), highlights });
      }
    }
    if (pending.length) {
      void this.fill(generation, pending, files.length);
    }
    return { games, pending: pending.length, total: files.length };
  }

  private async fill(generation: number, files: string[], total: number) {
    const done0 = total - files.length;
    log.info(`finding the highlights of ${files.length} replays`);
    for (let i = 0; i < files.length; i++) {
      if (generation !== this.generation) {
        return;
      }
      const file = files[i];
      let game: GameHighlights | undefined;
      try {
        const highlights = await this.service.getHighlights(file);
        if (hasAny(highlights)) {
          game = { path: file, date: await replayDate(file), highlights };
        }
      } catch (err) {
        log.warn(`highlights of ${file}: ${err}`);
      }
      if (generation !== this.generation) {
        return;
      }
      this.emit({ game, done: done0 + i + 1, total });
      await new Promise((resolve) => setTimeout(resolve, BETWEEN_REPLAYS_MS));
    }
  }
}
