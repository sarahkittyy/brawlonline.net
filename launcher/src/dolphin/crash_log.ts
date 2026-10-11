import { copyFile, readdir, rm } from "node:fs/promises";
import path from "path";

/** How many crash logs a User folder keeps. */
export const CRASH_LOGS_KEPT = 3;

/** A run ended abnormally: a non-zero exit code, or killed by a signal (no code). */
export function isAbnormalExit(exitCode: number | null, signal: string | null): boolean {
  return exitCode !== 0 || signal !== null;
}

function stamp(date: Date): string {
  const pad = (n: number) => n.toString().padStart(2, "0");
  return (
    `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}-` +
    `${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`
  );
}

/**
 * Copies the run's dolphin.log to crash-<time>.log next to it and keeps the newest
 * CRASH_LOGS_KEPT of them. Dolphin keeps only the last two runs' logs, so a player who starts the
 * game again after a crash (and quits to send the logs) overwrites the one that shows it
 * (staging feedback 2026-10-11: a zip of two clean runs for a Linux player's crashes).
 * Returns the copy's path, or null when there was no log.
 */
export async function keepCrashLog(logsFolder: string, now = new Date()): Promise<string | null> {
  const dest = path.join(logsFolder, `crash-${stamp(now)}.log`);
  try {
    await copyFile(path.join(logsFolder, "dolphin.log"), dest);
  } catch {
    return null;
  }
  // The stamp sorts by time.
  const crashLogs = (await readdir(logsFolder).catch(() => []))
    .filter((name) => /^crash-.*\.log$/.test(name))
    .sort()
    .reverse();
  for (const name of crashLogs.slice(CRASH_LOGS_KEPT)) {
    await rm(path.join(logsFolder, name), { force: true });
  }
  return dest;
}
