import { mkdtemp, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { concatMp4, describeMp4 } from "./mp4_concat";

// A small MP4 as our render Dolphins write them: a video track (B-frames: composition offsets, an
// edit list starting after the encoder's delay) and an AAC track (an edit list skipping the
// encoder's priming and hiding its padding). Sample bytes are their index, so a join's order shows.

const be32 = (...xs: number[]) => {
  const b = Buffer.alloc(4 * xs.length);
  xs.forEach((x, i) => b.writeInt32BE(x | 0, 4 * i));
  return b;
};
const box = (type: string, ...parts: Buffer[]) => {
  const body = Buffer.concat(parts);
  return Buffer.concat([be32(8 + body.length), Buffer.from(type, "latin1"), body]);
};
const full = (type: string, ...parts: Buffer[]) => box(type, be32(0), ...parts);

type TrackSpec = {
  handler: "vide" | "soun";
  timescale: number;
  deltas: number[];
  ctos?: number[];
  /** Edit: shown duration (movie timescale) and media start. */
  edit?: [number, number];
  entry: string;
  /** A byte per sample: what it holds. */
  bytes: number[];
};

function mp4(tracks: TrackSpec[], movieTimescale = 1000): Buffer {
  const ftyp = box("ftyp", Buffer.from("isom", "latin1"), be32(512), Buffer.from("isomavc1", "latin1"));
  const build = (offsets: number[][]) =>
    box(
      "moov",
      full("mvhd", be32(0, 0, movieTimescale, 1000), Buffer.alloc(80)),
      ...tracks.map((t, ti) =>
        box(
          "trak",
          full("tkhd", be32(0, 0, ti + 1, 0, 1000), Buffer.alloc(60)),
          ...(t.edit ? [box("edts", full("elst", be32(1, t.edit[0], t.edit[1]), Buffer.from([0, 1, 0, 0])))] : []),
          box(
            "mdia",
            full(
              "mdhd",
              be32(
                0,
                0,
                t.timescale,
                t.deltas.reduce((a, d) => a + d, 0),
              ),
              be32(0),
            ),
            full("hdlr", be32(0), Buffer.from(t.handler, "latin1"), Buffer.alloc(13)),
            box(
              "minf",
              box(
                "stbl",
                full("stsd", be32(1), box(t.entry, Buffer.alloc(8))),
                full("stts", be32(t.deltas.length), ...t.deltas.map((d) => be32(1, d))),
                ...(t.ctos ? [full("ctts", be32(t.ctos.length), ...t.ctos.map((c) => be32(1, c)))] : []),
                full("stsc", be32(1, 1, 1, 1)),
                full("stsz", be32(0, t.bytes.length), ...t.bytes.map(() => be32(1))),
                full("stco", be32(offsets[ti].length), ...offsets[ti].map((o) => be32(o))),
              ),
            ),
          ),
        ),
      ),
    );
  // mdat first (as Media Foundation writes them): its offsets don't depend on the moov.
  let at = ftyp.length + 8;
  const offsets = tracks.map((t) => t.bytes.map(() => at++));
  const mdat = box("mdat", Buffer.from(tracks.flatMap((t) => t.bytes)));
  return Buffer.concat([ftyp, mdat, build(offsets)]);
}

/** A piece: 4 video frames (delay 2 ticks, the last decode duration short as FFmpeg writes it) and 3 AAC frames. */
const piece = (tag: number, videoEntry = "avc1") =>
  mp4([
    {
      handler: "vide",
      timescale: 60,
      deltas: [1, 1, 1, 0],
      ctos: [2, 4, 2, 2],
      edit: [4 * (1000 / 60), 2],
      entry: videoEntry,
      bytes: [tag, tag + 1, tag + 2, tag + 3],
    },
    {
      handler: "soun",
      timescale: 48000,
      deltas: [1024, 1024, 1024],
      // Priming 1024 (the first frame), shown 1.5 frames: the rest is padding.
      edit: [32, 1024],
      entry: "mp4a",
      bytes: [tag + 10, tag + 11, tag + 12],
    },
  ]);

describe("concatMp4", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "mp4-concat-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("joins pieces into one movie: video back to back, audio without the later pieces' priming", async () => {
    const files = [path.join(dir, "a.mp4"), path.join(dir, "b.mp4")];
    await writeFile(files[0], piece(0));
    await writeFile(files[1], piece(100));
    const out = path.join(dir, "out.mp4");
    await concatMp4(files, out);

    const [video, audio] = await describeMp4(out);
    // Video: every frame; each piece lasts its 4 frames shown (its last decode duration made up).
    expect(video).toMatchObject({ handler: "vide", samples: 8, duration: 8 });
    expect(video.edits).toEqual([{ duration: Math.round(8 * (1000 / 60)), mediaTime: 2 }]);
    // Audio: the first piece's priming stays (the edit list skips it), the second's is dropped,
    // and each piece ends where its edit list did (1.5 frames shown: its last frame shortened).
    expect(audio).toMatchObject({ handler: "soun", samples: 3 + 2, duration: 1024 + 1536 + 1536 });
    expect(audio.edits).toEqual([{ duration: 64, mediaTime: 1024 }]);

    // Samples are copied in decode order, both tracks interleaved by time.
    const data = await readFile(out);
    const mdat = data.indexOf("mdat", 0, "latin1");
    const bytes = [...data.subarray(mdat + 12)];
    expect(bytes.filter((b) => b < 10 || (b >= 100 && b < 110))).toEqual([0, 1, 2, 3, 100, 101, 102, 103]);
    expect(bytes.filter((b) => (b >= 10 && b < 100) || b >= 110).sort((a, b) => a - b)).toEqual([10, 11, 12, 111, 112]);
  });

  it("gives a piece of another encoder its own sample description", async () => {
    const files = [path.join(dir, "a.mp4"), path.join(dir, "b.mp4")];
    await writeFile(files[0], piece(0));
    await writeFile(files[1], piece(100, "avc3"));
    const out = path.join(dir, "out.mp4");
    await concatMp4(files, out);
    const data = await readFile(out);
    expect(data.indexOf("avc1", 0, "latin1")).toBeGreaterThan(0);
    expect(data.indexOf("avc3", 0, "latin1")).toBeGreaterThan(0);
    const [video] = await describeMp4(out);
    expect(video.samples).toBe(8);
  });

  it("refuses pieces with other tracks", async () => {
    const files = [path.join(dir, "a.mp4"), path.join(dir, "b.mp4")];
    await writeFile(files[0], piece(0));
    await writeFile(files[1], mp4([{ handler: "vide", timescale: 60, deltas: [1], entry: "avc1", bytes: [1] }]));
    await expect(concatMp4(files, path.join(dir, "out.mp4"))).rejects.toThrow(/tracks/);
  });
});
