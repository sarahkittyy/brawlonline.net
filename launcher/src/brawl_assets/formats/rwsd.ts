// NW4R wave sound data (RWSD, magic "RWSD"), the file a BRSAR's wave sounds point into.
//
//   header (0x20): common NW4R header, DATA offset/length, WAVE offset/length
//   DATA: a reference list of sound entries; each has a WSD info block (pitch, pan, ...), a track table (note
//         events) and a note table (wave index, key, volume, pitch)
//   WAVE: offsets to wave infos (encoding, channels, sample rate, loop, sample count, data location, per-channel
//         data offset and DSP-ADPCM coefficients)
// The samples themselves are not in the RWSD header: they are in the file's wave data area of its BRSAR group, at
// `dataLocation + channel.dataOffset` (format versions up to 1.2, which is what Brawl uses; 1.3 moved them into an
// RWAR and is not supported here).
//
// Layout ported from BrawlLib (BrawlCrate, GPL-3.0): BrawlLib/SSBB/Types/Audio/RWSD.cs, RWAV.cs (WaveInfo,
// ChannelInfo, ADPCMInfo), BrawlLib/SSBB/ResourceNodes/RSAR/File Types/RWSD/*. https://github.com/soopercool101/BrawlCrate
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { decodeDspAdpcm, decodePcm8, decodePcm16BE, nibblesToSamples, samplesToNibbles } from "./dsp_adpcm";

export const enum WaveEncoding {
  PCM8 = 0,
  PCM16 = 1,
  ADPCM = 2,
}

export const WAVE_ENCODING_NAMES: Record<number, string> = { 0: "PCM8", 1: "PCM16", 2: "ADPCM" };

export type RwsdNoteEvent = { position: number; length: number; noteIndex: number };

export type RwsdNote = { waveIndex: number; originalKey: number; volume: number; pan: number; pitch: number };

export type RwsdEntry = {
  pitch: number;
  pan: number;
  tracks: RwsdNoteEvent[][];
  notes: RwsdNote[];
};

export type RwsdChannel = {
  /** Offset of the channel's samples from the wave's data location. */
  dataOffset: number;
  coefs: number[] | null;
  hist1: number;
  hist2: number;
};

export type RwsdWave = {
  encoding: number;
  looped: boolean;
  sampleRate: number;
  /** In samples. */
  loopStart: number;
  /** In samples (the loop end for looped waves). */
  sampleCount: number;
  /** Offset of the wave's samples in the file's wave data. */
  dataLocation: number;
  channels: RwsdChannel[];
};

export type Rwsd = { version: number; entries: RwsdEntry[]; waves: RwsdWave[] };

export function isRwsd(buf: Uint8Array): boolean {
  return buf.length >= 0x20 && buf[0] === 0x52 && buf[1] === 0x57 && buf[2] === 0x53 && buf[3] === 0x44;
}

function check(cond: boolean, msg: string): asserts cond {
  if (!cond) {
    throw new Error(`RWSD: ${msg}`);
  }
}

