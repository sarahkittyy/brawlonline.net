/**
 * Handing a room code from the launcher to the game (docs/rooms-protocol.md, "Launcher and game").
 *
 * Two small files in Dolphin's online folder (`<User>/Online/`, next to `user.json`, which
 * Dolphin already watches):
 *
 * - `join-room.json`, launcher -> game: `{version, id, code, createdAt}`. Written (atomically) when
 *   a room in the list is clicked. A running game picks it up, deletes it and jumps to the room's
 *   character select; a game that is starting reads it once it reaches the menus.
 * - `game-status.json`, game -> launcher: `{version, state, screen, room, updatedAt, pid,
 *   lastRequest}`. Rewritten on every change and at least every 5 s. `state` is `idle` (menus, an
 *   idle online character select, a room) or `busy` (a match, a search, a single-player mode);
 *   `lastRequest` answers the last `join-room.json` by its `id`.
 *
 * A Direct request (a launcher invite to a Direct 1v1, docs/social-protocol.md §5) is the same
 * file with `{version, id, kind: "direct", connectCode, createdAt}`: the game goes to Direct's
 * character select and, on lock-in, searches that code. Every function here that takes a room code
 * also takes a connect code (`ABCD#123`) for that, and `CREATE_ROOM` for a create request
 * (`{version, id, kind: "create", createdAt}`: the game creates a room as WITH FRIENDS > Create Room
 * does, from Home's "Create room" button).
 *
 * Dolphin builds without this (everything before the rooms phase) write no status. Then the
 * launcher still writes the request, and when nothing answers it within a few seconds it takes
 * the request back and tells the player the code to type in game.
 */

import { randomUUID } from "crypto";
import { readFile, rename, unlink, writeFile } from "fs/promises";
import path from "path";

import { CREATE_ROOM } from "./hand_off_target";

export { CREATE_ROOM };

export const ROOM_REQUEST_FILE = "join-room.json";
export const GAME_STATUS_FILE = "game-status.json";
/** A status older than this is from a game that is gone (or one that does not write it). */
export const STATUS_FRESH_MS = 15_000;
/** How long a running game has to answer a request. */
export const ACK_TIMEOUT_MS = 5_000;
const ACK_POLL_MS = 200;
/**
 * A create is answered `accepted` at once and `failed` (same id) when mm refuses it, within about a
 * second ("Too many rooms. Wait a moment."). Slower failures (no server for 20 s) show in game.
 */
export const FAILED_WATCH_MS = 3_000;

export type RoomRequest =
  | { version: 1; id: string; code: string; createdAt: string }
  | { version: 1; id: string; kind: "direct"; connectCode: string; createdAt: string }
  | { version: 1; id: string; kind: "create"; createdAt: string };

export type GameStatus = {
  version: number;
  state: "idle" | "busy";
  /** What the game shows: `menus`, `online-css`, `room`, `match`, `searching`, `single-player`, `other`. */
  screen?: string;
  /** The room the player is in, if any. */
  room?: string | null;
  updatedAt: string;
  pid?: number;
  lastRequest?: { id: string; result: "accepted" | "refused" | "failed"; message?: string } | null;
};

/** What happened to a room click while the game runs. */
export type RoomHandOffResult =
  /** The game took the code and is going to the room. */
  | { outcome: "accepted" }
  /** The game is busy (a match, a search, a single-player mode): nothing was sent. */
  | { outcome: "busy" }
  /** The game read the request and said no (`message` is its reason, if it gave one). */
  | { outcome: "refused"; message?: string }
  /** No game answered (an older build, or a game stuck loading): the request was taken back. */
  | { outcome: "no-response" }
  /** No netplay Dolphin is running: the caller starts one with Play. */
  | { outcome: "not-running" };

export type StatusRead =
  | { kind: "missing" }
  | { kind: "stale"; status: GameStatus }
  | { kind: "fresh"; status: GameStatus };

/** Room codes: 4 letters without vowels (and without Y). */
export function isRoomCode(code: string): boolean {
  return /^[BCDFGHJKLMNPQRSTVWXZ]{4}$/.test(code);
}

export function requestPath(onlineDir: string): string {
  return path.join(onlineDir, ROOM_REQUEST_FILE);
}

export function statusPath(onlineDir: string): string {
  return path.join(onlineDir, GAME_STATUS_FILE);
}

/** Connect codes: a 2-4 letter prefix, `#`, a number; 8 characters at most. */
export function isConnectCode(code: string): boolean {
  return code.length <= 8 && /^[A-Z]{2,4}#[1-9][0-9]{0,3}$/.test(code);
}

/** A room code, a connect code (Direct) or `CREATE_ROOM`. */
export function isHandOffCode(code: string): boolean {
  return isRoomCode(code) || isConnectCode(code) || code === CREATE_ROOM;
}

