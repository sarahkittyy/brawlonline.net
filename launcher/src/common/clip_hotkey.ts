/**
 * Settings > Clips > Manual Clip Hotkey: the buttons that clip the last 20 seconds when pressed
 * together in game (replays/highlights/lrz_clips.ts finds the presses in the replay's pads, after
 * the game). L+R+Z until the player sets another.
 *
 * The setting keeps every hotkey the player has set, with when it was set: a replay is searched
 * with the hotkey of the time it was played. A new hotkey makes clips in the games played after
 * it, and the clips of earlier games (made with the old one) stay; nothing in an old game turns
 * into a clip because it holds the new buttons (Sarah's board, 2026-11-03: "i don't think anyone
 * would want their old clips to go away").
 */

/**
 * A GameCube button: its bit in Dolphin's GCPadStatus (PAD_BUTTON_* / PAD_TRIGGER_*), which is
 * also its bit in Brawl's gfPadStatus buttons, a replay's pads. The D-pad's checked against
 * taunts in 200 replays (2026-11-03): 0x8 started up taunts, 0x4 down taunts, 0x2 side taunts.
 */
export type GcButton = { bit: number; name: string };

/** In the order a hotkey is written: shoulders, face buttons, Start, D-pad. */
export const GC_BUTTONS: readonly GcButton[] = [
  { bit: 0x0040, name: "L" },
  { bit: 0x0020, name: "R" },
  { bit: 0x0010, name: "Z" },
  { bit: 0x0100, name: "A" },
  { bit: 0x0200, name: "B" },
  { bit: 0x0400, name: "X" },
  { bit: 0x0800, name: "Y" },
  { bit: 0x1000, name: "Start" },
  { bit: 0x0008, name: "D-Pad Up" },
  { bit: 0x0004, name: "D-Pad Down" },
  { bit: 0x0001, name: "D-Pad Left" },
  { bit: 0x0002, name: "D-Pad Right" },
];

export const GC_BUTTON_MASK = GC_BUTTONS.reduce((mask, b) => mask | b.bit, 0);

/** L+R+Z: the hotkey until the player sets one. */
export const DEFAULT_CLIP_BUTTONS = 0x0070;

export type ClipHotkey = {
  /** GC_BUTTONS bits, all held together. */
  buttons: number;
  /** When it was set (ms since 1970): it is the hotkey of the games played from then on. */
  since: number;
};

/** The buttons in a mask, in GC_BUTTONS' order. */
export function hotkeyButtons(buttons: number): GcButton[] {
  return GC_BUTTONS.filter((b) => (buttons & b.bit) !== 0);
}

/** "L + R + Z". */
export function hotkeyLabel(buttons: number): string {
  return hotkeyButtons(buttons)
    .map((b) => b.name)
    .join(" + ");
}

const valid = (h: unknown): h is ClipHotkey =>
  typeof h === "object" &&
  h !== null &&
  Number.isInteger((h as ClipHotkey).buttons) &&
  ((h as ClipHotkey).buttons & GC_BUTTON_MASK) !== 0 &&
  Number.isFinite((h as ClipHotkey).since);

/** The hotkey now. */
export function currentClipButtons(history: readonly ClipHotkey[] | undefined): number {
  return clipButtonsAt(history, Number.POSITIVE_INFINITY);
}

/** The hotkey of a game played at `time` (ms since 1970). */
export function clipButtonsAt(history: readonly ClipHotkey[] | undefined, time: number): number {
  let buttons = DEFAULT_CLIP_BUTTONS;
  let since = Number.NEGATIVE_INFINITY;
  for (const h of history ?? []) {
    if (valid(h) && h.since <= time && h.since >= since) {
      buttons = h.buttons & GC_BUTTON_MASK;
      since = h.since;
    }
  }
  return buttons;
}

/** The history with `buttons` as the hotkey from `now` on (unchanged when it already is). */
export function withClipHotkey(history: readonly ClipHotkey[] | undefined, buttons: number, now: number): ClipHotkey[] {
  const kept = (history ?? []).filter(valid);
  if ((buttons & GC_BUTTON_MASK) === 0 || currentClipButtons(kept) === (buttons & GC_BUTTON_MASK)) {
    return kept;
  }
  return [...kept, { buttons: buttons & GC_BUTTON_MASK, since: now }];
}