export function parseRwsd(buf: Buffer): Rwsd {
  check(isRwsd(buf), "not an RWSD file");
  check(buf.readUInt16BE(4) === 0xfeff, "not big-endian");
  const version = buf.readUInt16BE(6);
  check((version & 0xff) < 3, `version ${version.toString(16)} (RWAR-based) is not supported`);
  const dataOffset = buf.readUInt32BE(0x10);
  const waveOffset = buf.readUInt32BE(0x18);
  check(buf.toString("latin1", dataOffset, dataOffset + 4) === "DATA", "missing DATA block");

  // DATA: references are 8-byte "ruint"s relative to the DATA block + 8.
  const base = dataOffset + 8;
  const refAt = (off: number) => base + buf.readUInt32BE(off + 4);
  const list = (addr: number) => {
    const n = buf.readUInt32BE(addr);
    check(addr + 4 + n * 8 <= buf.length, "DATA list out of range");
    return Array.from({ length: n }, (_, i) => refAt(addr + 4 + i * 8));
  };
  const entries: RwsdEntry[] = list(base).map((e) => {
    const wsd = refAt(e);
    // Track table -> track (a reference to its note event list) -> note events.
    const tracks = list(refAt(e + 8)).map((t) =>
      list(refAt(t)).map((ev) => ({
        position: buf.readFloatBE(ev),
        length: buf.readFloatBE(ev + 4),
        noteIndex: buf.readUInt32BE(ev + 8),
      })),
    );
    const notes = list(refAt(e + 16)).map((n) => ({
      waveIndex: buf.readInt32BE(n),
      originalKey: buf[n + 0x0c],
      volume: buf[n + 0x0d],
      pan: buf[n + 0x0e],
      pitch: buf.readFloatBE(n + 0x10),
    }));
    return { pitch: buf.readFloatBE(wsd), pan: buf[wsd + 4], tracks, notes };
  });

  // WAVE: u32 count, then offsets relative to the WAVE block.
  const waves: RwsdWave[] = [];
  if (waveOffset > 0 && buf.toString("latin1", waveOffset, waveOffset + 4) === "WAVE") {
    const count = buf.readUInt32BE(waveOffset + 8);
    for (let i = 0; i < count; i++) {
      waves.push(parseWaveInfo(buf, waveOffset + buf.readUInt32BE(waveOffset + 12 + i * 4)));
    }
  }
  return { version, entries, waves };
}

function parseWaveInfo(buf: Buffer, w: number): RwsdWave {
  check(w + 0x1c <= buf.length, "wave info out of range");
  const encoding = buf[w];
  const channelCount = buf[w + 2];
  const sampleRate = (buf[w + 3] << 16) | buf.readUInt16BE(w + 4);
  const toSamples = (v: number) => (encoding === WaveEncoding.ADPCM ? nibblesToSamples(v) : v);
  const table = w + buf.readUInt32BE(w + 0x10);
  const channels: RwsdChannel[] = [];
  for (let c = 0; c < channelCount; c++) {
    const ci = w + buf.readUInt32BE(table + c * 4);
    const adpcmOff = buf.readUInt32BE(ci + 4);
    let coefs: number[] | null = null;
    let hist1 = 0;
    let hist2 = 0;
    if (encoding === WaveEncoding.ADPCM && adpcmOff) {
      const a = w + adpcmOff;
      coefs = Array.from({ length: 16 }, (_, k) => buf.readInt16BE(a + k * 2));
      hist1 = buf.readInt16BE(a + 0x24);
      hist2 = buf.readInt16BE(a + 0x26);
    }
    channels.push({ dataOffset: buf.readUInt32BE(ci), coefs, hist1, hist2 });
  }
  return {
    encoding,
    looped: buf[w + 1] !== 0,
    sampleRate,
    loopStart: toSamples(buf.readUInt32BE(w + 8)),
    sampleCount: toSamples(buf.readUInt32BE(w + 0x0c)),
    dataLocation: buf.readUInt32BE(w + 0x14),
    channels,
  };
}

/** Decodes one wave of an RWSD to 16-bit PCM, one array per channel. */
export function decodeRwsdWave(wave: RwsdWave, waveData: Uint8Array): Int16Array[] {
  return wave.channels.map((ch) => {
    const start = wave.dataLocation + ch.dataOffset;
    if (start > waveData.length) {
      throw new Error(`RWSD: wave data at ${start} is out of range (${waveData.length} bytes)`);
    }
    const data = waveData.subarray(start);
    switch (wave.encoding) {
      case WaveEncoding.ADPCM:
        if (!ch.coefs) {
          throw new Error("RWSD: ADPCM wave without coefficients");
        }
        return decodeDspAdpcm(data, wave.sampleCount, { coefs: ch.coefs, hist1: ch.hist1, hist2: ch.hist2 });
      case WaveEncoding.PCM16:
        return decodePcm16BE(data, wave.sampleCount);
      case WaveEncoding.PCM8:
        return decodePcm8(data, wave.sampleCount);
      default:
        throw new Error(`RWSD: unknown wave encoding ${wave.encoding}`);
    }
  });
}

