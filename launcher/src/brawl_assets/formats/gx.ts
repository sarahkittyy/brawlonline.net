// GameCube/Wii GX texture decoding to RGBA8 (non-premultiplied, row-major, top-left origin).
//
// Block layouts and colour expansion ported from BrawlLib (BrawlCrate, GPL-3.0):
//   BrawlLib/Wii/Textures/{I4,I8,IA4,IA8,RGB565,RGB5A3,RGBA8,CI4,CI8,CMPR,TextureConverter}.cs
//   https://github.com/soopercool101/BrawlCrate
// Cross-checked against Dolphin's TextureDecoder (GPL-2.0-or-later) and
// https://wiki.tockdom.com/wiki/Image_Formats.
//
// SPDX-License-Identifier: GPL-3.0-or-later

export enum GxFormat {
  I4 = 0,
  I8 = 1,
  IA4 = 2,
  IA8 = 3,
  RGB565 = 4,
  RGB5A3 = 5,
  RGBA8 = 6,
  C4 = 8,
  C8 = 9,
  C14X2 = 10,
  CMPR = 14,
}

export enum PaletteFormat {
  IA8 = 0,
  RGB565 = 1,
  RGB5A3 = 2,
}

export const GX_FORMAT_NAMES: Record<number, string> = {
  0: "I4",
  1: "I8",
  2: "IA4",
  3: "IA8",
  4: "RGB565",
  5: "RGB5A3",
  6: "RGBA8",
  8: "C4",
  9: "C8",
  10: "C14X2",
  14: "CMPR",
};

type BlockInfo = { bw: number; bh: number; bytes: number };

const BLOCKS: Record<number, BlockInfo> = {
  [GxFormat.I4]: { bw: 8, bh: 8, bytes: 32 },
  [GxFormat.I8]: { bw: 8, bh: 4, bytes: 32 },
  [GxFormat.IA4]: { bw: 8, bh: 4, bytes: 32 },
  [GxFormat.IA8]: { bw: 4, bh: 4, bytes: 32 },
  [GxFormat.RGB565]: { bw: 4, bh: 4, bytes: 32 },
  [GxFormat.RGB5A3]: { bw: 4, bh: 4, bytes: 32 },
  [GxFormat.RGBA8]: { bw: 4, bh: 4, bytes: 64 },
  [GxFormat.C4]: { bw: 8, bh: 8, bytes: 32 },
  [GxFormat.C8]: { bw: 8, bh: 4, bytes: 32 },
  [GxFormat.C14X2]: { bw: 4, bh: 4, bytes: 32 },
  [GxFormat.CMPR]: { bw: 8, bh: 8, bytes: 32 },
};

export function isPaletted(format: number): boolean {
  return format === GxFormat.C4 || format === GxFormat.C8 || format === GxFormat.C14X2;
}

/** Byte size of one mip level of the given format. */
export function gxImageSize(format: number, width: number, height: number): number {
  const b = BLOCKS[format];
  if (!b) {
    throw new Error(`Unsupported GX texture format ${format}`);
  }
  return Math.ceil(width / b.bw) * Math.ceil(height / b.bh) * b.bytes;
}

const c3 = (v: number) => (v << 5) | (v << 2) | (v >> 1);
const c4 = (v: number) => (v << 4) | v;
const c5 = (v: number) => (v << 3) | (v >> 2);
const c6 = (v: number) => (v << 2) | (v >> 4);

function put(out: Uint8Array, i: number, r: number, g: number, b: number, a: number) {
  out[i] = r;
  out[i + 1] = g;
  out[i + 2] = b;
  out[i + 3] = a;
}

function rgb565(out: Uint8Array, i: number, v: number) {
  put(out, i, c5((v >> 11) & 0x1f), c6((v >> 5) & 0x3f), c5(v & 0x1f), 255);
}

