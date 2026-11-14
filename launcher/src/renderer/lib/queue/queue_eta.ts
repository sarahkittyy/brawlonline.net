import { QueueMessages } from "./queue.messages";

/**
 * The ETA beside a Play button and in the queue box (docs/design/launcher-redesign.md §7): `GET /v1/rooms`' queueEta,
 * the hour's median wait. null is "10+ min" (Sarah's words): most searches that hour gave up, or there were too few.
 */
export function formatEta(seconds: number | null): string {
  if (seconds == null || seconds >= 600) {
    return QueueMessages.etaLong();
  }
  return formatClock(seconds);
}

/** `m:ss`, as the search timer and the live games' clocks read. */
export function formatClock(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}
