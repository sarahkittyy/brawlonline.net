// Nintendo GameCube/Wii DSP-ADPCM (4-bit) decoding.
//
// The samples come in 8-byte frames: one header byte (high nibble: index of the coefficient pair, low nibble: scale
// exponent) and 14 signed 4-bit samples. Each sample is predicted from the two previous output samples:
//   out = ((nibble << scale) << 11 + 1024 + c1 * hist1 + c2 * hist2) >> 11, clamped to int16
// with (c1, c2) = coefs[2 * index], coefs[2 * index + 1] (16 coefficients per channel, from the ADPCM info block).
//
// Sizes are often given in "nibbles", counting the header nibbles too: 16 nibbles per frame for 14 samples.
//
// References: BrawlLib (BrawlCrate, GPL-3.0) BrawlLib/Internal/Audio/ADPCMState.cs; the DSPADPCM section of the
// GameCube SDK documentation as summarised on https://wiki.tockdom.com/wiki/BRSTM_(File_Format).
//
// SPDX-License-Identifier: GPL-3.0-or-later

export const SAMPLES_PER_FRAME = 14;
export const BYTES_PER_FRAME = 8;
export const NIBBLES_PER_FRAME = 16;

/** Converts a DSP-ADPCM nibble count (headers included) to a sample count. */
export function nibblesToSamples(nibbles: number): number {
  const frames = Math.floor(nibbles / NIBBLES_PER_FRAME);
  const rest = nibbles % NIBBLES_PER_FRAME;
  return frames * SAMPLES_PER_FRAME + Math.max(0, rest - 2);
}

/** Converts a sample count to the nibble count that addresses it. */
export function samplesToNibbles(samples: number): number {
  const frames = Math.floor(samples / SAMPLES_PER_FRAME);
  const rest = samples % SAMPLES_PER_FRAME;
  return frames * NIBBLES_PER_FRAME + (rest ? rest + 2 : 0);
}

/** Bytes needed to hold `samples` samples. */
export function adpcmByteLength(samples: number): number {
  return Math.ceil(samples / SAMPLES_PER_FRAME) * BYTES_PER_FRAME;
}

export type AdpcmChannelInfo = {
  /** 16 signed coefficients (8 pairs). */
  coefs: ArrayLike<number>;
  /** Initial history (most recent sample first); usually 0. */
  hist1?: number;
  hist2?: number;
};

const clamp16 = (v: number) => (v > 32767 ? 32767 : v < -32768 ? -32768 : v);

/**
 * Decodes `sampleCount` samples from `data` (starting at its first frame). Stops early if the data runs out, so the
 * result may be shorter than requested.
 */
export function decodeDspAdpcm(data: Uint8Array, sampleCount: number, info: AdpcmChannelInfo): Int16Array {
  const available = Math.floor(data.length / BYTES_PER_FRAME) * SAMPLES_PER_FRAME;
  const n = Math.max(0, Math.min(sampleCount, available));
  const out = new Int16Array(n);
  const coefs = info.coefs;
  let h1 = info.hist1 ?? 0;
  let h2 = info.hist2 ?? 0;
  let o = 0;
  for (let f = 0; o < n; f++) {
    const base = f * BYTES_PER_FRAME;
    const header = data[base];
    const scale = 1 << (header & 0x0f);
    const index = (header >> 4) & 0x07;
    const c1 = coefs[index * 2];
    const c2 = coefs[index * 2 + 1];
    for (let i = 0; i < SAMPLES_PER_FRAME && o < n; i++) {
      const byte = data[base + 1 + (i >> 1)];
      let nib = i & 1 ? byte & 0x0f : byte >> 4;
      if (nib >= 8) {
        nib -= 16;
      }
      // Plain arithmetic in doubles (no int32 overflow); floor division matches the C right shift.
      const s = clamp16(Math.floor((nib * scale * 2048 + 1024 + c1 * h1 + c2 * h2) / 2048));
      out[o++] = s;
      h2 = h1;
      h1 = s;
    }
  }
  return out;
}

/** Signed 8-bit PCM to 16-bit. */
export function decodePcm8(data: Uint8Array, sampleCount: number): Int16Array {
  const n = Math.min(sampleCount, data.length);
  const out = new Int16Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = ((data[i] << 24) >> 24) * 256;
  }
  return out;
}

/** Big-endian signed 16-bit PCM. */
export function decodePcm16BE(data: Uint8Array, sampleCount: number): Int16Array {
  const n = Math.min(sampleCount, Math.floor(data.length / 2));
  const out = new Int16Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = ((data[i * 2] << 24) >> 16) | data[i * 2 + 1];
  }
  return out;
}
