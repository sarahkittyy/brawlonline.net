import type { ThemePalette } from "./theme";
import { fallbackPalette } from "./theme";

/**
 * The player's theme colour (Settings > Appearance > Theme Color) and the look derived from it.
 *
 * Everything is worked out in OKLCH (perceived lightness, chroma, hue), so every hue comes out equally
 * readable: P+'s menu colours are the model. Its gradient's top (the frame tint and accent, #209a7a) is a
 * mid-lightness colour, its dark middle (#092f2a, the page) has the same lightness as the plain look's grey
 * page, and its bottom (#205e84, the second accent and the dialog frames) is darker and bluer. A chosen
 * colour takes the top's place and the other two follow it at P+'s lightness, so a red or a brown theme
 * has the same contrast as P+'s, and a colour from the wheel's grey centre gives a plain grey look.
 */

export type RGB = [number, number, number];
export type Oklch = { l: number; c: number; h: number };

/** The colour wheel's lightness range: darker frames hide their light labels' shadow, lighter ones the labels. */
export const THEME_LIGHTNESS = { min: 0.4, max: 0.68, initial: 0.61 } as const;

/** The plain look's accent, standing in for a colour while no theme colour is set and there is no P+ card. */
export const PLAIN_THEME_SEED = "#d0d0d0";

/** The page's lightness (P+'s gradient middle, L 0.277; the plain grey page is 0.269). */
const PAGE_LIGHTNESS = 0.277;
/** Paper (side panels) a step darker than the page, as the P+ look's 70 % luminance. */
const PAPER_LIGHTNESS = 0.245;
/** How much of the colour's chroma the dark page keeps (P+: 0.043 of 0.114), and a ceiling for loud colours. */
const PAGE_CHROMA = { ratio: 0.38, max: 0.05 } as const;
/**
 * The second colour: darker and a little less vivid, in the same hue. P+'s blue is 70 degrees from its teal,
 * but turned that far (or even 20) red goes orange and orange khaki, so a theme colour keeps one hue.
 */
const SECONDARY = { lightnessDrop: 0.15, chroma: 0.77 } as const;

/**
 * Accents drawn as text or lines on the dark page keep at least this lightness (P+'s teal is 0.614, its
 * second colour #2773a1 0.531). The second colour is also a background under light text (Settings > Help's
 * support box, a playing highlight) and stays at most 0.58.
 */
const ACCENT_LIGHTNESS = { primary: 0.6, secondary: [0.53, 0.58] } as const;

const toLinear = (v: number) => {
  const c = v / 255;
  return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
};
const fromLinear = (v: number) => 255 * (v <= 0.0031308 ? 12.92 * v : 1.055 * v ** (1 / 2.4) - 0.055);

