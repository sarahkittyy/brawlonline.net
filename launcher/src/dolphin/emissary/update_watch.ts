// A running EmissaryRecomp's update, as its game hears of it (docs/design/brawl-recompiled.md §16).
// mm tells a recomp older than the latest release so as soon as it reads the release
// (`emissary-update`); the game shows "Update Available" and writes it into game-status.json
// (`emissaryUpdate: {version, requested}`). While the recomp runs, this reads that file:
// - a version not seen before: GitHub is asked again at once (the launcher's own check runs every
//   30 minutes), and with "Update automatically" on, the release downloads beside the game
//   (staged; it applies when the game closes, or at the next Play);
// - Update Now (`requested`, a time): the release is downloaded if it isn't yet, the game is closed
//   once it is idle (never in a match, a search or a set) and Play starts the new one.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import type { GameStatus } from "../room_handoff";
import type { EmissaryState } from "./types";

/** How often game-status.json is read while the recomp runs. */
export const UPDATE_WATCH_MS = 2000;
/** An Update Now older than this when first read (the launcher started after it) is not acted on. */
export const REQUEST_MAX_AGE_MS = 2 * 60 * 1000;
/** How long Update Now waits for the game to be idle, and for GitHub to list the release. */
export const UPDATE_NOW_WAIT_MS = 10 * 60 * 1000;
const RETRY_MS = 5000;

export type UpdateWatchDeps = {
  running: () => boolean;
  status: () => Promise<GameStatus | null>;
  state: (refresh: boolean) => Promise<EmissaryState>;
  /** Downloads the latest release (staged while the game runs). */
  install: () => Promise<EmissaryState>;
  autoUpdate: () => boolean;
  /** Closes the game when it is idle and a release is staged; true once it is closed. */
  closeForUpdate: () => Promise<boolean>;
  /** Play again (the renderer's Play, so its progress and fallbacks show). */
  relaunch: () => void;
  log: (message: string) => void;
  now?: () => number;
  sleep?: (ms: number) => Promise<void>;
};

export type EmissaryUpdate = { version: string; requested: string | null };

/** game-status.json's `emissaryUpdate`, when it is well formed. */
export function readEmissaryUpdate(status: GameStatus | null): EmissaryUpdate | null {
  const u = (status as { emissaryUpdate?: unknown } | null)?.emissaryUpdate as
    | { version?: unknown; requested?: unknown }
    | undefined;
  if (!u || typeof u.version !== "string" || !/^v[0-9.]+$/.test(u.version)) {
    return null;
  }
  return { version: u.version, requested: typeof u.requested === "string" ? u.requested : null };
}

export class EmissaryUpdateWatch {
  private seenVersion: string | null = null;
  private seenRequest: string | null = null;
  private first = true;
  private updating: Promise<void> | null = null;
  private readonly now: () => number;
  private readonly sleep: (ms: number) => Promise<void>;

  constructor(private readonly deps: UpdateWatchDeps) {
    this.now = deps.now ?? Date.now;
    this.sleep = deps.sleep ?? ((ms) => new Promise((resolve) => setTimeout(resolve, ms)));
  }

  /** One read of game-status.json (every UPDATE_WATCH_MS). */
  async tick(): Promise<void> {
    if (!this.deps.running()) {
      // The next game starts afresh: its request, if any, is new.
      this.first = true;
      this.seenRequest = null;
      return;
    }
    const update = readEmissaryUpdate(await this.deps.status());
    const first = this.first;
    this.first = false;
    if (!update) {
      return;
    }
    if (update.version !== this.seenVersion) {
      this.seenVersion = update.version;
      await this.onNewVersion(update.version);
    }
    if (update.requested && update.requested !== this.seenRequest) {
      this.seenRequest = update.requested;
      const at = Date.parse(update.requested);
      if (first && (!Number.isFinite(at) || this.now() - at > REQUEST_MAX_AGE_MS)) {
        this.deps.log(`EmissaryRecomp: an old Update Now (${update.requested}) left alone`);
        return;
      }
      if (!this.updating) {
        this.updating = this.updateNow(update.version).finally(() => {
          this.updating = null;
        });
      }
    }
  }

  /** Resolves when the Update Now that runs (if any) is done; for tests. */
  async idle(): Promise<void> {
    await this.updating;
  }

  private async onNewVersion(version: string) {
    const state = await this.deps.state(true).catch(() => null);
    this.deps.log(
      `EmissaryRecomp: the server says ${version} is out (installed ${state?.installed?.version ?? "?"}, ` +
        `GitHub's latest ${state?.latest?.version ?? "unknown"})`,
    );
    if (this.deps.autoUpdate() && state?.installed && !state.upToDate && !state.job && !state.staged) {
      await this.deps
        .install()
        .catch((err) => this.deps.log(`EmissaryRecomp auto-update: ${err instanceof Error ? err.message : err}`));
    }
  }

  private async updateNow(version: string) {
    this.deps.log(`EmissaryRecomp: Update Now to ${version}`);
    const deadline = this.now() + UPDATE_NOW_WAIT_MS;
    // The release: GitHub's answer can trail the server's by a minute (its cache).
    for (;;) {
      if (!this.deps.running()) {
        return;
      }
      const state = await this.deps.state(true).catch(() => null);
      if (state?.staged) {
        break;
      }
      // Not downloading already (the auto-update's download is waited for).
      if (state?.installed && !state.upToDate && !state.job) {
        try {
          const after = await this.deps.install();
          if (after.staged) {
            break;
          }
        } catch (err) {
          this.deps.log(`EmissaryRecomp: Update Now's download failed: ${err instanceof Error ? err.message : err}`);
        }
      }
      if (this.now() >= deadline) {
        this.deps.log(`EmissaryRecomp: Update Now gave up: ${version} is not downloaded`);
        return;
      }
      await this.sleep(RETRY_MS);
    }
    // Downloaded: close the game when it is idle, then Play starts the new one.
    for (;;) {
      if (!this.deps.running()) {
        // The player closed it: the staged release applies; nothing to start.
        return;
      }
      if (await this.deps.closeForUpdate()) {
        this.deps.log("EmissaryRecomp: closed for Update Now, starting the new version");
        this.deps.relaunch();
        return;
      }
      if (this.now() >= deadline) {
        this.deps.log("EmissaryRecomp: Update Now gave up: the game stayed busy");
        return;
      }
      await this.sleep(RETRY_MS);
    }
  }
}
