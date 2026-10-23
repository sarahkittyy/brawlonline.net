/**
 * The online input delay setting (`inputDelay`, Dolphin.ini `[Online] InputDelay`): the frames
 * between a player's button press and the game using it, for that player's own inputs, as Slippi's
 * delay frames.
 *
 * A profile (0 Normal, the default; -1 Aggressive; -2 Smooth) has Dolphin pick the frames from the
 * ping at the start of each game (`Gprb::Session::ProfileInputDelay`), and in an Other Games room
 * each time someone joins (`Machine::PickInputDelay`). Each player plays their own profile: a
 * player's delay smooths their own screen only.
 * The launcher offers only the profiles (since 2026-10-23; before, also 1-4 frames, which applied in
 * Direct and rooms only). A stored number of frames plays Normal. Dolphin itself still takes 1-9
 * frames (the harness's `delay`).
 */
export const INPUT_DELAY_NORMAL = 0;
export const INPUT_DELAY_AGGRESSIVE = -1;
export const INPUT_DELAY_SMOOTH = -2;

/** The profiles, as the setting lists them: least delay first. */
export const INPUT_DELAY_PROFILES: readonly number[] = [INPUT_DELAY_AGGRESSIVE, INPUT_DELAY_NORMAL, INPUT_DELAY_SMOOTH];

/** A stored or ini value as the launcher uses it: a profile, else Normal. */
export function normalizeInputDelay(value: unknown): number {
  const n = typeof value === "string" ? Number(value.trim() === "" ? NaN : value) : value;
  return typeof n === "number" && INPUT_DELAY_PROFILES.includes(n) ? n : INPUT_DELAY_NORMAL;
}
