// Joins MP4 files into one, without re-encoding (docs/design/highlights.md §4): a combo movie whose
// replays were recorded on different Project+ releases is rendered in pieces (a Dolphin plays one
// release's card) and joined here, so the player only ever gets one movie.
//
// For the files our render Dolphins write (ClipRecorder.h: one H.264 track and one AAC track, the
// same encoder for every piece): each track's samples are the pieces' samples one after another,
// copied byte for byte into one `mdat` after a rebuilt `moov` (one sample per chunk, 64-bit
// offsets, the two tracks interleaved by time). The first piece's edit list stays for the whole
// file; a later piece's video is shifted onto the first piece's start (its composition offsets),
// and a later piece's audio drops the encoder's leading priming samples its own edit list skipped,
// so the tracks don't drift apart at the joins. A piece whose sample description differs (another
// encoder) adds an entry of its own to `stsd`.
import type { FileHandle } from "fs/promises";
import { open } from "fs/promises";

type Box = { type: string; start: number; header: number; size: number };

/** One sample: where its bytes are, how long it lasts, its composition offset and description. */
type Sample = { file: number; offset: number; size: number; delta: number; cto: number; sync: boolean; sdi: number };

type Track = {
  handler: string;
  timescale: number;
  /** The track's boxes to copy (tkhd, mdhd, hdlr, vmhd/smhd, dinf) as they are. */
  tkhd: Buffer;
  mdhd: Buffer;
  hdlr: Buffer;
  mhd: Buffer | null;
  dinf: Buffer | null;
  /** stsd's sample entries, each as bytes. */
  entries: Buffer[];
  /** The edit list (segment duration in the movie's timescale, media time; -1 empty). */
  edits: { duration: number; mediaTime: number }[];
  samples: Sample[];
};

type Movie = { ftyp: Buffer | null; timescale: number; mvhd: Buffer; tracks: Track[] };

const u32 = (b: Buffer, o: number) => b.readUInt32BE(o);
const u64 = (b: Buffer, o: number) => Number(b.readBigUInt64BE(o));

function children(buf: Buffer, from: number, to: number): Box[] {
  const boxes: Box[] = [];
  let pos = from;
  while (pos + 8 <= to) {
    let size = u32(buf, pos);
    const type = buf.toString("latin1", pos + 4, pos + 8);
    let header = 8;
    if (size === 1) {
      size = u64(buf, pos + 8);
      header = 16;
    } else if (size === 0) {
      size = to - pos;
    }
    if (size < header || pos + size > to) {
      throw new Error(`bad MP4 box ${type} at ${pos}`);
    }
    boxes.push({ type, start: pos, header, size });
    pos += size;
  }
  return boxes;
}

const find = (buf: Buffer, boxes: Box[], type: string) => boxes.find((b) => b.type === type);
const inside = (buf: Buffer, box: Box) => children(buf, box.start + box.header, box.start + box.size);
const bytes = (buf: Buffer, box: Box | undefined) => (box ? buf.subarray(box.start, box.start + box.size) : null);
const payload = (buf: Buffer, box: Box) => box.start + box.header;

function need<T>(x: T | undefined | null, what: string): T {
  if (x == null) {
    throw new Error(`MP4 without ${what}`);
  }
  return x;
}

/** The top-level boxes of a file (read in pieces: an mdat can be large). */
async function topLevel(fh: FileHandle): Promise<Box[]> {
  const { size: fileSize } = await fh.stat();
  const boxes: Box[] = [];
  const head = Buffer.alloc(16);
  let pos = 0;
  while (pos + 8 <= fileSize) {
    await fh.read(head, 0, 16, pos);
    let size = head.readUInt32BE(0);
    const type = head.toString("latin1", 4, 8);
    let header = 8;
    if (size === 1) {
      size = Number(head.readBigUInt64BE(8));
      header = 16;
    } else if (size === 0) {
      size = fileSize - pos;
    }
    if (size < header || pos + size > fileSize) {
      throw new Error(`bad MP4 box ${type} at ${pos}`);
    }
    boxes.push({ type, start: pos, header, size });
    pos += size;
  }
  return boxes;
}

