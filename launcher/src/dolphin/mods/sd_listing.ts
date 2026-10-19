/**
 * An Other Games room's SD card, asked about before Dolphin starts (docs/design/brawl-mods.md
 * §6.5; the user's, 2026-10-19: "it needs to be in the launcher, before ever starting dolphin",
 * with the files and the size that differ). A host's launcher puts its card's listing (the
 * manifest: a hash per 64 KiB chunk; and the card's files) on the accounts server when it hosts
 * (`DolphinTool sdhash --listing`). A joiner's launcher fetches it by the room's card root and
 * compares it with its own card (`DolphinTool sdcompare`, the session's own rule), then asks.
 * Dolphin starts only on Yes, told the answer, and downloads the card from the players.
 */

import { spawn } from "child_process";
import { randomUUID } from "crypto";
import { readFile, unlink, writeFile } from "fs/promises";
import os from "os";
import path from "path";

import type { ModSdCardCheck } from "./types";

/** The card was hashed just before (its hashes are cached), so these only read the cache. */
const TOOL_TIMEOUT_MS = 5 * 60_000;
/** How many differing files the dialog can name. */
const NAMES = 50;

/** Where a room card's listing is kept on the server (accounts `sd_cards.rs`). */
export type SdListingStore = {
  /** Stores the listing of the card `root` (the server keeps one per root). */
  upload(uid: string, root: string, listing: Uint8Array<ArrayBuffer>): Promise<void>;
  /** The listing of the card `root`, or null when the server has none. */
  fetch(uid: string, root: string): Promise<Uint8Array | null>;
};

export const noListings: SdListingStore = {
  upload: async () => undefined,
  fetch: async () => null,
};

/** Runs DolphinTool; resolves with its stdout, or rejects with its error line. */
function runTool(toolPath: string, args: string[]): Promise<string> {
  return new Promise((resolve, reject) => {
    const child = spawn(toolPath, args, { windowsHide: true });
    let out = "";
    let err = "";
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`${path.basename(toolPath)} ${args[0]} took too long.`));
    }, TOOL_TIMEOUT_MS);
    child.stdout.on("data", (d: Buffer) => {
      out += d.toString();
    });
    child.stderr.on("data", (d: Buffer) => {
      err += d.toString();
    });
    child.on("error", (e) => {
      clearTimeout(timer);
      reject(e);
    });
    child.on("close", (code) => {
      clearTimeout(timer);
      if (code === 0) {
        resolve(out);
      } else {
        const detail = err.trim() || out.trim() || `exit code ${code}`;
        reject(new Error(`${path.basename(toolPath)} ${args[0]}: ${detail.slice(0, 200)}`));
      }
    });
  });
}

const tempFile = (what: string) => path.join(os.tmpdir(), `brawl-online-${what}-${randomUUID()}.bin`);

/** The listing of the card at `sdPath` (DolphinTool sdhash --listing), and its root. */
export async function readSdListing(
  toolPath: string,
  sdPath: string,
  userDir: string,
): Promise<{ root: string; listing: Uint8Array<ArrayBuffer> }> {
  const file = tempFile("sd-listing");
  try {
    const out = await runTool(toolPath, ["sdhash", "-i", sdPath, "-u", userDir, "-l", file]);
    const root = out.trim().toLowerCase();
    if (!/^[0-9a-f]{64}$/.test(root)) {
      throw new Error(`${path.basename(toolPath)} sdhash: no root in "${out.trim().slice(0, 80)}"`);
    }
    const bytes = await readFile(file);
    const listing = new Uint8Array(new ArrayBuffer(bytes.byteLength));
    listing.set(bytes);
    return { root, listing };
  } finally {
    await unlink(file).catch(() => undefined);
  }
}

/** DolphinTool sdcompare's answer, or null for anything else. */
export function parseSdCompare(stdout: string): Extract<ModSdCardCheck, { kind: "differs" }> | null {
  let value: unknown;
  try {
    value = JSON.parse(stdout.trim());
  } catch {
    return null;
  }
  if (typeof value !== "object" || value === null) {
    return null;
  }
  const v = value as Record<string, unknown>;
  if (typeof v.bytes !== "number" || typeof v.files !== "number" || !Array.isArray(v.names)) {
    return null;
  }
  return {
    kind: "differs",
    bytes: v.bytes,
    files: v.files,
    names: v.names.filter((n): n is string => typeof n === "string").slice(0, NAMES),
    filesKnown: v.filesKnown !== false,
  };
}

/** What the card at `sdPath` would download of the card `root`, from that card's listing. */
export async function compareSdListing(
  toolPath: string,
  sdPath: string,
  userDir: string,
  listing: Uint8Array,
  root: string,
): Promise<Extract<ModSdCardCheck, { kind: "differs" }>> {
  const file = tempFile("sd-room-listing");
  try {
    await writeFile(file, listing);
    const out = await runTool(toolPath, [
      "sdcompare",
      "-i",
      sdPath,
      "-u",
      userDir,
      "-l",
      file,
      "-r",
      root,
      "-n",
      String(NAMES),
    ]);
    const parsed = parseSdCompare(out);
    if (!parsed) {
      throw new Error(`${path.basename(toolPath)} sdcompare: unreadable answer "${out.trim().slice(0, 80)}"`);
    }
    return parsed;
  } finally {
    await unlink(file).catch(() => undefined);
  }
}

/**
 * The question to ask before joining a room whose card is `roomRoot` ("" for none) with a game
 * whose card is `ownRoot` (null for none). A listing that cannot be had or compared asks without
 * sizes rather than not at all: the player still says yes before anything downloads.
 */
export async function checkRoomCard(options: {
  ownRoot: string | null;
  roomRoot: string;
  fetchListing: () => Promise<Uint8Array | null>;
  compare: (listing: Uint8Array) => Promise<Extract<ModSdCardCheck, { kind: "differs" }>>;
  log?: (message: string) => void;
}): Promise<ModSdCardCheck> {
  // No card on one side: rooms grey that out (room_match.ts); same card: nothing to download.
  if (!options.ownRoot || !options.roomRoot || options.ownRoot === options.roomRoot) {
    return { kind: "same" };
  }
  let listing: Uint8Array | null;
  try {
    listing = await options.fetchListing();
  } catch (err) {
    options.log?.(`the room's SD card listing could not be fetched: ${err}`);
    return { kind: "unknown" };
  }
  if (!listing) {
    return { kind: "unknown" };
  }
  try {
    const diff = await options.compare(listing);
    // Same contents under another root cannot happen (the root is the manifest's hash); a card
    // that only differs in chunks no file uses has the same root too. Nothing to fetch: no question.
    return diff.bytes === 0 ? { kind: "same" } : diff;
  } catch (err) {
    options.log?.(`the room's SD card listing could not be compared: ${err}`);
    return { kind: "unknown" };
  }
}
