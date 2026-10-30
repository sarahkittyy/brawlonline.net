import type { PublicModRoom } from "@accounts/types";
import { PPLUS_NETPLAY_LAUNCHER_DOL } from "@common/product";
import type { SettingsManager } from "@settings/settings_manager";
import { randomUUID } from "crypto";
import electronLog from "electron-log";
import { mkdir, readFile, stat, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { throttleProgress } from "utils/copy_file";

import { PPLUS_RELEASE } from "../install/pplus_release";
import type { DolphinManager } from "../manager";
import { DolphinLaunchType } from "../types";
import { BUILD_HASH_VERSION, buildHashKey, combineBuildHash, computeBuildParts, sdHashKey } from "./build_hash";
import { roomCodes } from "./code_rules";
import { cleanCodes, codeLists, dolphinCodes, ownDolphinCodes, parseDatabaseCodes, startingCodes } from "./codes";
import { defaultControllerKind, readDolphinControllers } from "./controllers";
import { discIdentity, isBrawlGameId, readDiscInfo } from "./disc_info";
import { findModParts } from "./folder_import";
import { gcSaveDir, importGcSave, summarizeGcSave } from "./gc_save";
import { knownDiscName, knownModName } from "./known_builds";
import type { ModsRoomArg } from "./mods_args";
import { readModRoomStatus, writeModRoomRequest } from "./room_files";
import { discLabel, matchRoom } from "./room_match";
import {
  findDolphinSaveNear,
  findOwnDolphinSave,
  importSave,
  moveSaveStore,
  ownDolphinUserFolders,
  sameSave,
  saveStoreDir,
  summarizeSave,
  titleDataDir,
  titleGameId,
} from "./save_import";
import type { SdHashFn } from "./sd_hash";
import { sdCardRoot } from "./sd_hash";
import type { SdListingStore } from "./sd_listing";
import { checkRoomCard, compareSdListing, noListings, readSdListing } from "./sd_listing";
import type {
  DolphinControllers,
  ModBuild,
  ModBuildCodes,
  ModBuildInput,
  ModCode,
  ModCodeLists,
  ModFolderParts,
  ModRoomBuild,
  ModRoomRequest,
  ModRoomStatus,
  ModSaveImportResult,
  ModSaveInfo,
  ModsController,
  ModSdCardCheck,
  ModsEvent,
  ModsHostOptions,
  ModsJoinByCodeOptions,
  ModsJoinByCodeResult,
  ModsJoinOptions,
  ModsJoinResult,
  ModsState,
} from "./types";

const log = electronLog.scope("dolphin/mods");

/** The code database Dolphin's Download Codes reads (codes.rc24.xyz, geckocodes.org's mirror): a
 * game's codes, kept a week in the netplay User folder's cache. */
const CODE_DATABASE_URL = "https://codes.rc24.xyz/txt.php?txt=";
const CODE_DATABASE_KEEP_MS = 7 * 24 * 3600 * 1000;

/** A game's codes in the code database (geckocodes.org's text), null when it has none. A copy
 * fetched in the last week is used, or an older one when the database can't be reached. */
export async function fetchCodeDatabase(cacheDir: string, gameId: string): Promise<string | null> {
  if (!/^[A-Z0-9]{4,6}$/.test(gameId)) {
    return null;
  }
  const file = path.join(cacheDir, `${gameId}.txt`);
  const cachedAt = (await stat(file).catch(() => null))?.mtimeMs;
  const cached = cachedAt !== undefined ? await readFile(file, "utf8").catch(() => null) : null;
  if (cached !== null && cachedAt !== undefined && Date.now() - cachedAt < CODE_DATABASE_KEEP_MS) {
    return cached || null;
  }
  try {
    const response = await fetch(CODE_DATABASE_URL + gameId, { signal: AbortSignal.timeout(15000) });
    if (!response.ok && response.status !== 404) {
      throw new Error(`The code database answered ${response.status}.`);
    }
    const text = response.ok ? await response.text() : "";
    await mkdir(cacheDir, { recursive: true });
    await writeFile(file, text);
    return text || null;
  } catch (err) {
    if (cached !== null) {
      log.warn(`The code database for ${gameId}: ${err}; using the copy kept before`);
      return cached || null;
    }
    throw err;
  }
}

/** How often the room file is read while Dolphin runs a room. */
const POLL_MS = 500;
const MAX_BUILD_NAME = 64;

/** A room code as players type it: letters and digits (mm's codes are upper case). */
export function normalizeRoomCode(code: string): string | null {
  const c = code.trim().toUpperCase();
  return /^[A-Z0-9]{2,16}$/.test(c) ? c : null;
}

function cleanName(name: string, fallback: string): string {
  // Control characters (a pasted name) are dropped.
  const printable = Array.from(name)
    .filter((c) => c >= " " && c !== "\u007f")
    .join("");
  const n = printable.trim().slice(0, MAX_BUILD_NAME);
  return n || fallback.slice(0, MAX_BUILD_NAME);
}

/** A name for a build the player did not name: the known mod's or game's, else the launcher's file
 * name for a mod, else the disc's title. */
export function autoName(build: Pick<ModBuild, "gameTitle" | "launcherPath" | "parts" | "discPath">): string {
  const mod = knownModName(build.parts?.launcher);
  if (mod) {
    return mod;
  }
  if (build.launcherPath) {
    return path.basename(build.launcherPath).replace(/\.(dol|elf)$/i, "");
  }
  return knownDiscName(build.parts?.disc) ?? build.gameTitle ?? path.basename(build.discPath);
}

/** A file of a build the player added earlier: still where it was, or what to do about it. */
async function assertBuildFile(build: ModBuild, file: string | null, what: string): Promise<void> {
  if (!file) {
    return;
  }
  const s = await stat(file).catch(() => null);
  if (!s || !s.isFile()) {
    throw new Error(
      `Can't find the ${what} for ${build.name}: "${path.basename(file)}" is no longer in ` +
        `${path.dirname(file)}. If you moved or renamed it, put it back, or remove ${build.name} ` +
        `and add it again from where it is now.`,
    );
  }
}

async function isFile(file: string): Promise<boolean> {
  return !!(await stat(file).catch(() => null))?.isFile();
}

/** The same file (Windows and macOS do not tell "User" from "user"). */
function samePath(a: string | null | undefined, b: string | null | undefined): boolean {
  if (!a || !b) {
    return !a && !b;
  }
  const norm = (p: string) => (process.platform === "linux" ? path.resolve(p) : path.resolve(p).toLowerCase());
  return norm(a) === norm(b);
}

async function assertFile(file: string | null, what: string): Promise<void> {
  if (!file) {
    return;
  }
  const s = await stat(file).catch(() => null);
  if (!s || !s.isFile()) {
    throw new Error(`The ${what} was not found: ${file}`);
  }
}

/**
 * The Brawl Mods tab's state in the main process (docs/design/brawl-mods.md §3, §4, §8): the
 * player's builds (settings `modBuilds`), their hashes, and the room our Dolphin runs.
 */
/** A build as a room shows it: its hash and name, and the parts rooms compare (docs/design/
 * brawl-mods.md §6.5), when it has them all: the disc, and the SD card's root ("" for none). */
export function roomBuild(build: ModBuild): ModRoomBuild {
  const out: ModRoomBuild = { hash: build.hash!, name: build.name };
  const parts = build.parts;
  if (parts && (!build.sdPath || parts.sdCard)) {
    out.disc = parts.disc;
    out.discLabel = discLabel(build);
    out.sd = build.sdPath ? parts.sdCard! : "";
  }
  return out;
}

/** `matchRoom`'s `discAlone`: the build hash of a disc played alone. */
const discAloneHash = (disc: string) => combineBuildHash({ disc, launcher: null, sd: false });

export class ModsManager {
  private running = false;
  private status: ModRoomStatus | null = null;
  private statusText = "";
  private pollTimer: ReturnType<typeof setInterval> | null = null;
  private readonly listeners = new Set<(event: ModsEvent) => void>();
  private readonly hashing = new Map<string, Promise<ModBuild>>();
  /** The progress of each hash running (`ModsState.hashing`). */
  private readonly hashProgress = new Map<string, number | null>();
  private controllers: DolphinControllers = { gc: [], wiimote: [] };
  private lastError: string | null = null;
  private controllersRead = false;
  /** The SD card listings this launcher put on the server (by root), not sent again. */
  private readonly listingsSent = new Set<string>();
  /** The games hashed again at start (rehashChanged), which a join by code waits for. */
  private rehashing: Promise<void> = Promise.resolve();
  /** The last refreshBuiltIn: one runs at a time. */
  private builtInRefresh: Promise<void> = Promise.resolve();
  /** The game the running room plays. */
  private runningBuildId: string | null = null;

  /** `toolPath` resolves DolphinTool (the bundled Dolphin's, once it is installed). */
  constructor(
    private readonly settings: SettingsManager,
    private readonly dolphin: DolphinManager,
    private readonly toolPath: () => Promise<string>,
    /** The room with this code (its game), or null when there is none. */
    private readonly lookupRoom: (uid: string, code: string) => Promise<PublicModRoom | null> = async () => null,
    /** An SD card's root (DolphinTool sdhash), its chunks' hashes kept in the netplay User folder. */
    private readonly sdHash: SdHashFn = sdCardRoot,
    /** Room cards' listings on the accounts server (sd_listing.ts). */
    private readonly listings: SdListingStore = noListings,
    /** The User folders of the player's own Dolphin, whose Wii saves a game can start from. */
    private readonly ownDolphinFolders: () => string[] = () =>
      ownDolphinUserFolders(process.platform, os.homedir(), process.env),
    /** A game's codes in the code database (room-codes.md), null when it has none. */
    private readonly codeDatabase: (gameId: string) => Promise<string | null> = (gameId) =>
      fetchCodeDatabase(path.join(this.dolphin.netplayUserFolder(), "Cache", "CodeDatabase"), gameId),
  ) {}

  async joinByCode(options: ModsJoinByCodeOptions): Promise<ModsJoinByCodeResult> {
    const code = normalizeRoomCode(options.code);
    if (!code) {
      throw new Error("Enter the room's code.");
    }
    const room = await this.lookupRoom(options.uid, code);
    if (!room) {
      throw new Error("Room not found.");
    }
    // The player's game on the room's disc (another SD card is synced before the join), by its
    // hash of today's launcher.
    await this.rehashing;
    await this.hashGamesOf(room.build);
    const match = matchRoom(room.build, this.builds(), discAloneHash);
    const build = match.joinable ? match.build : null;
    if (!build) {
      // A game of the room's with a part that differs is named, not said to be missing (bug
      // report #158: "it says that I don't have any game matching this hash and yet it is the
      // EXACT same game/mod"), as the public rooms' list shows it.
      const closest =
        match.build && match.hasParts
          ? {
              buildId: match.build.id,
              part: match.disc === "wrong" ? ("disc" as const) : ("sd" as const),
              roomHasCard: !!room.build.sd,
            }
          : undefined;
      // Warn: kept in the main.log a bug report sends.
      log.warn(
        `room ${code} (${room.build.name}, ${room.build.discLabel ?? "-"}, card ${
          room.build.sd?.slice(0, 12) ?? "-"
        }): no game plays it${closest ? `; ${match.build!.name}'s ${closest.part} differs` : ""}`,
      );
      return {
        outcome: "no-game",
        name: room.build.name,
        hash: room.build.hash,
        discLabel: room.build.discLabel,
        closest,
      };
    }
    return await this.join({
      buildId: build.id,
      code,
      uid: options.uid,
      ...(match.discOnly ? { discOnly: true } : {}),
    });
  }

  /**
   * Checks the games that could play the room (of its game ID, or all for a room that names
   * none) before they are matched with it: one still being checked (just added) or never checked
   * (its check failed) has no hash yet, so it never matched. A game checked since its files last
   * changed is not read again.
   */
  private async hashGamesOf(room: ModRoomBuild): Promise<void> {
    const gameId = room.discLabel?.split(" ")[0];
    for (const build of this.builds()) {
      if (gameId && build.gameId && build.gameId !== gameId) {
        continue;
      }
      await this.hashBuild(build.id).catch((err) => log.warn(`hashing ${build.name}: ${err}`));
    }
  }

  onEvent(listener: (event: ModsEvent) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  getState(): ModsState {
    if (!this.controllersRead) {
      this.controllersRead = true;
      void this.refreshControllers().catch((err) => log.warn(`reading Dolphin's controllers: ${err}`));
    }
    return {
      builds: this.builds(),
      running: this.running,
      status: this.status,
      lastError: this.lastError,
      controller: this.controller(),
      controllers: this.controllers,
      hashing: Object.fromEntries(this.hashProgress),
      runningBuildId: this.running ? this.runningBuildId : null,
      discAlone: Object.fromEntries(
        this.builds().flatMap((b) => (b.parts ? [[b.parts.disc, discAloneHash(b.parts.disc)]] : [])),
      ),
    };
  }

  /**
   * Brawl Online's own Project+ in the list (`ModBuild.builtIn`; Sarah's board, 2026-10-29: "on a
   * fresh install, if u wanna play the included p+ but through "other games", can u just add the
   * folder?", then "i would love to have it intuitive since it's a p+ install that's built in. i
   * just wouldn't want people overwriting the sd card"): the Brawl disc set in Settings with the
   * netplay User folder's P+ netplay launcher and plain SD card. Play boots a copy of that card
   * with our plugin (pponline-sd), so this plays P+ itself, without our online menus. It comes
   * once P+ is installed and the disc set, follows them when they change, and goes while one is
   * missing (its save stays in its store, named by its hash). A game the player added with the
   * same launcher and card (Add mod folder on the game files folder) becomes it. Run before the
   * state is read (the tab's getState).
   */
  refreshBuiltIn(): Promise<void> {
    this.builtInRefresh = this.builtInRefresh
      .then(() => this.doRefreshBuiltIn())
      .catch((err) => log.warn(`Project+ in Other Games: ${err}`));
    return this.builtInRefresh;
  }

  private async doRefreshBuiltIn(): Promise<void> {
    if (this.running) {
      return;
    }
    const user = this.dolphin.netplayUserFolder();
    const launcherPath = path.join(user, "Launcher", PPLUS_NETPLAY_LAUNCHER_DOL);
    const sdPath = path.join(user, "Wii", "sd.raw");
    const discPath = this.settings.get().settings?.isoPath || null;
    const ready = !!discPath && (await isFile(discPath)) && (await isFile(launcherPath)) && (await isFile(sdPath));
    const builds = this.builds();
    const current =
      builds.find((b) => b.builtIn) ??
      builds.find((b) => samePath(b.launcherPath, launcherPath) && samePath(b.sdPath, sdPath)) ??
      null;
    if (current && this.hashing.has(current.id)) {
      return;
    }
    if (!ready) {
      if (current?.builtIn) {
        log.info(`Project+ leaves Other Games: ${discPath ? "its files are not there" : "no Brawl disc is set"}`);
        await this.saveBuilds(builds.filter((b) => b.id !== current.id));
      }
      return;
    }
    const name = `Project+ ${PPLUS_RELEASE.version}`;
    const filesChanged =
      !current ||
      !samePath(current.discPath, discPath) ||
      !samePath(current.launcherPath, launcherPath) ||
      !samePath(current.sdPath, sdPath);
    if (current?.builtIn && !filesChanged && current.name === name) {
      return;
    }
    let build: ModBuild = current
      ? { ...current, name, autoName: false, builtIn: true, discPath, launcherPath, sdPath }
      : {
          id: randomUUID(),
          name,
          autoName: false,
          builtIn: true,
          discPath,
          launcherPath,
          sdPath,
          hash: null,
          hashKey: null,
        };
    if (filesChanged) {
      build = { ...build, hash: null, parts: null, hashKey: null };
      if (!current || !samePath(current.discPath, discPath)) {
        const info = await readDiscInfo(await this.toolPath(), discPath);
        build = {
          ...build,
          gameId: info.gameId,
          revision: info.revision,
          gameTitle: info.title,
          platform: info.platform,
          titleId: info.titleId,
        };
      }
    }
    // First in the list. (Rooms take it last of the games on its disc: room_match.ts.)
    await this.saveBuilds([build, ...this.builds().filter((b) => b.id !== build.id)]);
    log.info(`${name} is in Other Games${current ? ` (was ${current.name})` : ""}: ${discPath}, ${user}`);
    // Checked now for the room list, as an added game is, unless Dolphin is open (a match would
    // share the disk with the disc's read): then at its first Host or Join.
    if (filesChanged && !this.dolphin.isNetplayRunning()) {
      void this.hashBuild(build.id).catch((err) => log.warn(`hashing ${name}: ${err}`));
    }
  }

  /** What the player plays with: their pick, else a Wii Remote if only that is set up in Dolphin. */
  controller(): ModsController {
    return this.settings.get().modsController ?? defaultControllerKind(this.controllers);
  }

  async refreshControllers(): Promise<void> {
    const next = await readDolphinControllers(this.dolphin.netplayUserFolder());
    if (JSON.stringify(next) !== JSON.stringify(this.controllers)) {
      this.controllers = next;
      this.emitState();
    }
  }

  async setController(controller: ModsController): Promise<void> {
    await this.settings.updateSetting("modsController", controller);
    this.emitState();
    if (this.running) {
      // Dolphin takes it at its next frame (Machine::SetLocalController) and tells the room.
      await writeModRoomRequest(this.dolphin.netplayOnlineDir(), { op: "controller", controller });
    }
  }

  /** Dolphin's Controllers settings: over the room's game, or in a Dolphin of their own (closing
   * them closes it, and the menu reads what they set). */
  async configureControllers(): Promise<void> {
    if (this.running) {
      await writeModRoomRequest(this.dolphin.netplayOnlineDir(), { op: "configure" });
      return;
    }
    if (this.dolphin.isNetplayRunning() && !(await this.dolphin.closeNetplayIfIdle())) {
      throw new Error("Dolphin is already open with a game. Close it first.");
    }
    await this.dolphin.configureDolphin(DolphinLaunchType.NETPLAY, {
      extraArgs: ["--controllers"],
      onClose: () => void this.refreshControllers().catch((err) => log.warn(`reading Dolphin's controllers: ${err}`)),
    });
  }

  builds(): ModBuild[] {
    return this.settings.get().modBuilds ?? [];
  }

  async addBuild(input: ModBuildInput): Promise<ModBuild> {
    const same = this.builds().find(
      (b) =>
        b.discPath === input.discPath &&
        (b.launcherPath ?? null) === (input.launcherPath || null) &&
        (b.sdPath ?? null) === (input.sdPath || null),
    );
    if (same) {
      throw new Error(`You already have this game: ${same.name}.`);
    }
    await assertFile(input.discPath, "disc image");
    await assertFile(input.launcherPath, "launcher");
    await assertFile(input.sdPath, "SD card image");
    const info = await readDiscInfo(await this.toolPath(), input.discPath);
    const base = {
      discPath: input.discPath,
      launcherPath: input.launcherPath || null,
      sdPath: input.sdPath || null,
      gameTitle: info.title,
      parts: null,
    };
    const named = cleanName(input.name, "");
    const build: ModBuild = {
      id: randomUUID(),
      name: named || autoName(base),
      autoName: !named,
      ...base,
      gameId: info.gameId,
      revision: info.revision,
      platform: info.platform,
      titleId: info.titleId,
      hash: null,
      hashKey: null,
    };
    await this.saveBuilds([...this.builds(), build]);
    void this.buildCodes(build.id).catch((err) => log.warn(`codes of ${build.name}: ${err}`));
    // Hashed at once (in the background): a known game or mod gets its name, and Host is quick.
    void this.hashBuild(build.id).catch((err) => {
      log.warn(`hashing ${build.name}: ${err}`);
      // A duplicate was said already ("duplicate"), and the game is gone.
      if (this.builds().some((b) => b.id === build.id)) {
        this.emit({ type: "hashFailed", name: build.name, error: err instanceof Error ? err.message : String(err) });
      }
    });
    return build;
  }

  async inspectFolder(folder: string): Promise<ModFolderParts> {
    return findModParts(folder);
  }

  /** The pencil on a game (its name, disc, launcher or SD card). Changed files are hashed again in
   * the background, as a new game is. */
  async updateBuild(id: string, input: Partial<ModBuildInput>): Promise<ModBuild> {
    const current = this.findBuild(id);
    if (current.builtIn) {
      throw new Error(`${current.name}'s files are Brawl Online's own. Change your Brawl disc in Settings.`);
    }
    const next: ModBuild = { ...current };
    let filesChanged = false;
    for (const key of ["discPath", "launcherPath", "sdPath"] as const) {
      if (input[key] !== undefined && (input[key] || null) !== (current[key] ?? null)) {
        await assertFile(
          input[key] ?? null,
          key === "discPath" ? "disc image" : key === "sdPath" ? "SD card image" : "launcher",
        );
        (next as Record<string, unknown>)[key] = input[key] || null;
        next.hash = null;
        next.parts = null;
        next.hashKey = null;
        filesChanged = true;
      }
    }
    if (!next.discPath) {
      throw new Error("A build needs a disc image.");
    }
    if (filesChanged) {
      // A hash running now is of the old files, and would be taken for the new ones' (hashBuild).
      if (this.hashing.has(id)) {
        throw new Error("This game is still being checked. Change its files when that is done.");
      }
      const same = this.builds().find(
        (b) =>
          b.id !== id &&
          b.discPath === next.discPath &&
          (b.launcherPath ?? null) === (next.launcherPath ?? null) &&
          (b.sdPath ?? null) === (next.sdPath ?? null),
      );
      if (same) {
        throw new Error(`You already have this game: ${same.name}.`);
      }
    }
    if (next.discPath !== current.discPath) {
      const info = await readDiscInfo(await this.toolPath(), next.discPath);
      next.gameId = info.gameId;
      next.revision = info.revision;
      next.gameTitle = info.title;
      next.platform = info.platform;
      next.titleId = info.titleId;
    }
    // The name after the files: an empty one (or an automatic one kept) follows the new files.
    if (input.name !== undefined) {
      const named = cleanName(input.name, "");
      next.name = named || autoName(next);
      next.autoName = !named;
    } else if (next.autoName) {
      next.name = autoName(next);
    }
    await this.saveBuilds(this.builds().map((b) => (b.id === id ? next : b)));
    if (filesChanged) {
      void this.hashBuild(id).catch((err) => {
        log.warn(`hashing ${next.name}: ${err}`);
        if (this.builds().some((b) => b.id === id)) {
          this.emit({ type: "hashFailed", name: next.name, error: err instanceof Error ? err.message : String(err) });
        }
      });
    }
    return next;
  }

  async removeBuild(id: string): Promise<void> {
    if (this.builds().find((b) => b.id === id)?.builtIn) {
      throw new Error("Project+ is built into Brawl Online, so it stays in the list.");
    }
    await this.saveBuilds(this.builds().filter((b) => b.id !== id));
  }

  /** Hashes the build when a part changed since (or it never was); the same promise while it runs. */
  hashBuild(id: string): Promise<ModBuild> {
    const running = this.hashing.get(id);
    if (running) {
      return running;
    }
    const job = this.doHash(id).finally(() => {
      this.hashing.delete(id);
      if (this.hashProgress.delete(id)) {
        this.emitState();
      }
    });
    this.hashing.set(id, job);
    return job;
  }

  /**
   * Once per build hash version (settings `modsHashVersion`), at the first start of a launcher
   * with a new one: hashes again, one at a time in the background, every game an older launcher
   * hashed (`buildHashKey`'s version: v3 named a disc by its contents' SHA-1, v4 by its game data,
   * 82db6b5e, v5 a USB Loader GX WBFS by its game data too, bug report #110). Rooms are matched by the stored hash, so without this a game added before the
   * update showed every room of its disc as another disc until the player removed it and added it
   * again. A game that fails then (its files missing: an unplugged drive) is hashed at its next
   * Host or Join, as a game whose files changed always is.
   */
  rehashChanged(): Promise<void> {
    if (this.settings.get().modsHashVersion === BUILD_HASH_VERSION) {
      return this.rehashing;
    }
    this.rehashing = (async () => {
      for (const build of this.builds()) {
        if (!build.hash || build.hashKey?.startsWith(`${BUILD_HASH_VERSION}|`)) {
          continue;
        }
        log.info(`${build.name} was hashed by an older launcher (${build.hashKey ?? "-"}): hashing it again`);
        await this.hashBuild(build.id).catch((err) => log.warn(`hashing ${build.name} again: ${err}`));
      }
      await this.settings.updateSetting("modsHashVersion", BUILD_HASH_VERSION);
    })().catch((err) => log.warn(`hashing the games again: ${err}`));
    return this.rehashing;
  }

  async host(options: ModsHostOptions): Promise<void> {
    const build = await this.hashBuild(options.buildId);
    // The game's codes, as the player left them: the room runs them on every machine (room-codes.md).
    const codes = roomCodes(await this.buildCodes(build.id));
    if (codes.length > 0) {
      log.info(`${build.name}'s room runs ${codes.length} code(s): ${codes.map((c) => c.name).join(", ")}`);
    }
    await this.launch(build, {
      host: true,
      build: roomBuild(build),
      public: options.public,
      controller: await this.launchController(),
      ...(codes.length > 0 ? { codes } : {}),
    });
    // While Dolphin boots and opens the room: the card's listing, for joiners whose card differs.
    if (options.uid && build.sdPath) {
      void this.sendListing(options.uid, build).catch((err) =>
        log.warn(`SD card listing of ${build.name} not sent: ${err}`),
      );
    }
  }

  /** Puts the build's SD card listing on the server, once per card per run of the launcher. */
  private async sendListing(uid: string, build: ModBuild): Promise<void> {
    const root = build.parts?.sdCard;
    if (!root || this.listingsSent.has(root) || !build.sdPath) {
      return;
    }
    const started = Date.now();
    const read = await readSdListing(await this.toolPath(), build.sdPath, this.dolphin.netplayUserFolder());
    if (read.root !== root) {
      // The card changed since it was hashed: the room shows the old root, so its listing is no use.
      log.warn(
        `SD card of ${build.name} changed since it was checked (${root.slice(0, 12)} -> ${read.root.slice(0, 12)})`,
      );
      return;
    }
    await this.listings.upload(uid, root, read.listing);
    this.listingsSent.add(root);
    log.info(
      `SD card listing of ${build.name} (${root.slice(0, 12)}, ${(read.listing.byteLength / 1048576).toFixed(
        1,
      )} MB) sent in ${((Date.now() - started) / 1000).toFixed(1)} s`,
    );
  }

  /** What joining room `code` with `build` would download of the room's SD card, and the room's
   * game's name. */
  private async checkCard(
    build: ModBuild,
    code: string,
    uid: string | undefined,
  ): Promise<{ check: ModSdCardCheck; roomName?: string }> {
    if (!build.sdPath || !uid) {
      return { check: { kind: "same" } };
    }
    const room = await this.lookupRoom(uid, code);
    if (!room) {
      throw new Error("Room not found.");
    }
    const roomRoot = room.build.sd ?? "";
    const toolPath = await this.toolPath();
    const userDir = this.dolphin.netplayUserFolder();
    const sdPath = build.sdPath;
    const check = await checkRoomCard({
      ownRoot: build.parts?.sdCard ?? null,
      roomRoot,
      fetchListing: () => this.listings.fetch(uid, roomRoot),
      compare: (listing) => compareSdListing(toolPath, sdPath, userDir, listing, roomRoot),
      log: (message) => log.warn(message),
    });
    return { check, roomName: room.build.name };
  }

  async join(options: ModsJoinOptions): Promise<ModsJoinResult> {
    const code = normalizeRoomCode(options.code);
    if (!code) {
      throw new Error("Enter the room's code.");
    }
    const build = await this.hashBuild(
      options.discOnly ? (await this.discAloneBuild(options.buildId)).id : options.buildId,
    );
    // A card that differs from the room's: the player says yes before Dolphin starts.
    if (!options.sdSync) {
      const { check, roomName } = await this.checkCard(build, code, options.uid);
      if (check.kind !== "same") {
        log.info(
          `room ${code}'s SD card differs from ${build.name}'s: ${
            check.kind === "differs" ? `${check.bytes} bytes, ${check.files} files` : "no listing"
          }`,
        );
        return { outcome: "sd-card", buildId: build.id, code, check, roomName };
      }
    }
    // Brawl Online's own P+ card is never written over: a room's card is for that room only.
    const sdSync = options.sdSync === "overwrite" && build.builtIn ? "yes" : options.sdSync;
    await this.launch(build, {
      join: code,
      build: roomBuild(build),
      controller: await this.launchController(),
      ...(sdSync ? { sd_sync: sdSync } : {}),
    });
    return { outcome: "joining" };
  }

  /** The player's game of `sourceId`'s disc alone, no launcher and no SD card, for a room of the
   * plain disc (room_match.ts `discOnly`): added to the list the first time, with the disc's hash
   * the source already has (the disc is not read again). */
  private async discAloneBuild(sourceId: string): Promise<ModBuild> {
    const source = this.findBuild(sourceId);
    const same = this.builds().find((b) => b.discPath === source.discPath && !b.launcherPath && !b.sdPath);
    if (same) {
      return same;
    }
    const base = { discPath: source.discPath, launcherPath: null, sdPath: null, gameTitle: source.gameTitle ?? null };
    const known = source.parts && source.hashKey?.startsWith(`${BUILD_HASH_VERSION}|`);
    const parts = known ? { disc: source.parts!.disc, launcher: null, sd: false, sdCard: null } : null;
    const build: ModBuild = {
      id: randomUUID(),
      name: autoName({ ...base, parts }),
      autoName: true,
      ...base,
      gameId: source.gameId,
      revision: source.revision,
      platform: source.platform,
      titleId: source.titleId,
      parts,
      hash: parts ? combineBuildHash(parts) : null,
      hashKey: parts ? await buildHashKey(base) : null,
    };
    log.info(`a room plays ${source.name}'s disc alone: added ${build.name} (${build.discPath})`);
    await this.saveBuilds([...this.builds(), build]);
    return build;
  }

  async request(request: ModRoomRequest): Promise<void> {
    if (!this.running) {
      throw new Error("No Brawl Mods room is open.");
    }
    // Dolphin's own SD card question: as join's.
    const builtIn = this.builds().find((b) => b.id === this.runningBuildId)?.builtIn;
    const keepCard = request.op === "sd-sync" && request.choice === "overwrite" && builtIn;
    await writeModRoomRequest(this.dolphin.netplayOnlineDir(), keepCard ? { op: "sd-sync", choice: "yes" } : request);
  }

  /** The game's codes, and the Codes dialog's lists: the player's own Dolphins' (Brawl Online's and
   * their own), and Dolphin's for the game (room-codes.md). */
  async codeLists(id: string): Promise<{ codes: ModBuildCodes; lists: ModCodeLists }> {
    const codes = await this.buildCodes(id);
    const build = this.findBuild(id);
    if (!build.gameId || build.builtIn || build.launcherPath) {
      return { codes, lists: { own: [], ownFiles: [], dolphin: [] } };
    }
    const own = await ownDolphinCodes(this.codeFolders(), build.gameId, build.revision);
    return { codes, lists: codeLists(own, dolphinCodes(build.gameId, build.revision), codes.yours) };
  }

  /** Keeps the game's codes, checked (cleanCodes): its next room runs the ones on. Not while its
   * room is open: that room's games already run the codes it started with. */
  async setCodes(id: string, codes: ModBuildCodes): Promise<ModBuildCodes> {
    const build = this.findBuild(id);
    if (build.builtIn || build.launcherPath) {
      throw new Error(`${build.name} runs its own codes.`);
    }
    if (this.running && this.runningBuildId === id) {
      throw new Error("Leave the room to change its codes.");
    }
    const clean = cleanCodes(codes);
    await this.saveBuilds(this.builds().map((b) => (b.id === id ? { ...b, codes: clean } : b)));
    return clean;
  }

  /** The code database's codes for the game (none for a game it doesn't have). */
  async databaseCodes(id: string): Promise<ModCode[]> {
    const build = this.findBuild(id);
    if (!build.gameId || build.builtIn) {
      return [];
    }
    const text = await this.codeDatabase(build.gameId);
    return text ? parseDatabaseCodes(text) : [];
  }

  /** Every game's codes looked at once (in the background at start): a game added before codes
   * starts with the ones on in the player's own Dolphin, so its Codes button shows them. */
  async seedCodes(): Promise<void> {
    for (const build of this.builds()) {
      if (!build.codes && !build.builtIn && !build.launcherPath && build.gameId) {
        await this.buildCodes(build.id).catch((err) => log.warn(`codes of ${build.name}: ${err}`));
      }
    }
  }

  /** Brawl Online's Dolphin (the netplay User folder), then the player's own. */
  private codeFolders(): string[] {
    return [this.dolphin.netplayUserFolder(), ...this.ownDolphinFolders()];
  }

  /** The game's codes; the first time, the ones on in the player's own Dolphins (kept from then). */
  private async buildCodes(id: string): Promise<ModBuildCodes> {
    const build = this.findBuild(id);
    // A mod booted by its launcher (P+'s too) has its own Gecko code handler where Dolphin's goes:
    // its codes are the mod's.
    if (build.builtIn || build.launcherPath) {
      return { on: [], yours: [] };
    }
    if (build.codes) {
      return build.codes;
    }
    if (!build.gameId) {
      return { on: [], yours: [] };
    }
    // Not for Brawl: its inis hold an old P+ netplay setup's codes, which no mod's room wants.
    const codes = isBrawlGameId(build.gameId)
      ? { on: [], yours: [] }
      : startingCodes(
          await ownDolphinCodes(this.codeFolders(), build.gameId, build.revision),
          dolphinCodes(build.gameId, build.revision),
        );
    if (codes.on.length > 0) {
      log.info(
        `${build.name} starts with the codes on in the player's Dolphin: ${codes.on.map((c) => c.name).join(", ")}`,
      );
    }
    const latest = this.builds().find((b) => b.id === id);
    if (latest && !latest.codes) {
      await this.saveBuilds(this.builds().map((b) => (b.id === id ? { ...b, codes } : b)));
    }
    return latest?.codes ?? codes;
  }

  /** The game's save in rooms: null for a game not hashed yet (its store is named by its hash). A
   * GameCube game's is its memory card's saves (`gc_save.ts`). */
  async saveInfo(id: string): Promise<ModSaveInfo | null> {
    const build = this.findBuild(id);
    if (!build.hash) {
      return null;
    }
    const folder = await this.saveDataDir(id);
    if (build.platform === "gamecube") {
      return summarizeGcSave(folder);
    }
    const save = await summarizeSave(folder);
    const own = await findOwnDolphinSave(this.ownDolphinFolders(), this.findBuild(id).titleId!).catch(() => null);
    return { ...save, ownDolphin: own && !(await sameSave(own.folder, folder)) ? own : null };
  }

  /** The save's folder, made if it is not there yet (an empty one plays as no save). */
  async saveFolder(id: string): Promise<string> {
    const folder = await this.saveDataDir(id);
    await mkdir(folder, { recursive: true });
    return folder;
  }

  async importSave(id: string, source: string): Promise<ModSaveImportResult> {
    // A room's game writes its save back when it closes, over an import made meanwhile.
    if (this.running) {
      throw new Error("Close the room first: its game saves when it closes, and would undo the import.");
    }
    const folder = await this.saveDataDir(id);
    const build = this.findBuild(id);
    const result =
      build.platform === "gamecube"
        ? await importGcSave({
            source,
            gameId: build.gameId!,
            userFolder: this.dolphin.netplayUserFolder(),
            buildHash: build.hash!,
          })
        : await importSave({
            source,
            titleId: build.titleId!,
            userFolder: this.dolphin.netplayUserFolder(),
            buildHash: build.hash!,
          });
    log.info(
      `Imported a save of ${build.name} (${result.save.files} files) into ${folder}` +
        (result.backup ? `; the one before is in ${result.backup}` : ""),
    );
    return result;
  }

  /** The build's save folder in its store: a Wii game's `title/<hi>/<lo>/data`, a GameCube game's
   * memory card (`GC`, its `.gci` files). The disc's title ID (Wii) or game ID (GameCube) is read
   * from its header for a game added before the launcher kept it. */
  private async saveDataDir(id: string): Promise<string> {
    let build = this.findBuild(id);
    if (!build.hash) {
      throw new Error(`${build.name} is still being checked. Try again when that is done.`);
    }
    if (build.platform === "gamecube" ? !build.gameId : !build.titleId) {
      const info = await readDiscInfo(await this.toolPath(), build.discPath);
      build = { ...this.findBuild(id), gameId: info.gameId, titleId: info.titleId, platform: info.platform };
      await this.saveBuilds(this.builds().map((b) => (b.id === id ? build : b)));
    }
    if (build.platform === "gamecube") {
      return gcSaveDir(this.dolphin.netplayUserFolder(), build.hash!);
    }
    return titleDataDir(saveStoreDir(this.dolphin.netplayUserFolder(), build.hash!), build.titleId!);
  }

  /** The controller to start with, from Dolphin's settings as they are now. */
  private async launchController(): Promise<ModsController> {
    await this.refreshControllers().catch((err) => log.warn(`reading Dolphin's controllers: ${err}`));
    return this.controller();
  }

  private findBuild(id: string): ModBuild {
    const build = this.builds().find((b) => b.id === id);
    if (!build) {
      throw new Error("That build is gone.");
    }
    return build;
  }

  private async saveBuilds(builds: ModBuild[]): Promise<void> {
    await this.settings.updateSetting("modBuilds", builds);
    this.emitState();
  }

  private async doHash(id: string): Promise<ModBuild> {
    let build = this.findBuild(id);
    await assertBuildFile(build, build.discPath, "game file");
    await assertBuildFile(build, build.launcherPath, "launcher file");
    await assertBuildFile(build, build.sdPath, "SD card image");
    if (build.revision === undefined) {
      // A game added before rooms named the disc's revision ("RSBE01 Rev 1"): its header again.
      const info = await readDiscInfo(await this.toolPath(), build.discPath);
      build = { ...this.findBuild(id), revision: info.revision };
      await this.saveBuilds(this.builds().map((b) => (b.id === id ? build : b)));
    }
    const key = await buildHashKey(build);
    const sdKey = await sdHashKey(build);
    const sdCurrent = !build.sdPath || (build.parts?.sdCard != null && build.sdHashKey === sdKey);
    if (build.hash && build.hashKey === key) {
      if (sdCurrent) {
        return build;
      }
      // Only the SD card changed (or was never checked): its root, not the disc's hash again.
      const sdCard = await this.cardRoot(id, build.sdPath!, (done, total) =>
        this.setHashProgress(id, total > 0 ? done / total : 0),
      );
      const latest = this.findBuild(id);
      if ((await sdHashKey(latest)) !== sdKey || latest.hashKey !== key || !latest.parts) {
        return latest;
      }
      const updated: ModBuild = { ...latest, parts: { ...latest.parts, sdCard }, sdHashKey: sdKey };
      await this.saveBuilds(this.builds().map((b) => (b.id === id ? updated : b)));
      return updated;
    }
    const tool = await this.toolPath();
    // Until the disc's check reports (DolphinTool's SHA-1 of a compressed GameCube disc reports
    // nothing), the bar has no share.
    let discReports = false;
    const discSize = (await stat(build.discPath)).size;
    const progress = throttleProgress((done, total) => {
      this.setHashProgress(id, !discReports && done < discSize ? null : total > 0 ? done / total : 0);
    }, 200);
    const started = Date.now();
    log.info(`Hashing ${build.name}: ${build.discPath}`);
    const parts = await computeBuildParts(
      build,
      (disc, onDisc) =>
        discIdentity(tool, disc, (done, of) => {
          discReports = true;
          onDisc?.(done, of);
        }),
      progress,
    );
    // The SD card's root, which rooms compare (docs/design/brawl-mods.md §6.5).
    parts.sdCard = build.sdPath
      ? await this.cardRoot(id, build.sdPath, (done, total) => this.setHashProgress(id, total > 0 ? done / total : 0))
      : null;
    log.info(`Hashed ${build.name} in ${((Date.now() - started) / 1000).toFixed(1)} s`);
    const hash = combineBuildHash(parts);
    // The build may have been edited meanwhile: keep the newest, with this hash if its parts are the same.
    const latest = this.findBuild(id);
    let updated: ModBuild = latest;
    if ((await buildHashKey(latest)) === key) {
      // The same files as a game already in the list (another copy, an ISO and an RVZ of one
      // disc): this one goes again.
      const twin = this.builds().find((b) => b.id !== id && b.hash === hash);
      // Not a game hashed before (by an older launcher: rehashChanged): two of the player's games
      // that are now one disc (Brawl Rev 1 and Rev 2) both stay, as they were added. Nor Brawl
      // Online's own P+, which stays beside a copy of it the player added from elsewhere.
      if (twin && !latest.hash && !latest.builtIn) {
        await this.saveBuilds(this.builds().filter((b) => b.id !== id));
        this.emit({ type: "duplicate", name: twin.name });
        throw new Error(`You already have this game: ${twin.name}.`);
      }
      updated = { ...latest, hash, parts, hashKey: key, sdHashKey: sdKey };
      if (updated.autoName) {
        updated.name = autoName(updated);
      }
      // Hashed again by a newer launcher (its disc part changed): its save follows its hash.
      if (latest.hash && latest.hash !== hash) {
        const moved = await moveSaveStore(this.dolphin.netplayUserFolder(), latest.hash, hash).catch((err) => {
          log.warn(`moving the save of ${latest.name} to its new hash: ${err}`);
          return false;
        });
        if (moved) {
          log.info(
            `The save of ${latest.name} moved with its hash ${latest.hash.slice(0, 16)} -> ${hash.slice(0, 16)}`,
          );
        }
      }
    }
    await this.saveBuilds(this.builds().map((b) => (b.id === id ? updated : b)));
    if (updated.hash && updated.hash !== latest.hash) {
      await this.importSaveNearBuild(updated).catch((err) => log.warn(`importing the save of ${updated.name}: ${err}`));
    }
    return updated;
  }

  /**
   * A Wii game with no save in its store yet starts from the one in the Dolphin folder its
   * launcher or SD card is in, when there is one: the save the player has played the mod with
   * (bug report #144), and a plain disc the one in the player's own Dolphin (bug report #193).
   * Without it the room's game made a new save, its rules and unlocks the game's defaults. A store with a save is left alone, as is any game while a room runs (its
   * game writes its save back when it closes).
   */
  private async importSaveNearBuild(build: ModBuild): Promise<void> {
    if (build.platform !== "wii" || !build.titleId || !build.hash || this.running) {
      return;
    }
    const userFolder = this.dolphin.netplayUserFolder();
    if ((await summarizeSave(titleDataDir(saveStoreDir(userFolder, build.hash), build.titleId))).files > 0) {
      return;
    }
    let source = await findDolphinSaveNear([build.launcherPath, build.sdPath], build.titleId);
    // Else a plain disc's save in the player's own Dolphin (bug report #193). Not for Brawl, whose
    // save there is mostly a P+ card's (name tags, controls), which no build starts from.
    if (!source && !build.launcherPath && !build.sdPath && !isBrawlGameId(titleGameId(build.titleId))) {
      source = (await findOwnDolphinSave(this.ownDolphinFolders(), build.titleId))?.folder ?? null;
    }
    // Not P+'s own save (a card in the netplay User folder): no build starts from it (template_saves.ts).
    const fromUser = source ? path.relative(userFolder, source) : "";
    if (!source || !(fromUser.startsWith("..") || path.isAbsolute(fromUser))) {
      return;
    }
    const result = await importSave({ source, titleId: build.titleId, userFolder, buildHash: build.hash });
    log.warn(`${build.name} starts from the save in its Dolphin folder: ${source} (${result.save.files} files)`);
  }

  /** The SD card's root (DolphinTool sdhash), its hashes cached in the netplay User folder. */
  private async cardRoot(
    id: string,
    sdPath: string,
    onProgress: (done: number, total: number) => void,
  ): Promise<string> {
    const started = Date.now();
    const root = await this.sdHash(await this.toolPath(), sdPath, this.dolphin.netplayUserFolder(), onProgress);
    log.info(
      `SD card of ${this.findBuild(id).name}: ${root.slice(0, 12)} in ${((Date.now() - started) / 1000).toFixed(1)} s`,
    );
    return root;
  }

  private async launch(build: ModBuild, room: ModsRoomArg): Promise<void> {
    if (this.running) {
      throw new Error("Dolphin is already open. Close it first.");
    }
    // An open Dolphin without a game (Play's, or Configure's) closes first.
    if (this.dolphin.isNetplayRunning() && !(await this.dolphin.closeNetplayIfIdle())) {
      throw new Error("Dolphin is already open with a game. Close it first.");
    }
    await this.dolphin.launchModsDolphin(build, room, () => this.onDolphinClosed());
    this.running = true;
    this.runningBuildId = build.id;
    this.lastError = null;
    this.status = null;
    this.statusText = "";
    this.startPolling();
    this.emitState();
  }

  private async onDolphinClosed(): Promise<void> {
    // Dolphin closes itself when a join fails before any game was shared: the reason is in the
    // room file it wrote last (perhaps after the last poll).
    const last = await readModRoomStatus(this.dolphin.netplayOnlineDir()).catch(() => null);
    const status = last ?? this.status;
    this.lastError = status?.error || (status?.session?.phase === "error" ? status.session.error : "") || null;
    this.running = false;
    this.status = null;
    this.statusText = "";
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
    this.emitState();
  }

  private startPolling(): void {
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
    }
    const dir = this.dolphin.netplayOnlineDir();
    this.pollTimer = setInterval(() => {
      void readModRoomStatus(dir)
        .then((status) => {
          const text = status ? JSON.stringify({ ...status, updatedAt: "" }) : "";
          if (!this.running || text === this.statusText) {
            return;
          }
          this.statusText = text;
          this.status = status;
          this.emitState();
        })
        .catch((err) => log.warn(`reading the room file: ${err}`));
    }, POLL_MS);
  }

  private setHashProgress(id: string, fraction: number | null): void {
    const first = !this.hashProgress.has(id);
    this.hashProgress.set(id, fraction);
    if (first) {
      this.emitState();
    }
    this.emit({ type: "hashProgress", progress: { id, fraction } });
  }

  private emitState(): void {
    this.emit({ type: "state", state: this.getState() });
  }

  private emit(event: ModsEvent): void {
    for (const listener of this.listeners) {
      try {
        listener(event);
      } catch (err) {
        log.error(err);
      }
    }
  }
}
