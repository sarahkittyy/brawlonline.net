import { Preconditions } from "@common/preconditions";
import { IsoValidity } from "@common/types";
import crypto from "crypto";
import fs from "fs";
import { open } from "node:fs/promises";
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

/** Fast check of the disc header, so other games and dumps fail without hashing 8 GB. */
export async function checkIsoHeader(isoPath: string): Promise<boolean> {
  const handle = await open(isoPath, "r");
  try {
    const header = Buffer.alloc(8);
    const { bytesRead } = await handle.read(header, 0, 8, 0);
    if (bytesRead < 8) {
      return false;
    }
    return header.toString("latin1", 0, 6) === BRAWL_GAME_ID && ACCEPTED_REVISIONS.has(header.readUInt8(7));
  } finally {
    await handle.close();
  }
}

export function md5ToValidity(md5: string): IsoValidity {
  return ACCEPTED_ISO_MD5S.get(md5.toLowerCase())?.valid ?? IsoValidity.INVALID;
}

export async function verifyIso(isoPath: string): Promise<IsoValidity> {
  const exists = await fileExists(isoPath);
  Preconditions.checkState(exists, `Error verifying ISO: File ${isoPath} does not exist`);

  if (!(await checkIsoHeader(isoPath))) {
    return IsoValidity.INVALID;
  }

  const md5 = await hashFile(isoPath);
  return md5ToValidity(md5);
}

function hashFile(isoPath: string): Promise<string> {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash("md5");
    const input = fs.createReadStream(isoPath, { highWaterMark: 4 * 1024 * 1024 });
    input.on("error", (err) => reject(`Error reading ISO file ${isoPath}: ${err}`));
    input.on("data", (chunk) => hash.update(chunk as Buffer));
    input.on("end", () => resolve(hash.digest("hex")));
  });
}

type CacheEntry = { size: number; mtimeMs: number; valid: IsoValidity };

/**
 * `verifyIso` with a small on-disk cache keyed by path, size and modification
 * time: hashing an 8.5 GB Brawl image takes a while, and the launcher checks the
 * ISO on every start.
 */
export async function verifyIsoCached(isoPath: string, cacheFile: string): Promise<IsoValidity> {
  const stat = await fs.promises.stat(isoPath);
  let cache: Record<string, CacheEntry> = {};
  try {
    cache = JSON.parse(await fs.promises.readFile(cacheFile, "utf8"));
  } catch {
    cache = {};
  }
  const hit = cache[isoPath];
  if (hit && hit.size === stat.size && hit.mtimeMs === stat.mtimeMs) {
    return hit.valid;
  }
  const valid = await verifyIso(isoPath);
  cache[isoPath] = { size: stat.size, mtimeMs: stat.mtimeMs, valid };
  try {
    await fs.promises.writeFile(cacheFile, JSON.stringify(cache, null, 2));
  } catch {
    // The cache is an optimisation only.
  }
  return valid;
}
