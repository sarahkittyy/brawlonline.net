// Game file sources: the Brawl disc (an extracted DATA/files folder, or an ISO read on demand through DolphinTool)
// and the Project+ virtual SD card (a FAT32 sd.raw image, or a folder holding the SD contents).
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { execFile } from "child_process";
import fs from "fs";
import os from "os";
import path from "path";

import { FatImage } from "./fat32";

/** A read-only tree of game files addressed by relative, slash-separated paths (e.g. "menu2/sc_title_en.pac"). */
export interface GameFileSource {
  readonly kind: "disc" | "sd";
  readonly label: string;
  /** Returns the file contents, or null if the file does not exist. */
  read(relPath: string): Promise<Buffer | null>;
  close(): Promise<void>;
}

const normalizeRel = (p: string) => p.replace(/\\/g, "/").replace(/^\/+/, "");

async function isDir(p: string): Promise<boolean> {
  try {
    return (await fs.promises.stat(p)).isDirectory();
  } catch {
    return false;
  }
}

async function readIfExists(p: string): Promise<Buffer | null> {
  try {
    return await fs.promises.readFile(p);
  } catch (err: any) {
    if (err?.code === "ENOENT" || err?.code === "ENOTDIR") {
      return null;
    }
    throw err;
  }
}

/** Resolves a user-supplied extracted-disc path to the folder that directly contains "menu2", "system", ... */
export async function resolveDiscRoot(p: string): Promise<string> {
  for (const candidate of [path.join(p, "DATA", "files"), path.join(p, "files"), p]) {
    if (await isDir(path.join(candidate, "system"))) {
      return candidate;
    }
  }
  throw new Error(`No Brawl disc files found under ${p} (expected .../DATA/files/system)`);
}

export class DiscFolderSource implements GameFileSource {
  readonly kind = "disc" as const;
  private constructor(private readonly root: string) {}

  static async open(p: string): Promise<DiscFolderSource> {
    return new DiscFolderSource(await resolveDiscRoot(p));
  }

  get label() {
    return this.root;
  }

  read(relPath: string) {
    return readIfExists(path.join(this.root, ...normalizeRel(relPath).split("/")));
  }

  async close() {
    // nothing to release
  }
}

export type ExecFileFn = (file: string, args: string[]) => Promise<{ stdout: string; stderr: string }>;

const defaultExec: ExecFileFn = (file, args) =>
  new Promise((resolve, reject) => {
    execFile(file, args, { windowsHide: true, maxBuffer: 16 * 1024 * 1024 }, (err, stdout, stderr) => {
      if (err) {
        reject(Object.assign(err, { stdout, stderr }));
      } else {
        resolve({ stdout: String(stdout), stderr: String(stderr) });
      }
    });
  });

/**
 * Reads single files out of a Wii disc image (ISO/WBFS/RVZ/...) with Dolphin's command line tool:
 * `DolphinTool extract -i <image> -o <tmp> -p DATA -s /<path> -q`, which writes `<tmp>/DATA/files/<path>`.
 * The Wii disc encryption is handled entirely by DolphinTool; no keys are used here.
 */
export class DiscIsoSource implements GameFileSource {
  readonly kind = "disc" as const;
  private readonly cache = new Map<string, Promise<Buffer | null>>();
  private chain: Promise<unknown> = Promise.resolve();

  private constructor(
    private readonly isoPath: string,
    private readonly dolphinToolPath: string,
    private readonly tmpDir: string,
    private readonly ownsTmp: boolean,
    private readonly exec: ExecFileFn,
  ) {}

  static async open(opts: {
    isoPath: string;
    dolphinToolPath: string;
    tmpDir?: string;
    exec?: ExecFileFn;
  }): Promise<DiscIsoSource> {
    await fs.promises.access(opts.isoPath, fs.constants.R_OK);
    await fs.promises.access(opts.dolphinToolPath, fs.constants.F_OK);
    const ownsTmp = !opts.tmpDir;
    const tmp = opts.tmpDir ?? (await fs.promises.mkdtemp(path.join(os.tmpdir(), "brawl-assets-")));
    await fs.promises.mkdir(tmp, { recursive: true });
    return new DiscIsoSource(opts.isoPath, opts.dolphinToolPath, tmp, ownsTmp, opts.exec ?? defaultExec);
  }

  get label() {
    return this.isoPath;
  }

