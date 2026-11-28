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
//   versions/<tag>/             a release's program: CI's builds have `WiiCompiled[.exe]` with its
//                               DLLs, Sys/ and wii_bootstrap/ at the top; upstream's v0.6.0 had
//                               `engine/brawl-engine[.exe]` (it plays no one online). Without the
//                               upstream launcher (whose updater would replace files under us) and
//                               without the release's own portable.txt and UserData. On Windows,
//                               engine_usa_v2.dll (the engine for processors without AVX2) only
//                               when this computer's loader names it (missingEngine): releases
//                               since the split ship it in -Windows-OldCPU.zip
//   current.json                the version that plays (its program, its manifest, what it can do)
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
import { Readable } from "stream";
import type { ByteProgress } from "utils/copy_file";
import { throttleProgress } from "utils/copy_file";
import { fetchErrorMessage, fetchRetrying } from "utils/fetch_error";
import { moveTree } from "utils/move_tree";
import { removeTree } from "utils/remove_tree";

import { asDiskSpaceError, ensureFreeSpace } from "../install/disk_space";
import { unlinkBrawlSave, unlinkShared } from "./launch";
import type {
  EmissaryAsset,
  EmissaryFallback,
  EmissaryJob,
  EmissaryManifest,
  EmissaryRelease,
  EmissaryState,
  EmissaryStatus,
} from "./types";
import { EMISSARY_PROTOCOL, EMISSARY_REPO } from "./types";

export const EMISSARY_FOLDER = "emissary-recomp";
/**
 * `<emissary-recomp>/playback/`: the program that plays replays and spectated games, beside the one
 * Play runs (prepareForPlayback). Its own portable.txt and UserData (the runtime finds portable.txt
 * above its program), so it never touches the NAND, card or settings of a game that is open.
 */
export const EMISSARY_PLAYBACK_FOLDER = "playback";
/** In a playback version folder once every file of the release is linked in. */
const MIRROR_DONE = ".brawl-online-linked";
export const EMISSARY_API = `https://api.github.com/repos/${EMISSARY_REPO}/releases`;
const DOWNLOAD_PREFIX = `https://github.com/${EMISSARY_REPO}/releases/download/`;

const MAX_ARCHIVE = 1024 * 1024 * 1024;
const RELEASE_TTL_MS = 10 * 60 * 1000;
/**
 * Play asks for the latest release again when it was last asked longer ago than this (as the
 * launcher's own update check before Dolphin starts, app_updater.ts), and waits for the answer at
 * most PLAY_CHECK_TIMEOUT_MS: past it (offline, GitHub down) the installed release plays.
 */
export const PLAY_CHECK_MAX_AGE_MS = 60 * 1000;
export const PLAY_CHECK_TIMEOUT_MS = 5000;
const MAX_MANIFEST_BYTES = 64 * 1024;
/** The game partition of NTSC-U Brawl, extracted (6.8 GB). */
export const GAME_FILES_BYTES = 7_300_000_000;
/** An unpacked release is about twice its archive (two engines of ~180 MB, compressed ~2:1). */
const UNPACK_FACTOR = 2.2;

const PORTABLE_TEXT =
  "Brawl Online's EmissaryRecomp: settings, saves and logs stay in the UserData folder next to this file.\n";

export type Platform = "win32" | "linux" | "darwin" | string;

/** The Windows engine for processors without AVX2/FMA (x86-64-v2). */
export const OLD_CPU_ENGINE = "engine_usa_v2.dll";
/**
 * The download holding only `EmissaryRecomp/engine_usa_v2.dll` (EmissaryRecomp's tools/release.py,
 * since the split). Its name doesn't end in -Windows.zip (assetPattern).
 */
const OLD_CPU_ASSET = /-Windows-OldCPU\.zip$/i;
/** In a loader that answers `--engine` (older ones would start the game instead). */
const ENGINE_QUERY = "--engine";
/** How long `WiiCompiled.exe --engine` may take (an antivirus scans a new program first). */
const ENGINE_QUERY_TIMEOUT_MS = 20_000;

/**
 * The release archive for `platform`, or null. `arch` is the machine's (an x64 launcher on Apple
 * silicon runs under Rosetta): the macOS build is for Apple silicon only, so an Intel Mac has none.
 */
export function assetPattern(platform: Platform, arch: string = process.arch): RegExp | null {
  switch (platform) {
    case "win32":
      return /-Windows\.zip$/i;
    case "linux":
      return /-Linux\.tar\.gz$/i;
    case "darwin":
      return arch === "arm64" ? /-macOS\.tar\.gz$/i : null;
    default:
      return null;
  }
}

/** The program inside a version folder of upstream's v0.6.0 layout. */
export function engineRelativePath(platform: Platform): string {
  return path.join("engine", platform === "win32" ? "brawl-engine.exe" : "brawl-engine");
}

/**
 * The program inside a version folder of CI's builds (with Brawl Online's core). On macOS it is
 * inside Emissary.app (the Dock, the menu bar and Cmd-Tab show "Emissary" and its icon), whose
 * program uses the files beside the bundle.
 */
export function wiiCompiledRelativePath(platform: Platform): string {
  switch (platform) {
    case "win32":
      return "WiiCompiled.exe";
    case "darwin":
      return path.join("Emissary.app", "Contents", "MacOS", "Emissary");
    default:
      return "WiiCompiled";
  }
}

/** Where a release's program may be, the newer layout first (a macOS build without its app bundle: WiiCompiled). */
export function programRelativePaths(platform: Platform): string[] {
  return [
    wiiCompiledRelativePath(platform),
    ...(platform === "darwin" ? ["WiiCompiled"] : []),
    engineRelativePath(platform),
  ];
}

