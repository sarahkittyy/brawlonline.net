import { describe, expect, it } from "vitest";

import { normalizeSmoothMotion, SMOOTH_MOTION_RATES } from "./smooth_motion";

describe("normalizeSmoothMotion", () => {
  it("keeps the rates Dolphin takes and turns anything else off", () => {
    expect(SMOOTH_MOTION_RATES.map(normalizeSmoothMotion)).toEqual([120, 180, 240, 300, 360]);
    expect([0, 60, 144, 420, -120, NaN, undefined, null, "fast"].map(normalizeSmoothMotion)).toEqual(Array(9).fill(0));
    // A rate as text (Dolphin.ini's value, a hand-edited settings file) is that rate.
    expect(normalizeSmoothMotion("180")).toBe(180);
  });
});
