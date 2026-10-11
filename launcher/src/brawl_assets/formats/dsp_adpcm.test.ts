import { describe, expect, it } from "vitest";

import {
  adpcmByteLength,
  decodeDspAdpcm,
  decodePcm8,
  decodePcm16BE,
  nibblesToSamples,
  samplesToNibbles,
} from "./dsp_adpcm";

/** One 8-byte frame: header (predictor index, scale exponent) and 14 signed nibbles. */
function frame(index: number, scaleExp: number, nibbles: number[]): number[] {
  expect(nibbles).toHaveLength(14);
  const out = [(index << 4) | scaleExp];
  for (let i = 0; i < 14; i += 2) {
    out.push(((nibbles[i] & 0xf) << 4) | (nibbles[i + 1] & 0xf));
  }
  return out;
}

// Coefficient pairs (4.11 fixed point): 0 = silence predictor, 1 = repeat last sample, 2 = linear extrapolation,
// 3 = half the last sample.
const COEFS = [0, 0, 2048, 0, 4096, -2048, 1024, 0, 0, 0, 0, 0, 0, 0, 0, 0];

describe("DSP-ADPCM", () => {
  it("converts nibble counts (16 per frame, 2 of them header) to samples and back", () => {
    expect(nibblesToSamples(16)).toBe(14);
    expect(nibblesToSamples(18)).toBe(14);
    expect(nibblesToSamples(19)).toBe(15);
    expect(nibblesToSamples(32)).toBe(28);
    expect(samplesToNibbles(14)).toBe(16);
    expect(samplesToNibbles(15)).toBe(19);
    for (const n of [1, 13, 14, 15, 27, 28, 1000]) {
      expect(nibblesToSamples(samplesToNibbles(n))).toBe(n);
    }
    expect(adpcmByteLength(15)).toBe(16);
  });

  it("decodes the scaled nibbles with the zero predictor", () => {
    const nibbles = [1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -1, 0];
    const out = decodeDspAdpcm(Uint8Array.from(frame(0, 2, nibbles)), 14, { coefs: COEFS });
    expect(Array.from(out)).toEqual(nibbles.map((n) => n * 4));
  });

  it("predicts from the history, across frames and from the initial history", () => {
    const data = Uint8Array.from([
      // repeat-last predictor, scale 1: a running sum of the nibbles starting from hist1 = 100
      ...frame(1, 0, [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]),
      // linear extrapolation with zero residual continues the slope (+1 per sample)
      ...frame(2, 0, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ]);
    const out = Array.from(decodeDspAdpcm(data, 28, { coefs: COEFS, hist1: 100, hist2: 0 }));
    expect(out.slice(0, 14)).toEqual(Array.from({ length: 14 }, (_, i) => 101 + i));
    expect(out.slice(14)).toEqual(Array.from({ length: 14 }, (_, i) => 115 + i));
  });

  it("rounds like the hardware (arithmetic shift, +1024) and clamps to 16 bits", () => {
    // Half the last sample: (-3 * 1024 + 1024) >> 11 = -1; (-2 * 1024 + 1024) >> 11 = -1 (floor, not truncation
    // towards 0); then (-1 * 1024 + 1024) >> 11 = 0.
    const silent = Uint8Array.from(frame(3, 0, new Array(14).fill(0)));
    expect(Array.from(decodeDspAdpcm(silent, 1, { coefs: COEFS, hist1: -3 }))).toEqual([-1]);
    expect(Array.from(decodeDspAdpcm(silent, 3, { coefs: COEFS, hist1: -2 }))).toEqual([-1, 0, 0]);
    const loud = decodeDspAdpcm(Uint8Array.from(frame(0, 13, [7, -8, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])), 3, {
      coefs: COEFS,
    });
    expect(Array.from(loud)).toEqual([32767, -32768, 8192]);
  });

  it("stops at the end of the data and at the requested count", () => {
    const data = Uint8Array.from([...frame(0, 0, new Array(14).fill(1)), ...frame(0, 0, new Array(14).fill(2))]);
    expect(decodeDspAdpcm(data, 15, { coefs: COEFS })).toHaveLength(15);
    expect(decodeDspAdpcm(data.subarray(0, 12), 28, { coefs: COEFS })).toHaveLength(14);
  });

  it("decodes PCM8 and big-endian PCM16", () => {
    expect(Array.from(decodePcm8(Uint8Array.from([0x00, 0x7f, 0x80, 0xff]), 4))).toEqual([0, 32512, -32768, -256]);
    expect(Array.from(decodePcm16BE(Uint8Array.from([0x12, 0x34, 0xff, 0xfe, 0x80, 0x00]), 3))).toEqual([
      0x1234, -2, -32768,
    ]);
  });
});
