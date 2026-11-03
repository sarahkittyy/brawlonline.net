import { describe, expect, it } from "vitest";

import {
  clipButtonsAt,
  currentClipButtons,
  DEFAULT_CLIP_BUTTONS,
  hotkeyButtons,
  hotkeyLabel,
  withClipHotkey,
} from "./clip_hotkey";

const L_R_A = 0x0160;
const START_DOWN = 0x1004;

describe("clip hotkey", () => {
  it("is L+R+Z until one is set", () => {
    expect(currentClipButtons([])).toBe(DEFAULT_CLIP_BUTTONS);
    expect(currentClipButtons(undefined)).toBe(DEFAULT_CLIP_BUTTONS);
    expect(hotkeyLabel(DEFAULT_CLIP_BUTTONS)).toBe("L + R + Z");
  });

  it("writes the buttons in the controller's order", () => {
    expect(hotkeyLabel(0x1000 | 0x0100 | 0x0040 | 0x0001)).toBe("L + A + Start + D-Pad Left");
    expect(hotkeyButtons(START_DOWN).map((b) => b.name)).toEqual(["Start", "D-Pad Down"]);
  });

  it("searches a game with the hotkey of its time", () => {
    let history = withClipHotkey([], L_R_A, 1000);
    history = withClipHotkey(history, START_DOWN, 2000);
    expect(clipButtonsAt(history, 500)).toBe(DEFAULT_CLIP_BUTTONS);
    expect(clipButtonsAt(history, 1000)).toBe(L_R_A);
    expect(clipButtonsAt(history, 1999)).toBe(L_R_A);
    expect(clipButtonsAt(history, 2500)).toBe(START_DOWN);
    expect(currentClipButtons(history)).toBe(START_DOWN);
    // Back to L+R+Z: earlier games keep theirs.
    history = withClipHotkey(history, DEFAULT_CLIP_BUTTONS, 3000);
    expect(clipButtonsAt(history, 2500)).toBe(START_DOWN);
    expect(currentClipButtons(history)).toBe(DEFAULT_CLIP_BUTTONS);
  });

  it("adds nothing for the hotkey it already is, or for no buttons", () => {
    expect(withClipHotkey([], DEFAULT_CLIP_BUTTONS, 1000)).toEqual([]);
    const history = withClipHotkey([], L_R_A, 1000);
    expect(withClipHotkey(history, L_R_A, 2000)).toEqual(history);
    expect(withClipHotkey(history, 0, 2000)).toEqual(history);
    // Bits that are no GameCube button are dropped.
    expect(withClipHotkey([], 0x2000 | L_R_A, 1000)).toEqual([{ buttons: L_R_A, since: 1000 }]);
  });

  it("ignores entries that are not hotkeys", () => {
    const history = [{ buttons: 0, since: 10 }, { buttons: L_R_A, since: Number.NaN }, null] as never;
    expect(currentClipButtons(history)).toBe(DEFAULT_CLIP_BUTTONS);
  });
});
