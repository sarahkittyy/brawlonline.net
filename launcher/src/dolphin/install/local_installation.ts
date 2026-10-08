import { DOLPHIN_ONLINE_DIR, PPLUS_NETPLAY_LAUNCHER_DOL } from "@common/product";
import { sanitizeAppImageEnv } from "@dolphin/app_image_env/app_image_env";
import type { SyncedDolphinSettings } from "@dolphin/config/config";
import { addGamePath, getOnlineSettings, setDefaultIso, setOnlineSettings } from "@dolphin/config/config";
import { IniFile } from "@dolphin/config/ini_file";
import electronLog from "electron-log";
import { pathExists } from "fs-extra";
import { cp, lstat, mkdir, readdir, rename, rm } from "node:fs/promises";
import path from "path";
import type { ByteProgress } from "utils/copy_file";
import { copyFileWithProgress } from "utils/copy_file";

import type { DolphinInstallation, DolphinLaunchType } from "../types";
import { executeCommand } from "./execute_command";
import type { DolphinVersionResponse } from "./fetch_latest_version";
import { launchTypeFolder } from "./paths";

const log = electronLog.scope("dolphin/localInstallation");

// taken from https://semver.org/#is-there-a-suggested-regular-expression-regex-to-check-a-semver-string
const semverRegex =
  /(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-((?:0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?/;

/** Folders of a template User folder that are never copied (logs, caches, P+ HD textures). */
const TEMPLATE_SKIP = new Set(["Cache", "Logs", "Load", "Dump", "ScreenShots", "StateSaves", "Shaders"]);

/**
 * Seeds in flight, by User folder. Start-up asks for the User folder from two places at once (the
 * Dolphin set-up and the theme extraction); both wait for the same copy and both get its progress.
 */
const seeds = new Map<
  string,
  { promise: Promise<void>; listeners: Set<ByteProgress>; last: [number, number] | null }
>();

/**
 * A Dolphin build already on disk (our Dolphin fork), used for both netplay and
 * playback. Replaces Slippi's Ishiiruka/mainline installations, which download
 * Slippi's builds into userData.
 *
 * - Executable: from Settings, defaulting to the dev build or `userData/netplay` (see paths.ts).
 * - User folder: `<install>/User` when the build is portable (`portable.txt` next to the
 *   binary, Dolphin's own rule and Slippi's Windows layout), else `userData/<netplay|playback>/User`.
 *   Dolphin is always started with `-u <User folder>`, so the location is explicit.
 */
export class LocalDolphinInstallation implements DolphinInstallation {
  readonly installationFolder: string;
  private readonly _userFolder: string;

  constructor(
    private readonly dolphinLaunchType: DolphinLaunchType,
    private readonly executablePath: string,
    userDataDir: string,
    private readonly userTemplate: string | null = null,
    portable = false,
  ) {
    this.installationFolder = installationFolderOf(executablePath);
    this._userFolder = portable
      ? path.join(this.installationFolder, "User")
      : path.join(userDataDir, launchTypeFolder(dolphinLaunchType), "User");
  }

  /** True if `portable.txt` sits next to the binary (Dolphin then uses `<install>/User`). */
  static async isPortable(executablePath: string): Promise<boolean> {
    return pathExists(path.join(installationFolderOf(executablePath), "portable.txt"));
  }

  get userFolder(): string {
    return this._userFolder;
  }

  get sysFolder(): string {
    if (process.platform === "darwin") {
      // <install>/Dolphin.app/Contents/Resources/Sys
      return path.resolve(path.dirname(this.executablePath), "..", "Resources", "Sys");
    }
    return path.join(this.installationFolder, "Sys");
  }

  /** P+'s netplay launcher DOL inside the User folder. */
  get netplayLauncherDol(): string {
    return path.join(this.userFolder, "Launcher", PPLUS_NETPLAY_LAUNCHER_DOL);
  }

  /** The user's P+ SD card in the User folder. Only read: Play boots a patched copy (sd_card.ts). */
  get sdCardImage(): string {
    return path.join(this.userFolder, "Wii", "sd.raw");
  }

  async findDolphinExecutable(): Promise<string> {
    if (!(await pathExists(this.executablePath))) {
      throw new Error(
        `No ${this.dolphinLaunchType} Dolphin found at: ${this.executablePath}. Set the Dolphin executable in Settings > Dolphin.`,
      );
    }
    return this.executablePath;
  }

  /** Command line arguments that select this installation's User folder. */
  userArgs(): string[] {
    return ["-u", this.userFolder];
  }

  /**
   * Makes sure the User folder exists. A new one is seeded from the template (if configured, in
   * development) so P+'s launcher DOLs and SD card are in place. That copies ~2 GB, so it reports
   * the bytes copied. Each file is copied to `<name>.partial` and renamed, so an interrupted seed
   * leaves no truncated file, and the next start copies what is still missing.
   */
  async ensureUserFolder(onProgress?: ByteProgress): Promise<void> {
    await mkdir(this.userFolder, { recursive: true });
    const template = this.userTemplate;
    if (!template) {
      return;
    }
    const key = path.resolve(this.userFolder);
    let seed = seeds.get(key);
    if (!seed) {
      const listeners = new Set<ByteProgress>();
      const entry = { promise: Promise.resolve(), listeners, last: null as [number, number] | null };
      entry.promise = this._seedFromTemplate(template, (done, total) => {
        entry.last = [done, total];
        listeners.forEach((l) => l(done, total));
      }).finally(() => seeds.delete(key));
      seeds.set(key, entry);
      seed = entry;
    }
    if (onProgress) {
      seed.listeners.add(onProgress);
      if (seed.last) {
        onProgress(...seed.last);
      }
    }
    try {
      await seed.promise;
    } finally {
      if (onProgress) {
        seed.listeners.delete(onProgress);
      }
    }
  }

  private async _seedFromTemplate(template: string, onProgress: ByteProgress): Promise<void> {
    if ((await pathExists(this.netplayLauncherDol)) && (await pathExists(this.sdCardImage))) {
      return;
    }
    if (!(await pathExists(template))) {
      log.warn(`Dolphin User template not found: ${template}`);
      return;
    }
    log.info(`Seeding Dolphin User folder ${this.userFolder} from ${template}`);
    const started = Date.now();
    // Every template file that is not in the User folder yet (existing files are never replaced).
    const files: { from: string; to: string; size: number }[] = [];
    const walk = async (from: string, to: string) => {
      const st = await lstat(from);
      if (st.isDirectory()) {
        await mkdir(to, { recursive: true });
        for (const child of await readdir(from)) {
          await walk(path.join(from, child), path.join(to, child));
        }
      } else if (st.isFile() && !(await pathExists(to))) {
        files.push({ from, to, size: st.size });
      }
    };
    for (const entry of await readdir(template)) {
      if (!TEMPLATE_SKIP.has(entry)) {
        await walk(path.join(template, entry), path.join(this.userFolder, entry));
      }
    }
    const total = files.reduce((sum, f) => sum + f.size, 0);
    let reported = 0;
    const report = (done: number) => {
      // Monotonic and capped, also if a template file changes size while it is copied.
      reported = Math.min(Math.max(reported, done), total);
      onProgress(reported, total);
    };
    report(0);
    let done = 0;
    for (const f of files) {
      const partial = `${f.to}.partial`;
      await copyFileWithProgress(f.from, partial, { onProgress: (d) => report(done + d) });
      await rename(partial, f.to);
      done += f.size;
      report(done);
    }
    log.info(`Seeded ${files.length} files (${total} bytes) in ${((Date.now() - started) / 1000).toFixed(1)} s`);
  }

  /** Throws a user-facing error if P+'s files are missing from the User folder. */
  async assertProjectPlusFiles(): Promise<void> {
    const sd = this.sdCardImage;
    const missing: string[] = [];
    if (!(await pathExists(this.netplayLauncherDol))) {
      missing.push(path.join("Launcher", PPLUS_NETPLAY_LAUNCHER_DOL));
    }
    if (!(await pathExists(sd))) {
      missing.push(path.join("Wii", "sd.raw"));
    }
    if (missing.length > 0) {
      throw new Error(
        `Project+ files are missing from the Dolphin User folder (${this.userFolder}): ${missing.join(", ")}. ` +
          `They are downloaded from Project+'s official release when the launcher starts; ` +
          `check your connection and restart the launcher.`,
      );
    }
  }

  /**
   * "Hard reset": removes Dolphin's configuration and caches from the User folder.
   * The P+ files (`Wii/` with the SD card and saves, `Launcher/`) are kept, since
   * they hold the user's data and cannot be re-downloaded by the launcher.
   */
  async resetUserFolder(): Promise<void> {
    for (const name of ["Config", "GameSettings", "Cache", "Logs", DOLPHIN_ONLINE_DIR]) {
      await rm(path.join(this.userFolder, name), { recursive: true, force: true });
    }
    await this.ensureUserFolder();
  }

  async clearCache() {
    const cacheFolder = path.join(this.userFolder, "Cache");
    await rm(cacheFolder, { recursive: true, force: true });
  }

  async importConfig(fromPath: string) {
    const newUserFolder = this.userFolder;
    await mkdir(this.userFolder, { recursive: true });
    const oldUserFolder = path.join(fromPath, "User");

    if (!(await pathExists(oldUserFolder))) {
      return;
    }

    await cp(oldUserFolder, newUserFolder, { recursive: true, force: true });

    // we shouldn't keep the old cache folder since it might be out of date
    await this.clearCache();
  }

  async validate({
    onStart: _onStart,
    onProgress: _onProgress,
    onComplete,
    dolphinDownloadInfo: _info,
  }: {
    onStart: () => void;
    onProgress: (current: number, total: number) => void;
    onComplete: () => void;
    dolphinDownloadInfo: DolphinVersionResponse;
  }): Promise<void> {
    // No download yet: only check that the configured build exists.
    await this.findDolphinExecutable();
    await this.ensureUserFolder();
    onComplete();
  }

  async downloadAndInstall(_options: {
    dolphinDownloadInfo: DolphinVersionResponse;
    onProgress?: (current: number, total: number) => void;
    onComplete?: () => void;
    cleanInstall?: boolean;
  }): Promise<void> {
    throw new Error("Dolphin downloads are not available yet. Set the Dolphin executable in Settings > Dolphin.");
  }

  async addGamePath(gameDir: string): Promise<void> {
    const iniFile = await IniFile.init(this._dolphinIniPath());
    await addGamePath(iniFile, gameDir);
  }

  /** Sets `[Core] DefaultISO`, which P+'s launcher DOL boots after applying its codes. */
  async setDefaultIso(isoPath: string): Promise<void> {
    const iniFile = await IniFile.init(this._dolphinIniPath());
    await setDefaultIso(iniFile, isoPath);
  }

  async getSettings(): Promise<SyncedDolphinSettings> {
    const iniFile = await IniFile.init(this._dolphinIniPath());
    return await getOnlineSettings(iniFile);
  }

  async updateSettings(options: Partial<SyncedDolphinSettings>): Promise<void> {
    const iniFile = await IniFile.init(this._dolphinIniPath());
    await setOnlineSettings(iniFile, options);
  }

  async getDolphinVersion(): Promise<string | undefined> {
    try {
      const dolphinPath = await this.findDolphinExecutable();
      const dolphinEnv = sanitizeAppImageEnv();
      const out = await executeCommand(dolphinPath, ["--version"], { env: dolphinEnv });
      // Slippi's builds print a semver; ours (P+ Dolphin based) print e.g. "Project+ Dolphin 2332460493".
      const firstLine = out.trim().split(/\r?\n/)[0].trim();
      return out.match(semverRegex)?.[0] ?? (firstLine || undefined);
    } catch (err) {
      return undefined;
    }
  }

  async findPlayKey(): Promise<string> {
    const onlineDir = path.join(this.userFolder, DOLPHIN_ONLINE_DIR);
    await mkdir(onlineDir, { recursive: true });
    return path.resolve(onlineDir, "user.json");
  }

  private _dolphinIniPath(): string {
    return path.join(this.userFolder, "Config", "Dolphin.ini");
  }
}

function installationFolderOf(executablePath: string): string {
  // macOS: <install>/Dolphin.app/Contents/MacOS/Dolphin -> <install>
  const appIndex = executablePath.lastIndexOf(".app");
  if (appIndex !== -1) {
    return path.dirname(executablePath.slice(0, appIndex + 4));
  }
  return path.dirname(executablePath);
}
