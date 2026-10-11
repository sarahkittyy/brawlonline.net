/** The `notificationVolume` setting (0-100) when it is missing or not a number: a quarter of the first launcher's fixed 60 %. */
export const DEFAULT_VOLUME = 15;

/** The setting (0-100) as an audio volume (0-1), clamped. */
export function volumeOf(setting: unknown): number {
  const v = typeof setting === "number" && Number.isFinite(setting) ? setting : DEFAULT_VOLUME;
  return Math.min(100, Math.max(0, v)) / 100;
}