async function readBox(fh: FileHandle, box: Box): Promise<Buffer> {
  const buf = Buffer.alloc(box.size);
  await fh.read(buf, 0, box.size, box.start);
  return buf;
}

function parseTrack(buf: Buffer, trak: Box, file: number): Track {
  const tb = inside(buf, trak);
  const tkhd = need(find(buf, tb, "tkhd"), "tkhd");
  const mdia = need(find(buf, tb, "mdia"), "mdia");
  const mb = inside(buf, mdia);
  const mdhd = need(find(buf, mb, "mdhd"), "mdhd");
  const hdlr = need(find(buf, mb, "hdlr"), "hdlr");
  const minf = need(find(buf, mb, "minf"), "minf");
  const nb = inside(buf, minf);
  const stbl = need(find(buf, nb, "stbl"), "stbl");
  const sb = inside(buf, stbl);

  const mdhdAt = payload(buf, mdhd);
  const timescale = buf[mdhdAt] === 1 ? u32(buf, mdhdAt + 20) : u32(buf, mdhdAt + 12);
  const handler = buf.toString("latin1", payload(buf, hdlr) + 8, payload(buf, hdlr) + 12);

  // stsd: its sample entries.
  const stsd = need(find(buf, sb, "stsd"), "stsd");
  const entries = children(buf, payload(buf, stsd) + 8, stsd.start + stsd.size).map((b) => bytes(buf, b)!);

  // Edit list.
  const edits: Track["edits"] = [];
  const edts = find(buf, tb, "edts");
  const elst = edts ? find(buf, inside(buf, edts), "elst") : undefined;
  if (elst) {
    const at = payload(buf, elst);
    const v1 = buf[at] === 1;
    const n = u32(buf, at + 4);
    for (let i = 0, o = at + 8; i < n; i++, o += v1 ? 20 : 12) {
      edits.push(
        v1
          ? { duration: u64(buf, o), mediaTime: Number(buf.readBigInt64BE(o + 8)) }
          : { duration: u32(buf, o), mediaTime: buf.readInt32BE(o + 4) },
      );
    }
  }

  // Sizes.
  const stsz = need(find(buf, sb, "stsz"), "stsz");
  const szAt = payload(buf, stsz);
  const fixed = u32(buf, szAt + 4);
  const count = u32(buf, szAt + 8);
  const sizes = Array.from({ length: count }, (_, i) => (fixed ? fixed : u32(buf, szAt + 12 + 4 * i)));

  // Decode deltas.
  const deltas: number[] = [];
  const stts = need(find(buf, sb, "stts"), "stts");
  for (let i = 0, at = payload(buf, stts), n = u32(buf, at + 4); i < n; i++) {
    const c = u32(buf, at + 8 + 8 * i);
    const d = u32(buf, at + 12 + 8 * i);
    for (let k = 0; k < c; k++) {
      deltas.push(d);
    }
  }

  // Composition offsets.
  const ctos: number[] = [];
  const ctts = find(buf, sb, "ctts");
  if (ctts) {
    const at = payload(buf, ctts);
    const v1 = buf[at] === 1;
    for (let i = 0, n = u32(buf, at + 4); i < n; i++) {
      const c = u32(buf, at + 8 + 8 * i);
      const o = v1 ? buf.readInt32BE(at + 12 + 8 * i) : u32(buf, at + 12 + 8 * i);
      for (let k = 0; k < c; k++) {
        ctos.push(o);
      }
    }
  }

  // Sync samples (none listed: every one is).
  const stss = find(buf, sb, "stss");
  let sync: Set<number> | null = null;
  if (stss) {
    const at = payload(buf, stss);
    sync = new Set(Array.from({ length: u32(buf, at + 4) }, (_, i) => u32(buf, at + 8 + 4 * i) - 1));
  }

  // Chunk offsets and samples per chunk.
  const stco = find(buf, sb, "stco");
  const co64 = find(buf, sb, "co64");
  const chunkOffsets: number[] = [];
  if (stco) {
    const at = payload(buf, stco);
    for (let i = 0, n = u32(buf, at + 4); i < n; i++) {
      chunkOffsets.push(u32(buf, at + 8 + 4 * i));
    }
  } else {
    const at = payload(buf, need(co64, "stco"));
    for (let i = 0, n = u32(buf, at + 4); i < n; i++) {
      chunkOffsets.push(u64(buf, at + 8 + 8 * i));
    }
  }
  const stsc = need(find(buf, sb, "stsc"), "stsc");
  const runs: { first: number; per: number; sdi: number }[] = [];
  for (let i = 0, at = payload(buf, stsc), n = u32(buf, at + 4); i < n; i++) {
    runs.push({ first: u32(buf, at + 8 + 12 * i), per: u32(buf, at + 12 + 12 * i), sdi: u32(buf, at + 16 + 12 * i) });
  }

  const samples: Sample[] = [];
  let s = 0;
  for (let chunk = 1; chunk <= chunkOffsets.length && s < count; chunk++) {
    let run = runs[0];
    for (const r of runs) {
      if (r.first <= chunk) {
        run = r;
      }
    }
    let offset = chunkOffsets[chunk - 1];
    for (let k = 0; k < run.per && s < count; k++, s++) {
      samples.push({
        file,
        offset,
        size: sizes[s],
        delta: deltas[s] ?? 0,
        cto: ctos[s] ?? 0,
        sync: sync ? sync.has(s) : true,
        sdi: run.sdi,
      });
      offset += sizes[s];
    }
  }
  if (samples.length !== count) {
    throw new Error(`MP4 track's chunks hold ${samples.length} of its ${count} samples`);
  }

  return {
    handler,
    timescale,
    tkhd: bytes(buf, tkhd)!,
    mdhd: bytes(buf, mdhd)!,
    hdlr: bytes(buf, hdlr)!,
    mhd: bytes(buf, find(buf, nb, "vmhd") ?? find(buf, nb, "smhd")),
    dinf: bytes(buf, find(buf, nb, "dinf")),
    entries,
    edits,
    samples,
  };
}

