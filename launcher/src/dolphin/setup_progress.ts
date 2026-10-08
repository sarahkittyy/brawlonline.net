// How the set-up steps' progress reaches the Play button (Slippi's Dolphin download progress):
// DOWNLOAD_START when a step starts doing something, DOWNLOAD_PROGRESS with the step's phase,
// DOWNLOAD_COMPLETE when it ends.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import type { DolphinLaunchType, DolphinSetupPhase } from "./types";

/**
 * Decides which progress reports are sent: at most one every `intervalMs` per launch type (a 2 GB
 * step reports thousands of chunks), but always a step's first (0) and last (`current >= total`)
 * value and the first value of a new phase, so the button never misses a label or the end.
 */
export function createProgressThrottle(intervalMs: number, now: () => number = Date.now) {
  const last = new Map<DolphinLaunchType, { at: number; phase?: DolphinSetupPhase }>();
  return (dolphinType: DolphinLaunchType, current: number, total: number, phase?: DolphinSetupPhase): boolean => {
    const t = now();
    const prev = last.get(dolphinType);
    if (prev && prev.phase === phase && current > 0 && current < total && t - prev.at < intervalMs) {
      return false;
    }
    last.set(dolphinType, { at: t, phase });
    return true;
  };
}

/**
 * Runs a step that may report progress. The button shows the step from its first report and is
 * ready again when the step ends, also when it fails. A step with nothing to do (no report)
 * leaves the button alone.
 */
export async function runWithProgress<T>(
  step: (onProgress: (current: number, total: number) => void) => Promise<T>,
  events: {
    start: () => void;
    progress: (current: number, total: number) => void;
    complete: () => void;
  },
): Promise<T> {
  let started = false;
  try {
    return await step((current, total) => {
      if (!started) {
        started = true;
        events.start();
      }
      events.progress(current, total);
    });
  } finally {
    if (started) {
      events.complete();
    }
  }
}
