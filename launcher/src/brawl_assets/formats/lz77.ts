// Nintendo LZ77 (type 0x10) / extended LZ77 (type 0x11) and run-length (type 0x30) codecs.
//
// Decompression logic ported from BrawlLib (BrawlCrate, GPL-3.0):
//   BrawlLib/Wii/Compression/LZ77.cs, RunLength.cs, CompressionHeader.cs
//   https://github.com/soopercool101/BrawlCrate
// The compressors here are simple greedy encoders used for tests and tooling; they produce
// streams any standard decoder accepts.
//
// SPDX-License-Identifier: GPL-3.0-or-later

export type CompressionKind = "lz77" | "lz77ext" | "rle";

type Header = { kind: CompressionKind; size: number; dataOffset: number };

function readHeader(src: Uint8Array): Header | null {
  if (src.length < 4) {
    return null;
  }
  const algo = src[0];
  let kind: CompressionKind;
  if (algo === 0x10) {
    kind = "lz77";
  } else if (algo === 0x11) {
    kind = "lz77ext";
  } else if (algo === 0x30) {
    kind = "rle";
  } else {
    return null;
  }
  let size = src[1] | (src[2] << 8) | (src[3] << 16);
  let dataOffset = 4;
  if (size === 0) {
    if (src.length < 8) {
      return null;
    }
    size = (src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24)) >>> 0;
    dataOffset = 8;
  }
  return { kind, size, dataOffset };
}

/** Returns the compression type if the buffer starts with a plausible Nintendo compression header. */
export function detectCompression(src: Uint8Array): CompressionKind | null {
  const h = readHeader(src);
  if (!h) {
    return null;
  }
  // Sanity: compressed payloads are never larger than ~9/8 of the output plus header,
  // and outputs are bounded. Reject absurd sizes to avoid false positives on raw data.
  if (h.size === 0 || h.size > 512 * 1024 * 1024) {
    return null;
  }
  if (h.kind === "rle") {
    return src.length - h.dataOffset >= 2 ? h.kind : null;
  }
  if (src.length - h.dataOffset < 2) {
    return null;
  }
  // The first flag byte's top bit must be 0 (nothing to copy back from yet).
  if ((src[h.dataOffset] & 0x80) !== 0) {
    return null;
  }
  return h.kind;
}

/** Decompresses a Nintendo LZ77/LZ77ext/RLE stream (with its 4 or 8 byte header). */
export function decompress(src: Uint8Array): Buffer {
  const h = readHeader(src);
  if (!h) {
    throw new Error("Not a supported Nintendo compressed stream");
  }
  if (h.kind === "rle") {
    return expandRle(src, h.dataOffset, h.size);
  }
  return expandLz(src, h.dataOffset, h.size, h.kind === "lz77ext");
}

function expandLz(src: Uint8Array, start: number, size: number, ext: boolean): Buffer {
  const dst = Buffer.alloc(size);
  let s = start;
  let d = 0;
  while (d < size) {
    if (s >= src.length) {
      throw new Error("LZ77: unexpected end of input");
    }
    const flags = src[s++];
    for (let bit = 7; bit >= 0 && d < size; bit--) {
      if ((flags & (1 << bit)) === 0) {
        if (s >= src.length) {
          throw new Error("LZ77: unexpected end of input");
        }
        dst[d++] = src[s++];
        continue;
      }
      if (s + 1 >= src.length) {
        throw new Error("LZ77: unexpected end of input");
      }
      let len: number;
      const b0 = src[s];
      if (!ext) {
        len = (b0 >> 4) + 3;
      } else {
        const ind = b0 >> 4;
        if (ind === 0) {
          len = (((src[s] & 0x0f) << 4) | (src[s + 1] >> 4)) + 0x11;
          s += 1;
        } else if (ind === 1) {
          len = (((src[s] & 0x0f) << 12) | (src[s + 1] << 4) | (src[s + 2] >> 4)) + 0x111;
          s += 2;
        } else {
          len = ind + 1;
        }
      }
      const disp = (((src[s] & 0x0f) << 8) | src[s + 1]) + 1;
      s += 2;
      if (disp > d) {
        throw new Error("LZ77: back-reference before start of output");
      }
      for (let i = 0; i < len && d < size; i++, d++) {
        dst[d] = dst[d - disp];
      }
    }
  }
  return dst;
}

function expandRle(src: Uint8Array, start: number, size: number): Buffer {
  const dst = Buffer.alloc(size);
  let s = start;
  let d = 0;
  while (d < size) {
    const flag = src[s++];
    if (flag & 0x80) {
      const len = (flag & 0x7f) + 3;
      const v = src[s++];
      for (let i = 0; i < len && d < size; i++) {
        dst[d++] = v;
      }
    } else {
      const len = (flag & 0x7f) + 1;
      for (let i = 0; i < len && d < size; i++) {
        dst[d++] = src[s++];
      }
    }
  }
  return dst;
}

/** Greedy LZ77 compressor (type 0x10, or 0x11 when `extended`). Intended for tests/tooling. */
export function compressLz77(data: Uint8Array, extended = false): Buffer {
  const out: number[] = [];
  const size = data.length;
  if (size <= 0xffffff && size > 0) {
    out.push(extended ? 0x11 : 0x10, size & 0xff, (size >> 8) & 0xff, (size >> 16) & 0xff);
  } else {
    out.push(extended ? 0x11 : 0x10, 0, 0, 0, size & 0xff, (size >> 8) & 0xff, (size >> 16) & 0xff, size >>> 24);
  }
  const maxLen = extended ? 0x10110 : 18;
  const minLen = 3;
  let pos = 0;
  while (pos < size) {
    const flagIndex = out.length;
    out.push(0);
    let flags = 0;
    for (let bit = 7; bit >= 0 && pos < size; bit--) {
      let bestLen = 0;
      let bestDisp = 0;
      const windowStart = Math.max(0, pos - 0x1000);
      for (let cand = pos - 1; cand >= windowStart; cand--) {
        let l = 0;
        while (l < maxLen && pos + l < size && data[cand + l] === data[pos + l]) {
          l++;
        }
        if (l > bestLen) {
          bestLen = l;
          bestDisp = pos - cand;
          if (l === maxLen) {
            break;
          }
        }
      }
      if (bestLen >= minLen) {
        flags |= 1 << bit;
        const disp = bestDisp - 1;
        if (!extended) {
          out.push(((bestLen - 3) << 4) | (disp >> 8), disp & 0xff);
        } else if (bestLen <= 0x10) {
          out.push(((bestLen - 1) << 4) | (disp >> 8), disp & 0xff);
        } else if (bestLen <= 0x110) {
          const l = bestLen - 0x11;
          out.push(l >> 4, ((l & 0x0f) << 4) | (disp >> 8), disp & 0xff);
        } else {
          const l = bestLen - 0x111;
          out.push(0x10 | (l >> 12), (l >> 4) & 0xff, ((l & 0x0f) << 4) | (disp >> 8), disp & 0xff);
        }
        pos += bestLen;
      } else {
        out.push(data[pos++]);
      }
    }
    out[flagIndex] = flags;
  }
  while (out.length % 4 !== 0) {
    out.push(0);
  }
  return Buffer.from(out);
}
