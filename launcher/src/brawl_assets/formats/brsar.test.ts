import { describe, expect, it } from "vitest";

import { buildBrsar, fileGroups, findSound, parseBrsar, readBrsarFile, SoundType } from "./brsar";
import { BYTES_PER_FRAME } from "./dsp_adpcm";
import { buildRwsd, decodeRwsdSound, parseRwsd, WaveEncoding } from "./rwsd";
import { buildSawnd, parseSawnd, sawndFile, sawndPath } from "./sawnd";

// Synthetic sound data only: hand-made ADPCM frames and PCM16 ramps, no game audio.

/** Zero-predictor ADPCM frames whose decoded samples are `nibble << scale`. */
function adpcmFrames(nibbleRows: number[][], scaleExp = 4): Uint8Array {
  const out = new Uint8Array(nibbleRows.length * BYTES_PER_FRAME);
  nibbleRows.forEach((row, f) => {
    out[f * 8] = scaleExp;
    for (let i = 0; i < 14; i += 2) {
      out[f * 8 + 1 + i / 2] = ((row[i] & 0xf) << 4) | (row[i + 1] & 0xf);
    }
  });
  return out;
}

function pcm16BE(samples: number[]): Uint8Array {
  const b = Buffer.alloc(samples.length * 2);
  samples.forEach((s, i) => b.writeInt16BE(s, i * 2));
  return b;
}

const ROW_A = [1, 2, 3, 4, 5, 6, 7, -1, -2, -3, -4, -5, -6, -7];
const ROW_B = [7, 7, 7, 7, 7, 7, 7, -8, -8, -8, -8, -8, -8, -8];
const COEFS = new Array(16).fill(0);

function makeRwsd(rowsForAdpcm: number[][]) {
  return buildRwsd(
    [
      { waveIndex: 1 }, // entry 0: the stereo PCM16 wave
      { waveIndex: 0, pitch: 2 }, // entry 1: the ADPCM wave, an octave up
    ],
    [
      {
        encoding: WaveEncoding.ADPCM,
        sampleRate: 22050,
        sampleCount: rowsForAdpcm.length * 14 - 3,
        channels: [{ data: adpcmFrames(rowsForAdpcm), coefs: COEFS }],
      },
      {
        encoding: WaveEncoding.PCM16,
        sampleRate: 32000,
        sampleCount: 4,
        channels: [{ data: pcm16BE([0, 100, 200, 300]) }, { data: pcm16BE([-1, -2, -3, -4]) }],
      },
    ],
  );
}

