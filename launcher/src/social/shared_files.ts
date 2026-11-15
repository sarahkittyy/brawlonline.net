/**
 * The two chat files the launcher shares with the game, in Dolphin's online folder
 * (`<User>/Online/`, docs/chat-protocol.md §1 and §6):
 *
 * - `chat-identity.key`: the install's Ed25519 seed, 64 hex digits and a newline. Whoever needs it
 *   first makes it, with an exclusive create, and the other reads it; one identity key per install
 *   means a report about a DM verifies against the same key as one about in-game chat.
 * - `chat-hidden.json`: `{"hidden": [uid, …]}`, the ignored players (at most 500). "Ignore user" in
 *   the launcher ignores in the game and the other way round: both write it atomically (a temporary
 *   file renamed over it) and both re-read it when it changes.
 */

import { randomBytes } from "crypto";
import { chmod, link, mkdir, open, readFile, rename, stat, unlink, writeFile } from "fs/promises";
import path from "path";

export const IDENTITY_FILE = "chat-identity.key";
export const HIDDEN_FILE = "chat-hidden.json";
export const MAX_HIDDEN = 500;
const MAX_HIDDEN_FILE_BYTES = 64 * 1024;

const UID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

export function isUid(s: unknown): s is string {
  return typeof s === "string" && UID.test(s);
}

function parseSeed(text: string): Uint8Array | undefined {
  const hex = text.replace(/[\r\n ]+$/, "");
  return /^[0-9a-fA-F]{64}$/.test(hex) ? new Uint8Array(Buffer.from(hex, "hex")) : undefined;
}

/**
 * The identity seed, made if there is none. A file that can't be parsed is kept aside as `.bad`
 * (as Dolphin does) and a new key made.
 */
export async function loadIdentitySeed(onlineDir: string): Promise<Uint8Array> {
  await mkdir(onlineDir, { recursive: true });
  const file = path.join(onlineDir, IDENTITY_FILE);
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const st = await stat(file);
      if (st.size <= 256) {
        const seed = parseSeed(await readFile(file, "utf8"));
        if (seed) {
          return seed;
        }
      }
      await rename(file, `${file}.bad`);
    } catch (err: any) {
      if (err?.code !== "ENOENT") {
        throw err;
      }
    }
    const seed = new Uint8Array(randomBytes(32));
    if (await createExclusively(file, `${Buffer.from(seed).toString("hex")}\n`)) {
      return seed;
    }
    // The game made it meanwhile: read its key.
  }
  throw new Error(`Cannot read or make ${file}`);
}

/**
 * Puts `contents` at `file` only if there is no file there, never showing a half-written one (the
 * game may read it at any moment): written to a temporary file, then hard-linked into place, which
 * fails if the file exists. Where hard links don't work, an exclusive create. False if the file
 * exists.
 */
async function createExclusively(file: string, contents: string): Promise<boolean> {
  const tmp = `${file}.launcher.${process.pid}.tmp`;
  await writeFile(tmp, contents, { mode: 0o600 });
  try {
    await link(tmp, file);
    return true;
  } catch (err: any) {
    if (err?.code === "EEXIST") {
      return false;
    }
    try {
      const fd = await open(file, "wx", 0o600);
      try {
        await fd.writeFile(contents);
      } finally {
        await fd.close();
      }
      return true;
    } catch (err2: any) {
      if (err2?.code === "EEXIST") {
        return false;
      }
      throw err2;
    }
  } finally {
    await unlink(tmp).catch(() => undefined);
  }
}

/**
 * The launcher's own copy of the list as the account had it the last time the two were in step
 * (docs/design/friends.md, "The ignore list"): what changed in the game's file since then is
 * what the game changed. One per account.
 */
export const syncedHiddenFile = (uid: string) => `chat-hidden-synced-${uid}.json`;

/** An ignore list file; null when there is none, empty when it can't be read. */
export async function readHiddenFile(onlineDir: string, name: string): Promise<string[] | null> {
  const file = path.join(onlineDir, name);
  try {
    const st = await stat(file);
    if (st.size > MAX_HIDDEN_FILE_BYTES) {
      return [];
    }
    const root = JSON.parse(await readFile(file, "utf8"));
    const list = root?.hidden;
    if (!Array.isArray(list)) {
      return [];
    }
    return [...new Set(list.filter(isUid))].slice(0, MAX_HIDDEN);
  } catch (err: any) {
    return err?.code === "ENOENT" ? null : [];
  }
}

/** The ignore list; empty when there is no file or it can't be read. */
export async function readHidden(onlineDir: string): Promise<string[]> {
  return (await readHiddenFile(onlineDir, HIDDEN_FILE)) ?? [];
}

/** Writes the ignore list (or another list file: `name`) atomically. */
export async function writeHidden(onlineDir: string, uids: string[], name = HIDDEN_FILE): Promise<void> {
  await mkdir(onlineDir, { recursive: true });
  const file = path.join(onlineDir, name);
  const tmp = `${file}.launcher.${process.pid}.tmp`;
  const list = [...new Set(uids.filter(isUid))].slice(-MAX_HIDDEN).sort();
  try {
    await writeFile(tmp, JSON.stringify({ hidden: list }, null, 2), { mode: 0o600 });
    await chmod(tmp, 0o600).catch(() => undefined);
    // On Windows a rename fails while the game has the file open for reading: try again.
    for (let attempt = 0; ; attempt++) {
      try {
        await rename(tmp, file);
        break;
      } catch (err: any) {
        if (attempt >= 9 || !["EPERM", "EACCES", "EBUSY"].includes(err?.code)) {
          throw err;
        }
        await new Promise((resolve) => setTimeout(resolve, 50));
      }
    }
  } catch (err) {
    await unlink(tmp).catch(() => undefined);
    throw err;
  }
}

/** The file's modification time and size, to notice the game's changes (0 when missing). */
export async function hiddenStamp(onlineDir: string): Promise<string> {
  try {
    const st = await stat(path.join(onlineDir, HIDDEN_FILE));
    return `${st.mtimeMs}:${st.size}`;
  } catch {
    return "0";
  }
}
