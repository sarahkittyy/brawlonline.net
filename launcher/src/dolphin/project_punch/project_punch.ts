// Project Punch (https://github.com/WispSSBM/ProjectPunch, MIT, by Wisp): a Syriinge plugin for
// Project+ that adds frame advance, overlays, a ledgedash HUD and more to Training mode. Offline only.
//
// It cannot run beside our online plugin: both load into P+'s 64 KB Syringe heap, Project Punch
// v1.3.1 needs 0xB2A0 bytes of it and with our loader there the largest free block is 0xADE0, and
// sy_core then hangs the boot. It also acts in Versus matches (frame advance pauses the game), so it
// must never reach an online game. So it gets an SD card of its own, made once and kept:
// `<netplay>/project-punch/sd.raw`, a copy of P+'s plain card (never Play's patched one, never with
// PPOnline.rel) with the release's `Project+/` files written on it, booted by its own Play button
// with P+'s Offline Launcher. Its BootToCSS code is changed on that card only to boot to Training
// (its default case gets the Training string P+ boots to while L/R is held, see patchBootToTraining).
//
// The release comes from GitHub (the repo's latest release, its zip, checked against GitHub's
// sha256 digest). An update rewrites only Project Punch's files on the kept card; a new P+ release
// makes the card again. Removing it deletes the folder.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { Mutex } from "async-mutex";
import { createHash } from "crypto";
import fs from "fs";
import { async as AsyncStreamZip } from "node-stream-zip";
import path from "path";
import type { ByteProgress } from "utils/copy_file";
import { copyFileWithProgress, throttleProgress } from "utils/copy_file";

import { asDiskSpaceError, ensureFreeSpace } from "../install/disk_space";
import { PLUGIN_SD_PATH } from "../install/paths";
import type { ProjectPunchFile, ProjectPunchJob, ProjectPunchRelease, ProjectPunchState } from "./types";

export const PROJECT_PUNCH_FOLDER = "project-punch";
export const PROJECT_PUNCH_REPO = "WispSSBM/ProjectPunch";
export const PROJECT_PUNCH_API = `https://api.github.com/repos/${PROJECT_PUNCH_REPO}/releases`;
const DOWNLOAD_PREFIX = `https://github.com/${PROJECT_PUNCH_REPO}/releases/download/`;

const CARD_NAME = "sd.raw";
const MANIFEST_NAME = "manifest.json";
const MAX_ZIP = 16 * 1024 * 1024;
const MAX_FILES = 200;
const MAX_FILE = 16 * 1024 * 1024;
const MAX_TOTAL = 64 * 1024 * 1024;
const RELEASE_TTL_MS = 10 * 60 * 1000;
const CARD_SYNC_BYTES = 64 * 1024 * 1024;

/** P+'s codesets that hold BootToCSS: the Offline Launcher's (booted here) and the Netplay Launcher's. */
export const BOOT_CODESETS = ["/Project+/BOOST.GCT", "/Project+/NETBOOST.GCT"];

// BootToCSS v5.4's last port check and its default case, as assembled into the codeset (the same
// guard as game-code/PPOnline/source/boot_menu.cpp): blt LOOP_START; addi r4,r21,0x1B54
// ("sqVsMelee"); li r5,0; b %END%; then the Start case: addi r4,r21,0x1C94; li r5,0x14.
const BOOT_DEFAULT_CASE = [0x41a0ff84, 0x38951b54, 0x38a00000, 0x48000038, 0x38951c94, 0x38a00014];
// addi r4,r21,0x1870: "sqTraining" (the code's loadTraining), what L/R at boot picks.
const BOOT_TRAINING = 0x38951870;

type Fetch = (url: string, init?: { signal?: AbortSignal; headers?: Record<string, string> }) => Promise<Response>;

type Manifest = {
  v: 1;
  version: string;
  files: ProjectPunchFile[];
  pplusRelease: string;
  bootsToTraining: boolean;
  sourceSize: number;
  installedAt: string;
};

