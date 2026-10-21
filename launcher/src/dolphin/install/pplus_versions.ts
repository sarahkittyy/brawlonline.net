// Older Project+ releases for replays (docs/design/pplus-versions.md). A replay plays as it was
// played only on its own release's game files, but a player's card is always on the launcher's
// release (pplus_card_update.ts). For a replay from another release, the playback Dolphin gets a
// card of its own: a copy of the player's card with that release's files written and the current
// release's extra files deleted, made once and kept (`<netplay>/pplus-versions/cards/<release>`).
//
// Which files those are comes from the hosted release manifests (every file of a release's card,
// with its sha256; tools/pplus_packs/make_packs.py, published at https://brawlonline.net/updates/
// pplus/). The files themselves are content-addressed blobs (`blobs/<sha256>`, verified), from:
// - the download (`<base>/blobs/<sha256>.zlib`), about 49 MB for v3.2.0;
// - a card on this PC that is on that release (a Project+ install of it, or an old copy), read
//   file by file and checked;
// - the card update itself: the files it overwrites are kept (`keepReplacedFiles`), so the next
//   P+ update never needs a download for the replays from before it.
// Nothing here is needed to play online, and nothing of it changes the player's own card.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { comparePPlusReleases, isPPlusRelease } from "@common/pplus_releases";
import { Mutex } from "async-mutex";
import { createHash } from "crypto";
import fs from "fs";
import path from "path";
import { promisify } from "util";
import { inflate as inflateCb } from "zlib";

import type { PluginBinary, SdInstallResult } from "./sd_card";
import { installPluginOnSdCard } from "./sd_card";

const inflate = promisify(inflateCb);

export const PPLUS_VERSIONS_URL = "https://brawlonline.net/updates/pplus/";

/**
 * The releases the bundled game plugin runs on (game-code/PPOnline: the boot guard finds P+'s
 * BootToCSS words wherever a release's codeset has them, and the sy_core stub fix leaves v3.2.0's
 * stubs alone; the harness ran on both cards). A release missing here is listed, but its replays
 * are not offered: "this version of Brawl Online can't play them".
 */
export const PLAYABLE_PPLUS_RELEASES: readonly string[] = ["v3.2.0", "v3.2.1"];

const MAX_INDEX = 256 * 1024;
const MAX_MANIFEST = 8 * 1024 * 1024;
const MAX_BLOB = 64 * 1024 * 1024;
const PARALLEL = 4;

export type ManifestFile = { path: string; sha256: string; size: number };
export type ReleaseManifest = { v: 1; release: string; probe: { path: string; sha256: string }; files: ManifestFile[] };
export type VersionsIndex = {
  v: 1;
  newest: string;
  blobs: string;
  releases: {
    release: string;
    manifest: string;
    sha256: string;
    files: number;
    filesFromNewest: number;
    bytesFromNewest: number;
    downloadFromNewest: number;
  }[];
};

export type PPlusVersionErrorCode =
  | "offline" // the index or a file could not be fetched
  | "unknown-release" // not in the hosted index
  | "unplayable" // this launcher's plugin does not run on it
  | "card-unknown" // the player's card is not on the launcher's release
  | "damaged" // a fetched file did not match its sha256
  | "busy"; // the card is open in another Dolphin
export class PPlusVersionError extends Error {
  constructor(readonly code: PPlusVersionErrorCode, message: string) {
    super(message);
  }
}

/** One release's state on this PC, as Settings and the replay dialogs show it. */
export type PPlusVersionState = {
  release: string;
  current: boolean;
  playable: boolean;
  /** Every file it needs is here (a card can be made without the network). */
  filesReady: boolean;
  /** Its playback card has been made (it may still be remade after the player's card changed). */
  cardReady: boolean;
  /** What is still to download (compressed bytes, an estimate before the first byte). */
  downloadBytes: number;
  /** On disk: its files (only those no other kept release needs) and its card. */
  filesBytes: number;
  cardBytes: number;
};