/** `manifest.json` (EmissaryManifest), or null when it is not one. */
export function parseManifest(v: unknown): EmissaryManifest | null {
  const m = v as { version?: unknown; brawlOnline?: unknown; protocol?: unknown };
  if (
    !m ||
    typeof m !== "object" ||
    typeof m.version !== "string" ||
    typeof m.brawlOnline !== "string" ||
    typeof m.protocol !== "number" ||
    !Number.isInteger(m.protocol)
  ) {
    return null;
  }
  return { version: m.version.slice(0, 40), brawlOnline: m.brawlOnline.slice(0, 64), protocol: m.protocol };
}

export class EmissaryError extends Error {}

/** Cancel. */
export class EmissaryCancelled extends EmissaryError {
  constructor() {
    super("Cancelled");
  }
}

/** A release as GitHub's API describes it (`/releases/latest`, `/releases/tags/<tag>`), its archive for `platform`. */
export function parseRelease(v: unknown, platform: Platform, arch: string = process.arch): EmissaryRelease {
  const r = v as { tag_name?: unknown; assets?: unknown };
  if (!r || typeof r.tag_name !== "string" || !/^[\w.+-]{1,40}$/.test(r.tag_name) || !Array.isArray(r.assets)) {
    throw new EmissaryError("GitHub sent no EmissaryRecomp release.");
  }
  const pattern = assetPattern(platform, arch);
  if (!pattern) {
    throw new EmissaryError("EmissaryRecomp has no release for this computer yet.");
  }
  const assets = r.assets as { name?: unknown; size?: unknown; browser_download_url?: unknown; digest?: unknown }[];
  /** The asset whose name matches, when it is one to download; undefined: none; null: not a valid one. */
  const assetOf = (re: RegExp): EmissaryAsset | null | undefined => {
    const asset = assets.find((a) => typeof a?.name === "string" && re.test(a.name));
    if (!asset) {
      return undefined;
    }
    if (
      typeof asset.browser_download_url !== "string" ||
      !asset.browser_download_url.startsWith(DOWNLOAD_PREFIX) ||
      typeof asset.size !== "number" ||
      asset.size <= 0 ||
      asset.size > MAX_ARCHIVE
    ) {
      return null;
    }
    const digest = typeof asset.digest === "string" ? /^sha256:([0-9a-f]{64})$/i.exec(asset.digest) : null;
    return {
      assetName: String(asset.name),
      url: asset.browser_download_url,
      size: asset.size,
      sha256: digest ? digest[1].toLowerCase() : null,
    };
  };
  const asset = assetOf(pattern);
  if (!asset) {
    throw new EmissaryError(`EmissaryRecomp ${r.tag_name} has no download for this computer.`);
  }
  const manifest = (r.assets as { name?: unknown; browser_download_url?: unknown }[]).find(
    (a) =>
      a?.name === "manifest.json" &&
      typeof a.browser_download_url === "string" &&
      a.browser_download_url.startsWith(DOWNLOAD_PREFIX),
  );
  return {
    version: r.tag_name,
    ...asset,
    oldCpu: platform === "win32" ? assetOf(OLD_CPU_ASSET) ?? null : null,
    manifestUrl: manifest ? String(manifest.browser_download_url) : null,
    manifest: null,
  };
}

/** Asks a loader which engine it starts on this computer (askEngine, or a test double). */
export type EngineQuery = (exe: string) => Promise<string | null>;

/**
 * `WiiCompiled.exe --engine`: the engine file the loader starts on this computer
 * (`engine_usa.dll`, or `engine_usa_v2.dll` without AVX2), decided by the same check the engines
 * make of the processor themselves; null when it gives no answer. Only for loaders that answer it
 * (missingEngine): an older one would start the game.
 */
export const askEngine: EngineQuery = (exe) =>
  new Promise((resolve) => {
    let out = "";
    let child;
    try {
      child = spawn(exe, [ENGINE_QUERY], {
        cwd: path.dirname(exe),
        windowsHide: true,
        timeout: ENGINE_QUERY_TIMEOUT_MS,
        stdio: ["ignore", "pipe", "ignore"],
      });
    } catch {
      resolve(null);
      return;
    }
    child.stdout.on("data", (d) => {
      out += String(d);
      if (out.length > 256) {
        child.kill();
      }
    });
    child.on("error", () => resolve(null));
    child.on("close", (code) => {
      const name = out.trim();
      resolve(code === 0 && /^engine_[a-z]+(_v2)?\.dll$/i.test(name) ? name : null);
    });
  });

/** Compares release tags by their numbers (`v0.7.10` after `v0.7.9`). */
export function compareTags(a: string, b: string): number {
  const nums = (t: string) => (t.match(/\d+/g) ?? []).map(Number);
  const x = nums(a);
  const y = nums(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] ?? -1) - (y[i] ?? -1);
    if (d !== 0) {
      return d;
    }
  }
  return a.localeCompare(b);
}

/**
 * A release feed from a folder, for testing (`PPO_EMISSARY_RELEASES`): `<dir>/<tag>/` holds a
 * release's assets (`EmissaryRecomp-<tag>-Windows.zip`, `manifest.json`). Answers GitHub's API
 * (`/releases/latest`: the highest tag; `/releases/tags/<tag>`) and its download URLs, so the
 * add-on installs and updates from it exactly as from GitHub.
 */