async function parseMovie(fh: FileHandle, file: number): Promise<Movie> {
  const top = await topLevel(fh);
  const moovBox = need(
    top.find((b) => b.type === "moov"),
    "moov",
  );
  const moov = await readBox(fh, moovBox);
  const local = { ...moovBox, start: 0 };
  const mb = inside(moov, local);
  const mvhd = need(find(moov, mb, "mvhd"), "mvhd");
  const at = payload(moov, mvhd);
  const timescale = moov[at] === 1 ? u32(moov, at + 20) : u32(moov, at + 12);
  const ftypBox = top.find((b) => b.type === "ftyp");
  return {
    ftyp: ftypBox ? await readBox(fh, ftypBox) : null,
    timescale,
    mvhd: bytes(moov, mvhd)!,
    tracks: mb.filter((b) => b.type === "trak").map((b) => parseTrack(moov, b, file)),
  };
}

/** How long a track's edit list shows of it (the track's timescale), or null without one. */
function presentedLength(t: Track, movieTimescale: number): number | null {
  const shown = t.edits.filter((e) => e.mediaTime >= 0);
  if (!shown.length) {
    return null;
  }
  return Math.round((shown.reduce((a, e) => a + e.duration, 0) * t.timescale) / movieTimescale);
}

/** The most common sample duration: a frame's. */
function typicalDelta(samples: Sample[]): number {
  const counts = new Map<number, number>();
  samples.forEach((x) => counts.set(x.delta, (counts.get(x.delta) ?? 0) + 1));
  return [...counts.entries()].sort((a, b) => b[1] - a[1])[0]?.[0] ?? 1;
}

/** Where a track's presentation starts in its media (the first edit with media). */
const mediaStart = (t: Track) => t.edits.find((e) => e.mediaTime >= 0)?.mediaTime ?? 0;

// Writing.

function box(type: string, ...parts: Buffer[]): Buffer {
  const body = Buffer.concat(parts);
  const head = Buffer.alloc(8);
  head.writeUInt32BE(8 + body.length, 0);
  head.write(type, 4, "latin1");
  return Buffer.concat([head, body]);
}

