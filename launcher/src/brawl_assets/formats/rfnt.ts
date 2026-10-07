// NW4R bitmap fonts (RFNT / .brfnt): FINF, TGLP (glyph sheets), CWDH (widths), CMAP (character map).
//
// BrawlLib only stubs this format, so the layout follows the NintendoWare for Revolution font format as documented
// on https://wiki.tockdom.com/wiki/BRFNT_(File_Format) and implemented in BrawlLib's sibling projects; the block
// walk below does not depend on the FINF pointers, which point 8 bytes into each block.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { decodeGx, GX_FORMAT_NAMES } from "./gx";

export enum FontEncoding {
  UTF8 = 0,
  UTF16 = 1,
  SJIS = 2,
  CP1252 = 3,
}

export type CharWidth = { left: number; glyphWidth: number; charWidth: number };

export type RfntFont = {
  magic: string;
  version: number;
  fontType: number;
  lineFeed: number;
  alterCharIndex: number;
  defaultWidth: CharWidth;
  encoding: number;
  /** FINF height/width/ascent (0 when the file predates version 1.04). */
  height: number;
  width: number;
  ascent: number;
  cellWidth: number;
  cellHeight: number;
  baseline: number;
  maxCharWidth: number;
  sheetFormat: number;
  sheetFormatName: string;
  sheetCount: number;
  /** Cells per row / rows per sheet. */
  sheetRow: number;
  sheetLine: number;
  sheetWidth: number;
  sheetHeight: number;
  /** Raw GX data of each sheet. */
  sheets: Buffer[];
  /** Glyph index -> widths. */
  widths: Map<number, CharWidth>;
  /** Character code (in the font's encoding) -> glyph index. */
  codeToGlyph: Map<number, number>;
};

export function isRfnt(buf: Uint8Array): boolean {
  if (buf.length < 0x10) {
    return false;
  }
  const m = String.fromCharCode(buf[0], buf[1], buf[2], buf[3]);
  return (m === "RFNT" || m === "RFNA" || m === "RFNU") && buf[4] === 0xfe && buf[5] === 0xff;
}

