/**
 * The Input Delay setting, changed while the game runs (docs/rooms-protocol.md §5).
 *
 * `input-delay.json`, launcher -> game, in Dolphin's online folder (`<User>/Online/`, next to
 * `join-room.json`): `{version, id, inputDelay, createdAt}`, `inputDelay` 0 (automatic) or 1-9
 * frames. The game reads it within half a second, deletes it, keeps the setting (its Dolphin.ini,
 * which the launcher writes too) and plays a running game with it from its next frame: a P+ game
 * or an Other Games room alike. `game-status.json` then says `inputDelay: {setting, current,
 * request}` (`current`: the running game's delay, null without one; `request`: this file's `id`).
 *
 * A Dolphin that has not booted a game yet (Configure) reads it at its first boot, where the
 * setting is already in Dolphin.ini; the launcher removes it before each start.
 */

import { randomUUID } from "crypto";
import { rename, unlink, writeFile } from "fs/promises";
import path from "path";

export const INPUT_DELAY_REQUEST_FILE = "input-delay.json";

export type InputDelayRequest = { version: 1; id: string; inputDelay: number; createdAt: string };

export function inputDelayRequestPath(onlineDir: string): string {
  return path.join(onlineDir, INPUT_DELAY_REQUEST_FILE);
}

/** Writes `input-delay.json` atomically (a reader never sees half a file); a newer one replaces it. */
export async function writeInputDelayRequest(
  onlineDir: string,
  inputDelay: number,
  now = new Date(),
): Promise<InputDelayRequest> {
  if (!Number.isInteger(inputDelay) || inputDelay < 0 || inputDelay > 9) {
    throw new Error(`Not an input delay: ${inputDelay}`);
  }
  const req: InputDelayRequest = { version: 1, id: randomUUID(), inputDelay, createdAt: now.toISOString() };
  const file = inputDelayRequestPath(onlineDir);
  const tmp = `${file}.${req.id}.tmp`;
  await writeFile(tmp, JSON.stringify(req, null, 2));
  await rename(tmp, file);
  return req;
}

/** Removes `input-delay.json` (one the game never took). */
export async function clearInputDelayRequest(onlineDir: string): Promise<void> {
  await unlink(inputDelayRequestPath(onlineDir)).catch(() => undefined);
}