function fullBox(type: string, version: number, ...parts: Buffer[]): Buffer {
  const vf = Buffer.alloc(4);
  vf[0] = version;
  return box(type, vf, ...parts);
}

const be32 = (...xs: number[]) => {
  const b = Buffer.alloc(4 * xs.length);
  xs.forEach((x, i) => b.writeUInt32BE(x >>> 0, 4 * i));
  return b;
};

/** A box with its duration field (mvhd, tkhd, mdhd) replaced. */
function withDuration(src: Buffer, duration: number): Buffer {
  const b = Buffer.from(src);
  const type = b.toString("latin1", 4, 8);
  const v1 = b[8] === 1;
  // After the version/flags: creation, modification, then timescale (mvhd, mdhd) or track ID and
  // a reserved word (tkhd), then the duration.
  const at = 12 + (v1 ? 16 : 8) + (type === "tkhd" ? 8 : 4);
  if (v1) {
    b.writeBigUInt64BE(BigInt(Math.round(duration)), at);
  } else {
    b.writeUInt32BE(Math.min(0xffffffff, Math.round(duration)), at);
  }
  return b;
}

/** Run-length table entries (count, value) of consecutive equal values. */
function runs(values: number[]): [number, number][] {
  const out: [number, number][] = [];
  for (const v of values) {
    const last = out[out.length - 1];
    if (last && last[1] === v) {
      last[0]++;
    } else {
      out.push([1, v]);
    }
  }
  return out;
}

type OutTrack = { track: Track; samples: Sample[]; entries: Buffer[]; edits: Track["edits"]; mediaDuration: number };

function buildStbl(t: OutTrack, offsets: number[]): Buffer {
  const s = t.samples;
  const stsd = fullBox("stsd", 0, be32(t.entries.length), ...t.entries);
  const stts = fullBox(
    "stts",
    0,
    be32(runs(s.map((x) => x.delta)).length),
    ...runs(s.map((x) => x.delta)).map(([c, d]) => be32(c, d)),
  );
  const parts = [stsd, stts];
  if (s.some((x) => x.cto !== 0)) {
    const r = runs(s.map((x) => x.cto));
    const signed = r.some(([, o]) => o < 0);
    parts.push(
      fullBox(
        "ctts",
        signed ? 1 : 0,
        be32(r.length),
        ...r.map(([c, o]) => {
          const b = Buffer.alloc(8);
          b.writeUInt32BE(c, 0);
          if (signed) {
            b.writeInt32BE(o, 4);
          } else {
            b.writeUInt32BE(o, 4);
          }
          return b;
        }),
      ),
    );
  }
  if (s.some((x) => !x.sync)) {
    const syncs = s.flatMap((x, i) => (x.sync ? [i + 1] : []));
    parts.push(fullBox("stss", 0, be32(syncs.length), be32(...syncs)));
  }
  // One sample a chunk: an entry wherever the description changes.
  const stsc: number[][] = [];
  s.forEach((x, i) => {
    if (i === 0 || s[i - 1].sdi !== x.sdi) {
      stsc.push([i + 1, 1, x.sdi]);
    }
  });
  parts.push(fullBox("stsc", 0, be32(stsc.length), ...stsc.map((e) => be32(...e))));
  parts.push(fullBox("stsz", 0, be32(0, s.length), be32(...s.map((x) => x.size))));
  const co = Buffer.alloc(8 * offsets.length);
  offsets.forEach((o, i) => co.writeBigUInt64BE(BigInt(o), 8 * i));
  parts.push(fullBox("co64", 0, be32(offsets.length), co));
  return box("stbl", ...parts);
}

