// Converts an NW4R bitmap font (RFNT) into a TrueType font plus glyph-sheet PNGs and a metrics JSON.
//
// Glyph outlines are the 50% coverage iso-contours of the game's own glyph bitmaps (no drawing involved); advance
// widths, left bearings and the baseline come from the font's CWDH / TGLP / FINF blocks.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import type { RfntFont } from "../formats/rfnt";
import { codeToUnicode, decodeSheet, glyphCell, glyphWidth } from "../formats/rfnt";
import { encodePng } from "../png";
import { signedArea, simplify, traceContours } from "./trace";
import type { TtfGlyph, TtfPoint } from "./ttf";
import { writeTtf } from "./ttf";

/** Font units per source pixel. */
export const UNITS_PER_PIXEL = 64;

export type GlyphMetrics = {
  /** Unicode code point. */
  code: number;
  char: string;
  /** Glyph index in the RFNT. */
  index: number;
  sheet: number;
  /** Cell origin (top-left) within the sheet, in pixels. */
  x: number;
  y: number;
  left: number;
  glyphWidth: number;
  charWidth: number;
};

export type FontMetrics = {
  family: string;
  encoding: number;
  cellWidth: number;
  cellHeight: number;
  /** Baseline distance from the top of a cell. */
  baseline: number;
  ascent: number;
  height: number;
  lineFeed: number;
  sheetFormat: string;
  sheetRow: number;
  sheetLine: number;
  sheetWidth: number;
  sheetHeight: number;
  sheetFiles: string[];
  defaultWidth: { left: number; glyphWidth: number; charWidth: number };
  /** TTF metrics: units per em and units per source pixel (render at `height * k` CSS px for pixel-exact size). */
  unitsPerEm: number;
  unitsPerPixel: number;
  glyphs: GlyphMetrics[];
};

export type ConvertedFont = {
  ttf: Buffer;
  sheets: Buffer[];
  metrics: FontMetrics;
  glyphCount: number;
};

export type ConvertOptions = {
  family: string;
  /** Coverage threshold for the outline (0..1). Default 0.5. */
  threshold?: number;
  weightClass?: number;
  /** File names to record in metrics.sheetFiles (defaults to sheet_<n>.png). */
  sheetFileName?: (index: number) => string;
};

export function convertRfnt(font: RfntFont, opts: ConvertOptions): ConvertedFont {
  const threshold = opts.threshold ?? 0.5;
  const U = UNITS_PER_PIXEL;
  const emPx = font.height || font.cellHeight;
  const ascentPx = font.ascent || font.baseline;
  const sheetsRgba = font.sheets.map((_, i) => decodeSheet(font, i));
  const sheetName = opts.sheetFileName ?? ((i: number) => `sheet_${i}.png`);

  const outline = (glyph: number): TtfPoint[][] => {
    const w = glyphWidth(font, glyph);
    const cell = glyphCell(font, glyph);
    const rgba = sheetsRgba[cell.sheet];
    if (!rgba || w.glyphWidth === 0) {
      return [];
    }
    const gw = Math.min(w.glyphWidth, font.cellWidth);
    const gh = font.cellHeight;
    const cov = new Float32Array(gw * gh);
    for (let y = 0; y < gh; y++) {
      for (let x = 0; x < gw; x++) {
        const sx = cell.x + x;
        const sy = cell.y + y;
        if (sx < font.sheetWidth && sy < font.sheetHeight) {
          cov[y * gw + x] = rgba[(sy * font.sheetWidth + sx) * 4 + 3] / 255;
        }
      }
    }
    const contours = traceContours(cov, gw, gh, threshold)
      .map((c) => simplify(c, 0.06))
      .filter((c) => c.length >= 3)
      .map((c) => c.map((p) => ({ x: Math.round((w.left + p.x) * U), y: Math.round((font.baseline - p.y) * U) })));
    if (!contours.length) {
      return [];
    }
    // TrueType wants outer contours clockwise (negative area, y up). The tracer is consistent, so check the
    // largest contour (always an outer one) and flip everything if needed.
    const largest = contours.reduce((a, c) => (Math.abs(signedArea(c)) > Math.abs(signedArea(a)) ? c : a));
    if (signedArea(largest) > 0) {
      contours.forEach((c) => c.reverse());
    }
    return contours;
  };

  const entries: { code: number; index: number }[] = [];
  const seen = new Set<number>();
  for (const [code, index] of font.codeToGlyph) {
    const uni = codeToUnicode(font, code);
    if (uni === null || uni < 0x20 || uni >= 0xffff || seen.has(uni)) {
      continue;
    }
    seen.add(uni);
    entries.push({ code: uni, index });
  }
  entries.sort((a, b) => a.code - b.code);

  const outlineCache = new Map<number, TtfPoint[][]>();
  const outlineOf = (idx: number) => {
    let o = outlineCache.get(idx);
    if (!o) {
      o = outline(idx);
      outlineCache.set(idx, o);
    }
    return o;
  };
  const glyphs: TtfGlyph[] = [
    { contours: outlineOf(font.alterCharIndex), advanceWidth: glyphWidth(font, font.alterCharIndex).charWidth * U },
  ];
  const cmap = new Map<number, number>();
  const metricsGlyphs: GlyphMetrics[] = [];
  for (const e of entries) {
    const w = glyphWidth(font, e.index);
    const cell = glyphCell(font, e.index);
    cmap.set(e.code, glyphs.length);
    glyphs.push({ contours: outlineOf(e.index), advanceWidth: Math.max(0, w.charWidth) * U });
    metricsGlyphs.push({
      code: e.code,
      char: String.fromCodePoint(e.code),
      index: e.index,
      sheet: cell.sheet,
      x: cell.x,
      y: cell.y,
      left: w.left,
      glyphWidth: w.glyphWidth,
      charWidth: w.charWidth,
    });
  }
  const ttf = writeTtf({
    familyName: opts.family,
    unitsPerEm: emPx * U,
    ascender: ascentPx * U,
    descender: -(emPx - ascentPx) * U,
    lineGap: Math.max(0, font.lineFeed - emPx) * U,
    weightClass: opts.weightClass,
    glyphs,
    cmap,
  });
  const sheets = sheetsRgba.map((rgba) => encodePng(font.sheetWidth, font.sheetHeight, rgba));
  const metrics: FontMetrics = {
    family: opts.family,
    encoding: font.encoding,
    cellWidth: font.cellWidth,
    cellHeight: font.cellHeight,
    baseline: font.baseline,
    ascent: ascentPx,
    height: emPx,
    lineFeed: font.lineFeed,
    sheetFormat: font.sheetFormatName,
    sheetRow: font.sheetRow,
    sheetLine: font.sheetLine,
    sheetWidth: font.sheetWidth,
    sheetHeight: font.sheetHeight,
    sheetFiles: sheets.map((_, i) => sheetName(i)),
    defaultWidth: font.defaultWidth,
    unitsPerEm: emPx * U,
    unitsPerPixel: U,
    glyphs: metricsGlyphs,
  };
  return { ttf, sheets, metrics, glyphCount: glyphs.length };
}