export type DecodedSound = {
  /** Playback rate: the wave's sample rate times the entry's and note's pitch. */
  sampleRate: number;
  channels: Int16Array[];
  waveIndex: number;
  wave: RwsdWave;
  /** Note events in the entry (1 for nearly every sound effect; only the first is rendered). */
  noteEvents: number;
};

/** Decodes the wave a sound entry plays (its first note event, as the game plays a one-note wave sound). */
export function decodeRwsdSound(rwsd: Rwsd, waveData: Uint8Array, entryIndex: number): DecodedSound {
  const entry = rwsd.entries[entryIndex];
  if (!entry) {
    throw new Error(`RWSD: no entry ${entryIndex} (${rwsd.entries.length} entries)`);
  }
  const events = entry.tracks.flat();
  const note = entry.notes[events[0]?.noteIndex ?? 0];
  if (!note) {
    throw new Error(`RWSD: entry ${entryIndex} has no note`);
  }
  const wave = rwsd.waves[note.waveIndex];
  if (!wave) {
    throw new Error(`RWSD: entry ${entryIndex} plays missing wave ${note.waveIndex}`);
  }
  const pitch = (entry.pitch || 1) * (note.pitch || 1);
  return {
    sampleRate: Math.max(1, Math.round(wave.sampleRate * pitch)),
    channels: decodeRwsdWave(wave, waveData),
    waveIndex: note.waveIndex,
    wave,
    noteEvents: events.length,
  };
}

// ---------------------------------------------------------------------------------------------------------------
// Builder, for synthetic test fixtures.

export type BuildRwsdWave = {
  encoding: number;
  sampleRate: number;
  sampleCount: number;
  /** Per channel: the encoded sample bytes, and the ADPCM coefficients for ADPCM waves. */
  channels: { data: Uint8Array; coefs?: number[]; hist1?: number; hist2?: number }[];
  looped?: boolean;
  loopStart?: number;
};

export type BuildRwsdEntry = { waveIndex: number; pitch?: number; notePitch?: number };

