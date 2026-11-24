import { describe, expect, it } from "vitest";

import {
  DEFAULT_INPUT_DISPLAY,
  INPUT_DISPLAY_LAYOUTS,
  INPUT_DISPLAY_MAX_WIDTH,
  INPUT_DISPLAY_MIN_WIDTH,
  normalizeInputDisplay,
} from "./input_display";

describe("normalizeInputDisplay", () => {
  it("is the default for nothing or junk", () => {
    expect(normalizeInputDisplay(undefined)).toEqual(DEFAULT_INPUT_DISPLAY);
    expect(normalizeInputDisplay({ x: NaN, y: "3" as unknown as number, enabled: 1 as unknown as boolean })).toEqual(
      DEFAULT_INPUT_DISPLAY,
    );
  });

  it("keeps the width in its range and the box inside the picture", () => {
    expect(normalizeInputDisplay({ width: 5 }).width).toBe(INPUT_DISPLAY_MIN_WIDTH);
    expect(normalizeInputDisplay({ width: 5000 }).width).toBe(INPUT_DISPLAY_MAX_WIDTH);
    // 300 wide: its height is 300 * 0.5 * 16/9 = 267 per mille of the picture's height.
    expect(normalizeInputDisplay({ x: 900, y: 900, width: 300 })).toMatchObject({ x: 700, y: 733, width: 300 });
    expect(normalizeInputDisplay({ x: -40, y: -1, width: 200.4 })).toMatchObject({ x: 0, y: 0, width: 200 });
    expect(normalizeInputDisplay({ enabled: true, x: 10, y: 20, width: 250 })).toEqual({
      enabled: true,
      x: 10,
      y: 20,
      width: 250,
    });
  });
});

describe("INPUT_DISPLAY_LAYOUTS", () => {
  it("draws every controller inside its 200 x 100 canvas", () => {
    for (const elements of Object.values(INPUT_DISPLAY_LAYOUTS)) {
      for (const e of elements) {
        const [x0, y0, x1, y1] =
          "r" in e
            ? [e.cx - e.r, e.cy - e.r, e.cx + e.r, e.cy + e.r]
            : "arm" in e
            ? [e.cx - e.arm, e.cy - e.arm, e.cx + e.arm, e.cy + e.arm]
            : [e.x0, e.y0, e.x1, e.y1];
        expect(x0).toBeGreaterThanOrEqual(0);
        expect(y0).toBeGreaterThanOrEqual(0);
        expect(x1).toBeLessThanOrEqual(200);
        expect(y1).toBeLessThanOrEqual(100);
      }
    }
  });
});
