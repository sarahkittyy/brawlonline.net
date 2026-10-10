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
 * Dolphin builds without this (everything before the rooms phase) write no status. Then the
 * launcher still writes the request, and when nothing answers it within a few seconds it takes
 * the request back and tells the player the code to type in game.
 */

import { randomUUID } from "crypto";
import { readFile, rename, unlink, writeFile } from "fs/promises";
import path from "path";

export const ROOM_REQUEST_FILE = "join-room.json";
export const GAME_STATUS_FILE = "game-status.json";
/** A status older than this is from a game that is gone (or one that does not write it). */
export const STATUS_FRESH_MS = 15_000;
/** How long a running game has to answer a request. */
export const ACK_TIMEOUT_MS = 5_000;
const ACK_POLL_MS = 200;

export type RoomRequest = { version: 1; id: string; code: string; createdAt: string };

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

/** Writes `join-room.json` atomically (a reader never sees half a file). */
export async function writeRoomRequest(onlineDir: string, code: string, now = new Date()): Promise<RoomRequest> {
  if (!isRoomCode(code)) {
    throw new Error(`Not a room code: ${code}`);
  }
  const req: RoomRequest = { version: 1, id: randomUUID(), code, createdAt: now.toISOString() };
  const file = requestPath(onlineDir);
  const tmp = `${file}.${req.id}.tmp`;
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
  pollMs?: number;
  sleep?: (ms: number) => Promise<void>;
  now?: () => number;
};

/**
 * Hands `code` to a running game: refuses at once when its status says busy, else writes the
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
          return { outcome: "accepted" };
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
