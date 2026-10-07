import { describe, expect, it } from "vitest";

import { compressLz77, decompress, detectCompression } from "./lz77";

describe("LZ77", () => {
  it("decodes a hand-built type 0x10 stream", () => {
    // "abc" literals, then a back-reference of length 9 at distance 3, then "X".
    const src = Buffer.from([0x10, 0x0d, 0x00, 0x00, 0x10, 0x61, 0x62, 0x63, 0x60, 0x02, 0x58]);
    expect(detectCompression(src)).toBe("lz77");
    expect(decompress(src).toString("latin1")).toBe("abcabcabcabcX");
  });

  it("decodes a hand-built type 0x11 stream (3-byte and 4-byte back-references)", () => {
    // "abc" + 20 copied bytes (indicator 0: length - 0x11 = 3) + "X"
    const short = Buffer.from([0x11, 0x18, 0x00, 0x00, 0x10, 0x61, 0x62, 0x63, 0x00, 0x30, 0x02, 0x58]);
    expect(detectCompression(short)).toBe("lz77ext");
    expect(decompress(short).toString("latin1")).toBe("abc".repeat(7) + "ab" + "X");
    // "z" + 0x111 + 1 copied bytes (indicator 1: length - 0x111 = 1), distance 1
    const total = 1 + 0x112;
    const long = Buffer.from([0x11, total & 0xff, total >> 8, 0x00, 0x40, 0x7a, 0x10, 0x00, 0x10, 0x00]);
    expect(decompress(long).equals(Buffer.alloc(total, 0x7a))).toBe(true);
  });

  it("decodes 2-byte back-references in the extended format", () => {
    // "ab" then indicator 3 => length 4, distance 2 => "ababab"
    const src = Buffer.from([0x11, 0x06, 0x00, 0x00, 0x20, 0x61, 0x62, 0x30, 0x01]);
    expect(decompress(src).toString("latin1")).toBe("ababab");
  });

  it("decodes run-length (0x30) streams", () => {
    const src = Buffer.from([0x30, 0x07, 0x00, 0x00, 0x82, 0x7a, 0x01, 0x41, 0x42]);
    expect(decompress(src).toString("latin1")).toBe("zzzzzAB");
  });

  it("reads the 8-byte header used for sizes above 24 bits", () => {
    const src = Buffer.from([0x10, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43]);
    expect(decompress(src).toString("latin1")).toBe("ABC");
  });

  it("round-trips through the greedy compressors", () => {
    const parts: number[] = [];
    let seed = 1234;
    for (let i = 0; i < 6000; i++) {
      seed = (seed * 1103515245 + 12345) >>> 0;
      parts.push(i % 700 < 350 ? (seed >>> 24) & 0x0f : i & 0xff);
    }
    const data = Buffer.concat([Buffer.from(parts), Buffer.alloc(5000, 7)]);
    for (const ext of [false, true]) {
      const c = compressLz77(data, ext);
      expect(c.length).toBeLessThan(data.length);
      expect(decompress(c).equals(data)).toBe(true);
    }
  });

  it("rejects truncated input and bad back-references", () => {
    expect(() => decompress(Buffer.from([0x10, 0x10, 0x00, 0x00, 0x00, 0x41]))).toThrow();
    expect(() => decompress(Buffer.from([0x10, 0x05, 0x00, 0x00, 0x80, 0x20, 0x05]))).toThrow();
    expect(detectCompression(Buffer.from("ARC\0"))).toBeNull();
  });
});