function buildMoov(movie: Movie, tracks: OutTrack[], offsets: number[][]): Buffer {
  const toMovie = (t: OutTrack, d: number) => (d * movie.timescale) / t.track.timescale;
  const traks = tracks.map((t, i) => {
    // (Video's samples already last exactly what is shown: see concatMp4. Audio's first piece
    // starts after its priming.)
    const presentation = t.track.handler === "soun" ? t.mediaDuration - mediaStart(t.track) : t.mediaDuration;
    const edits = t.edits.map((e, k) =>
      // The last edit with media covers the whole joined track.
      k === t.edits.length - 1 && e.mediaTime >= 0 ? { ...e, duration: toMovie(t, presentation) } : e,
    );
    const total = edits.reduce((a, e) => a + e.duration, 0) || toMovie(t, t.mediaDuration);
    const elst =
      edits.length > 0
        ? box(
            "edts",
            fullBox(
              "elst",
              1,
              be32(edits.length),
              ...edits.map((e) => {
                const b = Buffer.alloc(20);
                b.writeBigUInt64BE(BigInt(Math.round(e.duration)), 0);
                b.writeBigInt64BE(BigInt(e.mediaTime), 8);
                b.writeUInt16BE(1, 16);
                return b;
              }),
            ),
          )
        : Buffer.alloc(0);
    const minf = box("minf", ...[t.track.mhd, t.track.dinf].filter((x): x is Buffer => !!x), buildStbl(t, offsets[i]));
    const mdia = box("mdia", withDuration(t.track.mdhd, t.mediaDuration), t.track.hdlr, minf);
    return { trak: box("trak", withDuration(t.track.tkhd, total), elst, mdia), total };
  });
  const duration = Math.max(...traks.map((t) => t.total));
  return box("moov", withDuration(movie.mvhd, duration), ...traks.map((t) => t.trak));
}

/**
 * Writes `parts` (MP4 files of the same tracks, in order) to `output` as one MP4. Throws when the
 * pieces don't have the same tracks.
 */