export class ProjectPunchError extends Error {}

/** The download or the card was cancelled (Cancel). */
export class ProjectPunchCancelled extends ProjectPunchError {
  constructor() {
    super("Cancelled");
  }
}

const sha256 = (data: Buffer) => createHash("sha256").update(data).digest("hex");

/**
 * `data` (a codeset) with BootToCSS's default case booting to Training instead of Versus, or null
 * when the code is not there (another boot code, a P+ release that changed it). A codeset patched
 * already (an update of the kept card) is found too.
 */
export function patchBootToTraining(data: Buffer): Buffer | null {
  for (let off = 0; off + BOOT_DEFAULT_CASE.length * 4 <= data.length; off += 4) {
    if (
      BOOT_DEFAULT_CASE.every((w, i) => {
        const word = data.readUInt32BE(off + i * 4);
        return word === w || (i === 1 && word === BOOT_TRAINING);
      })
    ) {
      const out = Buffer.from(data);
      out.writeUInt32BE(BOOT_TRAINING, off + 4);
      return out;
    }
  }
  return null;
}

/** A release as GitHub's API describes it (`/releases/latest`, `/releases/tags/<tag>`). */
export function parseRelease(v: unknown): ProjectPunchRelease {
  const r = v as { tag_name?: unknown; assets?: unknown };
  if (!r || typeof r.tag_name !== "string" || !/^[\w.+-]{1,40}$/.test(r.tag_name) || !Array.isArray(r.assets)) {
    throw new ProjectPunchError("GitHub sent no Project Punch release.");
  }
  const asset = (
    r.assets as { name?: unknown; size?: unknown; browser_download_url?: unknown; digest?: unknown }[]
  ).find((a) => typeof a?.name === "string" && /\.zip$/i.test(a.name));
  if (
    !asset ||
    typeof asset.browser_download_url !== "string" ||
    !asset.browser_download_url.startsWith(DOWNLOAD_PREFIX) ||
    typeof asset.size !== "number" ||
    asset.size <= 0 ||
    asset.size > MAX_ZIP
  ) {
    throw new ProjectPunchError(`Project Punch ${r.tag_name} has no zip to download.`);
  }
  const digest = typeof asset.digest === "string" ? /^sha256:([0-9a-f]{64})$/i.exec(asset.digest) : null;
  return {
    version: r.tag_name,
    assetName: String(asset.name),
    url: asset.browser_download_url,
    size: asset.size,
    sha256: digest ? digest[1].toLowerCase() : null,
  };
}

/** The SD path of a zip entry under its `Project+/` folder (`/Project+/...`), or null for anything else. */
export function cardPathOf(entryName: string): string | null {
  const parts = entryName
    .replace(/\\/g, "/")
    .split("/")
    .filter((p) => p !== "" && p !== ".");
  const at = parts.findIndex((p) => p.toLowerCase() === "project+");
  if (at < 0 || at === parts.length - 1 || parts.some((p) => p === ".." || p.includes(":"))) {
    return null;
  }
  return "/" + ["Project+", ...parts.slice(at + 1)].join("/");
}

export type ProjectPunchOptions = {
  /** `<netplay>/project-punch`. */
  root: string;
  /** P+'s plain card (`<netplay User>/Wii/sd.raw`): only read. */
  sourceCard: () => string;
  /** The P+ release the launcher plays (the card is made again when it changes). */
  pplusRelease: () => string;
  /** The release the card is on, when it is not `release` (pplus_builds.ts), else null. */
  otherPPlusBuild: (card: string, release: string) => Promise<string | null>;
  /** Install this tag instead of the latest (`PPO_PROJECT_PUNCH_TAG`, to test updates). */
  tag?: string;
  fetch?: Fetch;
  log?: (message: string) => void;
  onChange?: (state: ProjectPunchState) => void;
  /** Test hook: the copy-on-write clone (copyFileWithProgress). */
  clone?: (src: string, dst: string) => Promise<void>;
};