export function localReleasesFetch(dir: string): Fetch {
  const releaseOf = async (tag: string) => {
    const names = await fs.promises.readdir(path.join(dir, tag)).catch(() => [] as string[]);
    const assets = [];
    for (const name of names) {
      const stat = await fs.promises.stat(path.join(dir, tag, name)).catch(() => null);
      if (stat?.isFile()) {
        assets.push({ name, size: stat.size, browser_download_url: `${DOWNLOAD_PREFIX}${tag}/${name}` });
      }
    }
    return { tag_name: tag, assets };
  };
  return async (url) => {
    if (url === `${EMISSARY_API}/latest` || url.startsWith(`${EMISSARY_API}/tags/`)) {
      let tag = url.startsWith(`${EMISSARY_API}/tags/`)
        ? decodeURIComponent(url.slice(`${EMISSARY_API}/tags/`.length))
        : null;
      if (!tag) {
        const entries = await fs.promises.readdir(dir, { withFileTypes: true }).catch(() => [] as fs.Dirent[]);
        tag =
          entries
            .filter((e) => e.isDirectory())
            .map((e) => e.name)
            .sort(compareTags)
            .pop() ?? null;
      }
      if (!tag || !/^[\w.+-]{1,40}$/.test(tag) || !fs.existsSync(path.join(dir, tag))) {
        return new Response("", { status: 404 });
      }
      return new Response(JSON.stringify(await releaseOf(tag)), { status: 200 });
    }
    if (url.startsWith(DOWNLOAD_PREFIX)) {
      const [tag, name] = url.slice(DOWNLOAD_PREFIX.length).split("/");
      const file = path.join(dir, path.basename(tag ?? ""), path.basename(name ?? ""));
      if (!fs.existsSync(file)) {
        return new Response("", { status: 404 });
      }
      return new Response(Readable.toWeb(fs.createReadStream(file)) as ReadableStream, { status: 200 });
    }
    return new Response("", { status: 404 });
  };
}

/** `DolphinTool extract`'s progress line (`Extracting: <file> | 42%`), as a percent. */
export function parseExtractProgress(line: string): number | null {
  const m = /^Extracting: .* \| (\d{1,3})%\s*$/.exec(line.trim());
  return m ? Math.min(100, Number(m[1])) : null;
}

type Fetch = (url: string, init?: { signal?: AbortSignal; headers?: Record<string, string> }) => Promise<Response>;

type Current = {
  v: 1;
  version: string;
  programBytes: number;
  installedAt: string;
  /** What the release can do, read from its program (releaseFeatures); absent in records from before. */
  features?: EmissaryFeatures;
  /** The program, relative to the version folder (programRelativePaths); absent: v0.6.0's layout. */
  program?: string;
  /** Its manifest.json; absent for releases without one. */
  manifest?: EmissaryManifest;
};

/**
 * What a release supports, found by the switches its program reads. Play needs both: without the
 * read-only card the game would write into the online card the netplay Dolphin also boots, and
 * without Brawl Online's core (BRAWL_ONLINE_CORE: its online code, in-process) it plays no one.
 * EmissaryRecomp v0.6.0 has neither. (Records from before the core say `online`, for kalomaze's
 * companion, which is gone: they count as without it.)
 */
export type EmissaryFeatures = {
  readOnlyCard: boolean;
  core: boolean;
  /**
   * It plays a replay or a spectated game itself (BRAWL_ONLINE_REPLAY_COMM: the playback Dolphin's
   * `-i <comm file>`, docs/design/brawl-online-core.md §12). Absent in records from before it was read.
   */
  playback?: boolean;
};

const FEATURE_MARKERS: Record<keyof EmissaryFeatures, string> = {
  readOnlyCard: "BRAWL_SD_VOLATILE_WRITES",
  core: "BRAWL_ONLINE_CORE",
  playback: "BRAWL_ONLINE_REPLAY_COMM",
};

/** Whether `file` holds each of `markers` (read in chunks: the program is about 240 MB). */
async function fileHolds(file: string, markers: string[]): Promise<boolean[]> {
  const found = markers.map(() => false);
  const needles = markers.map((m) => Buffer.from(m, "latin1"));
  const overlap = Math.max(...needles.map((n) => n.length)) - 1;
  let tail = Buffer.alloc(0);
  try {
    for await (const chunk of fs.createReadStream(file, { highWaterMark: 4 * 1024 * 1024 })) {
      const data = Buffer.concat([tail, chunk as Buffer]);
      needles.forEach((n, i) => {
        found[i] ||= data.includes(n);
      });
      if (found.every(Boolean)) {
        break;
      }
      tail = data.subarray(Math.max(0, data.length - overlap));
    }
  } catch {
    // unreadable: none found
  }
  return found;
}

/** Scans the program's files (CI's WiiCompiled, v0.6.0's engine DLLs or Linux program) for the feature switches' names. */
export async function releaseFeatures(programDir: string): Promise<EmissaryFeatures> {
  const found: Required<EmissaryFeatures> = { readOnlyCard: false, core: false, playback: false };
  const names = await fs.promises.readdir(programDir).catch(() => [] as string[]);
  const targets = names.filter((n) =>
    // Emissary: the macOS program, in Emissary.app/Contents/MacOS.
    /^(WiiCompiled(\.exe)?|Emissary|engine_usa.*\.dll|brawl-engine(\.exe)?|libengine.*\.so)$/i.test(n),
  );
  const keys = Object.keys(FEATURE_MARKERS) as (keyof Required<EmissaryFeatures>)[];
  for (const name of targets) {
    const holds = await fileHolds(
      path.join(programDir, name),
      keys.map((k) => FEATURE_MARKERS[k]),
    );
    keys.forEach((k, i) => {
      found[k] ||= holds[i];
    });
  }
  return found;
}

