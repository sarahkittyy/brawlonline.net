// EmissaryRecomp (https://github.com/kalomaze/EmissaryRecomp, GPLv3): Super Smash Bros. Brawl
// statically recompiled into a native PC program, with Brawl Online's rollback. An add-on: the
// launcher downloads its release for this computer, keeps it up to date and removes it
// (docs/design/brawl-recompiled.md §5). It plays the files of the player's own Brawl disc, which
// are extracted once from the Settings disc image with the bundled DolphinTool.
//
// On disk, `<netplay>/emissary-recomp/`:
//   portable.txt, UserData/     the game's settings, saves and logs (the runtime looks for
//                               portable.txt up to 4 folders above its program, so they outlive
//                               every version)
//   versions/<tag>/engine/      a release's program (`brawl-engine[.exe]`), without the upstream
//                               launcher (whose updater would replace files under us) and without
//                               its own portable.txt
//   current.json                the version that plays
//   staged.json                 a newer version, downloaded while the game ran: played next time
//   game/DATA/                  the disc's game partition (sys/, files/); game.json says whose
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { Mutex } from "async-mutex";
import { spawn } from "child_process";
import { createHash } from "crypto";
import fs from "fs";
import { async as AsyncStreamZip } from "node-stream-zip";
import path from "path";
import type { ByteProgress } from "utils/copy_file";
import { throttleProgress } from "utils/copy_file";
import { removeTree } from "utils/remove_tree";

import { asDiskSpaceError, ensureFreeSpace } from "../install/disk_space";
import type { EmissaryJob, EmissaryRelease, EmissaryState, EmissaryStatus } from "./types";
import { EMISSARY_REPO } from "./types";

export const EMISSARY_FOLDER = "emissary-recomp";
export const EMISSARY_API = `https://api.github.com/repos/${EMISSARY_REPO}/releases`;
const DOWNLOAD_PREFIX = `https://github.com/${EMISSARY_REPO}/releases/download/`;

const MAX_ARCHIVE = 1024 * 1024 * 1024;
const RELEASE_TTL_MS = 10 * 60 * 1000;
/** The game partition of NTSC-U Brawl, extracted (6.8 GB). */
export const GAME_FILES_BYTES = 7_300_000_000;
/** An unpacked release is about twice its archive (two engines of ~180 MB, compressed ~2:1). */
const UNPACK_FACTOR = 2.2;

const PORTABLE_TEXT =
  "Brawl Online's EmissaryRecomp: settings, saves and logs stay in the UserData folder next to this file.\n";

export type Platform = "win32" | "linux" | "darwin" | string;

/** The release archive for `platform`, or null (none for macOS yet). */
export function assetPattern(platform: Platform): RegExp | null {
  switch (platform) {
    case "win32":
      return /-Windows\.zip$/i;
    case "linux":
      return /-Linux\.tar\.gz$/i;
    default:
      return null;
  }
}

/** The program inside a version folder. */
export function engineRelativePath(platform: Platform): string {
  return path.join("engine", platform === "win32" ? "brawl-engine.exe" : "brawl-engine");
}

export class EmissaryError extends Error {}

/** Cancel. */
export class EmissaryCancelled extends EmissaryError {
  constructor() {
    super("Cancelled");
  }
}