export class ProjectPunch {
  private readonly lock = new Mutex();
  private readonly fetchImpl: Fetch;
  private readonly log: (message: string) => void;
  private release: ProjectPunchRelease | null = null;
  private releaseAt = 0;
  private releaseError: string | null = null;
  private job: ProjectPunchJob | null = null;
  private abort: AbortController | null = null;
  private lastError: string | null = null;
  private running = false;

  constructor(private readonly opts: ProjectPunchOptions) {
    this.fetchImpl = opts.fetch ?? ((url, init) => fetch(url, init));
    this.log = opts.log ?? (() => undefined);
  }

  get root(): string {
    return this.opts.root;
  }

  get cardImage(): string {
    return path.join(this.opts.root, CARD_NAME);
  }

  private get manifestFile(): string {
    return path.join(this.opts.root, MANIFEST_NAME);
  }

  /** Its Dolphin is open (or closed): the card is neither updated nor removed meanwhile. */
  setRunning(running: boolean) {
    this.running = running;
    void this.emit();
  }

  private async readManifest(): Promise<Manifest | null> {
    try {
      const m = JSON.parse(await fs.promises.readFile(this.manifestFile, "utf8"));
      if (m && m.v === 1 && typeof m.version === "string" && Array.isArray(m.files)) {
        return m as Manifest;
      }
    } catch {
      // none
    }
    return null;
  }

  private async emit() {
    if (this.opts.onChange) {
      this.opts.onChange(await this.state());
    }
  }

  /** The latest release (asked again after 10 minutes, or with `refresh`), null when GitHub can't be reached. */
  async latest({ refresh = false }: { refresh?: boolean } = {}): Promise<ProjectPunchRelease | null> {
    if (this.release && !refresh && Date.now() - this.releaseAt < RELEASE_TTL_MS) {
      return this.release;
    }
    const url = this.opts.tag
      ? `${PROJECT_PUNCH_API}/tags/${encodeURIComponent(this.opts.tag)}`
      : `${PROJECT_PUNCH_API}/latest`;
    try {
      const res = await this.fetchImpl(url, {
        headers: { Accept: "application/vnd.github+json", "User-Agent": "BrawlOnline-Launcher" },
      });
      if (!res.ok) {
        throw new ProjectPunchError(`GitHub answered HTTP ${res.status}`);
      }
      this.release = parseRelease(await res.json());
      this.releaseAt = Date.now();
      this.releaseError = null;
    } catch (err) {
      this.releaseError = err instanceof Error ? err.message : String(err);
      this.log(`Project Punch: could not read the latest release (${this.releaseError})`);
    }
    return this.release;
  }

  async state({ refresh = false }: { refresh?: boolean } = {}): Promise<ProjectPunchState> {
    if (refresh) {
      await this.latest({ refresh: true });
    }
    const manifest = await this.readManifest();
    const cardBytes = await fs.promises.stat(this.cardImage).then(
      (s) => s.size,
      () => 0,
    );
    const sourceBytes = await fs.promises.stat(this.opts.sourceCard()).then(
      (s) => s.size,
      () => 0,
    );
    const installed = manifest && cardBytes > 0 ? manifest : null;
    const needsCard = !installed || installed.pplusRelease !== this.opts.pplusRelease() || cardBytes !== sourceBytes;
    const latest = this.release;
    const upToDate = installed != null && !needsCard && (latest == null || latest.version === installed.version);
    return {
      folder: this.opts.root,
      cardPath: this.cardImage,
      installed: installed
        ? {
            version: installed.version,
            cardBytes,
            pplusRelease: installed.pplusRelease,
            bootsToTraining: installed.bootsToTraining,
            installedAt: installed.installedAt,
          }
        : null,
      latest,
      latestError: latest ? null : this.releaseError,
      needsCard,
      upToDate,
      downloadBytes: upToDate ? 0 : (latest?.size ?? 0) + (needsCard ? sourceBytes : 0),
      job: this.job,
      running: this.running,
      error: this.lastError,
    };
  }