/**
 * Writes `join-room.json` atomically (a reader never sees half a file): a room request for a room
 * code, a Direct request for a connect code.
 */
export async function writeRoomRequest(onlineDir: string, code: string, now = new Date()): Promise<RoomRequest> {
  if (!isHandOffCode(code)) {
    throw new Error(`Not a room or connect code: ${code}`);
  }
  const id = randomUUID();
  const createdAt = now.toISOString();
  const req: RoomRequest =
    code === CREATE_ROOM
      ? { version: 1, id, kind: "create", createdAt }
      : isRoomCode(code)
      ? { version: 1, id, code, createdAt }
      : { version: 1, id, kind: "direct", connectCode: code, createdAt };
  const file = requestPath(onlineDir);
  const tmp = `${file}.${id}.tmp`;
  await writeFile(tmp, JSON.stringify(req, null, 2));
  await rename(tmp, file);
  return req;
}

/** Removes `join-room.json` if it is still this request (or any request, without `id`). */
export async function clearRoomRequest(onlineDir: string, id?: string): Promise<boolean> {
  const file = requestPath(onlineDir);
  try {
    if (id) {
      const current = JSON.parse(await readFile(file, "utf8")) as Partial<RoomRequest>;
      if (current.id !== id) {
        return false;
      }
    }
    await unlink(file);
    return true;
  } catch {
    return false;
  }
}

/** Reads `game-status.json`. Anything unreadable counts as missing. */
export async function readGameStatus(onlineDir: string, now = Date.now()): Promise<StatusRead> {
  let status: GameStatus;
  try {
    status = JSON.parse(await readFile(statusPath(onlineDir), "utf8")) as GameStatus;
  } catch {
    return { kind: "missing" };
  }
  if (!status || typeof status !== "object" || (status.state !== "idle" && status.state !== "busy")) {
    return { kind: "missing" };
  }
  const at = Date.parse(status.updatedAt);
  if (!Number.isFinite(at) || now - at > STATUS_FRESH_MS) {
    return { kind: "stale", status };
  }
  return { kind: "fresh", status };
}

export type HandOffOptions = {
  ackTimeoutMs?: number;
  /** How long to keep watching after `accepted` (default: a create's FAILED_WATCH_MS, else 0). */
  watchAfterAcceptMs?: number;
  pollMs?: number;
  sleep?: (ms: number) => Promise<void>;
  now?: () => number;
};

/**
 * Hands `code` (a room code, or a connect code for Direct) to a running game: refuses at once when its status says busy, else writes the
 * request and waits for the game's answer. Without an answer the request is taken back, so a later
 * game start does not act on it.
 */
export async function handOffRoom(
  onlineDir: string,
  code: string,
  opts: HandOffOptions = {},
): Promise<RoomHandOffResult> {
  const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
  const now = opts.now ?? Date.now;
  const before = await readGameStatus(onlineDir, now());
  if (before.kind === "fresh" && before.status.state === "busy") {
    return { outcome: "busy" };
  }
  const req = await writeRoomRequest(onlineDir, code, new Date(now()));
  const deadline = now() + (opts.ackTimeoutMs ?? ACK_TIMEOUT_MS);
  while (now() < deadline) {
    await sleep(opts.pollMs ?? ACK_POLL_MS);
    const s = await readGameStatus(onlineDir, now());
    const answer = s.kind === "missing" ? null : s.status.lastRequest;
    if (answer?.id === req.id) {
      switch (answer.result) {
        case "accepted":
          return await watchAfterAccept(onlineDir, req.id, opts, code === CREATE_ROOM ? FAILED_WATCH_MS : 0);
        case "refused":
          return s.kind === "fresh" && s.status.state === "busy"
            ? { outcome: "busy" }
            : { outcome: "refused", message: answer.message };
        default:
          return { outcome: "refused", message: answer.message };
      }
    }
  }
  await clearRoomRequest(onlineDir, req.id);
  return { outcome: "no-response" };
}

/** Keeps reading the status for `watchMs` after `accepted`: a `failed` for the same request wins. */
async function watchAfterAccept(
  onlineDir: string,
  id: string,
  opts: HandOffOptions,
  defaultMs: number,
): Promise<RoomHandOffResult> {
  const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
  const now = opts.now ?? Date.now;
  const until = now() + (opts.watchAfterAcceptMs ?? defaultMs);
  while (now() < until) {
    await sleep(opts.pollMs ?? ACK_POLL_MS);
    const s = await readGameStatus(onlineDir, now());
    const answer = s.kind === "missing" ? null : s.status.lastRequest;
    if (answer?.id === id && answer.result !== "accepted") {
      return { outcome: "refused", message: answer.message };
    }
  }
  return { outcome: "accepted" };
}
