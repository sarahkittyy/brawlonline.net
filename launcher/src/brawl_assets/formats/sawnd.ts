// Sound group replacements (".sawnd", from the Sawndz tool) that Project+ loads from the SD card instead of a BRSAR
// group: /Project+/pf/sfx/<group index as 3 hex digits>.sawnd, e.g. 000.sawnd for group 0 (snd_group_joucyu, the
// resident common sounds). The game keeps the disc BRSAR's INFO (sound names -> file + entry index) and takes the
// group's file headers and wave data from the .sawnd.
//
// Layout (big-endian), established from the P+ SD card and cross-checked against the disc BRSAR's groups:
//   u8  version (2)
//   u32 group id (the BRSAR group index + 7)
//   u32 wave data length
//   per file of the group (the BRSAR group's item count): u32 file id, u32 wave data offset, u32 wave data length
//   the files' headers back to back (RWSD/RSEQ/RBNK; each one's length is in its NW4R header at +8)
//   the wave data (the last `wave data length` bytes)
//
// SPDX-License-Identifier: GPL-3.0-or-later

import type { BrsarFileData, BrsarGroup } from "./brsar";

export const SAWND_GROUP_ID_BIAS = 7;

export type SawndFile = { fileId: number; header: Buffer; waveData: Buffer };

export type Sawnd = { version: number; groupId: number; files: SawndFile[] };

/** SD path (relative to the P+ root) of the replacement for a BRSAR group. */
export function sawndPath(groupIndex: number): string {
  return `sfx/${groupIndex.toString(16).toUpperCase().padStart(3, "0")}.sawnd`;
}

function check(cond: boolean, msg: string): asserts cond {
  if (!cond) {
    throw new Error(`sawnd: ${msg}`);
  }
}

const NW4R_MAGICS = new Set(["RWSD", "RSEQ", "RBNK", "RWAR", "RWAV"]);

function isNw4rMagic(buf: Buffer, at: number) {
  return at + 4 <= buf.length && NW4R_MAGICS.has(buf.toString("latin1", at, at + 4));
}

/**
 * Parses a .sawnd for a BRSAR group. The file count is not stored in the file, so it comes from the group (and the
 * file ids must match the group's, in order).
 */
export function parseSawnd(buf: Buffer, group: Pick<BrsarGroup, "id" | "items">): Sawnd {
  check(buf.length >= 9, "too short");
  const version = buf[0];
  check(version === 2, `unsupported version ${version}`);
  const groupId = buf.readUInt32BE(1);
  const waveLen = buf.readUInt32BE(5);
  const n = group.items.length;
  check(9 + n * 12 <= buf.length, "file table out of range");
  check(waveLen <= buf.length, "wave data length out of range");
  const waveStart = buf.length - waveLen;
  const files: SawndFile[] = [];
  let h = 9 + n * 12;
  for (let i = 0; i < n; i++) {
    const e = 9 + i * 12;
    const fileId = buf.readUInt32BE(e);
    const dataOffset = buf.readUInt32BE(e + 4);
    const dataLength = buf.readUInt32BE(e + 8);
    check(fileId === group.items[i].fileId, `file ${i} is ${fileId}, group ${group.id} has ${group.items[i].fileId}`);
    check(h + 0x10 <= waveStart, `header of file ${fileId} out of range`);
    const headerLength = buf.readUInt32BE(h + 8);
    check(h + headerLength <= waveStart, `header of file ${fileId} out of range`);
    check(dataOffset + dataLength <= waveLen, `wave data of file ${fileId} out of range`);
    files.push({
      fileId,
      header: buf.subarray(h, h + headerLength),
      waveData: buf.subarray(waveStart + dataOffset, waveStart + dataOffset + dataLength),
    });
    h += headerLength;
    // Headers may be padded beyond their NW4R length (P+'s 000.sawnd has 32 bytes after its RWSD): skip to the next
    // header's magic.
    if (i + 1 < n) {
      let pad = 0;
      while (pad <= 0x100 && h + pad + 4 <= waveStart && !isNw4rMagic(buf, h + pad)) {
        pad += 4;
      }
      check(isNw4rMagic(buf, h + pad), `no header for file ${group.items[i + 1].fileId}`);
      h += pad;
    }
  }
  return { version, groupId, files };
}

/** A file's header and wave data from a parsed .sawnd, in the shape readBrsarFile returns. */
export function sawndFile(sawnd: Sawnd, groupIndex: number, fileId: number): BrsarFileData | null {
  const f = sawnd.files.find((x) => x.fileId === fileId);
  return f ? { header: f.header, waveData: f.waveData, groupId: groupIndex } : null;
}

/** Builds a .sawnd (for synthetic test fixtures). */
export function buildSawnd(groupIndex: number, files: { fileId: number; header: Uint8Array; waveData: Uint8Array }[]) {
  const table = Buffer.alloc(9 + files.length * 12);
  table[0] = 2;
  table.writeUInt32BE(groupIndex + SAWND_GROUP_ID_BIAS, 1);
  let pos = 0;
  files.forEach((f, i) => {
    table.writeUInt32BE(f.fileId, 9 + i * 12);
    table.writeUInt32BE(pos, 13 + i * 12);
    table.writeUInt32BE(f.waveData.length, 17 + i * 12);
    pos += f.waveData.length;
  });
  table.writeUInt32BE(pos, 5);
  return Buffer.concat([
    table,
    ...files.map((f) => Buffer.from(f.header)),
    ...files.map((f) => Buffer.from(f.waveData)),
  ]);
}
