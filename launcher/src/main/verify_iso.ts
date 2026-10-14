import { Preconditions } from "@common/preconditions";
import { IsoValidity } from "@common/types";
import fs from "fs";
import { open } from "node:fs/promises";
import type { ByteProgress } from "utils/copy_file";
import { hashFileWithProgress } from "utils/copy_file";
import { fileExists } from "utils/file_exists";

type IsoHashInfo = {
  valid: IsoValidity;
  name: string;
};

/**
 * The only disc images we accept: full, unmodified NTSC-U Super Smash Bros. Brawl
 * dumps. Both revisions have identical game files; main.dol differs by one word
 * (the disc version the game expects), which has no gameplay effect.
 * Every other image is rejected (no "unknown, use anyway" as in Slippi), because
 * all players must run the same data for rollback to stay in sync.
 */
export const ACCEPTED_ISO_MD5S: ReadonlyMap<string, IsoHashInfo> = new Map([
  ["d18726e6dfdc8bdbdad540b561051087", { valid: IsoValidity.VALID, name: "NTSC-U Rev 1" }],
  ["52ce7160ced2505ad5e397477d0ea4fe", { valid: IsoValidity.VALID, name: "NTSC-U Rev 2" }],
]);

/** Game ID of NTSC-U Brawl, at offset 0 of the disc header. */
const BRAWL_GAME_ID = "RSBE01";
/** Disc revisions we accept (header byte 7). */
const ACCEPTED_REVISIONS = new Set([1, 2]);

/** The first bytes of the disc header: the game ID (6 bytes), the disc number and the revision. */
async function readIsoHeader(isoPath: string): Promise<Buffer> {
  const handle = await open(isoPath, "r");
  try {
    const header = Buffer.alloc(8);
    const { bytesRead } = await handle.read(header, 0, 8, 0);
    return header.subarray(0, bytesRead);
  } finally {
    await handle.close();
  }
}

function isAcceptedHeader(header: Buffer): boolean {
  return (
    header.length === 8 &&
    header.toString("latin1", 0, 6) === BRAWL_GAME_ID &&
    ACCEPTED_REVISIONS.has(header.readUInt8(7))
  );
}

/** The header for a log line: its bytes, the game ID as text and the revision. */
export function describeHeader(header: Buffer): string {
  const hex = [...header].map((b) => b.toString(16).padStart(2, "0")).join(" ");
  const id = [...header.subarray(0, 6)].map((b) => (b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : ".")).join("");
  const revision = header.length === 8 ? `, revision ${header.readUInt8(7)}` : "";
  return `[${hex}] (game ID "${id}"${revision})`;
}

/** Fast check of the disc header, so other games and dumps fail without hashing 8 GB. */
export async function checkIsoHeader(isoPath: string): Promise<boolean> {
  return isAcceptedHeader(await readIsoHeader(isoPath));
}

export function md5ToValidity(md5: string): IsoValidity {
  return ACCEPTED_ISO_MD5S.get(md5.toLowerCase())?.valid ?? IsoValidity.INVALID;
}

/** A check's result with what it saw, for the log: a rejected image says why. */
export type IsoCheck = { valid: IsoValidity; detail: string };

/** `onProgress` gets the bytes hashed (8.5 GB, ~25 s); it is not called when the header check fails. */
export async function verifyIsoDetailed(isoPath: string, onProgress?: ByteProgress): Promise<IsoCheck> {
  const exists = await fileExists(isoPath);
  Preconditions.checkState(exists, `Error verifying ISO: File ${isoPath} does not exist`);
  const { size } = await fs.promises.stat(isoPath);

  const header = await readIsoHeader(isoPath);
  if (!isAcceptedHeader(header)) {
    return {
      valid: IsoValidity.INVALID,
      detail: `${size} bytes, header ${describeHeader(header)} is not ${BRAWL_GAME_ID} revision 1 or 2`,
    };
  }

  let hashed = 0;
  let md5: string;
  try {
    md5 = await hashFileWithProgress(isoPath, "md5", (done, total) => {
      hashed = done;
      onProgress?.(done, total);
    });
  } catch (err) {
    const wrapped: NodeJS.ErrnoException = new Error(
      `Error reading ISO file ${isoPath} after ${hashed} of ${size} bytes: ${err}`,
    );
    wrapped.code = (err as NodeJS.ErrnoException)?.code;
    throw wrapped;
  }
  const accepted = ACCEPTED_ISO_MD5S.get(md5);
  const read = hashed === size ? `${size} bytes` : `read ${hashed} of ${size} bytes`;
  return accepted
    ? { valid: accepted.valid, detail: `${read}, MD5 ${md5} (${accepted.name})` }
    : {
        valid: IsoValidity.INVALID,
        detail: `${read}, header ${describeHeader(header)}, MD5 ${md5} is not an accepted image`,
      };
}

export async function verifyIso(isoPath: string, onProgress?: ByteProgress): Promise<IsoValidity> {
  return (await verifyIsoDetailed(isoPath, onProgress)).valid;
}

type CacheEntry = { size: number; mtimeMs: number; valid: IsoValidity; detail?: string; checkedAt?: string };

/**
 * `verifyIsoDetailed` with a small on-disk cache keyed by path, size and modification
 * time: hashing an 8.5 GB Brawl image takes a while, and the launcher checks the
 * ISO on every start. A cached result says so in its detail.
 */
export async function verifyIsoCached(
  isoPath: string,
  cacheFile: string,
  onProgress?: ByteProgress,
): Promise<IsoCheck & { cached: boolean }> {
  const stat = await fs.promises.stat(isoPath);
  let cache: Record<string, CacheEntry> = {};
  try {
    cache = JSON.parse(await fs.promises.readFile(cacheFile, "utf8"));
  } catch {
    cache = {};
  }
  const hit = cache[isoPath];
  if (hit && hit.size === stat.size && hit.mtimeMs === stat.mtimeMs) {
    const detail = `cached ${hit.checkedAt ?? "result"}: ${hit.detail ?? `${hit.size} bytes`}`;
    return { valid: hit.valid, detail, cached: true };
  }
  const result = await verifyIsoDetailed(isoPath, onProgress);
  cache[isoPath] = {
    size: stat.size,
    mtimeMs: stat.mtimeMs,
    valid: result.valid,
    detail: result.detail,
    checkedAt: new Date().toISOString(),
  };
  try {
    await fs.promises.writeFile(cacheFile, JSON.stringify(cache, null, 2));
  } catch {
    // The cache is an optimisation only.
  }
  return { ...result, cached: false };
}
