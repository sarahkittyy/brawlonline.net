/**
 * What a disc image is (docs/design/brawl-mods.md §4), read with DolphinTool so every format
 * Dolphin plays works the same (ISO, RVZ, WIA, GCZ, WBFS, CISO):
 *
 * - `header -j`: the game ID, the internal name, the revision; a Wii disc has a title ID.
 * - `verify -a sha1`: the SHA-1 of the disc's contents, not of the file: an ISO and an RVZ of the
 *   same disc hash the same (and match the published dump hashes), since the machine reads the
 *   same bytes from either.
 */

import type { ExecFn } from "../dolphin_tool";
import { execTool } from "../dolphin_tool";
import type { DiscPlatform } from "./types";

export type DiscInfo = {
  gameId: string;
  /** The disc's internal name ("New SUPER MARIO BROS. Wii"). */
  title: string;
  revision: number;
  platform: DiscPlatform;
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
  const platform: DiscPlatform = typeof o.title_id === "number" ? "wii" : "gamecube";
  return { gameId, title: title || gameId, revision, platform };
}

export async function readDiscInfo(toolPath: string, discPath: string, exec: ExecFn = execTool): Promise<DiscInfo> {
  const out = await exec(toolPath, ["header", "-i", discPath, "-j"], HEADER_TIMEOUT_MS);
  return parseHeaderJson(out);
}

/** The SHA-1 (hex) of the disc's contents. */
export async function discContentSha1(toolPath: string, discPath: string, exec: ExecFn = execTool): Promise<string> {
  const out = await exec(toolPath, ["verify", "-i", discPath, "-a", "sha1"], VERIFY_TIMEOUT_MS);
  const hash = out.trim().toLowerCase();
  if (!/^[0-9a-f]{40}$/.test(hash)) {
    throw new Error(`Could not check the disc: ${out.trim().slice(0, 200)}`);
  }
  return hash;
}

/** A Brawl disc (any region or revision): mods for Brawl boot it through their launcher. */
export function isBrawlGameId(gameId: string | null | undefined): boolean {
  return !!gameId && gameId.startsWith("RSB");
}
