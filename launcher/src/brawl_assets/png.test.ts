import { describe, expect, it } from "vitest";
import zlib from "zlib";

import { crc32, decodePng, encodePng, PNG_SIGNATURE } from "./png";

describe("PNG encoder", () => {
  it("computes the standard CRC-32", () => {
    expect(crc32(Buffer.from("123456789"))).toBe(0xcbf43926);
    expect(crc32(Buffer.from("IEND"))).toBe(0xae426082);
  });

  it("writes valid chunks that zlib and the CRC check accept", () => {
    const w = 3;
    const h = 2;
    const rgba = new Uint8Array([255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
    const png = encodePng(w, h, rgba);
    expect(png.subarray(0, 8).equals(PNG_SIGNATURE)).toBe(true);
    // Walk chunks manually.
    let off = 8;
    const types: string[] = [];
    let idat = Buffer.alloc(0);
    while (off < png.length) {
      const len = png.readUInt32BE(off);
      const type = png.toString("latin1", off + 4, off + 8);
      types.push(type);
      expect(png.readUInt32BE(off + 8 + len)).toBe(crc32(png.subarray(off + 4, off + 8 + len)));
      if (type === "IHDR") {
        expect(png.readUInt32BE(off + 8)).toBe(w);
        expect(png.readUInt32BE(off + 12)).toBe(h);
        expect([png[off + 16], png[off + 17]]).toEqual([8, 6]);
      }
      if (type === "IDAT") {
        idat = Buffer.concat([idat, png.subarray(off + 8, off + 8 + len)]);
      }
      off += 12 + len;
    }
    expect(types).toEqual(["IHDR", "IDAT", "IEND"]);
    const raw = zlib.inflateSync(idat);
    expect(raw.length).toBe((w * 4 + 1) * h);
    expect(raw[0]).toBe(0);
    expect(Array.from(raw.subarray(1, 5))).toEqual([255, 0, 0, 255]);
    expect(Array.from(decodePng(png).rgba)).toEqual(Array.from(rgba));
  });

  it("rejects mismatched buffer sizes", () => {
    expect(() => encodePng(2, 2, new Uint8Array(3))).toThrow();
  });
});
