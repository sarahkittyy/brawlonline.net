/**
 * The launcher's hand-off with a Brawl Mods room (docs/design/brawl-mods.md §3): two files in
 * Dolphin's online folder (`<User>/Online/`), as for P+ rooms (room_handoff.ts):
 *
 * - `mods-request.json`, launcher -> Dolphin: `{version: 1, id, op, slot?, open?, public?,
 *   createdAt}`, written atomically; Dolphin reads and deletes it within half a second and answers
 *   in the status's `lastRequest`.
 * - `mods-room.json`, Dolphin -> launcher: the room as mm shows it and the session, on every
 *   change and at least every 5 s.
 */

import { randomUUID } from "crypto";
import { readFile, rename, unlink, writeFile } from "fs/promises";
import path from "path";

import type { ModRoomRequest, ModRoomStatus } from "./types";

export const MODS_REQUEST_FILE = "mods-request.json";
export const MODS_STATUS_FILE = "mods-room.json";
/** A status older than this is from a Dolphin that is gone. */
export const MODS_STATUS_FRESH_MS = 15_000;

/** The status Dolphin last wrote, or null (none, unreadable, or not version 1). */
export async function readModRoomStatus(onlineDir: string): Promise<ModRoomStatus | null> {
  let text: string;
  try {
    text = await readFile(path.join(onlineDir, MODS_STATUS_FILE), "utf8");
  } catch {
    return null;
  }
  try {
    const value = JSON.parse(text) as ModRoomStatus;
    if (typeof value !== "object" || value === null || value.version !== 1) {
      return null;
    }
    return value;
  } catch {
    return null;
  }
}

/** Whether a status is recent enough to show (Dolphin rewrites it at least every 5 s). */
export function isFreshStatus(status: ModRoomStatus, now = Date.now()): boolean {
  const at = Date.parse(status.updatedAt);
  return Number.isFinite(at) && now - at < MODS_STATUS_FRESH_MS;
}

/** Writes a request for the running Dolphin; returns its id (answered in the status). */
export async function writeModRoomRequest(onlineDir: string, request: ModRoomRequest): Promise<string> {
  const id = randomUUID();
  const body = { version: 1, id, ...request, createdAt: new Date().toISOString() };
  const file = path.join(onlineDir, MODS_REQUEST_FILE);
  const tmp = `${file}.tmp`;
  await writeFile(tmp, JSON.stringify(body), "utf8");
  await rename(tmp, file);
  return id;
}

/** Removes the hand-off files a previous Dolphin left (before a launch and after it closed). */
export async function clearModRoomFiles(onlineDir: string): Promise<void> {
  for (const name of [MODS_REQUEST_FILE, MODS_STATUS_FILE]) {
    await unlink(path.join(onlineDir, name)).catch(() => undefined);
  }
}