export type PrepareProgress = {
  phase: "index" | "download" | "card";
  current: number;
  total: number;
};

type Fetch = (url: string, init?: { signal?: AbortSignal }) => Promise<Response>;

const sha256 = (data: Buffer) => createHash("sha256").update(data).digest("hex");
const isSha = (s: unknown): s is string => typeof s === "string" && /^[0-9a-f]{64}$/.test(s);

async function readJson<T>(file: string): Promise<T | null> {
  try {
    return JSON.parse(await fs.promises.readFile(file, "utf8")) as T;
  } catch {
    return null;
  }
}

async function writeAtomic(file: string, data: Buffer | string) {
  await fs.promises.mkdir(path.dirname(file), { recursive: true });
  const tmp = `${file}.${process.pid}.${Date.now()}.tmp`;
  await fs.promises.writeFile(tmp, data);
  await fs.promises.rename(tmp, file);
}

async function dirBytes(dir: string): Promise<number> {
  let total = 0;
  try {
    for (const e of await fs.promises.readdir(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      total += e.isDirectory() ? await dirBytes(p) : (await fs.promises.stat(p)).size;
    }
  } catch {
    // missing: 0
  }
  return total;
}

function parseIndex(v: unknown): VersionsIndex | null {
  const o = v as VersionsIndex;
  if (!o || o.v !== 1 || !isPPlusRelease(o.newest) || typeof o.blobs !== "string" || !Array.isArray(o.releases)) {
    return null;
  }
  if (!/^blobs\/\{sha256\}\.zlib$/.test(o.blobs)) {
    return null;
  }
  const releases = o.releases.filter(
    (r) =>
      r &&
      isPPlusRelease(r.release) &&
      typeof r.manifest === "string" &&
      /^releases\/v[0-9a-z.]{1,24}\.json$/.test(r.manifest) &&
      isSha(r.sha256) &&
      Number.isFinite(r.downloadFromNewest) &&
      Number.isFinite(r.bytesFromNewest),
  );
  return { ...o, releases };
}

function parseManifest(v: unknown, release: string): ReleaseManifest | null {
  const o = v as ReleaseManifest;
  if (!o || o.v !== 1 || o.release !== release || !Array.isArray(o.files) || !o.probe || !isSha(o.probe.sha256)) {
    return null;
  }
  // (P+ has names like "2.00 a.m..brstm": only a whole "." or ".." segment is refused.)
  const safePath = (p: unknown) =>
    typeof p === "string" &&
    p.startsWith("/") &&
    p.length < 1024 &&
    p
      .slice(1)
      .split("/")
      .every((seg) => seg !== "" && seg !== "." && seg !== ".." && !seg.includes("\\"));
  const ok = o.files.every((f) => f && safePath(f.path) && isSha(f.sha256));
  return ok ? o : null;
}

export type PPlusVersionsOptions = {
  /** `<netplay>/pplus-versions`. */
  root: string;
  /** The launcher's release (PPLUS_RELEASE.version): the player's card is on it. */
  currentRelease: string;
  baseUrl?: string;
  fetch?: Fetch;
  log?: (message: string) => void;
};

export class PPlusVersions {
  private readonly root: string;
  private readonly baseUrl: string;
  private readonly fetchImpl: Fetch;
  private readonly log: (message: string) => void;
  readonly currentRelease: string;
  private readonly lock = new Mutex();
  private indexCache: VersionsIndex | null = null;

  constructor(opts: PPlusVersionsOptions) {
    this.root = opts.root;
    this.currentRelease = opts.currentRelease;
    this.baseUrl = (opts.baseUrl ?? PPLUS_VERSIONS_URL).replace(/\/?$/, "/");
    this.fetchImpl = opts.fetch ?? ((url, init) => fetch(url, init));
    this.log = opts.log ?? (() => undefined);
  }

  cardDir(release: string): string {
    return path.join(this.root, "cards", release);
  }

  private blobPath(sha: string): string {
    return path.join(this.root, "blobs", sha);
  }

  private async fetchBytes(rel: string, max: number, signal?: AbortSignal): Promise<Buffer> {
    let res: Response;
    try {
      res = await this.fetchImpl(this.baseUrl + rel, { signal });
    } catch (err) {
      throw new PPlusVersionError(
        "offline",
        `Could not reach ${this.baseUrl} (${err instanceof Error ? err.message : err})`,
      );
    }
    if (!res.ok) {
      throw new PPlusVersionError("offline", `${this.baseUrl}${rel}: HTTP ${res.status}`);
    }
    const data = Buffer.from(await res.arrayBuffer());
    if (data.length > max) {
      throw new PPlusVersionError("damaged", `${rel} is larger than expected`);
    }
    return data;
  }

  /** The hosted index (fetched once a launch; the copy on disk when offline), or null without either. */
  async index({ refresh = false }: { refresh?: boolean } = {}): Promise<VersionsIndex | null> {
    if (this.indexCache && !refresh) {
      return this.indexCache;
    }
    const file = path.join(this.root, "index.json");
    try {
      const raw = await this.fetchBytes("index.json", MAX_INDEX);
      const parsed = parseIndex(JSON.parse(raw.toString("utf8")));
      if (parsed) {
        await writeAtomic(file, raw);
        this.indexCache = parsed;
        return parsed;
      }
      this.log("The hosted Project+ versions index is not one this launcher reads; using the one on disk");
    } catch (err) {
      this.log(`Project+ versions index: ${err instanceof Error ? err.message : err}; using the one on disk`);
    }
    this.indexCache = parseIndex(await readJson(file));
    return this.indexCache;
  }

  /** A release's manifest (on disk once fetched; checked against the index's sha256). */
  async manifest(release: string): Promise<ReleaseManifest> {
    const file = path.join(this.root, "releases", `${release}.json`);
    const cached = await fs.promises.readFile(file).catch(() => null);
    const index = await this.index();
    const entry = index?.releases.find((r) => r.release === release);
    if (cached && (!entry || sha256(cached) === entry.sha256)) {
      const m = parseManifest(JSON.parse(cached.toString("utf8")), release);
      if (m) {
        return m;
      }
    }
    if (!entry) {
      throw new PPlusVersionError(
        index ? "unknown-release" : "offline",
        index
          ? `Project+ ${release} is not one Brawl Online has files for`
          : "The Project+ versions list could not be fetched",
      );
    }
    const raw = await this.fetchBytes(entry.manifest, MAX_MANIFEST);
    if (sha256(raw) !== entry.sha256) {
      throw new PPlusVersionError("damaged", `Project+ ${release}'s file list did not download correctly`);
    }
    const m = parseManifest(JSON.parse(raw.toString("utf8")), release);
    if (!m) {
      throw new PPlusVersionError("damaged", `Project+ ${release}'s file list is not one this launcher reads`);
    }
    await writeAtomic(file, raw);
    return m;
  }

  /** What turns a card on the current release into `release`'s: files to write and to delete. */
  async plan(release: string): Promise<{ write: ManifestFile[]; remove: string[]; target: ReleaseManifest }> {
    const [current, target] = await Promise.all([this.manifest(this.currentRelease), this.manifest(release)]);
    const have = new Map(current.files.map((f) => [f.path.toLowerCase(), f.sha256]));
    const want = new Set(target.files.map((f) => f.path.toLowerCase()));
    return {
      write: target.files.filter((f) => have.get(f.path.toLowerCase()) !== f.sha256),
      remove: current.files.filter((f) => !want.has(f.path.toLowerCase())).map((f) => f.path),
      target,
    };
  }

  private async hasBlob(sha: string): Promise<boolean> {
    return fs.promises.stat(this.blobPath(sha)).then(
      () => true,
      () => false,
    );
  }

  /** Keeps a file's bytes when they are the ones `sha` names. */
  async storeBlob(data: Buffer, sha = sha256(data)): Promise<boolean> {
    if (sha256(data) !== sha) {
      return false;
    }
    if (!(await this.hasBlob(sha))) {
      await writeAtomic(this.blobPath(sha), data);
    }
    return true;
  }

  private async missing(files: ManifestFile[]): Promise<ManifestFile[]> {
    const out: ManifestFile[] = [];
    const seen = new Set<string>();
    for (const f of files) {
      if (!seen.has(f.sha256) && !(await this.hasBlob(f.sha256))) {
        out.push(f);
      }
      seen.add(f.sha256);
    }
    return out;
  }

  /**
   * Keeps the files a card update is about to overwrite or delete (the card is still on the old
   * release), so that release's replays need no download later. Errors are only logged.
   */
  async keepReplacedFiles(card: string, paths: readonly string[]): Promise<number> {
    let kept = 0;
    let image: FatImage | null = null;
    try {
      image = await FatImage.open(card);
      for (const p of paths) {
        try {
          if (await image.exists(p)) {
            kept += (await this.storeBlob(await image.readFile(p))) ? 1 : 0;
          }
        } catch {
          // a file that cannot be read is simply not kept
        }
      }
      this.log(`Kept ${kept} files of the Project+ release before the update (for its replays)`);
    } catch (err) {
      this.log(`Could not keep the files before the Project+ update: ${err instanceof Error ? err.message : err}`);
    } finally {
      await image?.close();
    }
    return kept;
  }

  /**
   * Takes `release`'s files from a card on this PC (a Project+ install of that release, or an old
   * copy): every file it has with the right sha256 is kept. Returns how many were still missing
   * before and after.
   */
  async importFromCard(release: string, card: string): Promise<{ needed: number; found: number; missing: number }> {
    const { write } = await this.plan(release);
    const need = await this.missing(write);
    let found = 0;
    let image: FatImage | null = null;
    try {
      image = await FatImage.open(card);
    } catch (err) {
      throw new PPlusVersionError(
        "damaged",
        `${card} is not an SD card image (${err instanceof Error ? err.message : err})`,
      );
    }
    try {
      for (const f of need) {
        try {
          if ((await image.exists(f.path)) && (await this.storeBlob(await image.readFile(f.path), f.sha256))) {
            found++;
          }
        } catch {
          // not this file
        }
      }
    } finally {
      await image.close();
    }
    this.log(`Project+ ${release}: ${found} of ${need.length} missing files found on ${card}`);
    return { needed: need.length, found, missing: need.length - found };
  }

  /** Downloads `release`'s missing files. Progress in the files' bytes. */
  async download(
    release: string,
    onProgress?: (current: number, total: number) => void,
    signal?: AbortSignal,
  ): Promise<number> {
    const { write } = await this.plan(release);
    const need = await this.missing(write);
    const raw = need.reduce((s, f) => s + Math.max(f.size, 0), 0);
    // Progress in what is downloaded: the files' bytes scaled to the compressed size the index gives.
    const entry = (await this.index())?.releases.find((r) => r.release === release);
    const ratio = entry && entry.bytesFromNewest > 0 ? entry.downloadFromNewest / entry.bytesFromNewest : 1;
    const total = Math.round(raw * ratio);
    let done = 0;
    let next = 0;
    onProgress?.(0, total);
    const worker = async () => {
      while (next < need.length) {
        const f = need[next++];
        const z = await this.fetchBytes(`blobs/${f.sha256}.zlib`, MAX_BLOB, signal);
        let data: Buffer;
        try {
          data = await inflate(z);
        } catch {
          throw new PPlusVersionError("damaged", `${f.path} did not download correctly`);
        }
        if (!(await this.storeBlob(data, f.sha256))) {
          throw new PPlusVersionError("damaged", `${f.path} did not download correctly`);
        }
        done += Math.max(f.size, 0);
        onProgress?.(Math.min(Math.round(done * ratio), total), total);
      }
    };
    await Promise.all(Array.from({ length: Math.min(PARALLEL, need.length) }, worker));
    if (need.length) {
      this.log(`Downloaded ${need.length} files of Project+ ${release} (${raw} bytes, about ${total} downloaded)`);
    }
    return need.length;
  }

  /**
   * The playback card for `release`: made (or checked) from the player's card with `release`'s
   * files, with the plugin on it. Needs every file (download/import first).
   */
  async card(
    release: string,
    sourceImage: string,
    plugin: PluginBinary,
    opts: { protectedPaths?: string[]; inUse?: boolean; onProgress?: (current: number, total: number) => void } = {},
  ): Promise<SdInstallResult> {
    return this.lock.runExclusive(async () => {
      const { write, remove, target } = await this.plan(release);
      const missing = await this.missing(write);
      if (missing.length) {
        throw new PPlusVersionError("offline", `${missing.length} of Project+ ${release}'s files are not here yet`);
      }
      await this.assertCardOnCurrent(sourceImage);
      return installPluginOnSdCard({
        sourceImage,
        outputDir: this.cardDir(release),
        plugin,
        protectedPaths: opts.protectedPaths,
        inUse: opts.inUse,
        onProgress: opts.onProgress,
        log: this.log,
        variant: {
          id: `${release}:${target.probe.sha256.slice(0, 16)}:${write.length}:${remove.length}`,
          apply: (image) => this.applyRelease(image, write, remove, target),
        },
      });
    });
  }

  private async assertCardOnCurrent(sourceImage: string) {
    const current = await this.manifest(this.currentRelease);
    let image: FatImage | null = null;
    let hash = "";
    try {
      image = await FatImage.open(sourceImage);
      hash = sha256(await image.readFile(current.probe.path));
    } catch {
      // unreadable: not on the release
    } finally {
      await image?.close();
    }
    if (hash !== current.probe.sha256) {
      throw new PPlusVersionError(
        "card-unknown",
        `Your Project+ SD card is not Project+ ${this.currentRelease}, so the older version can't be made from it.`,
      );
    }
  }

  private async applyRelease(image: string, write: ManifestFile[], remove: string[], target: ReleaseManifest) {
    const volume = await Fat32Volume.open(image, { writable: true });
    try {
      for (const f of write) {
        await volume.writeFile(f.path, await fs.promises.readFile(this.blobPath(f.sha256)));
      }
      for (const p of remove) {
        await volume.delete(p);
      }
      await volume.flush();
      const problems = await volume.check();
      if (problems.length) {
        throw new Error(`The Project+ ${target.release} card is inconsistent: ${problems.slice(0, 3).join("; ")}`);
      }
      if (sha256(await volume.readFile(target.probe.path)) !== target.probe.sha256) {
        throw new Error(`The card does not read as Project+ ${target.release}`);
      }
    } finally {
      await volume.close();
    }
    this.log(
      `Made a Project+ ${target.release} card for replays: ${write.length} files written, ${remove.length} removed`,
    );
  }

  /** Every release the index (or this PC) knows, oldest first. */
  async states(): Promise<PPlusVersionState[]> {
    const index = await this.index();
    const names = new Set<string>([this.currentRelease, ...(index?.releases.map((r) => r.release) ?? [])]);
    const cards = await fs.promises.readdir(path.join(this.root, "cards")).catch(() => [] as string[]);
    for (const c of cards) {
      if (isPPlusRelease(c)) {
        names.add(c);
      }
    }
    const out: PPlusVersionState[] = [];
    for (const release of [...names].sort(comparePPlusReleases)) {
      out.push(await this.state(release));
    }
    return out;
  }

  async state(release: string): Promise<PPlusVersionState> {
    const current = release === this.currentRelease;
    const playable = PLAYABLE_PPLUS_RELEASES.includes(release);
    const cardDir = this.cardDir(release);
    const cardReady =
      !current &&
      (await fs.promises.stat(path.join(cardDir, "sd.raw")).then(
        () => true,
        () => false,
      ));
    const base = { release, current, playable, cardReady, cardBytes: cardReady ? await dirBytes(cardDir) : 0 };
    if (current) {
      return { ...base, filesReady: true, downloadBytes: 0, filesBytes: 0 };
    }
    try {
      const { write } = await this.plan(release);
      const missing = await this.missing(write);
      const raw = missing.reduce((s, f) => s + Math.max(f.size, 0), 0);
      const entry = this.indexCache?.releases.find((r) => r.release === release);
      // The compressed size, in proportion of what is missing.
      const downloadBytes =
        entry && entry.bytesFromNewest > 0 ? Math.round((entry.downloadFromNewest * raw) / entry.bytesFromNewest) : raw;
      const filesBytes = write.filter((f) => !missing.includes(f)).reduce((s, f) => s + Math.max(f.size, 0), 0);
      return { ...base, filesReady: missing.length === 0, downloadBytes, filesBytes };
    } catch {
      const entry = this.indexCache?.releases.find((r) => r.release === release);
      return { ...base, filesReady: false, downloadBytes: entry?.downloadFromNewest ?? 0, filesBytes: 0 };
    }
  }

  /** Deletes `release`'s card and the files no other release needs. */
  async remove(release: string): Promise<void> {
    await this.lock.runExclusive(async () => {
      await fs.promises.rm(this.cardDir(release), { recursive: true, force: true });
      let mine: ManifestFile[] = [];
      try {
        mine = (await this.plan(release)).write;
      } catch {
        return; // without its manifest its files can't be told apart: they stay
      }
      const keep = new Set<string>();
      for (const s of await this.states()) {
        if (s.release !== release && !s.current && (s.cardReady || s.filesReady)) {
          (await this.plan(s.release)).write.forEach((f) => keep.add(f.sha256));
        }
      }
      for (const f of mine) {
        if (!keep.has(f.sha256)) {
          await fs.promises.rm(this.blobPath(f.sha256), { force: true });
        }
      }
      this.log(`Removed Project+ ${release}'s files for replays`);
    });
  }
}

/**
 * A Project+ SD card at `p`: the file itself, or `sd.raw` in a Project+ folder (its User/Wii, or
 * up to three folders down, as a release zip extracts).
 */
export async function findProjectPlusCard(p: string): Promise<string | null> {
  const st = await fs.promises.stat(p).catch(() => null);
  if (!st) {
    return null;
  }
  if (st.isFile()) {
    return p;
  }
  const visit = async (dir: string, depth: number): Promise<string | null> => {
    const entries = await fs.promises.readdir(dir, { withFileTypes: true }).catch(() => []);
    const wii = entries.find((e) => e.isDirectory() && e.name.toLowerCase() === "wii");
    if (wii) {
      const card = path.join(dir, wii.name, "sd.raw");
      if (
        await fs.promises.stat(card).then(
          (s) => s.isFile(),
          () => false,
        )
      ) {
        return card;
      }
    }
    const direct = entries.find((e) => e.isFile() && e.name.toLowerCase() === "sd.raw");
    if (direct) {
      return path.join(dir, direct.name);
    }
    if (depth <= 0) {
      return null;
    }
    for (const e of entries) {
      if (e.isDirectory() && !e.name.startsWith(".")) {
        const hit = await visit(path.join(dir, e.name), depth - 1);
        if (hit) {
          return hit;
        }
      }
    }
    return null;
  };
  return visit(p, 3);
}
