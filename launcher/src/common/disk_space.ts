// What the launcher tells a player whose disk is full (bug reports #68 and #91: a full C: drive
// showed "Error invoking remote method 'downloadDolphin': Error: ENOSPC: no space left on device,
// write", or the set-up toast's "close all Dolphin instances"). The main process words the message
// (dolphin/install/disk_space.ts); the renderer shows it as it is when it starts with the prefix.

/** How every disk-full message starts: the renderer shows such a message instead of its own. */
export const DISK_SPACE_MESSAGE_PREFIX = "Not enough disk space";

export const formatBytes = (n: number): string =>
  n >= 1e9
    ? `${(n / 1e9).toFixed(1)} GB`
    : n >= 1e6
    ? `${Math.max(1, Math.round(n / 1e6))} MB`
    : `${Math.ceil(n / 1e3)} KB`;

/** Whether `err` (maybe through IPC, behind Electron's prefix) carries a disk-full message. */
export function isDiskSpaceError(err: unknown): boolean {
  const message = err instanceof Error ? err.message : typeof err === "string" ? err : "";
  return message.includes(DISK_SPACE_MESSAGE_PREFIX);
}

/**
 * The message for `what` (e.g. "Dolphin") that needs `needBytes` free on `place` (a drive like
 * "C:", or "the disk of <folder>" where drives have no letter), which has `freeBytes`. `needBytes`
 * null: the disk filled up while writing. Null `freeBytes`: unknown.
 */
export function diskSpaceMessage({
  what,
  place,
  needBytes,
  freeBytes,
}: {
  what: string;
  place: string;
  needBytes: number | null;
  freeBytes: number | null;
}): string {
  const free = freeBytes == null ? null : `${formatBytes(freeBytes)} free`;
  const detail =
    needBytes == null
      ? `the disk is full${free ? ` (${free})` : ""}`
      : `${formatBytes(needBytes)} needed${free ? `, ${free}` : ""}`;
  return `${DISK_SPACE_MESSAGE_PREFIX} on ${place} for ${what}: ${detail}. Free up space on ${place}, then restart the launcher.`;
}

/** An error's message without Electron's "Error invoking remote method '…': Error: " prefix. */
export function withoutIpcPrefix(message: string): string {
  return message.replace(/^Error invoking remote method '[^']*': (\w*Error: )?/, "");
}