function rgb5a3(out: Uint8Array, i: number, v: number) {
  if (v & 0x8000) {
    put(out, i, c5((v >> 10) & 0x1f), c5((v >> 5) & 0x1f), c5(v & 0x1f), 255);
  } else {
    put(out, i, c4((v >> 8) & 0x0f), c4((v >> 4) & 0x0f), c4(v & 0x0f), c3((v >> 12) & 0x07));
  }
}

function ia8(out: Uint8Array, i: number, v: number) {
  const a = v >> 8;
  const l = v & 0xff;
  put(out, i, l, l, l, a);
}

/** Decodes a palette (PLT0 data) into RGBA8 entries. */
export function decodePalette(data: Uint8Array, format: number, count: number): Uint8Array {
  const out = new Uint8Array(count * 4);
  for (let n = 0; n < count; n++) {
    const v = (data[n * 2] << 8) | data[n * 2 + 1];
    if (format === PaletteFormat.IA8) {
      ia8(out, n * 4, v);
    } else if (format === PaletteFormat.RGB565) {
      rgb565(out, n * 4, v);
    } else if (format === PaletteFormat.RGB5A3) {
      rgb5a3(out, n * 4, v);
    } else {
      throw new Error(`Unsupported palette format ${format}`);
    }
  }
  return out;
}

/**
 * Decodes one GX image (a single mip level) to RGBA8. `palette` (RGBA8 entries from decodePalette) is required
 * for C4/C8/C14X2.
 */
export function decodeGx(
  format: number,
  data: Uint8Array,
  width: number,
  height: number,
  palette?: Uint8Array,
): Uint8Array {
  const info = BLOCKS[format];
  if (!info) {
    throw new Error(`Unsupported GX texture format ${format}`);
  }
  const need = gxImageSize(format, width, height);
  if (data.length < need) {
    throw new Error(`GX ${GX_FORMAT_NAMES[format]} ${width}x${height}: need ${need} bytes, have ${data.length}`);
  }
  if (isPaletted(format) && !palette) {
    throw new Error(`GX ${GX_FORMAT_NAMES[format]} requires a palette`);
  }
  const out = new Uint8Array(width * height * 4);
  const bx = Math.ceil(width / info.bw);
  const by = Math.ceil(height / info.bh);
  const px = new Uint8Array(info.bw * info.bh * 4); // decoded block, row-major
  let src = 0;
  for (let y0 = 0; y0 < by; y0++) {
    for (let x0 = 0; x0 < bx; x0++) {
      decodeBlock(format, data, src, px, palette);
      src += info.bytes;
      for (let y = 0; y < info.bh; y++) {
        const iy = y0 * info.bh + y;
        if (iy >= height) {
          break;
        }
        for (let x = 0; x < info.bw; x++) {
          const ix = x0 * info.bw + x;
          if (ix >= width) {
            break;
          }
          const s = (y * info.bw + x) * 4;
          const d = (iy * width + ix) * 4;
          out[d] = px[s];
          out[d + 1] = px[s + 1];
          out[d + 2] = px[s + 2];
          out[d + 3] = px[s + 3];
        }
      }
    }
  }
  return out;
}

function palLookup(px: Uint8Array, i: number, palette: Uint8Array, idx: number) {
  const p = idx * 4;
  if (p + 3 >= palette.length) {
    put(px, i, 0, 0, 0, 0);
    return;
  }
  put(px, i, palette[p], palette[p + 1], palette[p + 2], palette[p + 3]);
}

