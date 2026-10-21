/**
 * What a disc gives the game, whatever file holds it (docs/design/disc-images.md), from
 * `DolphinTool gamedata`: a Wii disc's game partition, where it starts on the disc and its TMD,
 * once every block the game can read has checked out against the partition's hashes. An ISO, an
 * RVZ, a WBFS, a scrubbed image and an image converted back to an ISO of one disc have the same
 * `gameData`; a disc whose game data was changed, lost or moved has another or none. A GameCube
 * disc has no hashes to check: its `gameData` is null.
 */

import { spawn } from "child_process";
import { createHash } from "crypto";
import path from "path";
import type { ByteProgress } from "utils/copy_file";

/** A dual-layer disc in a slow format on a USB hard drive. */
const GAME_DATA_TIMEOUT_MS = 30 * 60_000;

/** Where a retail Wii disc's game partition starts. Anywhere else, the emulated loading times differ. */
export const WII_DATA_PARTITION_OFFSET = 0xf800000;

export type GameData = {
  gameId: string;
  revision: number;
  platform: "wii" | "gamecube";
  /** The game partition's offset on the disc (Wii). */
  dataOffset: number | null;
  /** SHA-1 of the game partition's TMD (Wii). */
  tmdSha1: string | null;
  /** Whether the partition's H3 table matches its TMD (Wii). */
  h3Ok: boolean;
  /** Blocks of the game partition the game reads whose hashes fail (Wii; null if none were checked). */
  usedBlockErrors: number | null;
  /** Blocks the game never reads whose hashes fail: a scrubbed image or a WBFS has thousands. */
  unusedBlockErrors: number | null;
  /** The disc's game data (40 hex digits), or null when it did not check out (or for a GameCube disc). */
  gameData: string | null;
};

export type GameDataFn = (discPath: string, onProgress?: ByteProgress) => Promise<GameData>;

/** NTSC-U Super Smash Bros. Brawl's game data, Rev 1 and Rev 2 (the redump dumps, read with
 * DolphinTool gamedata; any other format of them has the same). */
export const BRAWL_NTSC_U_GAME_DATA: ReadonlyMap<string, string> = new Map([
  ["0d79fc54ef81e223b65eb8d4b1854643fc677554", "NTSC-U Rev 1"],
  ["e6ae2404f96cff10be1c4dda8559001c3a7dfd34", "NTSC-U Rev 2"],
]);

/**
 * NTSC-U Brawl Rev 1 and Rev 2 as one disc, as Other Games rooms compare discs: the two revisions
 * have the same game files and their main.dol differs in one word, the disc revision the game
 * checks at boot. A whole-machine session's checksums leave out the bytes that revision leaves in
 * RAM (Dolphin's MachineSession, BRAWL_DISC_REVISION), so Rev 1 and Rev 2 players play together.
 */
export const BRAWL_NTSC_U_DISC = createHash("sha1").update("disc: NTSC-U Brawl, Rev 1 or Rev 2").digest("hex");

const num = (v: unknown): number | null => (typeof v === "number" && Number.isFinite(v) ? v : null);
const hex40 = (v: unknown): string | null => (typeof v === "string" && /^[0-9a-f]{40}$/.test(v) ? v : null);

/** Parses `DolphinTool gamedata`'s JSON. */
export function parseGameData(text: string): GameData {
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
  return {
    gameId,
    revision: num(o.revision) ?? 0,
    platform: o.platform === "wii" ? "wii" : "gamecube",
    dataOffset: num(o.data_offset),
    tmdSha1: hex40(o.tmd_sha1),
    h3Ok: o.h3_ok === true,
    usedBlockErrors: num(o.used_block_errors),
    unusedBlockErrors: num(o.unused_block_errors),
    gameData: hex40(o.game_data),
  };
}

/** Parses DolphinTool's "progress <done> <total>" lines (stderr). */
export function parseToolProgress(line: string): { done: number; total: number } | null {
  const m = /^progress (\d+) (\d+)$/.exec(line.trim());
  return m ? { done: Number(m[1]), total: Number(m[2]) } : null;
}

/** Why a disc's game data did not check out, for the log ("" when it did). */
export function describeGameDataProblem(d: GameData): string {
  if (d.gameData) {
    return "";
  }
  if (d.platform !== "wii") {
    return "a GameCube disc has no hashes to check";
  }
  if (!d.h3Ok) {
    return "the game partition's hash table does not match its TMD";
  }
  if (d.usedBlockErrors === null) {
    return "the game partition could not be read";
  }
  return `${d.usedBlockErrors} blocks of the game's data fail their hash check (damaged or modified)`;
}

/** One line for the log: what the disc is and how its check went. */
export function describeGameData(d: GameData): string {
  const parts = [`game ID ${d.gameId}`, `revision ${d.revision}`];
  if (d.dataOffset !== null) {
    parts.push(`game partition at 0x${d.dataOffset.toString(16)}`);
  }
  parts.push(d.gameData ? `game data ${d.gameData}` : `no game data (${describeGameDataProblem(d)})`);
  if (d.unusedBlockErrors) {
    parts.push(`${d.unusedBlockErrors} unused blocks scrubbed`);
  }
  return parts.join(", ");
}

/** Runs `DolphinTool gamedata` on any disc image Dolphin reads, reporting the bytes read. */
export function readGameData(toolPath: string, discPath: string, onProgress?: ByteProgress): Promise<GameData> {
  return new Promise((resolve, reject) => {
    const child = spawn(toolPath, ["gamedata", "-i", discPath, "-p"], { windowsHide: true });
    let out = "";
    let err = "";
    let pending = "";
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error("Checking the disc took too long."));
    }, GAME_DATA_TIMEOUT_MS);
    child.stdout.on("data", (d: Buffer) => {
      out += d.toString();
    });
    child.stderr.on("data", (d: Buffer) => {
      pending += d.toString();
      const lines = pending.split(/\r?\n/);
      pending = lines.pop() ?? "";
      for (const line of lines) {
        const p = parseToolProgress(line);
        if (p) {
          onProgress?.(p.done, p.total);
        } else if (line.trim()) {
          err += `${line.trim()}\n`;
        }
      }
    });
    child.on("error", (e) => {
      clearTimeout(timer);
      reject(e);
    });
    child.on("close", (code) => {
      clearTimeout(timer);
      if (code !== 0) {
        const detail = err.trim() || out.trim() || `exit code ${code}`;
        reject(new Error(`${path.basename(toolPath)}: could not read the disc: ${detail.slice(0, 200)}`));
        return;
      }
      try {
        resolve(parseGameData(out.trim()));
      } catch (e) {
        reject(e);
      }
    });
  });
}

/**
 * The container a disc image file starts with (Dolphin's DiscIO blob formats), or null for a plain
 * image (the disc's own bytes, an ISO or GCM, starting with the game ID).
 */
export function discContainer(header: Buffer): string | null {
  if (header.length < 4) {
    return null;
  }
  const ascii = header.toString("latin1", 0, 4);
  switch (ascii) {
    case "WBFS":
      return "WBFS";
    case "RVZ\x01":
      return "RVZ";
    case "WIA\x01":
      return "WIA";
    case "CISO":
      return "CISO";
    case "EGGS":
      return "NFS";
  }
  switch (header.readUInt32LE(0)) {
    case 0xb10bc001:
      return "GCZ";
  }
  switch (header.readUInt32BE(0)) {
    case 0xae0f38a2:
      return "TGC";
  }
  return null;
}