export function parseRfnt(buf: Buffer): RfntFont {
  if (!isRfnt(buf)) {
    throw new Error("Not an RFNT font");
  }
  const magic = buf.toString("latin1", 0, 4);
  const version = buf.readUInt16BE(6);
  const headerSize = buf.readUInt16BE(0x0c);
  const numBlocks = buf.readUInt16BE(0x0e);
  let off = headerSize;
  let finf = -1;
  let tglp = -1;
  const cwdhs: number[] = [];
  const cmaps: number[] = [];
  for (let i = 0; i < numBlocks && off + 8 <= buf.length; i++) {
    const tag = buf.toString("latin1", off, off + 4);
    const size = buf.readUInt32BE(off + 4);
    if (tag === "FINF") {
      finf = off;
    } else if (tag === "TGLP") {
      tglp = off;
    } else if (tag === "CWDH") {
      cwdhs.push(off);
    } else if (tag === "CMAP") {
      cmaps.push(off);
    }
    if (size <= 0) {
      break;
    }
    off += size;
  }
  if (finf < 0 || tglp < 0) {
    throw new Error("RFNT: missing FINF or TGLP block");
  }
  const finfSize = buf.readUInt32BE(finf + 4);
  const font: RfntFont = {
    magic,
    version,
    fontType: buf[finf + 8],
    lineFeed: buf.readInt8(finf + 9),
    alterCharIndex: buf.readUInt16BE(finf + 0x0a),
    defaultWidth: {
      left: buf.readInt8(finf + 0x0c),
      glyphWidth: buf[finf + 0x0d],
      charWidth: buf.readInt8(finf + 0x0e),
    },
    encoding: buf[finf + 0x0f],
    height: finfSize >= 0x20 ? buf[finf + 0x1c] : 0,
    width: finfSize >= 0x20 ? buf[finf + 0x1d] : 0,
    ascent: finfSize >= 0x20 ? buf[finf + 0x1e] : 0,
    cellWidth: buf[tglp + 8],
    cellHeight: buf[tglp + 9],
    baseline: buf.readInt8(tglp + 0x0a),
    maxCharWidth: buf[tglp + 0x0b],
    sheetFormat: buf.readUInt16BE(tglp + 0x12) & 0x7fff,
    sheetFormatName: "",
    sheetCount: buf.readUInt16BE(tglp + 0x10),
    sheetRow: buf.readUInt16BE(tglp + 0x14),
    sheetLine: buf.readUInt16BE(tglp + 0x16),
    sheetWidth: buf.readUInt16BE(tglp + 0x18),
    sheetHeight: buf.readUInt16BE(tglp + 0x1a),
    sheets: [],
    widths: new Map(),
    codeToGlyph: new Map(),
  };
  font.sheetFormatName = GX_FORMAT_NAMES[font.sheetFormat] ?? `fmt${font.sheetFormat}`;
  const sheetSize = buf.readUInt32BE(tglp + 0x0c);
  const sheetOff = buf.readUInt32BE(tglp + 0x1c);
  for (let i = 0; i < font.sheetCount; i++) {
    const s = sheetOff + i * sheetSize;
    if (s + sheetSize > buf.length) {
      throw new Error(`RFNT: glyph sheet ${i} out of range`);
    }
    font.sheets.push(buf.subarray(s, s + sheetSize));
  }
  for (const c of cwdhs) {
    const begin = buf.readUInt16BE(c + 8);
    const end = buf.readUInt16BE(c + 0x0a);
    for (let idx = begin, p = c + 0x10; idx <= end; idx++, p += 3) {
      font.widths.set(idx, { left: buf.readInt8(p), glyphWidth: buf[p + 1], charWidth: buf.readInt8(p + 2) });
    }
  }
  for (const c of cmaps) {
    const begin = buf.readUInt16BE(c + 8);
    const end = buf.readUInt16BE(c + 0x0a);
    const method = buf.readUInt16BE(c + 0x0c);
    const info = c + 0x14;
    if (method === 0) {
      const base = buf.readUInt16BE(info);
      for (let code = begin; code <= end; code++) {
        font.codeToGlyph.set(code, base + (code - begin));
      }
    } else if (method === 1) {
      for (let code = begin, p = info; code <= end; code++, p += 2) {
        const idx = buf.readUInt16BE(p);
        if (idx !== 0xffff) {
          font.codeToGlyph.set(code, idx);
        }
      }
    } else if (method === 2) {
      const n = buf.readUInt16BE(info);
      for (let i = 0, p = info + 2; i < n; i++, p += 4) {
        font.codeToGlyph.set(buf.readUInt16BE(p), buf.readUInt16BE(p + 2));
      }
    } else {
      throw new Error(`RFNT: unknown CMAP mapping method ${method}`);
    }
  }
  return font;
}

export function glyphWidth(font: RfntFont, glyph: number): CharWidth {
  return font.widths.get(glyph) ?? font.defaultWidth;
}

/**
 * Cell origin of a glyph within its sheet. NW4R sheets pack cells with a 1-pixel gutter
 * (`sheetRow * (cellWidth + 1) <= sheetWidth`); fall back to no gutter when that does not fit.
 */
export function glyphCell(font: RfntFont, glyph: number): { sheet: number; x: number; y: number } {
  const perSheet = font.sheetRow * font.sheetLine;
  const sheet = Math.floor(glyph / perSheet);
  const cell = glyph % perSheet;
  const col = cell % font.sheetRow;
  const row = Math.floor(cell / font.sheetRow);
  const gx = font.sheetRow * (font.cellWidth + 1) <= font.sheetWidth ? 1 : 0;
  const gy = font.sheetLine * (font.cellHeight + 1) <= font.sheetHeight ? 1 : 0;
  return { sheet, x: col * (font.cellWidth + gx), y: row * (font.cellHeight + gy) };
}

