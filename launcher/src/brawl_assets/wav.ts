// Minimal RIFF WAVE (PCM 16-bit little-endian) writer and reader.
//
// SPDX-License-Identifier: GPL-3.0-or-later

/** Interleaves the channels into a 44-byte-header PCM16 WAV file. Channels shorter than the longest are padded. */
export function encodeWav(sampleRate: number, channels: Int16Array[]): Buffer {
  const n = channels.length;
  if (n === 0) {
    throw new Error("WAV: no channels");
  }
  const frames = Math.max(...channels.map((c) => c.length));
  const dataLen = frames * n * 2;
  const out = Buffer.alloc(44 + dataLen);
  out.write("RIFF", 0, "latin1");
  out.writeUInt32LE(36 + dataLen, 4);
  out.write("WAVE", 8, "latin1");
  out.write("fmt ", 12, "latin1");
  out.writeUInt32LE(16, 16);
  out.writeUInt16LE(1, 20); // PCM
  out.writeUInt16LE(n, 22);
  out.writeUInt32LE(sampleRate, 24);
  out.writeUInt32LE(sampleRate * n * 2, 28); // byte rate
  out.writeUInt16LE(n * 2, 32); // block align
  out.writeUInt16LE(16, 34); // bits per sample
  out.write("data", 36, "latin1");
  out.writeUInt32LE(dataLen, 40);
  let p = 44;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < n; c++) {
      out.writeInt16LE(channels[c][i] ?? 0, p);
      p += 2;
    }
  }
  return out;
}

export type DecodedWav = { sampleRate: number; channels: Int16Array[] };

/** Reads a PCM16 WAV file (as written by encodeWav; skips unknown chunks). */
export function decodeWav(buf: Buffer): DecodedWav {
  if (buf.toString("latin1", 0, 4) !== "RIFF" || buf.toString("latin1", 8, 12) !== "WAVE") {
    throw new Error("WAV: not a RIFF WAVE file");
  }
  let p = 12;
  let fmt: { channels: number; sampleRate: number; bits: number } | null = null;
  while (p + 8 <= buf.length) {
    const id = buf.toString("latin1", p, p + 4);
    const len = buf.readUInt32LE(p + 4);
    const body = p + 8;
    if (id === "fmt ") {
      if (buf.readUInt16LE(body) !== 1) {
        throw new Error("WAV: not PCM");
      }
      fmt = {
        channels: buf.readUInt16LE(body + 2),
        sampleRate: buf.readUInt32LE(body + 4),
        bits: buf.readUInt16LE(body + 14),
      };
    } else if (id === "data") {
      if (!fmt || fmt.bits !== 16) {
        throw new Error("WAV: expected 16-bit PCM");
      }
      const frames = Math.floor(len / (fmt.channels * 2));
      const channels = Array.from({ length: fmt.channels }, () => new Int16Array(frames));
      for (let i = 0; i < frames; i++) {
        for (let c = 0; c < fmt.channels; c++) {
          channels[c][i] = buf.readInt16LE(body + (i * fmt.channels + c) * 2);
        }
      }
      return { sampleRate: fmt.sampleRate, channels };
    }
    p = body + len + (len & 1);
  }
  throw new Error("WAV: no data chunk");
}
