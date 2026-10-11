import { describe, expect, it } from "vitest";

import { decodeWav, encodeWav } from "./wav";

describe("WAV", () => {
  it("writes a 44-byte RIFF PCM16 header and interleaved little-endian samples", () => {
    const wav = encodeWav(22050, [Int16Array.from([1, -2, 32767]), Int16Array.from([-32768, 5])]);
    expect(wav.length).toBe(44 + 3 * 2 * 2);
    expect(wav.toString("latin1", 0, 4)).toBe("RIFF");
    expect(wav.readUInt32LE(4)).toBe(wav.length - 8);
    expect(wav.toString("latin1", 8, 16)).toBe("WAVEfmt ");
    expect(wav.readUInt32LE(16)).toBe(16);
    expect(wav.readUInt16LE(20)).toBe(1); // PCM
    expect(wav.readUInt16LE(22)).toBe(2); // channels
    expect(wav.readUInt32LE(24)).toBe(22050);
    expect(wav.readUInt32LE(28)).toBe(22050 * 4); // byte rate
    expect(wav.readUInt16LE(32)).toBe(4); // block align
    expect(wav.readUInt16LE(34)).toBe(16);
    expect(wav.toString("latin1", 36, 40)).toBe("data");
    expect(wav.readUInt32LE(40)).toBe(12);
    expect([0, 1, 2, 3, 4, 5].map((i) => wav.readInt16LE(44 + i * 2))).toEqual([1, -32768, -2, 5, 32767, 0]);

    const back = decodeWav(wav);
    expect(back.sampleRate).toBe(22050);
    expect(back.channels.map((c) => Array.from(c))).toEqual([
      [1, -2, 32767],
      [-32768, 5, 0],
    ]);
  });

  it("refuses an empty channel list and non-WAV input", () => {
    expect(() => encodeWav(8000, [])).toThrow();
    expect(() => decodeWav(Buffer.from("RIFX0000WAVE"))).toThrow(/not a RIFF/);
  });
});
