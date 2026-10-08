import { describe, expect, it } from "vitest";

import { decodeGx, decodePalette, GxFormat, gxImageSize, PaletteFormat } from "./gx";

const px = (rgba: Uint8Array, w: number, x: number, y: number) =>
  Array.from(rgba.subarray((y * w + x) * 4, (y * w + x) * 4 + 4));

function be16(values: number[]): Uint8Array {
  const b = Buffer.alloc(values.length * 2);
  values.forEach((v, i) => b.writeUInt16BE(v, i * 2));
  return b;
}

describe("GX texture decoding", () => {
  it("computes image sizes from block geometry", () => {
    expect(gxImageSize(GxFormat.I4, 8, 8)).toBe(32);
    expect(gxImageSize(GxFormat.I4, 9, 8)).toBe(64);
    expect(gxImageSize(GxFormat.RGBA8, 4, 4)).toBe(64);
    expect(gxImageSize(GxFormat.CMPR, 4, 4)).toBe(32);
  });

  it("I4: 8x8 blocks, high nibble first, block order left to right", () => {
    const data = new Uint8Array(64);
    data[0] = 0x1f; // block 0, row 0: x0 = 0x11, x1 = 0xff
    data[4] = 0x80; // block 0, row 1: x0 = 0x88
    data[32] = 0x0c; // block 1 (x 8..15), row 0: x8 = 0, x9 = 0xcc
    const out = decodeGx(GxFormat.I4, data, 16, 8);
    expect(px(out, 16, 0, 0)).toEqual([0x11, 0x11, 0x11, 0x11]);
    expect(px(out, 16, 1, 0)).toEqual([0xff, 0xff, 0xff, 0xff]);
    expect(px(out, 16, 0, 1)).toEqual([0x88, 0x88, 0x88, 0x88]);
    expect(px(out, 16, 9, 0)).toEqual([0xcc, 0xcc, 0xcc, 0xcc]);
  });

  it("I8: 8x4 blocks", () => {
    const data = new Uint8Array(64);
    data[9] = 0x42; // row 1, x 1
    data[32 + 31] = 0x99; // block 1 last pixel => (15, 3)
    const out = decodeGx(GxFormat.I8, data, 16, 4);
    expect(px(out, 16, 1, 1)).toEqual([0x42, 0x42, 0x42, 0x42]);
    expect(px(out, 16, 15, 3)).toEqual([0x99, 0x99, 0x99, 0x99]);
  });

  it("IA4: alpha in the high nibble, intensity in the low nibble", () => {
    const data = new Uint8Array(32);
    data[0] = 0xa5;
    const out = decodeGx(GxFormat.IA4, data, 8, 4);
    expect(px(out, 8, 0, 0)).toEqual([0x55, 0x55, 0x55, 0xaa]);
  });

  it("IA8: alpha byte then intensity byte, 4x4 blocks", () => {
    const out = decodeGx(GxFormat.IA8, be16([0x80ff, ...new Array(15).fill(0)]), 4, 4);
    expect(px(out, 4, 0, 0)).toEqual([0xff, 0xff, 0xff, 0x80]);
  });

  it("RGB565", () => {
    const out = decodeGx(GxFormat.RGB565, be16([0xf800, 0x07e0, 0x001f, 0xffff, ...new Array(12).fill(0)]), 4, 4);
    expect(px(out, 4, 0, 0)).toEqual([255, 0, 0, 255]);
    expect(px(out, 4, 1, 0)).toEqual([0, 255, 0, 255]);
    expect(px(out, 4, 2, 0)).toEqual([0, 0, 255, 255]);
    expect(px(out, 4, 3, 0)).toEqual([255, 255, 255, 255]);
    expect(px(out, 4, 0, 1)).toEqual([0, 0, 0, 255]);
  });

  it("RGB5A3: opaque RGB555 and translucent ARGB3444", () => {
    const out = decodeGx(GxFormat.RGB5A3, be16([0xfc00, 0x7f00, 0x3f00, 0x00f0, ...new Array(12).fill(0)]), 4, 4);
    expect(px(out, 4, 0, 0)).toEqual([255, 0, 0, 255]);
    expect(px(out, 4, 1, 0)).toEqual([255, 0, 0, 255]);
    expect(px(out, 4, 2, 0)).toEqual([255, 0, 0, 109]);
    expect(px(out, 4, 3, 0)).toEqual([0, 255, 0, 0]);
  });

  it("RGBA8: AR plane followed by GB plane", () => {
    const data = new Uint8Array(64);
    data[0] = 0x11; // A
    data[1] = 0x22; // R
    data[32] = 0x33; // G
    data[33] = 0x44; // B
    data[30] = 0xff; // pixel 15 A
    data[31] = 0x01; // pixel 15 R
    const out = decodeGx(GxFormat.RGBA8, data, 4, 4);
    expect(px(out, 4, 0, 0)).toEqual([0x22, 0x33, 0x44, 0x11]);
    expect(px(out, 4, 3, 3)).toEqual([0x01, 0, 0, 0xff]);
  });

  it("C4 / C8 / C14X2 with IA8, RGB565 and RGB5A3 palettes", () => {
    const pal565 = decodePalette(be16([0xf800, 0x07e0, 0x001f]), PaletteFormat.RGB565, 3);
    const c4 = new Uint8Array(32);
    c4[0] = 0x12;
    let out = decodeGx(GxFormat.C4, c4, 8, 8, pal565);
    expect(px(out, 8, 0, 0)).toEqual([0, 255, 0, 255]);
    expect(px(out, 8, 1, 0)).toEqual([0, 0, 255, 255]);
    expect(px(out, 8, 2, 0)).toEqual([255, 0, 0, 255]);

    const palIa8 = decodePalette(be16([0xff10, 0x8020]), PaletteFormat.IA8, 2);
    const c8 = new Uint8Array(32);
    c8[8] = 1; // row 1, x 0
    out = decodeGx(GxFormat.C8, c8, 8, 4, palIa8);
    expect(px(out, 8, 0, 0)).toEqual([0x10, 0x10, 0x10, 0xff]);
    expect(px(out, 8, 0, 1)).toEqual([0x20, 0x20, 0x20, 0x80]);

    const big = new Array(300).fill(0x8000);
    big[257] = 0xfc00;
    const pal5a3 = decodePalette(be16(big), PaletteFormat.RGB5A3, 300);
    const c14 = be16([0xc101, ...new Array(15).fill(0)]); // index 0x0101 = 257 (top bits ignored)
    out = decodeGx(GxFormat.C14X2, c14, 4, 4, pal5a3);
    expect(px(out, 4, 0, 0)).toEqual([255, 0, 0, 255]);
    expect(px(out, 4, 1, 0)).toEqual([0, 0, 0, 255]);
  });

  it("CMPR: four DXT1 sub-blocks with big-endian colours, both colour modes", () => {
    const block = Buffer.alloc(32);
    // Sub-block 0 (top-left): red > blue => 4-colour mode; row 0 indices 0,1,2,3.
    block.writeUInt16BE(0xf800, 0);
    block.writeUInt16BE(0x001f, 2);
    block[4] = 0b00011011;
    // Sub-block 1 (top-right): black <= white => 3-colour mode with transparent index 3; row 0 indices 2,3,0,1.
    block.writeUInt16BE(0x0000, 8);
    block.writeUInt16BE(0xffff, 10);
    block[12] = 0b10110001;
    // Sub-block 3 (bottom-right): green only.
    block.writeUInt16BE(0x07e0, 24);
    block.writeUInt16BE(0x0000, 26);
    const out = decodeGx(GxFormat.CMPR, block, 8, 8);
    expect(px(out, 8, 0, 0)).toEqual([255, 0, 0, 255]);
    expect(px(out, 8, 1, 0)).toEqual([0, 0, 255, 255]);
    expect(px(out, 8, 2, 0)).toEqual([170, 0, 85, 255]);
    expect(px(out, 8, 3, 0)).toEqual([85, 0, 170, 255]);
    expect(px(out, 8, 4, 0)).toEqual([127, 127, 127, 255]);
    expect(px(out, 8, 5, 0)).toEqual([0, 0, 0, 0]);
    expect(px(out, 8, 6, 0)).toEqual([0, 0, 0, 255]);
    expect(px(out, 8, 7, 0)).toEqual([255, 255, 255, 255]);
    expect(px(out, 8, 7, 7)).toEqual([0, 255, 0, 255]);
    // A 4x4 CMPR texture still occupies a whole 8x8 block; only the top-left quarter is visible.
    const small = decodeGx(GxFormat.CMPR, block, 4, 4);
    expect(px(small, 4, 0, 0)).toEqual([255, 0, 0, 255]);
  });

  it("rejects short data, unknown formats and missing palettes", () => {
    expect(() => decodeGx(GxFormat.I4, new Uint8Array(10), 8, 8)).toThrow();
    expect(() => decodeGx(7, new Uint8Array(64), 4, 4)).toThrow();
    expect(() => decodeGx(GxFormat.C8, new Uint8Array(32), 8, 4)).toThrow();
  });
});
