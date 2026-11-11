import { describe, expect, it } from "vitest";

import {
  colorAtWheelPosition,
  gradientFromThemeColor,
  normalizeThemeColor,
  oklchToRgb,
  paletteFromThemeColor,
  parseHexColor,
  rgbToHex,
  rgbToOklch,
  THEME_LIGHTNESS,
  wheelPositionOf,
} from "./theme_color";

const oklchOf = (hex: string) => rgbToOklch(parseHexColor(hex)!);

/** WCAG contrast ratio of two colours. */
function contrast(a: string, b: string) {
  const lum = (hex: string) => {
    const [r, g, b] = parseHexColor(hex)!.map((v) => {
      const c = v / 255;
      return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
    });
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  };
  const [hi, lo] = [lum(a), lum(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

// Every colour the wheel offers: 24 hues, centre to rim, its whole lightness range.
const WHEEL_COLORS = Array.from({ length: 24 }, (_, i) => i * 15).flatMap((h) =>
  [0, 0.5, 1].flatMap((r) =>
    [THEME_LIGHTNESS.min, THEME_LIGHTNESS.initial, THEME_LIGHTNESS.max].map((l) => colorAtWheelPosition({ h, r, l })),
  ),
);

describe("theme colour", () => {
  it("reads only #rgb/#rrggbb settings", () => {
    expect(normalizeThemeColor("#209A7A")).toBe("#209a7a");
    expect(normalizeThemeColor("#abc")).toBe("#aabbcc");
    for (const bad of [null, undefined, "", "teal", "#12345", "209a7a", 42, "#gggggg"]) {
      expect(normalizeThemeColor(bad)).toBeNull();
    }
  });

  it("converts to OKLCH and back", () => {
    for (const hex of ["#209a7a", "#092f2a", "#205e84", "#d33c3c", "#000000", "#ffffff", "#808080"]) {
      expect(rgbToHex(oklchToRgb(oklchOf(hex)))).toBe(hex);
    }
    expect(oklchOf("#209a7a").l).toBeCloseTo(0.614, 3);
    expect(oklchOf("#808080").c).toBeLessThan(1e-4);
  });

  it("keeps an out-of-gamut colour's lightness and hue, lowering its chroma", () => {
    const vivid = oklchOf(rgbToHex(oklchToRgb({ l: 0.6, c: 0.4, h: 200 })));
    expect(vivid.l).toBeCloseTo(0.6, 2);
    expect(vivid.h).toBeCloseTo(200, 0);
    expect(vivid.c).toBeLessThan(0.4);
  });

  it("gives P+'s own teal P+'s accent and a page as dark as P+'s", () => {
    const palette = paletteFromThemeColor("#209a7a");
    expect(palette.primary).toBe("#209a7a");
    expect(oklchOf(palette.backgroundDefault).l).toBeCloseTo(oklchOf("#092f2a").l, 2);
    expect(gradientFromThemeColor("#209a7a").top).toEqual([0x20, 0x9a, 0x7a]);
  });

  it("gives a grey a plain grey look", () => {
    const palette = paletteFromThemeColor("#838383");
    for (const c of [palette.primary, palette.secondary, palette.backgroundDefault, palette.backgroundPaper]) {
      expect(oklchOf(c).c).toBeLessThan(0.002);
    }
  });

  it("keeps every wheel colour's accents readable on its own page", () => {
    for (const color of WHEEL_COLORS) {
      const p = paletteFromThemeColor(color);
      // Accents as text, lines and icons on the page and the side panels (WCAG's 3:1 for UI parts).
      expect(contrast(p.primary, p.backgroundPaper), `${color} primary`).toBeGreaterThanOrEqual(3);
      expect(contrast(p.primary, p.backgroundDefault), `${color} primary`).toBeGreaterThanOrEqual(3);
      expect(contrast(p.secondary, p.backgroundDefault), `${color} secondary`).toBeGreaterThanOrEqual(2);
      // The second colour is also a background under the light text (#e0e0e0).
      expect(contrast("#e0e0e0", p.secondary), `${color} secondary`).toBeGreaterThanOrEqual(2.5);
      // Body text on the page.
      expect(contrast(p.textPrimary, p.backgroundDefault), `${color} text`).toBeGreaterThanOrEqual(10);
    }
  });

  it("finds a colour's place on the wheel again", () => {
    for (const color of ["#209a7a", "#d33c3c", "#6d4e33", "#838383"]) {
      const back = colorAtWheelPosition(wheelPositionOf(color));
      const [a, b] = [parseHexColor(color)!, parseHexColor(back)!];
      expect(Math.max(...a.map((v, i) => Math.abs(v - b[i]))), `${color} -> ${back}`).toBeLessThanOrEqual(2);
    }
  });

  it("puts colours outside the wheel's lightness on its nearest ring", () => {
    const plain = wheelPositionOf("#d0d0d0");
    expect(plain.r).toBeCloseTo(0, 4);
    expect(plain.l).toBe(THEME_LIGHTNESS.max);
    expect(wheelPositionOf("#101010").l).toBe(THEME_LIGHTNESS.min);
  });
});
