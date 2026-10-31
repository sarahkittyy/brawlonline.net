/**
 * Play pressed again while the netplay Dolphin is still starting. Bug report #209 (2026-10-31,
 * "Dolphin insta-crashes after resetting settings"): Play clicks wait their turn (DolphinManager's
 * `sdCardStarts`), and the next one asked the Dolphin the last one had just started to close if
 * idle (quit_if_idle.ts). A Dolphin that is starting has no game yet (none in the Launch Dolphin
 * mode, and none until it boots P+ otherwise), so it closed 0.2 s after it opened, the player
 * pressed Play again while it closed, and that click closed the next one too.
 *
 * Now a Play while one is being started waits for that start, and a Play with the same boot
 * choice within `STARTING_GRACE_MS` of the start keeps the Dolphin it started. Later, a Dolphin
 * without a game still closes and starts again (its game was stopped: the user, 2026-10-18).
 */

/** How long after Play started the netplay Dolphin another Play keeps it instead of restarting it. */
export const STARTING_GRACE_MS = 10_000;

/** The netplay Dolphin Play started: whether it boots P+, and when (ms). */
export type NetplayStart = { bootsGame: boolean; at: number };

/** True when a Play that boots P+ or not (`bootsGame`) at `now` repeats `start` and keeps its Dolphin. */
export function repeatsStart(start: NetplayStart | null, bootsGame: boolean, now: number): boolean {
  return start != null && start.bootsGame === bootsGame && now - start.at < STARTING_GRACE_MS;
}

/**
 * One start at a time per boot choice: a call while one with the same `key` runs gets its result
 * instead of starting again.
 */
export class SharedStart<K, T> {
  private running: { key: K; promise: Promise<T> } | null = null;

  async run(key: K, start: () => Promise<T>, onJoin?: () => void): Promise<T> {
    const running = this.running;
    if (running && running.key === key) {
      onJoin?.();
      return await running.promise;
    }
    const promise = start();
    this.running = { key, promise };
    try {
      return await promise;
    } finally {
      if (this.running?.promise === promise) {
        this.running = null;
      }
    }
  }
}