/** A release as GitHub's API describes it (`/releases/latest`, `/releases/tags/<tag>`), its archive for `platform`. */
export function parseRelease(v: unknown, platform: Platform): EmissaryRelease {
  const r = v as { tag_name?: unknown; assets?: unknown };
  if (!r || typeof r.tag_name !== "string" || !/^[\w.+-]{1,40}$/.test(r.tag_name) || !Array.isArray(r.assets)) {
    throw new EmissaryError("GitHub sent no EmissaryRecomp release.");
  }
  const pattern = assetPattern(platform);
  if (!pattern) {
    throw new EmissaryError("EmissaryRecomp has no release for this computer yet.");
  }
  const asset = (
    r.assets as { name?: unknown; size?: unknown; browser_download_url?: unknown; digest?: unknown }[]
  ).find((a) => typeof a?.name === "string" && pattern.test(a.name));
  if (
    !asset ||
    typeof asset.browser_download_url !== "string" ||
    !asset.browser_download_url.startsWith(DOWNLOAD_PREFIX) ||
    typeof asset.size !== "number" ||
    asset.size <= 0 ||
    asset.size > MAX_ARCHIVE
  ) {
    throw new EmissaryError(`EmissaryRecomp ${r.tag_name} has no download for this computer.`);
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

/** `DolphinTool extract`'s progress line (`Extracting: <file> | 42%`), as a percent. */
export function parseExtractProgress(line: string): number | null {
  const m = /^Extracting: .* \| (\d{1,3})%\s*$/.exec(line.trim());
  return m ? Math.min(100, Number(m[1])) : null;
}

type Fetch = (url: string, init?: { signal?: AbortSignal; headers?: Record<string, string> }) => Promise<Response>;

type Current = { v: 1; version: string; programBytes: number; installedAt: string };
type GameFiles = { v: 1; iso: string; size: number; mtimeMs: number; bytes: number; extractedAt: string };

/** Runs `DolphinTool extract` (or a test double): resolves on success, rejects with its error output. */
export type ExtractFn = (
  tool: string,
  iso: string,
  out: string,
  onPercent: (percent: number) => void,
  signal: AbortSignal,
) => Promise<void>;

/** Unpacks an archive into `dir` (a test double replaces it). */
export type UnpackFn = (archive: string, dir: string, signal: AbortSignal) => Promise<void>;

export type EmissaryOptions = {
  /** `<netplay>/emissary-recomp`. */
  root: string;
  platform?: Platform;
  /** The Settings disc image, or null. */
  isoPath: () => string | null;
  /** The Settings disc is a valid NTSC-U Brawl image. */
  isoValid: () => boolean;
  /** The bundled DolphinTool. */
  dolphinTool: () => string;
  /** Install this tag instead of the latest (`PPO_EMISSARY_TAG`, to test updates). */
  tag?: string;
  fetch?: Fetch;
  extract?: ExtractFn;
  unpack?: UnpackFn;
  log?: (message: string) => void;
  onChange?: (state: EmissaryState) => void;
};

export class EmissaryRecomp {
  private readonly lock = new Mutex();
  private readonly fetchImpl: Fetch;
  private readonly log: (message: string) => void;
  private readonly platform: Platform;
  private release: EmissaryRelease | null = null;
  private releaseAt = 0;
  private releaseError: string | null = null;
  private job: EmissaryJob | null = null;
  private abort: AbortController | null = null;
  private lastError: string | null = null;
  private running = false;

  constructor(private readonly opts: EmissaryOptions) {
    this.fetchImpl = opts.fetch ?? ((url, init) => fetch(url, init));
    this.log = opts.log ?? (() => undefined);
    this.platform = opts.platform ?? process.platform;
  }

  get root(): string {
    return this.opts.root;
  }

  get supported(): boolean {
    return assetPattern(this.platform) != null;
  }

  /** The runtime's settings, saves and logs. */
  get userData(): string {
    return path.join(this.opts.root, "UserData");
  }

  /** The extracted disc (`sys/`, `files/`). */
  get gameFolder(): string {
    return path.join(this.opts.root, "game", "DATA");
  }

  private file(name: string): string {
    return path.join(this.opts.root, name);
  }

  private versionDir(version: string): string {
    return path.join(this.opts.root, "versions", version);
  }

  /** The program of the version that plays, or null. */
  async engine(): Promise<string | null> {
    const current = await this.readJson<Current>("current.json");
    if (!current) {
      return null;
    }
    const exe = path.join(this.versionDir(current.version), engineRelativePath(this.platform));
    return (await exists(exe)) ? exe : null;
  }

  /** Its game is open (or closed): nothing is removed or switched meanwhile; a staged version is switched to at close. */
  setRunning(running: boolean) {
    this.running = running;
    if (!running) {
      void this.applyStaged().catch((err) => this.log(`EmissaryRecomp: could not switch versions: ${err}`));
    }
    void this.emit();
  }

  private async readJson<T extends { v: 1 }>(name: string): Promise<T | null> {
    try {
      const v = JSON.parse(await fs.promises.readFile(this.file(name), "utf8"));
      return v && v.v === 1 ? (v as T) : null;
    } catch {
      return null;
    }
  }

  private async writeJson(name: string, value: object | null) {
    if (value == null) {
      await fs.promises.rm(this.file(name), { force: true });
      return;
    }
    await fs.promises.mkdir(this.opts.root, { recursive: true });
    await fs.promises.writeFile(`${this.file(name)}.tmp`, JSON.stringify(value, null, 2) + "\n");
    await fs.promises.rename(`${this.file(name)}.tmp`, this.file(name));
  }

  private async emit() {
    if (this.opts.onChange) {
      this.opts.onChange(await this.state());
    }
  }

  /** The latest release for this computer (asked again after 10 minutes, or with `refresh`); null when unknown. */
  async latest({ refresh = false }: { refresh?: boolean } = {}): Promise<EmissaryRelease | null> {
    if (!this.supported) {
      return null;
    }
    if (this.release && !refresh && Date.now() - this.releaseAt < RELEASE_TTL_MS) {
      return this.release;
    }
    const url = this.opts.tag ? `${EMISSARY_API}/tags/${encodeURIComponent(this.opts.tag)}` : `${EMISSARY_API}/latest`;
    try {
      const res = await this.fetchImpl(url, {
        headers: { Accept: "application/vnd.github+json", "User-Agent": "BrawlOnline-Launcher" },
      });
      if (!res.ok) {
        throw new EmissaryError(`GitHub answered HTTP ${res.status}`);
      }
      this.release = parseRelease(await res.json(), this.platform);
      this.releaseAt = Date.now();
      this.releaseError = null;
    } catch (err) {
      this.releaseError = err instanceof Error ? err.message : String(err);
      this.log(`EmissaryRecomp: could not read the latest release (${this.releaseError})`);
    }
    return this.release;
  }

  /** The extracted files are the Settings disc's. */
  private async gameFilesCurrent(files: GameFiles | null): Promise<boolean> {
    const iso = this.opts.isoPath();
    if (!files || !iso || files.iso !== iso) {
      return false;
    }
    const stat = await fs.promises.stat(iso).catch(() => null);
    return stat != null && stat.size === files.size && Math.trunc(stat.mtimeMs) === Math.trunc(files.mtimeMs);
  }

  async state({ refresh = false }: { refresh?: boolean } = {}): Promise<EmissaryState> {
    if (refresh) {
      await this.latest({ refresh: true });
    }
    const current = await this.readJson<Current>("current.json");
    const program = current && (await this.engine()) ? current : null;
    const staged = await this.readJson<Current>("staged.json");
    const files = await this.readJson<GameFiles>("game.json");
    const filesCurrent = await this.gameFilesCurrent(files);
    const discReady = this.opts.isoPath() != null && this.opts.isoValid();
    const latest = this.release;
    const newest = staged?.version ?? program?.version;
    const upToDate = program != null && (latest == null || latest.version === newest);

    let status: EmissaryStatus;
    if (!this.supported) {
      status = "unavailable";
    } else if (this.job) {
      status = this.job.phase === "gameFiles" ? "preparingFiles" : "downloading";
    } else if (this.running) {
      status = "running";
    } else if (!discReady && !(program && filesCurrent)) {
      status = "needsDisc";
    } else if (!program) {
      status = "notInstalled";
    } else if (!filesCurrent) {
      status = "needsFiles";
    } else if (!upToDate) {
      status = "updateAvailable";
    } else {
      status = "ready";
    }
    const archive = latest?.size ?? 0;
    return {
      status,
      folder: this.opts.root,
      supported: this.supported,
      installed: program
        ? { version: program.version, programBytes: program.programBytes, installedAt: program.installedAt }
        : null,
      gameFiles: files ? { bytes: files.bytes } : null,
      gameFilesCurrent: filesCurrent,
      discReady,
      latest,
      latestError: latest ? null : this.releaseError,
      upToDate,
      downloadBytes: upToDate ? 0 : archive,
      diskBytes:
        (program ? 0 : Math.round(archive * UNPACK_FACTOR)) + (filesCurrent ? 0 : files?.bytes ?? GAME_FILES_BYTES),
      staged: staged?.version ?? null,
      job: this.job,
      running: this.running,
      error: this.lastError,
    };
  }

  /** The program to start, or an error when it can't play yet (Play then uses Dolphin). */
  async engineForPlay(): Promise<string> {
    const s = await this.state();
    if (s.status !== "ready" && s.status !== "updateAvailable") {
      throw new EmissaryError(`EmissaryRecomp is not ready (${s.status}).`);
    }
    const exe = await this.engine();
    if (!exe) {
      throw new EmissaryError("EmissaryRecomp is not installed.");
    }
    return exe;
  }

  /** Stops the job that runs. */
  cancel() {
    this.abort?.abort();
  }

  /**
   * Brings everything up to date: the latest release when it is newer than the installed one (into
   * staged.json while the game is open), then the disc's files when they are not the Settings
   * disc's.
   */
  async install(): Promise<EmissaryState> {
    if (!this.supported) {
      throw new EmissaryError("EmissaryRecomp has no release for this computer yet.");
    }
    if (this.job) {
      throw new EmissaryError("EmissaryRecomp is already being downloaded.");
    }
    const abort = new AbortController();
    this.abort = abort;
    this.lastError = null;
    try {
      await this.lock.runExclusive(() => this._install(abort.signal));
    } catch (err) {
      if (err instanceof EmissaryCancelled) {
        this.log("EmissaryRecomp: cancelled");
      } else {
        this.lastError = err instanceof Error ? err.message : String(err);
        this.log(`EmissaryRecomp: ${this.lastError}`);
      }
      throw err;
    } finally {
      await removeTree(this.file("download"), { tries: 4, delayMs: 100 }).catch(() => undefined);
      this.job = null;
      this.abort = null;
      await this.emit();
    }
    return this.state();
  }

  private setJob(job: EmissaryJob | null) {
    this.job = job;
    void this.emit();
  }

  private async _install(signal: AbortSignal) {
    const check = () => {
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
    };
    const current = await this.readJson<Current>("current.json");
    const staged = await this.readJson<Current>("staged.json");
    const have = staged?.version ?? ((await this.engine()) ? current?.version : undefined);
    const release = await this.latest({ refresh: true });
    if (!release && !have) {
      throw new EmissaryError(
        `Could not reach GitHub to download EmissaryRecomp (${this.releaseError ?? "no answer"}).`,
      );
    }
    if (release && release.version !== have) {
      await this.installRelease(release, have ? "update" : "install", signal);
    }
    check();
    if (!this.opts.isoValid() || !this.opts.isoPath()) {
      if (!(await this.gameFilesCurrent(await this.readJson<GameFiles>("game.json")))) {
        throw new EmissaryError("Choose your Brawl disc image in Settings > Game first.");
      }
      return;
    }
    if (!(await this.gameFilesCurrent(await this.readJson<GameFiles>("game.json")))) {
      if (this.running) {
        throw new EmissaryError("Close EmissaryRecomp first: it plays the game files being replaced.");
      }
      await this.prepareGameFiles(signal);
    }
  }

  private async installRelease(release: EmissaryRelease, kind: "install" | "update", signal: AbortSignal) {
    const report = throttleProgress((c, t) => {
      if (this.job) {
        this.job = { ...this.job, current: c, total: t };
        void this.emit();
      }
    }, 150);
    this.setJob({ kind, phase: "download", current: 0, total: release.size });
    await fs.promises.mkdir(this.opts.root, { recursive: true });
    await ensureFreeSpace(this.opts.root, release.size + Math.round(release.size * UNPACK_FACTOR), "EmissaryRecomp");
    const archive = await this.download(release, signal, report);
    if (signal.aborted) {
      throw new EmissaryCancelled();
    }

    this.setJob({ kind, phase: "unpack", current: 0, total: 0 });
    const dest = this.versionDir(release.version);
    const partial = `${dest}.partial`;
    await removeTree(partial);
    await fs.promises.mkdir(partial, { recursive: true });
    try {
      await (this.opts.unpack ?? unpackArchive)(archive, partial, signal);
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
      const top = await findProgramRoot(partial, this.platform);
      if (!top) {
        throw new EmissaryError(
          `EmissaryRecomp ${release.version}'s download has no ${engineRelativePath(this.platform)}.`,
        );
      }
      // Only the game: the upstream launcher would update files under us, and the release's own
      // portable.txt and UserData would hide ours above it.
      for (const name of ["Brawl Recompiled.exe", "Brawl Recompiled"]) {
        await fs.promises.rm(path.join(top, name), { force: true });
      }
      await fs.promises.rm(path.join(top, "engine", "portable.txt"), { force: true });
      await removeTree(path.join(top, "engine", "UserData"));
      await removeTree(dest);
      await fs.promises.rename(top, dest);
    } catch (err) {
      throw await asDiskSpaceError(err, this.opts.root, "EmissaryRecomp");
    } finally {
      await removeTree(partial).catch(() => undefined);
    }
    if (this.platform !== "win32") {
      await fs.promises.chmod(path.join(dest, engineRelativePath(this.platform)), 0o755);
    }
    await this.ensurePortableRoot();
    const record: Current = {
      v: 1,
      version: release.version,
      programBytes: await folderBytes(dest),
      installedAt: new Date().toISOString(),
    };
    if (this.running) {
      await this.writeJson("staged.json", record);
      this.log(`EmissaryRecomp ${release.version} downloaded; it plays once the game is closed`);
    } else {
      await this.switchTo(record);
    }
  }

  /** Makes `record` the version that plays and deletes the others. */
  private async switchTo(record: Current) {
    const before = await this.readJson<Current>("current.json");
    await this.writeJson("current.json", record);
    await this.writeJson("staged.json", null);
    const versions = path.join(this.opts.root, "versions");
    for (const name of await fs.promises.readdir(versions).catch(() => [] as string[])) {
      if (name !== record.version) {
        await removeTree(path.join(versions, name)).catch(() => undefined);
      }
    }
    this.log(
      `EmissaryRecomp ${record.version} ${
        before && before.version !== record.version ? `replaced ${before.version}` : "installed"
      }`,
    );
  }

  private async applyStaged() {
    await this.lock.runExclusive(async () => {
      const staged = await this.readJson<Current>("staged.json");
      if (staged && !this.running) {
        await this.switchTo(staged);
      }
    });
    await this.emit();
  }

  private async ensurePortableRoot() {
    await fs.promises.mkdir(this.userData, { recursive: true });
    await fs.promises.writeFile(this.file("portable.txt"), PORTABLE_TEXT);
  }

  private async download(release: EmissaryRelease, signal: AbortSignal, onProgress: ByteProgress): Promise<string> {
    const file = path.join(this.file("download"), path.basename(release.assetName).replace(/[^\w.+-]/g, "_"));
    await fs.promises.mkdir(path.dirname(file), { recursive: true });
    let res: Response;
    try {
      res = await this.fetchImpl(release.url, { signal, headers: { "User-Agent": "BrawlOnline-Launcher" } });
    } catch (err) {
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
      throw new EmissaryError(`Could not download EmissaryRecomp (${err instanceof Error ? err.message : err})`);
    }
    if (!res.ok || !res.body) {
      throw new EmissaryError(`Could not download EmissaryRecomp: HTTP ${res.status}`);
    }
    const hash = createHash("sha256");
    const out = fs.createWriteStream(file);
    let got = 0;
    const reader = res.body.getReader();
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) {
          break;
        }
        got += value.length;
        if (got > release.size) {
          throw new EmissaryError("EmissaryRecomp's download is larger than expected.");
        }
        hash.update(value);
        if (!out.write(value)) {
          await new Promise<void>((resolve) => out.once("drain", () => resolve()));
        }
        onProgress(got, release.size);
      }
    } catch (err) {
      out.destroy();
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
      throw await asDiskSpaceError(err, this.opts.root, "EmissaryRecomp");
    }
    await new Promise<void>((resolve, reject) => out.end((err?: Error | null) => (err ? reject(err) : resolve())));
    if (got !== release.size || (release.sha256 && hash.digest("hex") !== release.sha256)) {
      throw new EmissaryError("EmissaryRecomp's download is damaged (its size or sha256 does not match). Try again.");
    }
    return file;
  }

  /** Extracts the Settings disc's game partition into game/DATA. */
  private async prepareGameFiles(signal: AbortSignal) {
    const iso = this.opts.isoPath();
    if (!iso) {
      throw new EmissaryError("Choose your Brawl disc image in Settings > Game first.");
    }
    const stat = await fs.promises.stat(iso).catch(() => null);
    if (!stat) {
      throw new EmissaryError(`The disc image ${iso} was not found.`);
    }
    this.setJob({ kind: "gameFiles", phase: "gameFiles", current: 0, total: 100 });
    const gameRoot = path.join(this.opts.root, "game");
    const partial = `${gameRoot}.partial`;
    await this.writeJson("game.json", null);
    await removeTree(partial);
    // The old files go first: a new disc's need the same room.
    await removeTree(gameRoot);
    await ensureFreeSpace(this.opts.root, GAME_FILES_BYTES, "Brawl's game files");
    try {
      await (this.opts.extract ?? runExtract)(
        this.opts.dolphinTool(),
        iso,
        partial,
        (percent) => {
          if (this.job && this.job.current !== percent) {
            this.job = { ...this.job, current: percent };
            void this.emit();
          }
        },
        signal,
      );
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
      if (!(await exists(path.join(partial, "DATA", "sys", "main.dol")))) {
        throw new EmissaryError("The disc image's game files could not be read (no sys/main.dol).");
      }
      await fs.promises.rename(partial, gameRoot);
    } catch (err) {
      await removeTree(partial).catch(() => undefined);
      throw await asDiskSpaceError(err, this.opts.root, "Brawl's game files", { needBytes: GAME_FILES_BYTES });
    }
    const files: GameFiles = {
      v: 1,
      iso,
      size: stat.size,
      mtimeMs: stat.mtimeMs,
      bytes: await folderBytes(gameRoot),
      extractedAt: new Date().toISOString(),
    };
    await this.writeJson("game.json", files);
    this.log(`EmissaryRecomp: extracted ${iso}'s game files (${files.bytes} bytes) to ${gameRoot}`);
  }

  /** Deletes the program, the disc's files, its settings and saves. */
  async remove(): Promise<void> {
    if (this.running) {
      throw new EmissaryError("Close EmissaryRecomp first.");
    }
    if (this.job) {
      throw new EmissaryError("EmissaryRecomp is being downloaded. Cancel that first.");
    }
    await this.lock.runExclusive(async () => {
      await removeTree(this.opts.root);
    });
    this.lastError = null;
    this.log(`EmissaryRecomp: removed ${this.opts.root}`);
    await this.emit();
  }
}