  /** The card to boot, or an error when there is none or it must be made again first. */
  async cardForPlay(): Promise<string> {
    const s = await this.state();
    if (!s.installed) {
      throw new ProjectPunchError("Project Punch is not downloaded.");
    }
    if (s.needsCard) {
      throw new ProjectPunchError("Project Punch's SD card is from another Project+ version. Update it first.");
    }
    if (this.job) {
      throw new ProjectPunchError("Project Punch is being updated.");
    }
    return this.cardImage;
  }

  /** Stops a download or card copy that is running. */
  cancel() {
    this.abort?.abort();
  }

  /**
   * Downloads the latest release (or the pinned tag) and puts it on the card: a fresh copy of P+'s
   * card when there is none or it is from another P+ release, else only its files are rewritten.
   */
  async install(): Promise<ProjectPunchState> {
    if (this.running) {
      throw new ProjectPunchError("Close Project Punch's game first.");
    }
    if (this.job) {
      throw new ProjectPunchError("Project Punch is already being downloaded.");
    }
    const abort = new AbortController();
    this.abort = abort;
    this.lastError = null;
    const before = await this.readManifest();
    this.job = { kind: before ? "update" : "install", phase: "download", current: 0, total: 0 };
    await this.emit();
    const report = throttleProgress((current, total) => {
      if (this.job) {
        this.job = { ...this.job, current, total };
        void this.emit();
      }
    }, 150);
    try {
      await this.lock.runExclusive(() =>
        this._install(abort.signal, (phase, current, total) => {
          if (this.job && this.job.phase !== phase) {
            this.job = { ...this.job, phase, current, total };
            void this.emit();
          }
          report(current, total);
        }),
      );
    } catch (err) {
      if (!(err instanceof ProjectPunchCancelled)) {
        this.lastError = err instanceof Error ? err.message : String(err);
        this.log(`Project Punch: ${this.lastError}`);
      } else {
        this.log("Project Punch: cancelled");
      }
      throw err;
    } finally {
      this.job = null;
      this.abort = null;
      await this.emit();
    }
    return this.state();
  }

  private async _install(
    signal: AbortSignal,
    progress: (phase: "download" | "card", current: number, total: number) => void,
  ) {
    const check = () => {
      if (signal.aborted) {
        throw new ProjectPunchCancelled();
      }
    };
    const release = await this.latest({ refresh: true });
    if (!release) {
      throw new ProjectPunchError(
        `Could not reach GitHub to download Project Punch (${this.releaseError ?? "no answer"}).`,
      );
    }
    check();
    await fs.promises.mkdir(this.opts.root, { recursive: true });
    const zip = await this.download(release, signal, (c, t) => progress("download", c, t));
    check();
    const files = await this.readZip(zip, release);
    check();

    const source = this.opts.sourceCard();
    const sourceStat = await fs.promises.stat(source).catch(() => null);
    if (!sourceStat) {
      throw new ProjectPunchError(`Project+'s SD card was not found at ${source}. Press Play once to get Project+.`);
    }
    const pplusRelease = this.opts.pplusRelease();
    const other = await this.opts.otherPPlusBuild(source, pplusRelease);
    if (other) {
      throw new ProjectPunchError(`Project+'s SD card is on Project+ ${other}, not ${pplusRelease}. Press Play first.`);
    }
    const before = await this.readManifest();
    const cardSize = await fs.promises.stat(this.cardImage).then(
      (s) => s.size,
      () => 0,
    );
    // The manifest goes first: an interrupted card is never taken for a whole one.
    await fs.promises.rm(this.manifestFile, { force: true });
    let bootsToTraining: boolean;
    if (before && before.pplusRelease === pplusRelease && cardSize === sourceStat.size) {
      progress("card", 0, 1);
      bootsToTraining = await this.writeFiles(this.cardImage, files, before.files);
      progress("card", 1, 1);
    } else {
      bootsToTraining = await this.freshCard(source, sourceStat.size, files, signal, (c, t) => progress("card", c, t));
    }
    const manifest: Manifest = {
      v: 1,
      version: release.version,
      files: files.map((f) => ({ path: f.path, sha256: sha256(f.data), size: f.data.length })),
      pplusRelease,
      bootsToTraining,
      sourceSize: sourceStat.size,
      installedAt: new Date().toISOString(),
    };
    await fs.promises.writeFile(`${this.manifestFile}.tmp`, JSON.stringify(manifest, null, 2) + "\n");
    await fs.promises.rename(`${this.manifestFile}.tmp`, this.manifestFile);
    await fs.promises.rm(zip, { force: true });
    this.log(
      `Project Punch ${release.version} ${before ? `replaced ${before.version}` : "installed"} on ${this.cardImage} ` +
        `(${files.length} files${bootsToTraining ? ", boots to Training" : ""})`,
    );
  }