export function decodeSheet(font: RfntFont, index: number): Uint8Array {
  return decodeGx(font.sheetFormat, font.sheets[index], font.sheetWidth, font.sheetHeight);
}

let sjisDecoder: TextDecoder | null | undefined;

/** Converts a character code in the font's encoding to a Unicode code point (or null when unmappable). */
export function codeToUnicode(font: RfntFont, code: number): number | null {
  switch (font.encoding) {
    case FontEncoding.UTF16:
    case FontEncoding.UTF8:
      // UTF-8 fonts store code points in CMAP as well.
      return code;
    case FontEncoding.CP1252:
      if (code >= 0x80 && code < 0xa0) {
        return CP1252_HIGH[code - 0x80] || null;
      }
      return code;
    case FontEncoding.SJIS: {
      if (sjisDecoder === undefined) {
        try {
          sjisDecoder = new TextDecoder("shift_jis", { fatal: true });
        } catch {
          sjisDecoder = null;
        }
      }
      if (code < 0x80) {
        return code;
      }
      if (!sjisDecoder) {
        return null;
      }
      try {
        const bytes = code > 0xff ? new Uint8Array([code >> 8, code & 0xff]) : new Uint8Array([code]);
        const s = sjisDecoder.decode(bytes);
        return s.length ? s.codePointAt(0)! : null;
      } catch {
        return null;
      }
    }
    default:
      return code;
  }
}

// Windows-1252 0x80-0x9F.
const CP1252_HIGH = [
  0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0, 0,
  0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178,
];

// ---------------------------------------------------------------------------------------------------------------
// Builder used by tests to create synthetic fonts.

export type RfntBuildInput = {
  cellWidth: number;
  cellHeight: number;
  baseline: number;
  ascent: number;
  lineFeed: number;
  sheetFormat: number;
  sheetRow: number;
  sheetLine: number;
  sheetWidth: number;
  sheetHeight: number;
  sheets: Uint8Array[];
  widths: CharWidth[];
  /** CMAP blocks. */
  maps: (
    | { method: 0; begin: number; end: number; base: number }
    | { method: 1; begin: number; end: number; table: number[] }
    | { method: 2; pairs: [number, number][] }
  )[];
  encoding?: number;
};

