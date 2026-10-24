/**
 * Manual clips (docs/design/highlights.md §9): the playback Dolphin's Clip on the seek bar, then
 * "Save as highlight", lists the clip in its status (`savedClips`, Core/Rollback/ReplayPlayback.h).
 * Each is kept once in the settings (`manualClips`) with what the Highlights tab shows of its game
 * (players, date), and listed in Replays > Highlights > Manual Clips.
 *
 * A spectated game's file is a cache cleared at every start (docs/design/spectating.md §6.1): a
 * clip of one keeps a copy of the game in <Root Replay Folder>/Spectated, the clip pointing at the
 * copy. The game may still be going: the copy is cut after its last whole chunk (it plays as a
 * replay that ended there) and copied again as the game goes on, until its trailer is there.
 */
import { safeFileName } from "@dolphin/render_queue";
import type { ReplayPlaybackStatus } from "@dolphin/types";
import { crc32, readReplayHeader } from "@replays/replay_format";
import electronLog from "electron-log";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import path from "node:path";

import { replayDate } from "./all_highlights";
import type { HighlightPlayer, ManualClip } from "./types";

const log = electronLog.scope("highlights");

/** A kept spectated game is copied again this often while the game goes on. */
const FOLLOW_MS = 5000;
/** ... and given up on once its file hasn't changed for this long (the stream stopped). */
const FOLLOW_IDLE_MS = 15 * 60_000;
/** The kept games' folder, under the root replay folder. */
export const SPECTATED_FOLDER = "Spectated";

export type ManualClipsDeps = {
  clips: () => ManualClip[];
  save: (clips: ManualClip[]) => Promise<void>;
  /** `<playback User folder>/Spectate`. */
  spectateDir: () => string;
  /** The root replay folder. */
  replayDir: () => string;
};

type SavedClip = NonNullable<ReplayPlaybackStatus["savedClips"]>[number];

export class ManualClips {
  /** The status' clips already handled in this launcher run. */
  private seen = new Set<string>();
  /** Spectated games kept: the spectated file, its copy. */
  private kept = new Map<string, KeptGame>();
  private queue: Promise<void> = Promise.resolve();

  constructor(private readonly deps: ManualClipsDeps) {}

  /** A playback Dolphin's status: its clips not yet kept, one at a time. */
  onStatus(status: ReplayPlaybackStatus) {
    for (const clip of status.savedClips ?? []) {
      if (
        !clip ||
        typeof clip.path !== "string" ||
        !Number.isFinite(clip.startFrame) ||
        !Number.isFinite(clip.endFrame) ||
        clip.endFrame <= clip.startFrame
      ) {
        continue;
      }
      const key = `${clip.path}|${clip.startFrame}|${clip.endFrame}`;
      if (this.seen.has(key)) {
        continue;
      }
      this.seen.add(key);
      this.queue = this.queue.then(() => this.addLogged(clip));
    }
  }

  /** Waits for the clips handed over so far (tests). */
  idle(): Promise<void> {
    return this.queue;
  }

  private async addLogged(clip: SavedClip) {
    try {
      await this.add(clip);
    } catch (err) {
      log.error(`manual clip of ${clip.path}: ${err}`);
    }
  }

  private async add(clip: SavedClip) {
    const spectated = isInside(clip.path, this.deps.spectateDir());
    const file = spectated ? await this.keep(clip.path) : clip.path;
    const known = this.deps
      .clips()
      .some((c) => c.path === file && c.startFrame === clip.startFrame && c.endFrame === clip.endFrame);
    if (known) {
      return;
    }
    const header = await readReplayHeader(file);
    const players: HighlightPlayer[] = header.meta.players.map(
      ({ port, displayName, connectCode, characterId, costume, teamId }) => ({
        port,
        displayName,
        connectCode,
        characterId,
        costume,
        teamId,
      }),
    );
    const saved: ManualClip = {
      path: file,
      startFrame: clip.startFrame,
      endFrame: clip.endFrame,
      savedAt: new Date().toISOString(),
      date: header.meta.startTime ?? (await replayDate(file)),
      players,
      isTeams: header.meta.isTeams,
      recorderPort: spectated ? null : header.meta.recorderPort,
      ...(spectated ? { spectated: true } : {}),
    };
    // (From the settings as they are now: the player may have removed one meanwhile.)
    await this.deps.save([...this.deps.clips(), saved]);
    log.info(`manual clip kept: frames ${clip.startFrame}-${clip.endFrame} of ${file}`);
  }

  /** The kept copy of the spectated game `source`, as far as it has come. */
  private async keep(source: string): Promise<string> {
    let game = this.kept.get(source);
    if (!game) {
      const header = await readReplayHeader(source);
      const folder = path.join(this.deps.replayDir(), SPECTATED_FOLDER);
      await mkdir(folder, { recursive: true });
      const dest = await freeName(folder, spectatedName(header.meta, new Date()));
      game = new KeptGame(source, dest);
      this.kept.set(source, game);
    }
    await game.copy();
    game.follow();
    return game.dest;
  }
}