describe("BRSAR / RWSD", () => {
  const fox = makeRwsd([ROW_A, ROW_B]);
  const other = buildRwsd(
    [{ waveIndex: 0 }],
    [
      {
        encoding: WaveEncoding.PCM8,
        sampleRate: 8000,
        sampleCount: 3,
        channels: [{ data: Uint8Array.from([1, 2, 3]) }],
      },
    ],
  );
  const archive = buildBrsar(
    [
      { name: "snd_se_common_test", fileId: 1, index: 0 },
      { name: "snd_se_fox_pcm", fileId: 0, index: 0 },
      { name: "snd_se_fox_adpcm", fileId: 0, index: 1 },
      { name: "snd_seq_test", fileId: 1, index: 0, soundType: SoundType.Sequence },
    ],
    [
      { name: "snd_group_common", files: [{ fileId: 1, ...other }] },
      { name: "snd_group_fox", files: [{ fileId: 0, ...fox }] },
    ],
  );

  it("reads names, sounds, files and groups", () => {
    const b = parseBrsar(archive);
    expect(b.sounds.map((s) => s.name)).toEqual([
      "snd_se_common_test",
      "snd_se_fox_pcm",
      "snd_se_fox_adpcm",
      "snd_seq_test",
    ]);
    expect(findSound(b, "snd_se_fox_adpcm")).toMatchObject({ id: 2, fileId: 0, index: 1, soundType: SoundType.Wave });
    expect(findSound(b, 3)?.soundType).toBe(SoundType.Sequence);
    expect(findSound(b, "nope")).toBeNull();
    expect(b.groups.map((g) => g.name)).toEqual(["snd_group_common", "snd_group_fox"]);
    expect(b.files[0].positions).toEqual([{ groupId: 1, index: 0 }]);
    expect(fileGroups(b, 0).map((g) => g.name)).toEqual(["snd_group_fox"]);
    expect(b.groups[1].items[0]).toMatchObject({ fileId: 0, headerOffset: 0, dataOffset: 0 });
  });

  it("finds a file's header and wave data and decodes its entries", () => {
    const b = parseBrsar(archive);
    const data = readBrsarFile(archive, b, 0)!;
    expect(data.groupId).toBe(1);
    expect(data.header.subarray(0, fox.header.length)).toEqual(fox.header);
    const rwsd = parseRwsd(data.header);
    expect(rwsd.version).toBe(0x0102);
    expect(rwsd.entries).toHaveLength(2);
    expect(rwsd.waves.map((w) => [w.encoding, w.sampleRate, w.sampleCount, w.channels.length])).toEqual([
      [WaveEncoding.ADPCM, 22050, 25, 1],
      [WaveEncoding.PCM16, 32000, 4, 2],
    ]);

    const pcm = decodeRwsdSound(rwsd, data.waveData, 0);
    expect(pcm.sampleRate).toBe(32000);
    expect(pcm.channels.map((c) => Array.from(c))).toEqual([
      [0, 100, 200, 300],
      [-1, -2, -3, -4],
    ]);

    const adpcm = decodeRwsdSound(rwsd, data.waveData, 1);
    expect(adpcm.sampleRate).toBe(44100); // the entry's pitch of 2
    expect(Array.from(adpcm.channels[0])).toEqual([...ROW_A, ...ROW_B].slice(0, 25).map((n) => n * 16));

    const pcm8 = decodeRwsdSound(
      parseRwsd(readBrsarFile(archive, b, 1)!.header),
      readBrsarFile(archive, b, 1)!.waveData,
      0,
    );
    expect(Array.from(pcm8.channels[0])).toEqual([256, 512, 768]);
    expect(() => decodeRwsdSound(rwsd, data.waveData, 5)).toThrow(/no entry 5/);
  });

  it("reads a group replacement (.sawnd), with padded headers", () => {
    const b = parseBrsar(archive);
    expect(sawndPath(1)).toBe("sfx/001.sawnd");
    expect(sawndPath(0xd5)).toBe("sfx/0D5.sawnd");
    const replaced = makeRwsd([ROW_B, ROW_A]);
    const sawnd = buildSawnd(1, [{ fileId: 0, header: replaced.header, waveData: replaced.waveData }]);
    expect(sawnd.readUInt32BE(1)).toBe(8); // group index + 7
    const parsed = parseSawnd(sawnd, b.groups[1]);
    const data = sawndFile(parsed, 1, 0)!;
    const adpcm = decodeRwsdSound(parseRwsd(data.header), data.waveData, 1);
    expect(Array.from(adpcm.channels[0])).toEqual([...ROW_B, ...ROW_A].slice(0, 25).map((n) => n * 16));

    // Two files, the first header padded past its NW4R length.
    const group = {
      id: 0,
      items: [{ fileId: 4 }, { fileId: 9 }] as { fileId: number }[],
    } as Parameters<typeof parseSawnd>[1];
    const padded = Buffer.concat([replaced.header, Buffer.alloc(32)]);
    const two = parseSawnd(
      buildSawnd(0, [
        { fileId: 4, header: padded, waveData: replaced.waveData },
        { fileId: 9, header: other.header, waveData: other.waveData },
      ]),
      group,
    );
    expect(two.files.map((f) => f.header.length)).toEqual([replaced.header.length, other.header.length]);
    expect(two.files[1].waveData).toEqual(other.waveData);
    expect(() =>
      parseSawnd(
        buildSawnd(0, [
          { fileId: 5, ...other },
          { fileId: 9, ...other },
        ]),
        group,
      ),
    ).toThrow(/file 0 is 5/);
  });

  it("rejects what it cannot read", () => {
    expect(() => parseBrsar(Buffer.from("not a sound archive at all, really not one"))).toThrow(/not an RSAR/);
    const v13 = Buffer.from(fox.header);
    v13.writeUInt16BE(0x0103, 6);
    expect(() => parseRwsd(v13)).toThrow(/not supported/);
  });
});