  private async download(release: ProjectPunchRelease, signal: AbortSignal, onProgress: ByteProgress): Promise<string> {
    const file = path.join(this.opts.root, "download", path.basename(release.assetName).replace(/[^\w.+-]/g, "_"));
    await fs.promises.mkdir(path.dirname(file), { recursive: true });
    let res: Response;
    try {
      res = await this.fetchImpl(release.url, { signal, headers: { "User-Agent": "BrawlOnline-Launcher" } });
    } catch (err) {
      if (signal.aborted) {
        throw new ProjectPunchCancelled();
      }
      throw new ProjectPunchError(`Could not download Project Punch (${err instanceof Error ? err.message : err})`);
    }
    if (!res.ok || !res.body) {
      throw new ProjectPunchError(`Could not download Project Punch: HTTP ${res.status}`);
    }
    const chunks: Buffer[] = [];
    let got = 0;
    const reader = res.body.getReader();
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) {
          break;
        }
        got += value.length;
        if (got > MAX_ZIP) {
          throw new ProjectPunchError("Project Punch's download is larger than expected.");
        }
        chunks.push(Buffer.from(value));
        onProgress(got, Math.max(release.size, got));
      }
    } catch (err) {
      if (signal.aborted) {
        throw new ProjectPunchCancelled();
      }
      throw err;
    }
    const data = Buffer.concat(chunks);
    if (data.length !== release.size || (release.sha256 && sha256(data) !== release.sha256)) {
      throw new ProjectPunchError(
        "Project Punch's download is damaged (its size or sha256 does not match). Try again.",
      );
    }
    await fs.promises.writeFile(file, data);
    return file;
  }

  private async readZip(file: string, release: ProjectPunchRelease): Promise<{ path: string; data: Buffer }[]> {
    const zip = new AsyncStreamZip({ file, storeEntries: true });
    try {
      const entries = Object.values(await zip.entries()).filter((e) => !e.isDirectory);
      const files: { path: string; data: Buffer }[] = [];
      let total = 0;
      for (const e of entries) {
        const sdPath = cardPathOf(e.name);
        if (!sdPath || sdPath.toLowerCase() === PLUGIN_SD_PATH.toLowerCase()) {
          continue;
        }
        if (files.length >= MAX_FILES || e.size > MAX_FILE || (total += e.size) > MAX_TOTAL) {
          throw new ProjectPunchError(`Project Punch ${release.version}'s zip is larger than expected.`);
        }
        files.push({ path: sdPath, data: await zip.entryData(e.name) });
      }
      if (!files.some((f) => /^\/Project\+\/pf\/plugins\/[^/]+\.rel$/i.test(f.path))) {
        throw new ProjectPunchError(`Project Punch ${release.version}'s zip has no plugin in Project+/pf/plugins.`);
      }
      return files;
    } catch (err) {
      if (err instanceof ProjectPunchError) {
        throw err;
      }
      throw new ProjectPunchError(
        `Project Punch ${release.version}'s zip could not be read: ${err instanceof Error ? err.message : err}`,
      );
    } finally {
      await zip.close().catch(() => undefined);
    }
  }

  private async freshCard(
    source: string,
    sourceSize: number,
    files: { path: string; data: Buffer }[],
    signal: AbortSignal,
    onProgress: ByteProgress,
  ): Promise<boolean> {
    const partial = `${this.cardImage}.partial`;
    await fs.promises.rm(this.cardImage, { force: true });
    await fs.promises.rm(partial, { force: true });
    await ensureFreeSpace(this.opts.root, sourceSize, "Project Punch's SD card");
    this.log(`Project Punch: copying Project+'s SD card ${source} to ${partial}`);
    try {
      await copyFileWithProgress(source, partial, {
        clone: this.opts.clone,
        syncEvery: CARD_SYNC_BYTES,
        onProgress: (c, t) => {
          if (signal.aborted) {
            throw new ProjectPunchCancelled();
          }
          onProgress(c, t);
        },
      });
      if (signal.aborted) {
        throw new ProjectPunchCancelled();
      }
      const boots = await this.writeFiles(partial, files, []);
      await fs.promises.rename(partial, this.cardImage);
      return boots;
    } catch (err) {
      await fs.promises.rm(partial, { force: true }).catch(() => undefined);
      throw await asDiskSpaceError(err, this.opts.root, "Project Punch's SD card", { needBytes: sourceSize });
    }
  }

  /**
   * Writes the release's files on `image` (deleting the previous release's that it no longer has, and
   * our online plugin should it be there), sets BootToCSS to Training, then checks the FAT and reads
   * every file back. True when the boot code was found.
   */
  private async writeFiles(
    image: string,
    files: { path: string; data: Buffer }[],
    previous: ProjectPunchFile[],
  ): Promise<boolean> {
    const keep = new Set(files.map((f) => f.path.toLowerCase()));
    const vol = await Fat32Volume.open(image, { writable: true });
    const patched: { path: string; data: Buffer }[] = [];
    let problemsBefore: string[];
    let problemsAfter: string[];
    try {
      problemsBefore = await vol.check();
      for (const p of [...previous.map((f) => f.path), PLUGIN_SD_PATH]) {
        if (!keep.has(p.toLowerCase())) {
          await vol.delete(p);
        }
      }
      for (const f of files) {
        await vol.writeFile(f.path, f.data);
      }
      for (const codeset of BOOT_CODESETS) {
        if (!(await vol.lookup(codeset))) {
          continue;
        }
        const data = patchBootToTraining(await vol.readFile(codeset));
        if (data) {
          await vol.writeFile(codeset, data);
          patched.push({ path: codeset, data });
        } else {
          this.log(`Project Punch: no BootToCSS default case in ${codeset}; it boots as P+ does`);
        }
      }
      await vol.flush();
      problemsAfter = await vol.check();
    } finally {
      await vol.close();
    }
    const known = new Set(problemsBefore);
    const added = problemsAfter.filter((p) => !known.has(p));
    if (added.length) {
      throw new ProjectPunchError(`Project Punch's SD card failed its check: ${added.slice(0, 3).join("; ")}`);
    }
    const fat = await FatImage.open(image);
    try {
      for (const f of [...files, ...patched]) {
        if (sha256(await fat.readFile(f.path)) !== sha256(f.data)) {
          throw new ProjectPunchError(`${f.path} does not read back from Project Punch's SD card.`);
        }
      }
    } finally {
      await fat.close();
    }
    return patched.some((p) => p.path === BOOT_CODESETS[0]);
  }

  /** Deletes the card and everything else in the folder. */
  async remove(): Promise<void> {
    if (this.running) {
      throw new ProjectPunchError("Close Project Punch's game first.");
    }
    if (this.job) {
      throw new ProjectPunchError("Project Punch is being downloaded. Cancel that first.");
    }
    await this.lock.runExclusive(async () => {
      await fs.promises.rm(this.opts.root, { recursive: true, force: true });
    });
    this.lastError = null;
    this.log(`Project Punch: removed ${this.opts.root}`);
    await this.emit();
  }
}
