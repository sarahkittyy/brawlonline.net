// Closing a running EmissaryRecomp for the launcher (docs/design/brawl-recompiled.md §8.4): for an
// Other Games room or Project Punch (they play in Dolphin), for an update downloaded while it ran,
// and when the launcher quits in its Launch Project+ mode.
//
// Dolphin answers quit-if-idle.json from its main window, which stays open without a game; the
// recomp has no window without its game, so "idle" is what its game says in game-status.json (the
// Brawl Online core in it writes the same file as Dolphin's): `idle` is the menus, an idle online
// character select or a room between games; a match, a search, a set (Ranked games 2 and 3 come
// back to the character select of the same session, which stays `busy`) and single-player modes
// are `busy`. A busy recomp is never closed.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { execFile } from "child_process";

import { readGameStatus } from "../room_handoff";

/** How long the recomp may take to leave its room, log out and exit after it was asked to close. */
export const EMISSARY_CLOSE_TIMEOUT_MS = 15_000;
/** How long it may take to exit once ended. */
const KILL_TIMEOUT_MS = 5_000;
const POLL_MS = 100;

export type EmissaryIdle = { idle: true } | { idle: false; why: string };

/**
 * Whether the recomp of process `pid` (its game) is idle: its game-status.json is fresh, its own
 * (the pid it writes) and says `idle`. Missing or stale (a game stuck, or not written yet): not idle.
 */
export async function emissaryIdle(
  onlineDir: string,
  pid: number | undefined,
  now = Date.now(),
): Promise<EmissaryIdle> {
  const read = await readGameStatus(onlineDir, now);
  if (read.kind !== "fresh") {
    return { idle: false, why: `its game-status.json is ${read.kind}` };
  }
  const status = read.status;
  if (pid != null && status.pid != null && status.pid !== pid) {
    return { idle: false, why: `its game-status.json is another game's (pid ${status.pid})` };
  }
  if (status.state !== "idle") {
    return { idle: false, why: `its game is busy (${status.screen ?? "?"})` };
  }
  return { idle: true };
}

export type CloseOptions = {
  platform?: NodeJS.Platform;
  timeoutMs?: number;
  killTimeoutMs?: number;
  /** Asks the program to close as its window's close button does. */
  ask?: (pid: number) => Promise<void>;
  sleep?: (ms: number) => Promise<void>;
  now?: () => number;
  log?: (message: string) => void;
};

/**
 * Asks the program to close as its window's close button does (Windows: WM_CLOSE through
 * `taskkill` without /F; elsewhere SIGTERM, which SDL turns into the same quit event), so the core
 * leaves its room and logs out; ends it (`kill`) when it is still running after the timeout.
 * Resolves to true once it has exited.
 */
export async function closeEmissary(
  pid: number | undefined,
  isOpen: () => boolean,
  kill: () => void,
  opts: CloseOptions = {},
): Promise<boolean> {
  if (!isOpen()) {
    return true;
  }
  const platform = opts.platform ?? process.platform;
  const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
  const now = opts.now ?? Date.now;
  const ask =
    opts.ask ??
    ((p: number) =>
      platform === "win32"
        ? new Promise<void>((resolve) =>
            execFile("taskkill", ["/PID", String(p)], { windowsHide: true }, () => resolve()),
          )
        : Promise.resolve(void process.kill(p, "SIGTERM")));
  const waitClosed = async (ms: number) => {
    const deadline = now() + ms;
    while (isOpen() && now() < deadline) {
      await sleep(POLL_MS);
    }
    return !isOpen();
  };
  if (pid != null) {
    await ask(pid).catch((err) => opts.log?.(`EmissaryRecomp: could not ask it to close (${err})`));
    if (await waitClosed(opts.timeoutMs ?? EMISSARY_CLOSE_TIMEOUT_MS)) {
      opts.log?.("EmissaryRecomp closed");
      return true;
    }
    opts.log?.("EmissaryRecomp did not close when asked: ending it");
  }
  kill();
  const closed = await waitClosed(opts.killTimeoutMs ?? KILL_TIMEOUT_MS);
  opts.log?.(closed ? "EmissaryRecomp ended" : "EmissaryRecomp is still running after it was ended");
  return closed;
}
