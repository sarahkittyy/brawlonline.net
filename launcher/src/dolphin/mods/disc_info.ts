/**
 * What a disc image is (docs/design/brawl-mods.md §4), read with DolphinTool so every format
 * Dolphin plays works the same (ISO, RVZ, WIA, GCZ, WBFS, CISO):
 *
 * - `header -j`: the game ID, the internal name, the revision; a Wii disc has a title ID.
 * - `verify -a sha1`: the SHA-1 of the disc's contents, not of the file: an ISO and an RVZ of the
 *   same disc hash the same (and match the published dump hashes), since the machine reads the
 *   same bytes from either.
 *
 * A plain disc image (an ISO or GCM: the disc's bytes as they are) is the disc's contents, so its
 * SHA-1 is the file's, hashed here instead: DolphinTool also checks every Wii block's hashes, and
 * reports nothing until it is done (minutes for an 8.5 GB Brawl disc on a USB hard drive, with the
 * bar at 0%), while the file's hash reports its progress.
 */

import { open } from "fs/promises";
import type { ByteProgress } from "utils/copy_file";
import { hashFileWithProgress } from "utils/copy_file";

import type { ExecFn } from "../dolphin_tool";
import { execTool, withScratchUserFolder } from "../dolphin_tool";
import type { GameData } from "../game_data";
import { BRAWL_NTSC_U_DISC, BRAWL_NTSC_U_GAME_DATA, readGameData } from "../game_data";
import { titleIdHex } from "./save_import";
import type { DiscPlatform } from "./types";

export type DiscInfo = {
  gameId: string;
  /** The disc's internal name ("New SUPER MARIO BROS. Wii"). */
  title: string;
  revision: number;
  platform: DiscPlatform;
  /** A Wii disc's title ID (16 hex digits), where its save is kept; null for a GameCube disc. */
  titleId: string | null;
};

const HEADER_TIMEOUT_MS = 60_000;
/** A dual-layer disc from a slow drive. */
const VERIFY_TIMEOUT_MS = 30 * 60_000;

/** Parses `DolphinTool header -j`. */
export function parseHeaderJson(text: string): DiscInfo {
  let json: unknown;
  try {
    json = JSON.parse(text);
  } catch {
    throw new Error("This file is not a GameCube or Wii disc image Dolphin can read.");
  }
  const o = (json ?? {}) as Record<string, unknown>;
  const gameId = typeof o.game_id === "string" ? o.game_id : "";
  if (!/^[A-Z0-9]{4,6}$/.test(gameId)) {
    throw new Error("This file is not a GameCube or Wii disc image Dolphin can read.");
  }
  const title = typeof o.internal_name === "string" ? o.internal_name.trim() : "";
  const revision = typeof o.revision === "number" && Number.isInteger(o.revision) ? o.revision : 0;
  // Only a Wii disc has a title ID.
  const titleId = typeof o.title_id === "number" && Number.isSafeInteger(o.title_id) ? titleIdHex(o.title_id) : null;
  const platform: DiscPlatform = typeof o.title_id === "number" ? "wii" : "gamecube";
  return { gameId, title: title || gameId, revision, platform, titleId };
}

export async function readDiscInfo(toolPath: string, discPath: string, exec: ExecFn = execTool): Promise<DiscInfo> {
  const out = await exec(toolPath, ["header", "-i", discPath, "-j"], HEADER_TIMEOUT_MS);
  return parseHeaderJson(out);
}

/** The magic numbers of a disc's header (Dolphin's DiscIO/VolumeWii.h and VolumeGC.h). */
const WII_DISC_MAGIC = 0x5d1c9ea3;
const GC_DISC_MAGIC = 0xc2339f3d;

/**
 * Whether the file holds the disc's bytes as they are: its header's magic is where the disc has
 * it (a compressed image starts with a header of its own, so Dolphin's CISO, GCZ, TGC, WBFS, WIA,
 * RVZ and NFS are not). Split images ("game.part0.iso" and on) go to DolphinTool, which reads
 * all the parts.
 */
export async function isPlainDiscImage(discPath: string): Promise<boolean> {
  if (/\.part\d+\.iso$/i.test(discPath)) {
    return false;
  }
  const file = await open(discPath, "r");
  try {
    const header = Buffer.alloc(0x20);
    const { bytesRead } = await file.read(header, 0, header.length, 0);
    if (bytesRead < header.length) {
      return false;
    }
    return header.readUInt32BE(0x18) === WII_DISC_MAGIC || header.readUInt32BE(0x1c) === GC_DISC_MAGIC;
  } finally {
    await file.close();
  }
}

/** The SHA-1 (hex) of the disc's contents: the file's for a plain image (`onProgress` follows it),
 * else DolphinTool's (`onProgress` hears nothing until it is done). */
export async function discContentSha1WithProgress(
  toolPath: string,
  discPath: string,
  onProgress?: ByteProgress,
  exec: ExecFn = execTool,
): Promise<string> {
  if (await isPlainDiscImage(discPath)) {
    return hashFileWithProgress(discPath, "sha1", onProgress);
  }
  return discContentSha1(toolPath, discPath, exec);
}

/** The SHA-1 (hex) of the disc's contents, from DolphinTool (any format). */
export async function discContentSha1(toolPath: string, discPath: string, exec: ExecFn = execTool): Promise<string> {
  const out = await withScratchUserFolder((userDir) =>
    exec(toolPath, ["verify", "-u", userDir, "-i", discPath, "-a", "sha1"], VERIFY_TIMEOUT_MS),
  );
  const hash = out.trim().toLowerCase();
  if (!/^[0-9a-f]{40}$/.test(hash)) {
    throw new Error(`Could not check the disc: ${out.trim().slice(0, 200)}`);
  }
  return hash;
}

/**
 * A build's disc part, as rooms compare discs (docs/design/disc-images.md): what the disc gives the
 * game, whatever file holds it. A Wii disc is its game data, so an ISO, an RVZ, a WBFS, a scrubbed
 * image and an image converted back to an ISO of one disc are one disc, and NTSC-U Brawl's Rev 1
 * and Rev 2 are one too (BRAWL_NTSC_U_DISC). A GameCube disc (no hashes to check) or a Wii disc
 * whose game data does not check out (damaged, modified) is its contents' SHA-1, as before.
 * `onProgress` gets the disc's bytes read; the SHA-1 of a compressed GameCube image reports none.
 */
export async function discIdentity(
  toolPath: string,
  discPath: string,
  onProgress?: ByteProgress,
  read: (toolPath: string, discPath: string, onProgress?: ByteProgress) => Promise<GameData> = readGameData,
  contentSha1: typeof discContentSha1WithProgress = discContentSha1WithProgress,
): Promise<string> {
  const data = await read(toolPath, discPath, onProgress);
  if (data.gameData) {
    return BRAWL_NTSC_U_GAME_DATA.has(data.gameData) ? BRAWL_NTSC_U_DISC : data.gameData;
  }
  return contentSha1(toolPath, discPath, onProgress);
}

/** A Brawl disc (any region or revision): mods for Brawl boot it through their launcher. */
export function isBrawlGameId(gameId: string | null | undefined): boolean {
  return !!gameId && gameId.startsWith("RSB");
}
