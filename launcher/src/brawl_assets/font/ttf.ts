// Minimal TrueType (glyf-flavoured sfnt) writer: head, hhea, maxp, OS/2, name, cmap (format 4), post (3.0),
// loca (long), glyf (simple glyphs, on-curve points only) and hmtx. Plus a small reader used by tests.
//
// Table layouts follow the OpenType specification (https://learn.microsoft.com/typography/opentype/spec/).
//
// SPDX-License-Identifier: GPL-3.0-or-later

export type TtfPoint = { x: number; y: number };

export type TtfGlyph = {
  /** Closed polygons in font units (y up). Outer contours clockwise, holes counter-clockwise. */
  contours: TtfPoint[][];
  advanceWidth: number;
};

export type TtfFont = {
  familyName: string;
  styleName?: string;
  version?: string;
  unitsPerEm: number;
  ascender: number;
  /** Negative. */
  descender: number;
  lineGap: number;
  weightClass?: number;
  /** Glyph 0 must be .notdef. */
  glyphs: TtfGlyph[];
  /** Unicode BMP code point -> glyph index. */
  cmap: Map<number, number>;
};

class Writer {
  private parts: number[] = [];
  u8(v: number) {
    this.parts.push(v & 0xff);
    return this;
  }
  u16(v: number) {
    return this.u8(v >> 8).u8(v);
  }
  i16(v: number) {
    return this.u16(v < 0 ? v + 0x10000 : v);
  }
  u32(v: number) {
    return this.u16(Math.floor(v / 0x10000) & 0xffff).u16(v & 0xffff);
  }
  i32(v: number) {
    return this.u32(v < 0 ? v + 0x100000000 : v);
  }
  fixed(v: number) {
    return this.u32(Math.round(v * 65536) >>> 0);
  }
  bytes(b: ArrayLike<number>) {
    for (let i = 0; i < b.length; i++) {
      this.parts.push(b[i] & 0xff);
    }
    return this;
  }
  tag(s: string) {
    return this.bytes(Buffer.from(s.padEnd(4, " ").slice(0, 4), "latin1"));
  }
  get length() {
    return this.parts.length;
  }
  buffer() {
    return Buffer.from(this.parts);
  }
}

type Bounds = { xMin: number; yMin: number; xMax: number; yMax: number };

function boundsOf(contours: TtfPoint[][]): Bounds | null {
  let b: Bounds | null = null;
  for (const c of contours) {
    for (const p of c) {
      if (!b) {
        b = { xMin: p.x, yMin: p.y, xMax: p.x, yMax: p.y };
      } else {
        b.xMin = Math.min(b.xMin, p.x);
        b.yMin = Math.min(b.yMin, p.y);
        b.xMax = Math.max(b.xMax, p.x);
        b.yMax = Math.max(b.yMax, p.y);
      }
    }
  }
  return b;
}

function encodeGlyph(g: TtfGlyph): { data: Buffer; bounds: Bounds | null; points: number } {
  const contours = g.contours
    .filter((c) => c.length >= 3)
    .map((c) => c.map((p) => ({ x: Math.round(p.x), y: Math.round(p.y) })));
  const bounds = boundsOf(contours);
  if (!bounds) {
    return { data: Buffer.alloc(0), bounds: null, points: 0 };
  }
  const w = new Writer();
  w.i16(contours.length).i16(bounds.xMin).i16(bounds.yMin).i16(bounds.xMax).i16(bounds.yMax);
  let end = -1;
  for (const c of contours) {
    end += c.length;
    w.u16(end);
  }
  w.u16(0); // instructionLength
  const flags: number[] = [];
  const xs = new Writer();
  const ys = new Writer();
  let px = 0;
  let py = 0;
  for (const c of contours) {
    for (const p of c) {
      let f = 0x01; // on curve
      const dx = p.x - px;
      const dy = p.y - py;
      if (dx === 0) {
        f |= 0x10;
      } else if (Math.abs(dx) < 256) {
        f |= 0x02 | (dx > 0 ? 0x10 : 0);
        xs.u8(Math.abs(dx));
      } else {
        xs.i16(dx);
      }
      if (dy === 0) {
        f |= 0x20;
      } else if (Math.abs(dy) < 256) {
        f |= 0x04 | (dy > 0 ? 0x20 : 0);
        ys.u8(Math.abs(dy));
      } else {
        ys.i16(dy);
      }
      flags.push(f);
      px = p.x;
      py = p.y;
    }
  }
  w.bytes(flags).bytes(xs.buffer()).bytes(ys.buffer());
  while (w.length % 4) {
    w.u8(0);
  }
  return { data: w.buffer(), bounds, points: flags.length };
}