/** `file` is in `dir` (or below). */
function isInside(file: string, dir: string): boolean {
  if (!dir) {
    return false;
  }
  const rel = path.relative(path.resolve(dir), path.resolve(file));
  return rel !== "" && !rel.startsWith("..") && !path.isAbsolute(rel);
}

const MODE_LABELS: Record<string, string> = {
  ranked: "Ranked",
  unranked: "Unranked",
  direct: "Direct",
  teams: "Teams",
  room: "Room",
};

/**
 * A kept spectated game's name, as Dolphin names replays (ReplayRecorder.cpp's FileStem, without
 * the characters): "2026-10-24 1530 Spectated Ranked G1 - Alice vs Bob".
 */
export function spectatedName(
  meta: {
    startTime: string | null;
    mode: string;
    gameIndex: number;
    players: { displayName: string; connectCode: string; port: number }[];
  },
  now: Date,
): string {
  const t = meta.startTime ? new Date(meta.startTime) : now;
  const when = Number.isNaN(t.getTime()) ? now : t;
  const pad = (n: number) => String(n).padStart(2, "0");
  const stamp = `${when.getFullYear()}-${pad(when.getMonth() + 1)}-${pad(when.getDate())} ${pad(when.getHours())}${pad(
    when.getMinutes(),
  )}`;
  const who = meta.players.map((p) => p.displayName || p.connectCode || `P${p.port}`).join(" vs ");
  const name = `${stamp} Spectated ${MODE_LABELS[meta.mode] ?? "Online"} G${Math.max(meta.gameIndex, 1)}${
    who ? ` - ${who}` : ""
  }`;
  return safeFileName(name);
}

/** `<folder>/<stem>.brep`, or ` (2)`, ` (3)`... when taken. */
async function freeName(folder: string, stem: string): Promise<string> {
  for (let n = 1; ; n++) {
    const file = path.join(folder, `${stem}${n > 1 ? ` (${n})` : ""}.brep`);
    try {
      await stat(file);
    } catch {
      return file;
    }
  }
}

/** Replay files (Core/Rollback/ReplayFile.h): the header's size at +12, chunks of a 20-byte header and their stored bytes (+8), then the trailer. */
const CHUNK_HEADER_SIZE = 20;
const TRAILER_SIZE = 16;

/** How much of a replay file that may still be written is whole: all of it with its trailer, else up to its last whole chunk. */
export function wholeLength(data: Buffer): { length: number; complete: boolean } {
  if (data.length >= TRAILER_SIZE) {
    const t = data.subarray(data.length - TRAILER_SIZE);
    if (t.toString("latin1", 0, 4) === "BRPT" && t.readUInt32LE(12) === crc32(t.subarray(0, 12))) {
      return { length: data.length, complete: true };
    }
  }
  if (data.length < 16) {
    return { length: 0, complete: false };
  }
  let at = data.readUInt32LE(12);
  if (at > data.length) {
    return { length: 0, complete: false };
  }
  while (at + CHUNK_HEADER_SIZE <= data.length) {
    const next = at + CHUNK_HEADER_SIZE + data.readUInt32LE(at + 8);
    if (next > data.length) {
      break;
    }
    at = next;
  }
  return { length: at, complete: false };
}

/** A spectated game's copy, taken again while its file grows. */
class KeptGame {
  private copied = -1;
  private complete = false;
  private timer: ReturnType<typeof setInterval> | null = null;
  private changedAt = Date.now();
  private busy = false;

  constructor(readonly source: string, readonly dest: string) {}

  async copy() {
    if (this.complete) {
      return;
    }
    let data: Buffer;
    try {
      data = await readFile(this.source);
    } catch {
      // (Gone: the launcher cleared it. The copy is what came.)
      this.stop();
      return;
    }
    const { length, complete } = wholeLength(data);
    if (length > this.copied) {
      await writeFile(this.dest, data.subarray(0, length));
      this.copied = length;
      this.changedAt = Date.now();
    }
    if (complete) {
      this.complete = true;
      this.stop();
    }
  }

  follow() {
    if (this.timer || this.complete) {
      return;
    }
    this.timer = setInterval(() => {
      if (this.busy) {
        return;
      }
      if (Date.now() - this.changedAt > FOLLOW_IDLE_MS) {
        this.stop();
        return;
      }
      this.busy = true;
      void this.copy()
        .catch((err) => log.warn(`kept spectated game ${this.dest}: ${err}`))
        .finally(() => (this.busy = false));
    }, FOLLOW_MS);
    this.timer.unref?.();
  }

  private stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }
}