/** The record plays online with this launcher: the read-only card, the core, and (with a manifest) our protocol. */
function playsOnline(record: Current | null): boolean {
  return (
    record?.features?.readOnlyCard === true &&
    record.features.core === true &&
    (!record.manifest || record.manifest.protocol === EMISSARY_PROTOCOL)
  );
}
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
  /** The machine's architecture (default process.arch; arm64 for an x64 launcher under Rosetta). */
  arch?: string;
  /** The Settings disc image, or null. */
  isoPath: () => string | null;
  /** The Settings disc is a valid NTSC-U Brawl image. */
  isoValid: () => boolean;
  /**
   * Checks the Settings disc again (isoValid answers from the last check, which runs in the
   * background at start-up and after the disc changes). Play waits for it: Play refuses a missing
   * or invalid disc before it gets here, so "needs your disc" then only means "not checked yet".
   */
  checkDisc?: () => Promise<void>;
  /** The bundled DolphinTool. */
  dolphinTool: () => string;
  /** Install this tag instead of the latest (`PPO_EMISSARY_TAG`, to test updates). */
  tag?: string;
  fetch?: Fetch;
  extract?: ExtractFn;
  unpack?: UnpackFn;
  /** Asks a loader for its engine (askEngine by default). */
  askEngine?: EngineQuery;
  log?: (message: string) => void;
  onChange?: (state: EmissaryState) => void;
};

export class EmissaryRecomp {
  private readonly lock = new Mutex();
  private readonly fetchImpl: Fetch;
  private readonly log: (message: string) => void;
  private readonly platform: Platform;
  private readonly arch: string;
  private release: EmissaryRelease | null = null;
  private releaseAt = 0;
  private releaseError: string | null = null;
  private job: EmissaryJob | null = null;
  private abort: AbortController | null = null;
  private lastError: string | null = null;
  private running = false;
  /** The install() that runs (Play waits for it). */
  private installing: Promise<EmissaryState> | null = null;
  /** Play's progress (prepareForPlay), while it waits for a download. */
  private onJob: ((job: EmissaryJob) => void) | null = null;
  /** A replay or spectated game plays in the playback program (prepareForPlayback). */
  private playbackRunning = false;
  /** Whether a version plays replays, for records from before current.json said (prepareForPlayback). */
  private readonly playbackFeature = new Map<string, boolean>();
  /**
   * The engine this computer's loader names (missingEngine), asked once while the launcher runs:
   * never written down, so an install moved to another computer is asked again there.
   */
  private engineAnswer: string | null = null;

  constructor(private readonly opts: EmissaryOptions) {
    this.fetchImpl = opts.fetch ?? ((url, init) => fetch(url, init));
    this.log = opts.log ?? (() => undefined);
    this.platform = opts.platform ?? process.platform;
    this.arch = opts.arch ?? process.arch;
  }

  get root(): string {
    return this.opts.root;
  }

  get supported(): boolean {
    return assetPattern(this.platform, this.arch) != null;
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
    const exe = path.join(this.versionDir(current.version), current.program ?? engineRelativePath(this.platform));
    return (await exists(exe)) ? exe : null;
  }

  /** Its game is open. */
  get isRunning(): boolean {
    return this.running;
  }