function checksum(buf: Buffer): number {
  let sum = 0;
  const padded = buf.length % 4 ? Buffer.concat([buf, Buffer.alloc(4 - (buf.length % 4))]) : buf;
  for (let i = 0; i < padded.length; i += 4) {
    sum = (sum + padded.readUInt32BE(i)) >>> 0;
  }
  return sum;
}

function nameTable(records: [number, string][]): Buffer {
  const w = new Writer();
  w.u16(0)
    .u16(records.length)
    .u16(6 + records.length * 12);
  const strings: Buffer[] = [];
  let off = 0;
  for (const [id, s] of records) {
    const b = Buffer.from(s, "utf16le").swap16();
    w.u16(3).u16(1).u16(0x409).u16(id).u16(b.length).u16(off);
    strings.push(b);
    off += b.length;
  }
  return Buffer.concat([w.buffer(), ...strings]);
}

function cmapTable(cmap: Map<number, number>): Buffer {
  const codes = [...cmap.keys()].filter((c) => c >= 0 && c < 0xffff).sort((a, b) => a - b);
  // Segments: runs of consecutive codes whose glyph ids are also consecutive (idDelta only).
  const segs: { start: number; end: number; delta: number }[] = [];
  for (const c of codes) {
    const g = cmap.get(c)!;
    const last = segs[segs.length - 1];
    if (last && last.end === c - 1 && (last.delta + c) % 0x10000 === g) {
      last.end = c;
    } else {
      segs.push({ start: c, end: c, delta: (g - c + 0x10000) % 0x10000 });
    }
  }
  segs.push({ start: 0xffff, end: 0xffff, delta: 1 });
  const segX2 = segs.length * 2;
  let searchRange = 2;
  let entrySelector = 0;
  while (searchRange * 2 <= segX2) {
    searchRange *= 2;
    entrySelector++;
  }
  const sub = new Writer();
  const length = 16 + segs.length * 8;
  sub
    .u16(4)
    .u16(length)
    .u16(0)
    .u16(segX2)
    .u16(searchRange)
    .u16(entrySelector)
    .u16(segX2 - searchRange);
  segs.forEach((s) => sub.u16(s.end));
  sub.u16(0);
  segs.forEach((s) => sub.u16(s.start));
  segs.forEach((s) => sub.u16(s.delta));
  segs.forEach(() => sub.u16(0));
  const w = new Writer();
  w.u16(0).u16(2);
  w.u16(0)
    .u16(3)
    .u32(4 + 2 * 8); // Unicode BMP
  w.u16(3)
    .u16(1)
    .u32(4 + 2 * 8); // Windows Unicode BMP (same subtable)
  return Buffer.concat([w.buffer(), sub.buffer()]);
}

