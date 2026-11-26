/**
 * Room watch's clips (Online/RoomWatch.h; Sarah's board, 2026-11-26: "it should be in room watch, along with
 * render"). A player sitting out in a room watches the room's game in their own netplay game (Dolphin or
 * EmissaryRecomp), whose seek bar has Clip, "Save as highlight" and Render as the playback Dolphin's has. The game
 * keeps a clipped game in `<Online>/RoomWatch/<uid>.brep` (a game nobody clipped is never written) and says what was
 * clipped in `<Online>/room-watch-clips.json`:
 *   {version: 1, run, savedClips: [{path, startFrame, endFrame}], renderRequests: [{seq, path, startFrame, endFrame,
 *    music, clip, label?}], showRender?: {seq, id}}
 * the fields of the playback Dolphin's status (types.ts ReplayPlaybackStatus), `run` naming the game's run (`seq`
 * counts from 1 in each). The launcher's side of its seek bar (the colour, the renders' progress) is
 * `<Online>/room-watch-overlay.json`, the playback Dolphin's `<comm>.overlay.json`.
 *
 * This follows both while a netplay game is open: each new clips file is handed on as a status of the instance
 * ROOM_WATCH_INSTANCE_ID (manual_clips.ts keeps the clips, a copy of the game in <Root Replay Folder>/Spectated, as a
 * spectated game's; use_playback_overlay.ts renders what is asked), and the overlay is written when it changed.
 */
import electronLog from "electron-log";
import { readFile, rename, unlink, writeFile } from "fs/promises";
import path from "path";

import type { PlaybackOverlayFile, ReplayPlaybackStatus } from "./types";

const log = electronLog.scope("dolphin/room-watch");

export const ROOM_WATCH_CLIPS_FILE = "room-watch-clips.json";
export const ROOM_WATCH_OVERLAY_FILE = "room-watch-overlay.json";
/** The clipped games' folder in `<Online>/` (manual_clips.ts copies them out as spectated games). */
export const ROOM_WATCH_FOLDER = "RoomWatch";
/** The clips file is at most this large (500 clips and renders, as the game keeps at most). */
const MAX_CLIPS_FILE = 512 << 10;

/** The clips file as a playback status (only its clip fields, each checked), or null when it isn't one. */
export function parseRoomWatchClips(text: string): ReplayPlaybackStatus | null {
  let raw: unknown;
  try {
    raw = JSON.parse(text);
  } catch {
    return null;
  }
  if (!raw || typeof raw !== "object") {
    return null;
  }
  const o = raw as Record<string, unknown>;
  if (o.version !== 1 || typeof o.run !== "string") {
    return null;
  }
  const frame = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v) && v >= 0;
  const savedClips = (Array.isArray(o.savedClips) ? o.savedClips : [])
    .filter((c): c is Record<string, unknown> => !!c && typeof c === "object")
    .filter((c) => typeof c.path === "string" && frame(c.startFrame) && frame(c.endFrame))
    .map((c) => ({ path: c.path as string, startFrame: c.startFrame as number, endFrame: c.endFrame as number }));
  const renderRequests = (Array.isArray(o.renderRequests) ? o.renderRequests : [])
    .filter((a): a is Record<string, unknown> => !!a && typeof a === "object")
    .filter(
      (a) =>
        frame(a.seq) &&
        typeof a.path === "string" &&
        frame(a.startFrame) &&
        frame(a.endFrame) &&
        typeof a.music === "boolean",
    )
    .map((a) => ({
      seq: a.seq as number,
      path: a.path as string,
      startFrame: a.startFrame as number,
      endFrame: a.endFrame as number,
      music: a.music as boolean,
      clip: a.clip !== false,
      ...(typeof a.label === "string" ? { label: a.label } : {}),
    }));
  const show = o.showRender as Record<string, unknown> | undefined;
  return {
    run: o.run,
    savedClips,
    renderRequests,
    ...(show && typeof show === "object" && frame(show.seq) && typeof show.id === "string"
      ? { showRender: { seq: show.seq, id: show.id } }
      : {}),
  };
}

export type RoomWatchFilesDeps = {
  /** The open netplay game's `<User>/Online/`, or null when none is open. */
  onlineDir: () => string | null;
  /** The launcher's side of the seek bar, as the playback Dolphins get it (null: none). */
  overlay: () => PlaybackOverlayFile | null;
  /** A clips file that changed. */
  onStatus: (status: ReplayPlaybackStatus) => void;
};

export class RoomWatchFiles {
  private clipsRead = "";
  private overlayWritten: { dir: string; text: string } | null = null;
  private busy = false;

  constructor(private readonly deps: RoomWatchFilesDeps) {}

  /** One look at both files (a timer's; a tick still going is not run twice). */
  async tick(): Promise<void> {
    if (this.busy) {
      return;
    }
    this.busy = true;
    try {
      const dir = this.deps.onlineDir();
      if (dir) {
        await this.readClips(dir);
        await this.writeOverlay(dir);
      }
    } finally {
      this.busy = false;
    }
  }

  private async readClips(dir: string) {
    let text: string;
    try {
      text = await readFile(path.join(dir, ROOM_WATCH_CLIPS_FILE), "utf8");
    } catch {
      // (None: nothing clipped in this run of the game, which deletes the last run's at its start.)
      this.clipsRead = "";
      return;
    }
    if (text === this.clipsRead || text.length > MAX_CLIPS_FILE) {
      return;
    }
    const status = parseRoomWatchClips(text);
    if (!status) {
      // (A file being replaced is renamed over whole: this is not one of ours.)
      return;
    }
    this.clipsRead = text;
    this.deps.onStatus(status);
  }

  private async writeOverlay(dir: string) {
    const overlay = this.deps.overlay();
    const text = overlay ? JSON.stringify(overlay) : "";
    if (this.overlayWritten?.dir === dir && this.overlayWritten.text === text) {
      return;
    }
    const file = path.join(dir, ROOM_WATCH_OVERLAY_FILE);
    try {
      if (overlay) {
        const tmp = `${file}.tmp`;
        await writeFile(tmp, text);
        await rename(tmp, file);
      } else {
        await unlink(file).catch(() => undefined);
      }
      this.overlayWritten = { dir, text };
    } catch (err) {
      log.warn(`room watch overlay: ${err instanceof Error ? err.message : err}`);
    }
  }
}
