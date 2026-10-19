import type { PublicModRoom } from "@accounts/types";
import type { SettingsManager } from "@settings/settings_manager";
import { randomUUID } from "crypto";
import electronLog from "electron-log";
import { mkdir, stat } from "fs/promises";
import path from "path";
import { throttleProgress } from "utils/copy_file";

import type { DolphinManager } from "../manager";
import { DolphinLaunchType } from "../types";
import { buildHashKey, combineBuildHash, computeBuildParts, sdHashKey } from "./build_hash";
import { defaultControllerKind, readDolphinControllers } from "./controllers";
import { discContentSha1WithProgress, isPlainDiscImage, readDiscInfo } from "./disc_info";
import { findModParts } from "./folder_import";
import { knownDiscName, knownModName } from "./known_builds";
import type { ModsRoomArg } from "./mods_args";
import { readModRoomStatus, writeModRoomRequest } from "./room_files";
import { discLabel, matchRoom } from "./room_match";
import { importSave, saveStoreDir, summarizeSave, titleDataDir } from "./save_import";
import type { SdHashFn } from "./sd_hash";
import { sdCardRoot } from "./sd_hash";
import type { SdListingStore } from "./sd_listing";
import { checkRoomCard, compareSdListing, noListings, readSdListing } from "./sd_listing";
import type {
  DolphinControllers,
  ModBuild,
  ModBuildInput,
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
    // The player's game on the room's disc (another SD card is synced before the join).
    const match = matchRoom(room.build, this.builds());
    const build = match.joinable ? match.build : null;
    if (!build) {
      return { outcome: "no-game", name: room.build.name, hash: room.build.hash };
    }
    return await this.join({ buildId: build.id, code, uid: options.uid });
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
    };
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

  async host(options: ModsHostOptions): Promise<void> {
    const build = await this.hashBuild(options.buildId);
    await this.launch(build, {
      host: true,
      build: roomBuild(build),
      public: options.public,
      controller: await this.launchController(),
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

  /** What joining room `code` with `build` would download of the room's SD card. */
  private async checkCard(build: ModBuild, code: string, uid: string | undefined): Promise<ModSdCardCheck> {
    if (!build.sdPath || !uid) {
      return { kind: "same" };
    }
    const room = await this.lookupRoom(uid, code);
    if (!room) {
      throw new Error("Room not found.");
    }
    const roomRoot = room.build.sd ?? "";
    const toolPath = await this.toolPath();
    const userDir = this.dolphin.netplayUserFolder();
    const sdPath = build.sdPath;
    return await checkRoomCard({
      ownRoot: build.parts?.sdCard ?? null,
      roomRoot,
      fetchListing: () => this.listings.fetch(uid, roomRoot),
      compare: (listing) => compareSdListing(toolPath, sdPath, userDir, listing, roomRoot),
      log: (message) => log.warn(message),
    });
  }

  async join(options: ModsJoinOptions): Promise<ModsJoinResult> {
    const code = normalizeRoomCode(options.code);
    if (!code) {
      throw new Error("Enter the room's code.");
    }
    const build = await this.hashBuild(options.buildId);
    // A card that differs from the room's: the player says yes before Dolphin starts.
    if (!options.sdSync) {
      const check = await this.checkCard(build, code, options.uid);
      if (check.kind !== "same") {
        log.info(
          `room ${code}'s SD card differs from ${build.name}'s: ${
            check.kind === "differs" ? `${check.bytes} bytes, ${check.files} files` : "no listing"
          }`,
        );
        return { outcome: "sd-card", buildId: build.id, code, check };
      }
    }
    await this.launch(build, {
      join: code,
      build: roomBuild(build),
      controller: await this.launchController(),
      ...(options.sdSync ? { sd_sync: options.sdSync } : {}),
    });
    return { outcome: "joining" };
  }

  async request(request: ModRoomRequest): Promise<void> {
    if (!this.running) {
      throw new Error("No Brawl Mods room is open.");
    }
    await writeModRoomRequest(this.dolphin.netplayOnlineDir(), request);
  }

  /** The game's save in rooms: null for a GameCube game (memory cards are not kept) or a game
   * not hashed yet (its store is named by its hash). */
  async saveInfo(id: string): Promise<ModSaveInfo | null> {
    const build = this.findBuild(id);
    if (build.platform === "gamecube" || !build.hash) {
      return null;
    }
    return summarizeSave(await this.saveDataDir(id));
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
    const result = await importSave({
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

  /** The build's save folder in its store (`title/<hi>/<lo>/data`): its title ID read from the
   * disc's header for a game added before the launcher kept it. */
  private async saveDataDir(id: string): Promise<string> {
    let build = this.findBuild(id);
    if (build.platform === "gamecube") {
      throw new Error("GameCube games keep their saves on memory cards, which rooms don't keep.");
    }
    if (!build.hash) {
      throw new Error(`${build.name} is still being checked. Try again when that is done.`);
    }
    if (!build.titleId) {
      const info = await readDiscInfo(await this.toolPath(), build.discPath);
      if (!info.titleId) {
        throw new Error("GameCube games keep their saves on memory cards, which rooms don't keep.");
      }
      build = { ...this.findBuild(id), titleId: info.titleId };
      await this.saveBuilds(this.builds().map((b) => (b.id === id ? build : b)));
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
    // DolphinTool reports nothing while it hashes a compressed disc: the bar has no share then.
    const plain = await isPlainDiscImage(build.discPath);
    const discSize = (await stat(build.discPath)).size;
    const progress = throttleProgress((done, total) => {
      this.setHashProgress(id, !plain && done < discSize ? null : total > 0 ? done / total : 0);
    }, 200);
    const started = Date.now();
    log.info(`Hashing ${build.name} (${plain ? "plain disc image" : "DolphinTool"}): ${build.discPath}`);
    const parts = await computeBuildParts(
      build,
      (disc, onDisc) => discContentSha1WithProgress(tool, disc, onDisc),
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
      if (twin) {
        await this.saveBuilds(this.builds().filter((b) => b.id !== id));
        this.emit({ type: "duplicate", name: twin.name });
        throw new Error(`You already have this game: ${twin.name}.`);
      }
      updated = { ...latest, hash, parts, hashKey: key, sdHashKey: sdKey };
      if (updated.autoName) {
        updated.name = autoName(updated);
      }
    }
    await this.saveBuilds(this.builds().map((b) => (b.id === id ? updated : b)));
    return updated;
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