export async function concatMp4(parts: string[], output: string): Promise<void> {
  const handles = await Promise.all(parts.map((p) => open(p, "r")));
  try {
    const movies = await Promise.all(handles.map((fh, i) => parseMovie(fh, i)));
    const first = movies[0];
    const tracks: OutTrack[] = first.tracks.map((t) => ({
      track: t,
      samples: [],
      entries: [...t.entries],
      edits: t.edits,
      mediaDuration: 0,
    }));
    // Each video track's first frame shown in the first piece (its own decode time).
    const videoStart = new Map<number, number>();
    movies.forEach((movie, p) => {
      if (movie.tracks.length !== tracks.length) {
        throw new Error(`${parts[p]} has ${movie.tracks.length} tracks, the first piece ${tracks.length}`);
      }
      movie.tracks.forEach((t, i) => {
        const out = tracks[i];
        if (t.handler !== out.track.handler || t.timescale !== out.track.timescale) {
          throw new Error(`${parts[p]}'s ${t.handler} track doesn't match the first piece's`);
        }
        // Its sample descriptions, as entries of the joined track's stsd.
        const sdiMap = t.entries.map((entry) => {
          const k = out.entries.findIndex((e) => e.equals(entry));
          if (k >= 0) {
            return k + 1;
          }
          out.entries.push(entry);
          return out.entries.length;
        });
        let samples = t.samples.map((s) => ({ ...s, sdi: sdiMap[s.sdi - 1] ?? 1 }));
        if (t.handler === "soun" && samples.length) {
          // The encoder's priming (before) and padding (after) that the piece's edit list hides:
          // a later piece's priming is dropped (whole samples), and every piece ends where its
          // edit list does (the last sample shortened), so audio doesn't drift at the joins.
          const start = mediaStart(t);
          let head = 0;
          if (p > 0) {
            while (samples.length > 1 && head + samples[0].delta <= start) {
              head += samples[0].delta;
              samples = samples.slice(1);
            }
          }
          const shown = presentedLength(t, movie.timescale);
          if (shown != null) {
            const target = (p === 0 ? start : start - head) + shown;
            let total = samples.reduce((a, x) => a + x.delta, 0);
            while (samples.length > 1 && total - samples[samples.length - 1].delta >= target) {
              total -= samples[samples.length - 1].delta;
              samples = samples.slice(0, -1);
            }
            const last = samples[samples.length - 1];
            samples[samples.length - 1] = { ...last, delta: Math.max(1, last.delta - (total - target)) };
          }
        }
        if (t.handler !== "soun" && samples.length) {
          // Video (B-frames: shown in another order than decoded): the piece lasts from its first
          // frame shown to its last one's end, whatever its last decode duration says (FFmpeg's
          // and Apple's end short of it), and a later piece is shown from where the first
          // piece's first frame is (the joined track's edit list keeps the first piece's start).
          let dts = 0;
          let first = Infinity;
          let last = -Infinity;
          for (const x of samples) {
            first = Math.min(first, dts + x.cto);
            last = Math.max(last, dts + x.cto);
            dts += x.delta;
          }
          const frame = typicalDelta(samples);
          const length = last + frame - first;
          const others = dts - samples[samples.length - 1].delta;
          samples[samples.length - 1] = { ...samples[samples.length - 1], delta: Math.max(1, length - others) };
          if (p === 0) {
            videoStart.set(i, first);
          } else {
            const shift = (videoStart.get(i) ?? first) - first;
            samples = samples.map((x) => ({ ...x, cto: x.cto + shift }));
          }
        }
        out.samples.push(...samples);
        out.mediaDuration += samples.reduce((a, s) => a + s.delta, 0);
      });
    });

    // The mdat's order: both tracks by decode time.
    const order: { track: number; index: number; time: number }[] = [];
    tracks.forEach((t, ti) => {
      let time = 0;
      t.samples.forEach((s, index) => {
        order.push({ track: ti, index, time: time / t.track.timescale });
        time += s.delta;
      });
    });
    order.sort((a, b) => a.time - b.time || a.track - b.track);

    const ftyp =
      first.ftyp ?? box("ftyp", Buffer.from("isom", "latin1"), be32(512), Buffer.from("isomiso2avc1mp41", "latin1"));
    const placeholder = tracks.map((t) => t.samples.map(() => 0));
    const moovSize = buildMoov(first, tracks, placeholder).length;
    const dataSize = order.reduce((a, o) => a + tracks[o.track].samples[o.index].size, 0);
    const mdatHeader = Buffer.alloc(16);
    mdatHeader.writeUInt32BE(1, 0);
    mdatHeader.write("mdat", 4, "latin1");
    mdatHeader.writeBigUInt64BE(BigInt(16 + dataSize), 8);
    let at = ftyp.length + moovSize + 16;
    const offsets = tracks.map((t) => t.samples.map(() => 0));
    for (const o of order) {
      offsets[o.track][o.index] = at;
      at += tracks[o.track].samples[o.index].size;
    }
    const moov = buildMoov(first, tracks, offsets);
    if (moov.length !== moovSize) {
      throw new Error("MP4 index changed size");
    }

    const out = await open(output, "w");
    try {
      let pos = 0;
      const write = async (b: Buffer) => {
        await out.write(b, 0, b.length, pos);
        pos += b.length;
      };
      await write(ftyp);
      await write(moov);
      await write(mdatHeader);
      const buf = Buffer.alloc(4 << 20);
      for (const o of order) {
        const s = tracks[o.track].samples[o.index];
        const b = s.size <= buf.length ? buf.subarray(0, s.size) : Buffer.alloc(s.size);
        const { bytesRead } = await handles[s.file].read(b, 0, s.size, s.offset);
        if (bytesRead !== s.size) {
          throw new Error(`${parts[s.file]} ends inside a sample`);
        }
        await write(b);
      }
    } finally {
      await out.close();
    }
  } finally {
    await Promise.all(handles.map((h) => h.close()));
  }
}

/** A file's tracks, as the joiner reads them (tests, diagnostics). */
export async function describeMp4(
  file: string,
): Promise<
  { handler: string; timescale: number; samples: number; duration: number; ctos: number[]; edits: Track["edits"] }[]
> {
  const fh = await open(file, "r");
  try {
    const movie = await parseMovie(fh, 0);
    return movie.tracks.map((t) => ({
      handler: t.handler,
      timescale: t.timescale,
      samples: t.samples.length,
      duration: t.samples.reduce((a, s) => a + s.delta, 0),
      ctos: t.samples.map((s) => s.cto),
      edits: t.edits,
    }));
  } finally {
    await fh.close();
  }
}