/** Serialises a font to a TrueType (.ttf) buffer. */
export function writeTtf(font: TtfFont): Buffer {
  if (font.glyphs.length === 0) {
    throw new Error("TTF: at least one glyph (.notdef) is required");
  }
  const style = font.styleName ?? "Regular";
  const version = font.version ?? "Version 1.000";
  const encoded = font.glyphs.map(encodeGlyph);
  const glyf = Buffer.concat(encoded.map((e) => e.data));
  const loca = new Writer();
  let off = 0;
  for (const e of encoded) {
    loca.u32(off);
    off += e.data.length;
  }
  loca.u32(off);
  let xMin = 0;
  let yMin = 0;
  let xMax = 0;
  let yMax = 0;
  let first = true;
  let maxPoints = 0;
  let maxContours = 0;
  let minLsb = 0;
  let minRsb = 0;
  let maxExtent = 0;
  let advMax = 0;
  const hmtx = new Writer();
  font.glyphs.forEach((g, i) => {
    const b = encoded[i].bounds;
    const lsb = b ? b.xMin : 0;
    hmtx.u16(Math.max(0, Math.round(g.advanceWidth))).i16(lsb);
    advMax = Math.max(advMax, Math.round(g.advanceWidth));
    maxPoints = Math.max(maxPoints, encoded[i].points);
    maxContours = Math.max(maxContours, g.contours.length);
    if (b) {
      if (first) {
        ({ xMin, yMin, xMax, yMax } = b);
        minLsb = b.xMin;
        minRsb = g.advanceWidth - b.xMax;
        first = false;
      }
      xMin = Math.min(xMin, b.xMin);
      yMin = Math.min(yMin, b.yMin);
      xMax = Math.max(xMax, b.xMax);
      yMax = Math.max(yMax, b.yMax);
      minLsb = Math.min(minLsb, b.xMin);
      minRsb = Math.min(minRsb, Math.round(g.advanceWidth) - b.xMax);
      maxExtent = Math.max(maxExtent, b.xMax);
    }
  });
  const numGlyphs = font.glyphs.length;
  const head = new Writer();
  // LONGDATETIME: seconds since 1904-01-01; a fixed value keeps output deterministic.
  const created = 3786825600; // 2024-01-01T00:00:00Z
  head
    .fixed(1)
    .fixed(1)
    .u32(0) // checkSumAdjustment, patched below
    .u32(0x5f0f3cf5)
    .u16(0x000b)
    .u16(font.unitsPerEm)
    .u32(0)
    .u32(created)
    .u32(0)
    .u32(created)
    .i16(xMin)
    .i16(yMin)
    .i16(xMax)
    .i16(yMax)
    .u16(0)
    .u16(8)
    .i16(2)
    .i16(1) // indexToLocFormat: long
    .i16(0);
  const hhea = new Writer();
  hhea
    .fixed(1)
    .i16(font.ascender)
    .i16(font.descender)
    .i16(font.lineGap)
    .u16(advMax)
    .i16(minLsb)
    .i16(minRsb)
    .i16(maxExtent)
    .i16(1)
    .i16(0)
    .i16(0)
    .i16(0)
    .i16(0)
    .i16(0)
    .i16(0)
    .i16(0)
    .u16(numGlyphs);
  const maxp = new Writer();
  maxp.fixed(1).u16(numGlyphs).u16(maxPoints).u16(maxContours).u16(0).u16(0).u16(2);
  // maxTwilightPoints, maxStorage, maxFunctionDefs, maxInstructionDefs, maxStackElements, maxSizeOfInstructions,
  // maxComponentElements, maxComponentDepth
  for (let i = 0; i < 8; i++) {
    maxp.u16(0);
  }
  const codes = [...font.cmap.keys()].filter((c) => c < 0xffff).sort((a, b) => a - b);
  const avg = Math.round(font.glyphs.reduce((a, g) => a + g.advanceWidth, 0) / numGlyphs);
  const xGlyph = font.cmap.get(0x78);
  const hGlyph = font.cmap.get(0x48);
  const xHeight = xGlyph !== undefined ? encoded[xGlyph].bounds?.yMax ?? 0 : 0;
  const capHeight = hGlyph !== undefined ? encoded[hGlyph].bounds?.yMax ?? 0 : 0;
  const os2 = new Writer();
  os2
    .u16(4)
    .i16(avg)
    .u16(font.weightClass ?? 400)
    .u16(5)
    .u16(0)
    .i16(Math.round(font.unitsPerEm * 0.65))
    .i16(Math.round(font.unitsPerEm * 0.6))
    .i16(0)
    .i16(Math.round(font.unitsPerEm * 0.075))
    .i16(Math.round(font.unitsPerEm * 0.65))
    .i16(Math.round(font.unitsPerEm * 0.6))
    .i16(0)
    .i16(Math.round(font.unitsPerEm * 0.35))
    .i16(Math.round(font.unitsPerEm * 0.05))
    .i16(Math.round(font.unitsPerEm * 0.26))
    .i16(0)
    .bytes(new Array(10).fill(0))
    .u32(1) // Basic Latin
    .u32(0)
    .u32(0)
    .u32(0)
    .tag("NONE")
    .u16((font.weightClass ?? 400) >= 700 ? 0x20 : 0x40)
    .u16(codes.length ? codes[0] : 0x20)
    .u16(codes.length ? codes[codes.length - 1] : 0x20)
    .i16(font.ascender)
    .i16(font.descender)
    .i16(font.lineGap)
    .u16(Math.max(font.ascender, yMax))
    .u16(Math.max(-font.descender, -yMin))
    .u32(1) // Latin 1
    .u32(0)
    .i16(xHeight)
    .i16(capHeight)
    .u16(0)
    .u16(0x20)
    .u16(1);
  const psName = `${font.familyName}-${style}`.replace(/[^A-Za-z0-9-]/g, "");
  const name = nameTable([
    [1, font.familyName],
    [2, style],
    [3, `${font.familyName} ${style}`],
    [4, `${font.familyName} ${style}`],
    [5, version],
    [6, psName],
  ]);
  const post = new Writer();
  post
    .fixed(3)
    .fixed(0)
    .i16(Math.round(-font.unitsPerEm * 0.1))
    .i16(Math.round(font.unitsPerEm * 0.05))
    .u32(0)
    .u32(0)
    .u32(0)
    .u32(0)
    .u32(0);
  const tables: [string, Buffer][] = [
    ["OS/2", os2.buffer()],
    ["cmap", cmapTable(font.cmap)],
    ["glyf", glyf],
    ["head", head.buffer()],
    ["hhea", hhea.buffer()],
    ["hmtx", hmtx.buffer()],
    ["loca", loca.buffer()],
    ["maxp", maxp.buffer()],
    ["name", name],
    ["post", post.buffer()],
  ];
  tables.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  const numTables = tables.length;
  let searchRange = 1;
  let entrySelector = 0;
  while (searchRange * 2 <= numTables) {
    searchRange *= 2;
    entrySelector++;
  }
  searchRange *= 16;
  const dir = new Writer();
  dir
    .u32(0x00010000)
    .u16(numTables)
    .u16(searchRange)
    .u16(entrySelector)
    .u16(numTables * 16 - searchRange);
  let dataOff = 12 + numTables * 16;
  const bodies: Buffer[] = [];
  let headOffset = 0;
  for (const [tag, body] of tables) {
    dir.tag(tag).u32(checksum(body)).u32(dataOff).u32(body.length);
    if (tag === "head") {
      headOffset = dataOff;
    }
    const padded = body.length % 4 ? Buffer.concat([body, Buffer.alloc(4 - (body.length % 4))]) : body;
    bodies.push(padded);
    dataOff += padded.length;
  }
  const out = Buffer.concat([dir.buffer(), ...bodies]);
  const adjust = (0xb1b0afba - checksum(out)) >>> 0;
  out.writeUInt32BE(adjust, headOffset + 8);
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Reader (tests / verification).

export type ParsedTtf = {
  tables: Map<string, { offset: number; length: number; checksum: number }>;
  unitsPerEm: number;
  numGlyphs: number;
  advance: (glyph: number) => number;
  glyphFor: (code: number) => number;
  contours: (glyph: number) => TtfPoint[][];
  familyName: string;
};

export function readTtf(buf: Buffer): ParsedTtf {
  const numTables = buf.readUInt16BE(4);
  const tables = new Map<string, { offset: number; length: number; checksum: number }>();
  for (let i = 0; i < numTables; i++) {
    const o = 12 + i * 16;
    tables.set(buf.toString("latin1", o, o + 4), {
      checksum: buf.readUInt32BE(o + 4),
      offset: buf.readUInt32BE(o + 8),
      length: buf.readUInt32BE(o + 12),
    });
  }
  const t = (tag: string) => {
    const e = tables.get(tag);
    if (!e) {
      throw new Error(`TTF: missing ${tag}`);
    }
    return e;
  };
  const head = t("head").offset;
  const unitsPerEm = buf.readUInt16BE(head + 18);
  const longLoca = buf.readInt16BE(head + 50) === 1;
  const numGlyphs = buf.readUInt16BE(t("maxp").offset + 4);
  const numHMetrics = buf.readUInt16BE(t("hhea").offset + 34);
  const hmtx = t("hmtx").offset;
  const loca = t("loca").offset;
  const glyf = t("glyf").offset;
  const locaAt = (i: number) => (longLoca ? buf.readUInt32BE(loca + i * 4) : buf.readUInt16BE(loca + i * 2) * 2);
  const cmap = t("cmap").offset;
  const nSub = buf.readUInt16BE(cmap + 2);
  let sub = -1;
  for (let i = 0; i < nSub; i++) {
    const p = buf.readUInt16BE(cmap + 4 + i * 8);
    const e = buf.readUInt16BE(cmap + 6 + i * 8);
    if ((p === 3 && e === 1) || p === 0) {
      sub = cmap + buf.readUInt32BE(cmap + 8 + i * 8);
    }
  }
  const nameOff = t("name").offset;
  let familyName = "";
  const count = buf.readUInt16BE(nameOff + 2);
  const strOff = nameOff + buf.readUInt16BE(nameOff + 4);
  for (let i = 0; i < count; i++) {
    const r = nameOff + 6 + i * 12;
    if (buf.readUInt16BE(r + 6) === 1 && buf.readUInt16BE(r) === 3) {
      const len = buf.readUInt16BE(r + 8);
      const o = strOff + buf.readUInt16BE(r + 10);
      familyName = Buffer.from(buf.subarray(o, o + len))
        .swap16()
        .toString("utf16le");
    }
  }
  return {
    tables,
    unitsPerEm,
    numGlyphs,
    familyName,
    advance: (g) => buf.readUInt16BE(hmtx + Math.min(g, numHMetrics - 1) * 4),
    glyphFor: (code) => {
      if (sub < 0 || buf.readUInt16BE(sub) !== 4) {
        return 0;
      }
      const segX2 = buf.readUInt16BE(sub + 6);
      const ends = sub + 14;
      const starts = ends + segX2 + 2;
      const deltas = starts + segX2;
      const ranges = deltas + segX2;
      for (let s = 0; s < segX2 / 2; s++) {
        const end = buf.readUInt16BE(ends + s * 2);
        if (code > end) {
          continue;
        }
        const start = buf.readUInt16BE(starts + s * 2);
        if (code < start) {
          return 0;
        }
        const delta = buf.readUInt16BE(deltas + s * 2);
        const ro = buf.readUInt16BE(ranges + s * 2);
        if (ro === 0) {
          return (code + delta) & 0xffff;
        }
        const g = buf.readUInt16BE(ranges + s * 2 + ro + (code - start) * 2);
        return g === 0 ? 0 : (g + delta) & 0xffff;
      }
      return 0;
    },
    contours: (g) => {
      const start = glyf + locaAt(g);
      const end = glyf + locaAt(g + 1);
      if (end <= start) {
        return [];
      }
      const n = buf.readInt16BE(start);
      if (n < 0) {
        throw new Error("TTF reader: composite glyphs not supported");
      }
      const endPts: number[] = [];
      for (let i = 0; i < n; i++) {
        endPts.push(buf.readUInt16BE(start + 10 + i * 2));
      }
      const total = n ? endPts[n - 1] + 1 : 0;
      let p = start + 10 + n * 2;
      p += 2 + buf.readUInt16BE(p);
      const flags: number[] = [];
      while (flags.length < total) {
        const f = buf[p++];
        flags.push(f);
        if (f & 0x08) {
          let r = buf[p++];
          while (r-- > 0) {
            flags.push(f);
          }
        }
      }
      const read = (shortBit: number, sameBit: number) => {
        const out: number[] = [];
        let v = 0;
        for (const f of flags) {
          if (f & shortBit) {
            const d = buf[p++];
            v += f & sameBit ? d : -d;
          } else if (!(f & sameBit)) {
            v += buf.readInt16BE(p);
            p += 2;
          }
          out.push(v);
        }
        return out;
      };
      const xs = read(0x02, 0x10);
      const ys = read(0x04, 0x20);
      const contours: TtfPoint[][] = [];
      let s = 0;
      for (const e of endPts) {
        const c: TtfPoint[] = [];
        for (let i = s; i <= e; i++) {
          c.push({ x: xs[i], y: ys[i] });
        }
        contours.push(c);
        s = e + 1;
      }
      return contours;
    },
  };
}

export function ttfChecksum(buf: Buffer): number {
  return checksum(buf);
}