async function exists(p: string): Promise<boolean> {
  return fs.promises.stat(p).then(
    () => true,
    () => false,
  );
}

/** The folder holding `engine/brawl-engine[.exe]`: the archive's top folder (`BrawlRecompiled/`) or itself. */
export async function findProgramRoot(dir: string, platform: Platform): Promise<string | null> {
  const rel = engineRelativePath(platform);
  if (await exists(path.join(dir, rel))) {
    return dir;
  }
  for (const entry of await fs.promises.readdir(dir, { withFileTypes: true })) {
    if (entry.isDirectory() && (await exists(path.join(dir, entry.name, rel)))) {
      return path.join(dir, entry.name);
    }
  }
  return null;
}

export async function folderBytes(dir: string): Promise<number> {
  let total = 0;
  for (const entry of await fs.promises.readdir(dir, { withFileTypes: true }).catch(() => [] as fs.Dirent[])) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      total += await folderBytes(p);
    } else if (entry.isFile()) {
      total += (await fs.promises.stat(p).catch(() => ({ size: 0 }))).size;
    }
  }
  return total;
}

/** A zip with node-stream-zip (it refuses entries outside `dir`), a .tar.gz with the system's tar. */
export const unpackArchive: UnpackFn = async (archive, dir, signal) => {
  if (/\.zip$/i.test(archive)) {
    const zip = new AsyncStreamZip({ file: archive, storeEntries: true });
    try {
      await zip.extract(null, dir);
    } finally {
      await zip.close().catch(() => undefined);
    }
    return;
  }
  await new Promise<void>((resolve, reject) => {
    const child = spawn("tar", ["-xzf", archive, "-C", dir], { windowsHide: true, signal });
    let stderr = "";
    child.stderr.on("data", (d) => (stderr += String(d)));
    child.on("error", reject);
    child.on("close", (code) =>
      code === 0
        ? resolve()
        : reject(new EmissaryError(`tar could not unpack EmissaryRecomp: ${stderr.trim() || code}`)),
    );
  });
};

/** `DolphinTool extract -g`: only the game partition, as `<out>/DATA/{sys,files}`. */
export const runExtract: ExtractFn = (tool, iso, out, onPercent, signal) =>
  new Promise<void>((resolve, reject) => {
    const child = spawn(tool, ["extract", "-i", iso, "-o", out, "-g"], { windowsHide: true, signal });
    let pending = "";
    let lastError = "";
    child.stderr.on("data", (d) => {
      pending += String(d);
      const lines = pending.split(/\r?\n/);
      pending = lines.pop() ?? "";
      for (const line of lines) {
        const percent = parseExtractProgress(line);
        if (percent != null) {
          onPercent(percent);
        } else if (line.trim()) {
          lastError = line.trim();
        }
      }
    });
    child.stdout.resume();
    child.on("error", (err) => reject(signal.aborted ? new EmissaryCancelled() : err));
    child.on("close", (code) => {
      if (signal.aborted) {
        reject(new EmissaryCancelled());
      } else if (code === 0) {
        resolve();
      } else {
        reject(new EmissaryError(`DolphinTool could not extract the disc: ${lastError || `exit ${code}`}`));
      }
    });
  });