export function buildRfnt(input: RfntBuildInput): Buffer {
  const pad4 = (b: Buffer) => (b.length % 4 ? Buffer.concat([b, Buffer.alloc(4 - (b.length % 4))]) : b);
  const block = (tag: string, body: Buffer) => {
    const b = pad4(Buffer.concat([Buffer.alloc(8), body]));
    b.write(tag, 0, "latin1");
    b.writeUInt32BE(b.length, 4);
    return b;
  };
  const finfBody = Buffer.alloc(0x18);
  finfBody[0] = 1;
  finfBody.writeInt8(input.lineFeed, 1);
  finfBody.writeUInt16BE(0, 2);
  finfBody.writeInt8(0, 4);
  finfBody[5] = input.cellWidth;
  finfBody.writeInt8(input.cellWidth, 6);
  finfBody[7] = input.encoding ?? FontEncoding.UTF16;
  finfBody[0x14] = input.cellHeight;
  finfBody[0x15] = input.cellWidth;
  finfBody[0x16] = input.ascent;
  const finf = block("FINF", finfBody);
  const sheetSize = input.sheets[0].length;
  const tglpBody = Buffer.alloc(0x18);
  tglpBody[0] = input.cellWidth;
  tglpBody[1] = input.cellHeight;
  tglpBody.writeInt8(input.baseline, 2);
  tglpBody[3] = input.cellWidth;
  tglpBody.writeUInt32BE(sheetSize, 4);
  tglpBody.writeUInt16BE(input.sheets.length, 8);
  tglpBody.writeUInt16BE(input.sheetFormat, 0x0a);
  tglpBody.writeUInt16BE(input.sheetRow, 0x0c);
  tglpBody.writeUInt16BE(input.sheetLine, 0x0e);
  tglpBody.writeUInt16BE(input.sheetWidth, 0x10);
  tglpBody.writeUInt16BE(input.sheetHeight, 0x12);
  const headerSize = 0x10;
  const tglpStart = headerSize + finf.length;
  const tglpHeaderLen = 8 + tglpBody.length;
  const sheetStart = Math.ceil((tglpStart + tglpHeaderLen) / 0x20) * 0x20;
  tglpBody.writeUInt32BE(sheetStart, 0x14);
  const tglpRaw = Buffer.alloc(sheetStart - tglpStart + sheetSize * input.sheets.length);
  tglpRaw.write("TGLP", 0, "latin1");
  tglpRaw.writeUInt32BE(tglpRaw.length, 4);
  tglpBody.copy(tglpRaw, 8);
  input.sheets.forEach((s, i) => Buffer.from(s).copy(tglpRaw, sheetStart - tglpStart + i * sheetSize));
  const cwdhBody = Buffer.alloc(8 + input.widths.length * 3);
  cwdhBody.writeUInt16BE(0, 0);
  cwdhBody.writeUInt16BE(input.widths.length - 1, 2);
  input.widths.forEach((w, i) => {
    cwdhBody.writeInt8(w.left, 8 + i * 3);
    cwdhBody[9 + i * 3] = w.glyphWidth;
    cwdhBody.writeInt8(w.charWidth, 10 + i * 3);
  });
  const cwdh = block("CWDH", cwdhBody);
  const cmaps = input.maps.map((m) => {
    let info: Buffer;
    let begin: number;
    let end: number;
    if (m.method === 0) {
      info = Buffer.alloc(2);
      info.writeUInt16BE(m.base, 0);
      begin = m.begin;
      end = m.end;
    } else if (m.method === 1) {
      info = Buffer.alloc(m.table.length * 2);
      m.table.forEach((v, i) => info.writeUInt16BE(v, i * 2));
      begin = m.begin;
      end = m.end;
    } else {
      info = Buffer.alloc(2 + m.pairs.length * 4);
      info.writeUInt16BE(m.pairs.length, 0);
      m.pairs.forEach(([c, g], i) => {
        info.writeUInt16BE(c, 2 + i * 4);
        info.writeUInt16BE(g, 4 + i * 4);
      });
      begin = 0;
      end = 0xffff;
    }
    const head = Buffer.alloc(0x0c);
    head.writeUInt16BE(begin, 0);
    head.writeUInt16BE(end, 2);
    head.writeUInt16BE(m.method, 4);
    return block("CMAP", Buffer.concat([head, info]));
  });
  const blocks = [finf, tglpRaw, cwdh, ...cmaps];
  const total = headerSize + blocks.reduce((a, b) => a + b.length, 0);
  const header = Buffer.alloc(headerSize);
  header.write("RFNT", 0, "latin1");
  header.writeUInt16BE(0xfeff, 4);
  header.writeUInt16BE(0x0104, 6);
  header.writeUInt32BE(total, 8);
  header.writeUInt16BE(headerSize, 0x0c);
  header.writeUInt16BE(blocks.length, 0x0e);
  // FINF pointers (to block data, i.e. block start + 8)
  const out = Buffer.concat([header, ...blocks]);
  const cwdhOff = headerSize + finf.length + tglpRaw.length;
  out.writeUInt32BE(tglpStart + 8, headerSize + 0x10);
  out.writeUInt32BE(cwdhOff + 8, headerSize + 0x14);
  out.writeUInt32BE(cwdhOff + cwdh.length + 8, headerSize + 0x18);
  return out;
}
