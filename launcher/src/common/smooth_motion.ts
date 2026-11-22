/**
 * The Smooth Motion setting (`smoothMotion`, Dolphin.ini `[Online] SmoothMotion`): 0 (off), or the
 * screen rate Dolphin draws in-between pictures for: one a frame at 120 Hz, two at 180, up to five
 * at 360, by geometry-based frame interpolation (VideoCommon/FrameInterpolation.h). The game still
 * runs at 60; each frame's own picture shows a part of a frame later, and an online match plays one
 * frame of input delay lower to more than make up for it. Only matches and replays get them.
 */
export const SMOOTH_MOTION_RATES: readonly number[] = [120, 180, 240, 300, 360];

/**
 * A stored value as Dolphin takes it: one of the rates (a number, or its text: Dolphin.ini's, a
 * hand-edited settings file's), anything else off.
 */
export function normalizeSmoothMotion(value: unknown): number {
  const rate = Number(value);
  return SMOOTH_MOTION_RATES.includes(rate) ? rate : 0;
}