function decodeBlock(format: number, d: Uint8Array, s: number, px: Uint8Array, palette?: Uint8Array) {
  switch (format) {
    case GxFormat.I4:
      for (let n = 0; n < 32; n++) {
        const b = d[s + n];
        const hi = c4(b >> 4);
        const lo = c4(b & 0x0f);
        put(px, n * 8, hi, hi, hi, hi);
        put(px, n * 8 + 4, lo, lo, lo, lo);
      }
      return;
    case GxFormat.I8:
      for (let n = 0; n < 32; n++) {
        const v = d[s + n];
        put(px, n * 4, v, v, v, v);
      }
      return;
    case GxFormat.IA4:
      for (let n = 0; n < 32; n++) {
        const b = d[s + n];
        const l = c4(b & 0x0f);
        put(px, n * 4, l, l, l, c4(b >> 4));
      }
      return;
    case GxFormat.IA8:
      for (let n = 0; n < 16; n++) {
        ia8(px, n * 4, (d[s + n * 2] << 8) | d[s + n * 2 + 1]);
      }
      return;
    case GxFormat.RGB565:
      for (let n = 0; n < 16; n++) {
        rgb565(px, n * 4, (d[s + n * 2] << 8) | d[s + n * 2 + 1]);
      }
      return;
    case GxFormat.RGB5A3:
      for (let n = 0; n < 16; n++) {
        rgb5a3(px, n * 4, (d[s + n * 2] << 8) | d[s + n * 2 + 1]);
      }
      return;
    case GxFormat.RGBA8:
      // 32 bytes of AR pairs followed by 32 bytes of GB pairs.
      for (let n = 0; n < 16; n++) {
        put(px, n * 4, d[s + n * 2 + 1], d[s + 32 + n * 2], d[s + 32 + n * 2 + 1], d[s + n * 2]);
      }
      return;
    case GxFormat.C4:
      for (let n = 0; n < 32; n++) {
        const b = d[s + n];
        palLookup(px, n * 8, palette!, b >> 4);
        palLookup(px, n * 8 + 4, palette!, b & 0x0f);
      }
      return;
    case GxFormat.C8:
      for (let n = 0; n < 32; n++) {
        palLookup(px, n * 4, palette!, d[s + n]);
      }
      return;
    case GxFormat.C14X2:
      for (let n = 0; n < 16; n++) {
        palLookup(px, n * 4, palette!, ((d[s + n * 2] << 8) | d[s + n * 2 + 1]) & 0x3fff);
      }
      return;
    case GxFormat.CMPR:
      // Four DXT1 sub-blocks (top-left, top-right, bottom-left, bottom-right) with big-endian colours and
      // 2-bit indices packed MSB-first per row.
      for (let sub = 0; sub < 4; sub++) {
        decodeDxt1(d, s + sub * 8, px, (sub & 1) * 4, (sub >> 1) * 4);
      }
      return;
    default:
      throw new Error(`Unsupported GX texture format ${format}`);
  }
}

const dxtPal = new Uint8Array(16);

function decodeDxt1(d: Uint8Array, s: number, px: Uint8Array, ox: number, oy: number) {
  const v0 = (d[s] << 8) | d[s + 1];
  const v1 = (d[s + 2] << 8) | d[s + 3];
  rgb565(dxtPal, 0, v0);
  rgb565(dxtPal, 4, v1);
  if (v0 > v1) {
    for (let ch = 0; ch < 3; ch++) {
      dxtPal[8 + ch] = Math.floor((2 * dxtPal[ch] + dxtPal[4 + ch]) / 3);
      dxtPal[12 + ch] = Math.floor((dxtPal[ch] + 2 * dxtPal[4 + ch]) / 3);
    }
    dxtPal[11] = 255;
    dxtPal[15] = 255;
  } else {
    for (let ch = 0; ch < 3; ch++) {
      dxtPal[8 + ch] = (dxtPal[ch] + dxtPal[4 + ch]) >> 1;
      dxtPal[12 + ch] = 0;
    }
    dxtPal[11] = 255;
    dxtPal[15] = 0;
  }
  for (let y = 0; y < 4; y++) {
    const row = d[s + 4 + y];
    for (let x = 0; x < 4; x++) {
      const idx = (row >> (6 - x * 2)) & 3;
      const o = ((oy + y) * 8 + ox + x) * 4;
      px[o] = dxtPal[idx * 4];
      px[o + 1] = dxtPal[idx * 4 + 1];
      px[o + 2] = dxtPal[idx * 4 + 2];
      px[o + 3] = dxtPal[idx * 4 + 3];
    }
  }
}