/** "#rrggbb" (or "#rgb") to RGB, or null for anything else. */
export function parseHexColor(value: unknown): RGB | null {
  if (typeof value !== "string") {
    return null;
  }
  const m = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(value.trim());
  if (!m) {
    return null;
  }
  const digits = m[1].length === 3 ? [...m[1]].map((d) => d + d).join("") : m[1];
  const n = parseInt(digits, 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

export const rgbToHex = (rgb: RGB) =>
  "#" +
  rgb
    .map((v) =>
      Math.max(0, Math.min(255, Math.round(v)))
        .toString(16)
        .padStart(2, "0"),
    )
    .join("");

/** The setting's value as "#rrggbb", or null when it is unset or not a colour. */
export function normalizeThemeColor(value: unknown): string | null {
  const rgb = parseHexColor(value);
  return rgb ? rgbToHex(rgb) : null;
}

export function rgbToOklch([r8, g8, b8]: RGB): Oklch {
  const [r, g, b] = [toLinear(r8), toLinear(g8), toLinear(b8)];
  const l_ = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m_ = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s_ = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  const l = 0.2104542553 * l_ + 0.793617785 * m_ - 0.0040720468 * s_;
  const a = 1.9779984951 * l_ - 2.428592205 * m_ + 0.4505937099 * s_;
  const bb = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.808675766 * s_;
  const c = Math.hypot(a, bb);
  // A grey has no hue; 0 keeps it stable.
  const h = c < 1e-4 ? 0 : ((Math.atan2(bb, a) * 180) / Math.PI + 360) % 360;
  return { l, c, h };
}

/** Linear sRGB (0-1, may fall outside it) of an OKLCH colour. */
function oklchToLinear({ l, c, h }: Oklch): [number, number, number] {
  const hr = (h * Math.PI) / 180;
  const a = c * Math.cos(hr);
  const b = c * Math.sin(hr);
  const l_ = (l + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m_ = (l - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s_ = (l - 0.0894841775 * a - 1.291485548 * b) ** 3;
  return [
    4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_,
    -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_,
    -0.0041960863 * l_ - 0.7034186147 * m_ + 1.707614701 * s_,
  ];
}

const EPSILON = 1e-5;
const inGamut = (lin: [number, number, number]) => lin.every((v) => v >= -EPSILON && v <= 1 + EPSILON);

/** The most chroma a hue has at a lightness and still is an sRGB colour. */
export function maxChroma(l: number, h: number): number {
  let lo = 0;
  let hi = 0.4;
  for (let i = 0; i < 18; i++) {
    const mid = (lo + hi) / 2;
    if (inGamut(oklchToLinear({ l, c: mid, h }))) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/** An OKLCH colour as sRGB, its chroma lowered into the sRGB gamut (lightness and hue are kept). */
export function oklchToRgb(color: Oklch): RGB {
  const l = Math.max(0, Math.min(1, color.l));
  let lin = oklchToLinear({ ...color, l });
  if (!inGamut(lin)) {
    lin = oklchToLinear({ ...color, l, c: Math.min(color.c, maxChroma(l, color.h)) });
  }
  return lin.map((v) => fromLinear(Math.max(0, Math.min(1, v)))) as RGB;
}

export const oklchToHex = (color: Oklch) => rgbToHex(oklchToRgb(color));

/**
 * P+'s three gradient colours (top, middle, bottom) made from a theme colour: game_theme.tsx tints the menu
 * frames with them exactly as it does with the card's own gradient.
 */
export function gradientFromThemeColor(color: string): { top: RGB; middle: RGB; bottom: RGB } {
  const top = parseHexColor(color) ?? parseHexColor(PLAIN_THEME_SEED)!;
  const { l, c, h } = rgbToOklch(top);
  return {
    top,
    middle: oklchToRgb({ l: PAGE_LIGHTNESS, c: Math.min(c * PAGE_CHROMA.ratio, PAGE_CHROMA.max), h }),
    bottom: oklchToRgb(secondaryOf({ l, c, h })),
  };
}

const secondaryOf = ({ l, c, h }: Oklch): Oklch => ({
  l: l - SECONDARY.lightnessDrop,
  c: c * SECONDARY.chroma,
  h,
});

/** The launcher's colours (MUI palette and the CSS variables, see game_theme.tsx paletteVars) from a theme colour. */
export function paletteFromThemeColor(color: string): ThemePalette {
  const seed = rgbToOklch(parseHexColor(color) ?? parseHexColor(PLAIN_THEME_SEED)!);
  const second = secondaryOf(seed);
  return {
    ...fallbackPalette,
    primary: oklchToHex({ ...seed, l: Math.max(seed.l, ACCENT_LIGHTNESS.primary) }),
    secondary: oklchToHex({
      ...second,
      l: Math.min(Math.max(second.l, ACCENT_LIGHTNESS.secondary[0]), ACCENT_LIGHTNESS.secondary[1]),
    }),
    backgroundDefault: oklchToHex({
      l: PAGE_LIGHTNESS,
      c: Math.min(seed.c * PAGE_CHROMA.ratio, PAGE_CHROMA.max),
      h: seed.h,
    }),
    backgroundPaper: oklchToHex({
      l: PAPER_LIGHTNESS,
      c: Math.min(seed.c * PAGE_CHROMA.ratio * 0.8, PAGE_CHROMA.max * 0.8),
      h: seed.h,
    }),
  };
}

/** A position on the colour wheel: hue, distance from the grey centre (share of the hue's most chroma) and lightness. */
export type WheelPosition = { h: number; r: number; l: number };

export function wheelPositionOf(color: string): WheelPosition {
  const { l: rawL, c, h } = rgbToOklch(parseHexColor(color) ?? parseHexColor(PLAIN_THEME_SEED)!);
  const l = Math.max(THEME_LIGHTNESS.min, Math.min(THEME_LIGHTNESS.max, rawL));
  const max = maxChroma(l, h);
  return { h, r: max > 0 ? Math.min(1, c / max) : 0, l };
}

export const colorAtWheelPosition = ({ h, r, l }: WheelPosition) => oklchToHex({ l, c: r * maxChroma(l, h), h });

/** Relative luminance (WCAG) of a colour, 0-1: picks a light or dark pencil over the theme swatch. */
export function relativeLuminance(color: string): number {
  const [r, g, b] = (parseHexColor(color) ?? [0, 0, 0]).map(toLinear);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
