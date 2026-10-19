/**
 * Asks the open netplay Dolphin whether it has a game, and to close if it has none (its game was
 * stopped and only its main window is left, or Configure's): `quit-if-idle.json` in its online
 * folder, which its main window reads 4 times a second. It answers in `quit-if-idle-answer.json`,
 * `{version, id, result}`: `closing` (no game: it closes, as when its window is closed) or
 * `in-game` (a game is running or booting: it stays, and the launcher hands the click to the game).
 *
 * game-status.json cannot tell: the rooms thread keeps writing it, with the last screen the game
 * showed, until Dolphin exits, so a Dolphin whose game was stopped looks like one in the menus
 * (the user, 2026-10-18: "it says 'sending you there', and then i have to close dolphin and click
 * again"). Dolphins before the answer only delete the request (and close without a game), so for
 * them the wait runs out while a game runs.
 */

import { randomUUID } from "crypto";
import { readFile, rename, unlink, writeFile } from "fs/promises";
import path from "path";

export const QUIT_IF_IDLE_FILE = "quit-if-idle.json";
export const QUIT_IF_IDLE_ANSWER_FILE = "quit-if-idle-answer.json";
/** How long a Dolphin that has answered nothing may take to close (or a game to keep it open). */
export const ANSWER_TIMEOUT_MS = 4_000;
/** How long a Dolphin that answered `closing` may take to exit (it leaves its room, logs out). */
export const CLOSING_TIMEOUT_MS = 15_000;
const POLL_MS = 100;

export type QuitIfIdleAnswer = { version: number; id: string | null; result: "closing" | "in-game" };

export type QuitIfIdleOptions = {
  answerTimeoutMs?: number;
  closingTimeoutMs?: number;
  pollMs?: number;
  sleep?: (ms: number) => Promise<void>;
  now?: () => number;
  log?: (message: string) => void;
};

async function readAnswer(onlineDir: string): Promise<QuitIfIdleAnswer | null> {
  try {
    const answer = JSON.parse(await readFile(path.join(onlineDir, QUIT_IF_IDLE_ANSWER_FILE), "utf8"));
    return answer && (answer.result === "closing" || answer.result === "in-game") ? answer : null;
  } catch {
    return null;
  }
}

/**
 * True when no Dolphin is open any more (it had no game and closed), false while one with a game
 * is open. `isOpen` says whether the launcher's netplay Dolphin is still running.
 */
export async function closeIfIdle(
  onlineDir: string,
  isOpen: () => boolean,
  opts: QuitIfIdleOptions = {},
): Promise<boolean> {
  if (!isOpen()) {
    return true;
  }
  const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
  const now = opts.now ?? Date.now;
  const file = path.join(onlineDir, QUIT_IF_IDLE_FILE);
  const answerFile = path.join(onlineDir, QUIT_IF_IDLE_ANSWER_FILE);
  const id = randomUUID();
  await unlink(answerFile).catch(() => undefined);
  const tmp = `${file}.${id}.tmp`;
  await writeFile(tmp, JSON.stringify({ version: 1, id, createdAt: new Date(now()).toISOString() }), "utf8");
  await rename(tmp, file);
  let deadline = now() + (opts.answerTimeoutMs ?? ANSWER_TIMEOUT_MS);
  let answered = false;
  try {
    while (isOpen() && now() < deadline) {
      await sleep(opts.pollMs ?? POLL_MS);
      if (answered || !isOpen()) {
        continue;
      }
      const answer = await readAnswer(onlineDir);
      if (answer?.id !== id) {
        continue;
      }
      if (answer.result === "in-game") {
        opts.log?.("The open Dolphin is in a game");
        return false;
      }
      // Closing: leaving a room and logging out can take longer than an answer.
      answered = true;
      deadline = now() + (opts.closingTimeoutMs ?? CLOSING_TIMEOUT_MS);
    }
  } finally {
    await unlink(file).catch(() => undefined);
    await unlink(answerFile).catch(() => undefined);
  }
  const closed = !isOpen();
  opts.log?.(
    closed
      ? "The open Dolphin had no game and closed"
      : answered
      ? "The open Dolphin said it would close but is still open"
      : "The open Dolphin did not answer and stayed open (a game, or an older build in one)",
  );
  return closed;
}