  read(relPath: string): Promise<Buffer | null> {
    const rel = normalizeRel(relPath);
    let p = this.cache.get(rel);
    if (!p) {
      // DolphinTool runs are serialised; concurrent runs on one image gain nothing.
      p = this.chain.then(() => this.extract(rel));
      this.chain = p.catch(() => undefined);
      this.cache.set(rel, p);
    }
    return p;
  }

  private async extract(rel: string): Promise<Buffer | null> {
    const out = path.join(this.tmpDir, "x");
    const args = ["extract", "-i", this.isoPath, "-o", out, "-p", "DATA", "-s", `/${rel}`, "-q"];
    try {
      await this.exec(this.dolphinToolPath, args);
    } catch (err: any) {
      const msg = `${err?.stderr ?? ""}${err?.stdout ?? ""}`;
      // DolphinTool prints "No file/folder was extracted." and exits 1 for a path that is not on the disc.
      if (/no file\/folder was extracted|not found|does not exist|no such/i.test(msg)) {
        return null;
      }
      throw new Error(`DolphinTool failed to extract /${rel}: ${msg.trim() || err?.message}`);
    }
    const parts = rel.split("/");
    for (const candidate of [
      path.join(out, "DATA", "files", ...parts),
      path.join(out, "files", ...parts),
      path.join(out, ...parts),
    ]) {
      const data = await readIfExists(candidate);
      if (data) {
        await fs.promises.rm(candidate, { force: true });
        return data;
      }
    }
    return null;
  }

  async close() {
    await this.chain.catch(() => undefined);
    if (this.ownsTmp) {
      await fs.promises.rm(this.tmpDir, { recursive: true, force: true });
    } else {
      await fs.promises.rm(path.join(this.tmpDir, "x"), { recursive: true, force: true });
    }
  }
}

/** Default location of the Project+ game files on the SD card. */
export const PPLUS_ROOT = "/Project+/pf/";

/**
 * The Project+ SD card: a FAT32 image (sd.raw) or a folder with the card contents. Paths are relative to the P+
 * root (`/Project+/pf/`).
 */
export class SdSource implements GameFileSource {
  readonly kind = "sd" as const;

  private constructor(
    readonly label: string,
    private readonly fat: FatImage | null,
    private readonly folder: string | null,
    private readonly root: string,
  ) {}

  static async open(sdPath: string, root = PPLUS_ROOT): Promise<SdSource> {
    if (await isDir(sdPath)) {
      return new SdSource(sdPath, null, sdPath, root);
    }
    return new SdSource(sdPath, await FatImage.open(sdPath), null, root);
  }

  static fromFat(fat: FatImage, label = "sd", root = PPLUS_ROOT): SdSource {
    return new SdSource(label, fat, null, root);
  }

  async read(relPath: string): Promise<Buffer | null> {
    const full = `${this.root.replace(/\/+$/, "")}/${normalizeRel(relPath)}`;
    if (this.fat) {
      const e = await this.fat.stat(full);
      if (!e || e.isDirectory) {
        return null;
      }
      return this.fat.readFile(full);
    }
    return readIfExists(path.join(this.folder!, ...full.split("/").filter(Boolean)));
  }

  async close() {
    await this.fat?.close();
  }
}

/**
 * Names a P+ SD file may have for a disc path: P+ ships un-localised names ("mu_menumain.pac") for files the disc
 * localises ("mu_menumain_en.pac"), and sometimes the localised name as well.
 */
export function sdNameVariants(discRelPath: string): string[] {
  const rel = normalizeRel(discRelPath);
  const out = [rel];
  const stripped = rel.replace(/_(en|us|fr|sp|de|it|jp)(\.[^./]+)$/i, "$2");
  if (stripped !== rel) {
    out.unshift(stripped);
  }
  return out;
}

export type LayeredHit = { origin: "disc" | "sd"; path: string; data: Buffer };

/** Looks a disc-relative path up on the SD (P+ overrides) first, then on the disc. */
export async function readLayered(disc: GameFileSource, sd: GameFileSource | null, discRelPath: string) {
  const hits: LayeredHit[] = [];
  if (sd) {
    for (const v of sdNameVariants(discRelPath)) {
      const data = await sd.read(v);
      if (data) {
        hits.push({ origin: "sd", path: v, data });
        break;
      }
    }
  }
  const d = await disc.read(discRelPath);
  if (d) {
    hits.push({ origin: "disc", path: normalizeRel(discRelPath), data: d });
  }
  return hits;
}