/** Builds an RWSD header (version 1.2) and the matching wave data area. */
export function buildRwsd(entries: BuildRwsdEntry[], waves: BuildRwsdWave[]): { header: Buffer; waveData: Buffer } {
  const align = (v: number, a: number) => Math.ceil(v / a) * a;

  // DATA block: list, then per entry: DATAEntry (0x18), WSD info (0x20), track table (list of 1), track (a reference),
  // event list (list of 1), note event (0x10), note table (list of 1), note info (0x30).
  const perEntry = 0x18 + 0x20 + 12 + 8 + 12 + 0x10 + 12 + 0x30;
  const listLen = 4 + entries.length * 8;
  const dataBodyLen = align(listLen + entries.length * perEntry, 4);
  const data = Buffer.alloc(8 + dataBodyLen);
  data.write("DATA", 0, "latin1");
  data.writeUInt32BE(data.length, 4);
  const b = 8; // offsets below are relative to here
  const ref = (at: number, off: number) => {
    data[b + at] = 1;
    data.writeUInt32BE(off, b + at + 4);
  };
  data.writeUInt32BE(entries.length, b);
  entries.forEach((e, i) => {
    const p = listLen + i * perEntry;
    ref(4 + i * 8, p);
    const wsd = p + 0x18;
    const trackTable = wsd + 0x20;
    const track = trackTable + 12;
    const eventList = track + 8;
    const event = eventList + 12;
    const noteTable = event + 0x10;
    const note = noteTable + 12;
    ref(p, wsd);
    ref(p + 8, trackTable);
    ref(p + 16, noteTable);
    data.writeFloatBE(e.pitch ?? 1, b + wsd);
    data[b + wsd + 4] = 64;
    data.writeUInt32BE(1, b + trackTable);
    ref(trackTable + 4, track);
    ref(track, eventList);
    data.writeUInt32BE(1, b + eventList);
    ref(eventList + 4, event);
    data.writeFloatBE(0, b + event);
    data.writeFloatBE(1, b + event + 4);
    data.writeUInt32BE(0, b + event + 8);
    data.writeUInt32BE(1, b + noteTable);
    ref(noteTable + 4, note);
    data.writeInt32BE(e.waveIndex, b + note);
    data[b + note + 0x0c] = 60;
    data[b + note + 0x0d] = 127;
    data[b + note + 0x0e] = 64;
    data.writeFloatBE(e.notePitch ?? 1, b + note + 0x10);
  });

  // WAVE block and wave data.
  const infos: Buffer[] = [];
  const dataParts: Buffer[] = [];
  let dataPos = 0;
  for (const w of waves) {
    const n = w.channels.length;
    const adpcm = w.encoding === WaveEncoding.ADPCM;
    const tableOff = 0x1c;
    const chanOff = tableOff + n * 4;
    const adpcmOff = chanOff + n * 0x1c;
    const info = Buffer.alloc(adpcmOff + (adpcm ? n * 0x30 : 0));
    info[0] = w.encoding;
    info[1] = w.looped ? 1 : 0;
    info[2] = n;
    info[3] = (w.sampleRate >> 16) & 0xff;
    info.writeUInt16BE(w.sampleRate & 0xffff, 4);
    const units = (s: number) => (adpcm ? samplesToNibbles(s) : s);
    info.writeUInt32BE(units(w.loopStart ?? 0), 8);
    info.writeUInt32BE(units(w.sampleCount), 0x0c);
    info.writeUInt32BE(tableOff, 0x10);
    info.writeUInt32BE(dataPos, 0x14);
    let chData = 0;
    w.channels.forEach((ch, c) => {
      info.writeUInt32BE(chanOff + c * 0x1c, tableOff + c * 4);
      const ci = chanOff + c * 0x1c;
      info.writeUInt32BE(chData, ci);
      if (adpcm) {
        const a = adpcmOff + c * 0x30;
        info.writeUInt32BE(a, ci + 4);
        (ch.coefs ?? []).forEach((v, k) => info.writeInt16BE(v, a + k * 2));
        info.writeInt16BE(ch.hist1 ?? 0, a + 0x24);
        info.writeInt16BE(ch.hist2 ?? 0, a + 0x26);
      }
      const padded = Buffer.alloc(align(ch.data.length, 0x20));
      Buffer.from(ch.data).copy(padded);
      dataParts.push(padded);
      chData += padded.length;
    });
    dataPos += chData;
    infos.push(info);
  }
  const waveHeadLen = align(12 + waves.length * 4, 4);
  const wave = Buffer.alloc(waveHeadLen + infos.reduce((s, i) => s + align(i.length, 4), 0));
  wave.write("WAVE", 0, "latin1");
  wave.writeUInt32BE(wave.length, 4);
  wave.writeUInt32BE(waves.length, 8);
  let q = waveHeadLen;
  infos.forEach((info, i) => {
    wave.writeUInt32BE(q, 12 + i * 4);
    info.copy(wave, q);
    q += align(info.length, 4);
  });

  const header = Buffer.alloc(0x20);
  header.write("RWSD", 0, "latin1");
  header.writeUInt16BE(0xfeff, 4);
  header.writeUInt16BE(0x0102, 6);
  header.writeUInt32BE(0x20 + data.length + wave.length, 8);
  header.writeUInt16BE(0x20, 0x0c);
  header.writeUInt16BE(2, 0x0e);
  header.writeUInt32BE(0x20, 0x10);
  header.writeUInt32BE(data.length, 0x14);
  header.writeUInt32BE(0x20 + data.length, 0x18);
  header.writeUInt32BE(wave.length, 0x1c);
  return { header: Buffer.concat([header, data, wave]), waveData: Buffer.concat(dataParts) };
}
