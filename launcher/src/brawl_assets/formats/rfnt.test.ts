import { describe, expect, it } from "vitest";

import { convertRfnt, UNITS_PER_PIXEL } from "../font/convert";
import { signedArea } from "../font/trace";
import { readTtf, ttfChecksum } from "../font/ttf";
import { decodePng } from "../png";
import { GxFormat } from "./gx";
import { buildRfnt, codeToUnicode, decodeSheet, FontEncoding, glyphCell, parseRfnt } from "./rfnt";

/** Packs 4-bit intensities (row-major, w x h) into GX I4 8x8 blocks. */
function encodeI4(pix: number[], w: number, h: number): Uint8Array {
  const out = new Uint8Array((Math.ceil(w / 8) * Math.ceil(h / 8) * 32) | 0);
  let o = 0;
  for (let by = 0; by < h; by += 8) {
    for (let bx = 0; bx < w; bx += 8) {
      for (let y = 0; y < 8; y++) {
        for (let x = 0; x < 8; x += 2) {
          const get = (xx: number, yy: number) => (xx < w && yy < h ? pix[yy * w + xx] : 0);
          out[o++] = (get(bx + x, by + y) << 4) | get(bx + x + 1, by + y);
        }
      }
    }
  }
  return out;
}

const CELL_W = 6;
const CELL_H = 8;
const SHEET_W = 16;
const SHEET_H = 24;

function makeSheets() {
  const s0 = new Array(SHEET_W * SHEET_H).fill(0);
  const s1 = new Array(SHEET_W * SHEET_H).fill(0);
  const fill = (s: number[], x0: number, y0: number, w: number, h: number, v = 15) => {
    for (let y = y0; y < y0 + h; y++) {
      for (let x = x0; x < x0 + w; x++) {
        s[y * SHEET_W + x] = v;
      }
    }
  };
  // Glyph 0 (cell 0,0): solid 4x6 block at (1,1).
  fill(s0, 1, 1, 4, 6);
  // Glyph 1 (cell 1,0 => x 7): 5x6 ring with a 1x2 hole... use a 3x2 hole for a clear contour.
  fill(s0, 7, 1, 5, 6);
  fill(s0, 8, 3, 3, 2, 0);
  // Glyph 2 (cell 0,1): empty (space).
  // Glyph 3 (cell 1,1 => x 7, y 9): single row bar.
  fill(s0, 7, 12, 6, 2);
  // Glyph 4 (sheet 1, cell 0): vertical bar.
  fill(s1, 2, 0, 2, 8);
  return [encodeI4(s0, SHEET_W, SHEET_H), encodeI4(s1, SHEET_W, SHEET_H)];
}

function makeFont() {
  return buildRfnt({
    cellWidth: CELL_W,
    cellHeight: CELL_H,
    baseline: 7,
    ascent: 7,
    lineFeed: 10,
    sheetFormat: GxFormat.I4,
    sheetRow: 2,
    sheetLine: 2,
    sheetWidth: SHEET_W,
    sheetHeight: SHEET_H,
    sheets: makeSheets(),
    widths: [
      { left: 0, glyphWidth: 6, charWidth: 6 },
      { left: 0, glyphWidth: 6, charWidth: 7 },
      { left: 0, glyphWidth: 0, charWidth: 3 },
      { left: -1, glyphWidth: 6, charWidth: 5 },
      { left: 1, glyphWidth: 6, charWidth: 4 },
    ],
    maps: [
      { method: 0, begin: 0x41, end: 0x42, base: 0 },
      { method: 1, begin: 0x20, end: 0x21, table: [2, 0xffff] },
      {
        method: 2,
        pairs: [
          [0x43, 4],
          [0x5f, 3],
        ],
      },
    ],
  });
}