  /** A newer version is downloaded and waits for the game to close (staged.json). */
  async stagedVersion(): Promise<string | null> {
    return (await this.readJson<Current>("staged.json"))?.version ?? null;
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
      const res = await fetchRetrying(
        this.fetchImpl,
        url,
        { headers: { Accept: "application/vnd.github+json", "User-Agent": "BrawlOnline-Launcher" } },
        (err) => this.log(`EmissaryRecomp: no answer from GitHub, asking again (${fetchErrorMessage(err)})`),
      );
      if (!res.ok) {
        throw new EmissaryError(`GitHub answered HTTP ${res.status}`);
      }
      const release = parseRelease(await res.json(), this.platform, this.arch);
      release.manifest = await this.readManifest(release);
      this.release = release;
      this.releaseAt = Date.now();
      this.releaseError = null;
    } catch (err) {
      this.releaseError = fetchErrorMessage(err);
      this.log(`EmissaryRecomp: could not read the latest release (${this.releaseError})`);
    }
    return this.release;
  }

  /** The release `version` (the latest one when it is that one); null when GitHub doesn't say. */
  private async releaseOf(version: string): Promise<EmissaryRelease | null> {
    const latest = await this.latest();
    if (latest?.version === version) {
      return latest;
    }
    try {
      const res = await fetchRetrying(
        this.fetchImpl,
        `${EMISSARY_API}/tags/${encodeURIComponent(version)}`,
        { headers: { Accept: "application/vnd.github+json", "User-Agent": "BrawlOnline-Launcher" } },
        (err) => this.log(`EmissaryRecomp: no answer from GitHub, asking again (${fetchErrorMessage(err)})`),
      );
      if (!res.ok) {
        throw new EmissaryError(`GitHub answered HTTP ${res.status}`);
      }
      return parseRelease(await res.json(), this.platform, this.arch);
    } catch (err) {
      this.log(`EmissaryRecomp ${version}: could not read its release (${fetchErrorMessage(err)})`);
      return null;
    }
  }

  /** A release's manifest.json (the same one again for the same release); null without one or when unreadable. */
  private async readManifest(release: EmissaryRelease): Promise<EmissaryManifest | null> {
    if (!release.manifestUrl) {
      return null;
    }
    if (this.release?.version === release.version && this.release.manifest) {
      return this.release.manifest;
    }
    try {
      const res = await this.fetchImpl(release.manifestUrl, { headers: { "User-Agent": "BrawlOnline-Launcher" } });
      const text = res.ok ? await res.text() : "";
      const manifest = text.length <= MAX_MANIFEST_BYTES ? parseManifest(JSON.parse(text)) : null;
      if (!manifest) {
        this.log(`EmissaryRecomp ${release.version}: its manifest.json could not be read (HTTP ${res.status})`);
      }
      return manifest;
    } catch (err) {
      this.log(`EmissaryRecomp ${release.version}: its manifest.json could not be read (${fetchErrorMessage(err)})`);
      return null;
    }
  }

  /** The release can play with this launcher, as far as its manifest says (one without a manifest may). */
  private static manifestFits(release: EmissaryRelease): boolean {
    return !release.manifest || release.manifest.protocol === EMISSARY_PROTOCOL;
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
    // A latest release made for another launcher protocol is not an update for this launcher.
    const upToDate =
      program != null && (latest == null || latest.version === newest || !EmissaryRecomp.manifestFits(latest));

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
        ? {
            version: program.version,
            programBytes: program.programBytes,
            installedAt: program.installedAt,
            playsOnline: playsOnline(program),
            brawlOnline: program.manifest?.brawlOnline ?? null,
          }
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
    if (!playsOnline(await this.readJson<Current>("current.json"))) {
      throw new EmissaryError(`EmissaryRecomp ${s.installed?.version} can't play online yet.`);
    }
    return exe;
  }

  /**
   * Play with EmissaryRecomp (manager._launchEmissary), the way the launcher's own update gates a
   * Dolphin start (app_updater.ts assertUpToDate): the latest release is asked for again when the
   * last answer is older than a minute (waiting at most 5 s; offline, the installed release plays),
   * a release downloaded while the game ran is switched to, and a newer one is downloaded now
   * (`onJob`: its progress, for the Play button) before the game starts. Only ever before a start:
   * a running game is never stopped for an update (it is staged and played next time, setup.ts).
   * Resolves to the program to start, or to why Dolphin plays instead.
   */
  async prepareForPlay(
    onJob?: (job: EmissaryJob) => void,
  ): Promise<{ exe: string; version: string } | { fallback: EmissaryFallback }> {
    if (!this.supported) {
      return { fallback: { reason: "status", status: "unavailable" } };
    }
    if (Date.now() - this.releaseAt > PLAY_CHECK_MAX_AGE_MS) {
      let timer: NodeJS.Timeout | undefined;
      const timeout = new Promise<void>((resolve) => {
        timer = setTimeout(resolve, PLAY_CHECK_TIMEOUT_MS);
      });
      await Promise.race([this.latest({ refresh: true }), timeout]);
      clearTimeout(timer);
    }
    // A download the auto-update started: Play waits for it, with its progress.
    this.onJob = onJob ?? null;
    try {
      if (this.installing) {
        await this.installing.catch(() => undefined);
      }
      if (!this.running) {
        await this.applyStaged();
      }
      let s = await this.state();
      if (s.status === "needsDisc" && this.opts.checkDisc) {
        await this.opts.checkDisc();
        s = await this.state();
      }
      if (s.status === "updateAvailable") {
        this.log(
          `EmissaryRecomp: ${s.installed?.version} is not the latest (${s.latest?.version}): updating before playing`,
        );
        try {
          s = await this.install();
        } catch (err) {
          const message = err instanceof Error ? err.message : String(err);
          this.log(`EmissaryRecomp: the update failed, Dolphin plays (${message})`);
          return { fallback: { reason: "updateFailed", message } };
        }
      }
      if (s.status === "ready") {
        try {
          await this.ensureEngine({ reinstall: true });
        } catch (err) {
          const message = err instanceof Error ? err.message : String(err);
          this.log(`EmissaryRecomp: its engine for this computer could not be downloaded, Dolphin plays (${message})`);
          return { fallback: { reason: "updateFailed", message } };
        }
        s = await this.state();
      }
      if (s.status !== "ready") {
        return { fallback: { reason: "status", status: s.status } };
      }
      const exe = await this.engine();
      if (!exe || !s.installed) {
        return { fallback: { reason: "status", status: "notInstalled" } };
      }
      if (!s.installed.playsOnline) {
        return { fallback: { reason: "noOnline" } };
      }
      return { exe, version: s.installed.version };
    } finally {
      this.onJob = null;
    }
  }

  /** The playback program's portable root (EMISSARY_PLAYBACK_FOLDER). */
  get playbackRoot(): string {
    return path.join(this.opts.root, EMISSARY_PLAYBACK_FOLDER);
  }

  /** The playback program's settings, saves and logs. */
  get playbackUserData(): string {
    return path.join(this.playbackRoot, "UserData");
  }

  /** A replay or spectated game plays in it: not removed meanwhile (setPlaybackRunning). */
  setPlaybackRunning(running: boolean) {
    this.playbackRunning = running;
  }

  /**
   * The program for a replay, highlights or a spectated game when Play plays with EmissaryRecomp
   * (manager._launchPlaybackDolphin), or why the playback Dolphin plays instead. The installed
   * version, never a download first (a replay doesn't wait for one; Play updates it), and also while
   * Play's game is open: it runs from `playback/` (hard links of that version's files, with a
   * portable root of its own), so two of them never share a UserData.
   */
  async prepareForPlayback(): Promise<{ exe: string; version: string } | { why: string }> {
    if (!this.supported) {
      return { why: "it isn't made for this computer" };
    }
    if (this.job?.phase === "gameFiles") {
      return { why: "its game files are being prepared" };
    }
    const current = await this.readJson<Current>("current.json");
    const exe = await this.engine();
    if (!current || !exe) {
      return { why: "it isn't installed" };
    }
    if (!(await this.gameFilesCurrent(await this.readJson<GameFiles>("game.json")))) {
      return { why: "its game files aren't the Settings disc's" };
    }
    if (!playsOnline(current)) {
      return { why: `${current.version} can't play online` };
    }
    const programDir = path.dirname(exe);
    let playback = current.features?.playback ?? this.playbackFeature.get(current.version);
    if (playback === undefined) {
      // Records from before the switch was read: read now, once.
      playback = (await releaseFeatures(programDir)).playback === true;
      this.playbackFeature.set(current.version, playback);
    }
    if (!playback) {
      return { why: `${current.version} can't play replays` };
    }
    const versionDir = this.versionDir(current.version);
    const program = path.relative(versionDir, exe);
    const need = await this.missingEngine(versionDir, program);
    if (need) {
      // Not waited for (Play downloads it before its start): this time the playback Dolphin plays.
      if (need === OLD_CPU_ENGINE && !this.job && !this.installing) {
        void this.ensureEngine({ reinstall: false }).catch(() => undefined);
      }
      return { why: `${current.version} has no ${need} for this computer yet` };
    }
    const linked = path.join(this.playbackRoot, "versions", current.version);
    await fs.promises.mkdir(this.playbackUserData, { recursive: true });
    await fs.promises.writeFile(path.join(this.playbackRoot, "portable.txt"), PORTABLE_TEXT);
    // (The same version installed again is new files, and an engine added since is one more: linked again.)
    const engines = (await fs.promises.readdir(path.dirname(exe)).catch(() => [] as string[]))
      .filter((n) => /^engine_.*\.dll$/i.test(n))
      .sort();
    const done = [current.version, current.installedAt, ...engines].join(" ");
    const marker = await fs.promises.readFile(path.join(linked, MIRROR_DONE), "utf8").catch(() => null);
    if (marker !== done) {
      await removeTree(linked);
      await linkTree(versionDir, linked);
      await fs.promises.writeFile(path.join(linked, MIRROR_DONE), done);
      this.log(`EmissaryRecomp ${current.version}: linked into ${linked} for replays and spectating`);
    }
    // Other versions' links (one may still run: then it goes next time).
    const versions = path.join(this.playbackRoot, "versions");
    for (const name of await fs.promises.readdir(versions).catch(() => [] as string[])) {
      if (name !== current.version) {
        await removeTree(path.join(versions, name)).catch(() => undefined);
      }
    }
    return { exe: path.join(linked, program), version: current.version };
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
    if (this.job || this.installing) {
      throw new EmissaryError("EmissaryRecomp is already being downloaded.");
    }
    const run = this._installJob();
    this.installing = run;
    try {
      return await run;
    } finally {
      this.installing = null;
    }
  }

  private async _installJob(): Promise<EmissaryState> {
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
    if (job) {
      this.onJob?.(job);
    }
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
    // A release for another launcher protocol doesn't replace one that plays (it is installed when
    // there is none: the card then says it can't play online).
    if (release && release.version !== have && (!have || EmissaryRecomp.manifestFits(release))) {
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

  /** A download's bytes, into the job that runs (throttled). */
  private jobProgress(): ByteProgress {
    return throttleProgress((c, t) => {
      if (this.job) {
        this.job = { ...this.job, current: c, total: t };
        this.onJob?.(this.job);
        void this.emit();
      }
    }, 150);
  }

  private async installRelease(release: EmissaryRelease, kind: "install" | "update", signal: AbortSignal) {
    const report = this.jobProgress();
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
    let top = partial;
    let program = wiiCompiledRelativePath(this.platform);
    try {
      await (this.opts.unpack ?? unpackArchive)(archive, partial, signal);
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
      const found = await findProgramRoot(partial, this.platform);
      if (!found) {
        throw new EmissaryError(
          `EmissaryRecomp ${release.version}'s download has no ${wiiCompiledRelativePath(this.platform)}.`,
        );
      }
      top = found.top;
      program = found.program;
      // The engine this computer needs, before the version is one that plays (missingEngine).
      const engine = await this.missingEngine(top, program);
      if (engine === OLD_CPU_ENGINE) {
        await this.addEngine(release, engine, path.dirname(path.join(top, program)), kind, signal);
        this.setJob({ kind, phase: "unpack", current: 0, total: 0 });
      } else if (engine) {
        throw new EmissaryError(`EmissaryRecomp ${release.version}'s download has no ${engine}.`);
      }
      // Only the game: the upstream launcher would update files under us, and the release's own
      // portable.txt and UserData would hide ours above it.
      for (const name of ["Brawl Recompiled.exe", "Brawl Recompiled"]) {
        await fs.promises.rm(path.join(top, name), { force: true });
      }
      for (const dir of [path.dirname(path.join(top, program)), top]) {
        await fs.promises.rm(path.join(dir, "portable.txt"), { force: true });
        await removeTree(path.join(dir, "UserData"));
      }
      await removeTree(dest);
      // An antivirus scanning the new program holds the folder for a while (EPERM on Windows).
      await moveTree(top, dest, { copyFallback: true, log: (m) => this.log(`EmissaryRecomp: ${m}`) });
    } catch (err) {
      throw await asDiskSpaceError(err, this.opts.root, "EmissaryRecomp");
    } finally {
      await removeTree(partial).catch(() => undefined);
    }
    if (this.platform !== "win32") {
      await fs.promises.chmod(path.join(dest, program), 0o755);
    }
    await this.ensurePortableRoot();
    if (release.manifest && release.manifest.version !== release.version) {
      this.log(`EmissaryRecomp ${release.version}: its manifest says version ${release.manifest.version}`);
    }
    const record: Current = {
      v: 1,
      version: release.version,
      programBytes: await folderBytes(dest),
      installedAt: new Date().toISOString(),
      features: await releaseFeatures(path.dirname(path.join(dest, program))),
      program,
      ...(release.manifest ? { manifest: release.manifest } : {}),
    };
    this.log(
      `EmissaryRecomp ${release.version}: ${program}, read-only card ${record.features?.readOnlyCard}, ` +
        `Brawl Online core ${record.features?.core}, playback ${record.features?.playback}` +
        (release.manifest
          ? `, built from Brawl Online ${release.manifest.brawlOnline}, protocol ${release.manifest.protocol}`
          : ", no manifest"),
    );
    if (this.running) {
      await this.writeJson("staged.json", record);
      this.log(`EmissaryRecomp ${release.version} downloaded; it plays once the game is closed`);
    } else {
      await this.switchTo(record);
    }
  }

  /**
   * The engine file the program `program` in `dir` needs on this computer and lacks, or null.
   * Asked of its loader (askEngine: `WiiCompiled.exe --engine`, the check that picks the engine at
   * every start) and remembered only while the launcher runs, so an install moved to another
   * computer is checked there. Only Windows loaders that answer it are asked (an older one would
   * start the game; its release holds both engines). No answer: the older CPUs' engine, when it
   * isn't there (a needless download never stops a start; a missing engine would).
   */
  private async missingEngine(dir: string, program: string): Promise<string | null> {
    if (this.platform !== "win32" || path.basename(program).toLowerCase() !== "wiicompiled.exe") {
      return null;
    }
    const exe = path.join(dir, program);
    const [answers] = await fileHolds(exe, [ENGINE_QUERY]);
    if (!answers) {
      return null;
    }
    if (!this.engineAnswer) {
      this.engineAnswer = await (this.opts.askEngine ?? askEngine)(exe);
      this.log(
        this.engineAnswer
          ? `EmissaryRecomp: this computer plays with ${this.engineAnswer}`
          : `EmissaryRecomp: ${exe} ${ENGINE_QUERY} gave no answer: ${OLD_CPU_ENGINE} is fetched when missing`,
      );
    }
    const need = this.engineAnswer ?? OLD_CPU_ENGINE;
    return (await exists(path.join(path.dirname(exe), need))) ? null : need;
  }

  /** Downloads `name` (the older CPUs' engine) from `release`'s -Windows-OldCPU.zip into `programDir`. */
  private async addEngine(
    release: EmissaryRelease,
    name: string,
    programDir: string,
    kind: "install" | "update",
    signal: AbortSignal,
  ) {
    const asset = name === OLD_CPU_ENGINE ? release.oldCpu : null;
    if (!asset) {
      throw new EmissaryError(`EmissaryRecomp ${release.version} has no download for this computer.`);
    }
    this.log(`EmissaryRecomp ${release.version}: this computer needs ${name}, downloading ${asset.assetName}`);
    this.setJob({ kind, phase: "download", current: 0, total: asset.size });
    // The DLL is about 3 times its archive.
    await ensureFreeSpace(this.opts.root, asset.size * 4, "EmissaryRecomp");
    const archive = await this.download(asset, signal, this.jobProgress());
    if (signal.aborted) {
      throw new EmissaryCancelled();
    }
    this.setJob({ kind, phase: "unpack", current: 0, total: 0 });
    const unpacked = path.join(this.file("download"), "engine");
    const dest = path.join(programDir, name);
    await removeTree(unpacked);
    await fs.promises.mkdir(unpacked, { recursive: true });
    try {
      await (this.opts.unpack ?? unpackArchive)(archive, unpacked, signal);
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
      // At the archive's top, or in its one top folder (EmissaryRecomp/).
      const tops = await fs.promises.readdir(unpacked).catch(() => [] as string[]);
      let found: string | null = null;
      for (const candidate of [path.join(unpacked, name), ...tops.map((t) => path.join(unpacked, t, name))]) {
        if ((await fs.promises.stat(candidate).catch(() => null))?.isFile()) {
          found = candidate;
          break;
        }
      }
      if (!found) {
        throw new EmissaryError(`EmissaryRecomp ${release.version}'s download has no ${name}.`);
      }
      await fs.promises.copyFile(found, `${dest}.partial`);
      await fs.promises.rename(`${dest}.partial`, dest);
    } catch (err) {
      await fs.promises.rm(`${dest}.partial`, { force: true }).catch(() => undefined);
      throw await asDiskSpaceError(err, this.opts.root, "EmissaryRecomp");
    } finally {
      await removeTree(unpacked).catch(() => undefined);
    }
    this.log(`EmissaryRecomp ${release.version}: ${name} added`);
  }

  /**
   * The installed version has the engine this computer needs (missingEngine): the older CPUs'
   * engine is downloaded into it now when it hasn't (Play waits for it like an install); a version
   * without its main engine is installed again (`reinstall`, Play only). Throws when that can't be
   * done.
   */
  private async ensureEngine({ reinstall }: { reinstall: boolean }): Promise<void> {
    const current = await this.readJson<Current>("current.json");
    if (!current?.program) {
      return;
    }
    const versionDir = this.versionDir(current.version);
    const need = await this.missingEngine(versionDir, current.program);
    if (!need) {
      return;
    }
    if (need !== OLD_CPU_ENGINE) {
      if (!reinstall) {
        throw new EmissaryError(`EmissaryRecomp ${current.version} has no ${need}.`);
      }
      this.log(`EmissaryRecomp ${current.version} has no ${need}: installing it again`);
      await this.writeJson("current.json", null);
      await this.install();
      return;
    }
    if (this.job || this.installing) {
      throw new EmissaryError("EmissaryRecomp is already being downloaded.");
    }
    const run = (async () => {
      const abort = new AbortController();
      this.abort = abort;
      this.lastError = null;
      try {
        await this.lock.runExclusive(async () => {
          const release = await this.releaseOf(current.version);
          if (!release) {
            throw new EmissaryError(
              `Could not reach GitHub to download EmissaryRecomp (${this.releaseError ?? "no answer"}).`,
            );
          }
          await this.addEngine(
            release,
            need,
            path.dirname(path.join(versionDir, current.program ?? "")),
            "update",
            abort.signal,
          );
        });
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
    })();
    this.installing = run;
    try {
      await run;
    } finally {
      this.installing = null;
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

  private async download(release: EmissaryAsset, signal: AbortSignal, onProgress: ByteProgress): Promise<string> {
    const file = path.join(this.file("download"), path.basename(release.assetName).replace(/[^\w.+-]/g, "_"));
    await fs.promises.mkdir(path.dirname(file), { recursive: true });
    let res: Response;
    try {
      res = await fetchRetrying(
        this.fetchImpl,
        release.url,
        { signal, headers: { "User-Agent": "BrawlOnline-Launcher" } },
        (err) => this.log(`EmissaryRecomp: no answer from GitHub, asking again (${fetchErrorMessage(err)})`),
      );
    } catch (err) {
      if (signal.aborted) {
        throw new EmissaryCancelled();
      }
      throw new EmissaryError(`Could not download EmissaryRecomp (${fetchErrorMessage(err)})`);
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
      await moveTree(partial, gameRoot, { log: (m) => this.log(`EmissaryRecomp: ${m}`) }).catch((err) => {
        throw heldError(err, "Brawl's game files");
      });
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
    if (this.running || this.playbackRunning) {
      throw new EmissaryError("Close EmissaryRecomp first.");
    }
    if (this.job) {
      throw new EmissaryError("EmissaryRecomp is being downloaded. Cancel that first.");
    }
    await this.lock.runExclusive(async () => {
      // Its Brawl save is a link to the netplay Dolphin's (launch.ts): the link goes, the save stays
      await unlinkBrawlSave(path.join(this.userData, "NAND"));
      await unlinkBrawlSave(path.join(this.playbackUserData, "NAND"));
      await unlinkShared(this.playbackUserData);
      await removeTree(this.opts.root);
    });
    this.lastError = null;
    this.log(`EmissaryRecomp: removed ${this.opts.root}`);
    await this.emit();
  }
}

/** A move another program kept failing (Windows: an antivirus scan of the new files), said so. */
function heldError(err: unknown, what: string): unknown {
  const code = (err as NodeJS.ErrnoException | undefined)?.code;
  if (code !== "EPERM" && code !== "EACCES" && code !== "EBUSY") {
    return err;
  }
  return new EmissaryError(
    `Another program (often an antivirus scan) kept ${what} open, so they could not be moved into ` +
      `place. Try again in a minute. (${(err as Error).message})`,
  );
}

/**
 * `dest` as `src`: its folders made, its files hard links of src's (one copy on disk, and no copy
 * of 500 MB at each start), symbolic links (a macOS bundle's) as they are. A file that can't be
 * linked (a file system without hard links) is copied.
 */
export async function linkTree(src: string, dest: string): Promise<void> {
  await fs.promises.mkdir(dest, { recursive: true });
  for (const entry of await fs.promises.readdir(src, { withFileTypes: true })) {
    const from = path.join(src, entry.name);
    const to = path.join(dest, entry.name);
    if (entry.isDirectory()) {
      await linkTree(from, to);
    } else if (entry.isSymbolicLink()) {
      await fs.promises.symlink(await fs.promises.readlink(from), to);
    } else {
      await fs.promises.link(from, to).catch(() => fs.promises.copyFile(from, to));
    }
  }
}

async function exists(p: string): Promise<boolean> {
  return fs.promises.stat(p).then(
    () => true,
    () => false,
  );
}

/**
 * The release's folder (the archive itself, or its one top folder, `EmissaryRecomp/`) and its
 * program in it: CI's `WiiCompiled[.exe]`, or v0.6.0's `engine/brawl-engine[.exe]`.
 */
export async function findProgramRoot(
  dir: string,
  platform: Platform,
): Promise<{ top: string; program: string } | null> {
  const tops = [dir];
  for (const entry of await fs.promises.readdir(dir, { withFileTypes: true })) {
    if (entry.isDirectory()) {
      tops.push(path.join(dir, entry.name));
    }
  }
  for (const top of tops) {
    for (const program of programRelativePaths(platform)) {
      if (await exists(path.join(top, program))) {
        return { top, program };
      }
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

/**
 * The system's tar. On Windows its own (bsdtar, in System32): a GNU tar earlier on the PATH, such
 * as Git for Windows', takes the `C:` of a path for a remote host.
 */
export const systemTar = () =>
  process.platform === "win32" ? path.join(process.env.SystemRoot ?? "C:\\Windows", "System32", "tar.exe") : "tar";

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
    const child = spawn(systemTar(), ["-xzf", archive, "-C", dir], { windowsHide: true, signal });
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