describe("RFNT", () => {
  it("parses FINF/TGLP/CWDH and all three CMAP methods", () => {
    const f = parseRfnt(makeFont());
    expect(f.magic).toBe("RFNT");
    expect(f.version).toBe(0x0104);
    expect([f.cellWidth, f.cellHeight, f.baseline, f.lineFeed, f.ascent]).toEqual([6, 8, 7, 10, 7]);
    expect(f.sheetFormatName).toBe("I4");
    expect(f.sheets.length).toBe(2);
    expect(Object.fromEntries(f.codeToGlyph)).toEqual({ 0x41: 0, 0x42: 1, 0x20: 2, 0x43: 4, 0x5f: 3 });
    expect(f.widths.get(3)).toEqual({ left: -1, glyphWidth: 6, charWidth: 5 });
    expect(codeToUnicode(f, 0x41)).toBe(0x41);
  });

  it("locates glyph cells with the 1-pixel gutter and across sheets", () => {
    const f = parseRfnt(makeFont());
    expect(glyphCell(f, 0)).toEqual({ sheet: 0, x: 0, y: 0 });
    expect(glyphCell(f, 1)).toEqual({ sheet: 0, x: 7, y: 0 });
    expect(glyphCell(f, 3)).toEqual({ sheet: 0, x: 7, y: 9 });
    expect(glyphCell(f, 4)).toEqual({ sheet: 1, x: 0, y: 0 });
    const sheet = decodeSheet(f, 0);
    expect(sheet[(1 * SHEET_W + 1) * 4 + 3]).toBe(255);
    expect(sheet[(0 * SHEET_W + 0) * 4 + 3]).toBe(0);
  });

  it("maps CP1252 codes to Unicode", () => {
    const f = parseRfnt(makeFont());
    f.encoding = FontEncoding.CP1252;
    expect(codeToUnicode(f, 0x80)).toBe(0x20ac);
    expect(codeToUnicode(f, 0xe9)).toBe(0xe9);
  });
});

describe("RFNT -> TrueType", () => {
  it("writes a TTF with correct cmap, advances and outline winding", () => {
    const f = parseRfnt(makeFont());
    const conv = convertRfnt(f, { family: "Test Font" });
    const ttf = readTtf(conv.ttf);
    expect(ttf.familyName).toBe("Test Font");
    expect(ttf.unitsPerEm).toBe(CELL_H * UNITS_PER_PIXEL);
    // Whole-font checksum must be 0xB1B0AFBA once checkSumAdjustment is applied.
    expect(ttfChecksum(conv.ttf)).toBe(0xb1b0afba);
    for (const [tag, t] of ttf.tables) {
      if (tag !== "head") {
        expect(ttfChecksum(conv.ttf.subarray(t.offset, t.offset + t.length))).toBe(t.checksum);
      }
    }
    const gA = ttf.glyphFor(0x41);
    const gB = ttf.glyphFor(0x42);
    const gSpace = ttf.glyphFor(0x20);
    const gC = ttf.glyphFor(0x43);
    expect([gA, gB, gSpace, gC].every((g) => g > 0)).toBe(true);
    expect(ttf.glyphFor(0x21)).toBe(0);
    expect(ttf.advance(gA)).toBe(6 * UNITS_PER_PIXEL);
    expect(ttf.advance(gB)).toBe(7 * UNITS_PER_PIXEL);
    expect(ttf.advance(gSpace)).toBe(3 * UNITS_PER_PIXEL);
    expect(ttf.contours(gSpace)).toEqual([]);

    const a = ttf.contours(gA);
    expect(a.length).toBe(1);
    expect(signedArea(a[0])).toBeLessThan(0); // clockwise outer contour (y up)
    const xs = a[0].map((p) => p.x);
    const ys = a[0].map((p) => p.y);
    // 4x6 block at x 1..5, rows 1..7 of the cell, baseline 7 => y from 0 to 6 pixels; edges at the 50% iso-line.
    expect(Math.min(...xs)).toBe(1 * UNITS_PER_PIXEL);
    expect(Math.max(...xs)).toBe(5 * UNITS_PER_PIXEL);
    expect(Math.min(...ys)).toBe(0);
    expect(Math.max(...ys)).toBe(6 * UNITS_PER_PIXEL);

    const b = ttf.contours(gB);
    expect(b.length).toBe(2);
    const areas = b.map(signedArea).sort((p, q) => Math.abs(q) - Math.abs(p));
    expect(areas[0]).toBeLessThan(0);
    expect(areas[1]).toBeGreaterThan(0); // hole runs the other way

    // Glyph from the second sheet, with a left bearing of +1 pixel: bar at x 2..4 => 3..5.
    const c = ttf.contours(gC);
    expect(Math.min(...c[0].map((p) => p.x))).toBe(3 * UNITS_PER_PIXEL);
  });

  it("exports glyph sheets and metrics", () => {
    const f = parseRfnt(makeFont());
    const conv = convertRfnt(f, { family: "T" });
    expect(conv.sheets.length).toBe(2);
    const png = decodePng(conv.sheets[1]);
    expect([png.width, png.height]).toEqual([SHEET_W, SHEET_H]);
    expect(conv.metrics.sheetFiles).toEqual(["sheet_0.png", "sheet_1.png"]);
    const c = conv.metrics.glyphs.find((g) => g.char === "C")!;
    expect(c).toMatchObject({ index: 4, sheet: 1, x: 0, y: 0, left: 1, charWidth: 4 });
    expect(conv.glyphCount).toBe(1 + 5);
  });
});
