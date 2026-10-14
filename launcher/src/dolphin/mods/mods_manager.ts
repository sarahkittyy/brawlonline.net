import type { SettingsManager } from "@settings/settings_manager";
import { randomUUID } from "crypto";
import electronLog from "electron-log";
import { stat } from "fs/promises";
import path from "path";
import { throttleProgress } from "utils/copy_file";

import type { DolphinManager } from "../manager";
import { DolphinLaunchType } from "../types";
import { buildHashKey, combineBuildHash, computeBuildParts } from "./build_hash";
import { defaultControllerKind, readDolphinControllers } from "./controllers";
import { discContentSha1, readDiscInfo } from "./disc_info";
import { findModParts } from "./folder_import";
import { knownDiscName, knownModName } from "./known_builds";
import type { ModsRoomArg } from "./mods_args";
import { readModRoomStatus, writeModRoomRequest } from "./room_files";
import type {
  DolphinControllers,
  ModBuild,
  ModBuildInput,
  ModFolderParts,
  ModRoomRequest,
  ModRoomStatus,
  ModsController,
  ModsEvent,
  ModsHostOptions,
  ModsJoinOptions,
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
  const mod = knownModName(build.parts?.launcher, build.parts?.sd);
  if (mod) {
    return mod;
  }
  if (build.launcherPath) {
    return path.basename(build.launcherPath).replace(/\.(dol|elf)$/i, "");
  }
  return knownDiscName(build.parts?.disc) ?? build.gameTitle ?? path.basename(build.discPath);
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
export class ModsManager {
  private running = false;
  private status: ModRoomStatus | null = null;
  private statusText = "";
  private pollTimer: ReturnType<typeof setInterval> | null = null;
  private readonly listeners = new Set<(event: ModsEvent) => void>();
  private readonly hashing = new Map<string, Promise<ModBuild>>();
  private controllers: DolphinControllers = { gc: [], wiimote: [] };
  private controllersRead = false;

  /** `toolPath` resolves DolphinTool (the bundled Dolphin's, once it is installed). */
  constructor(
    private readonly settings: SettingsManager,
    private readonly dolphin: DolphinManager,
    private readonly toolPath: () => Promise<string>,
  ) {}

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
      controller: this.controller(),
      controllers: this.controllers,
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
    if (this.dolphin.isNetplayRunning()) {
      throw new Error("Dolphin is already open. Close it first.");
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
      platform: info.platform,
      hash: null,
      hashKey: null,
    };
    await this.saveBuilds([...this.builds(), build]);
    // Hashed at once (in the background): a known game or mod gets its name, and Host is quick.
    void this.hashBuild(build.id).catch((err) => log.warn(`hashing ${build.name}: ${err}`));
    return build;
  }

  async inspectFolder(folder: string): Promise<ModFolderParts> {
    return findModParts(folder);
  }

  async updateBuild(id: string, input: Partial<ModBuildInput>): Promise<ModBuild> {
    const current = this.findBuild(id);
    const next: ModBuild = { ...current };
    if (input.name !== undefined) {
      const named = cleanName(input.name, "");
      next.name = named || autoName(current);
      next.autoName = !named;
    }
    for (const key of ["discPath", "launcherPath", "sdPath"] as const) {
      if (input[key] !== undefined && input[key] !== current[key]) {
        await assertFile(
          input[key] ?? null,
          key === "discPath" ? "disc image" : key === "sdPath" ? "SD card image" : "launcher",
        );
        (next as Record<string, unknown>)[key] = input[key] || null;
        next.hash = null;
        next.parts = null;
        next.hashKey = null;
      }
    }
    if (!next.discPath) {
      throw new Error("A build needs a disc image.");
    }
    if (next.discPath !== current.discPath) {
      const info = await readDiscInfo(await this.toolPath(), next.discPath);
      next.gameId = info.gameId;
      next.gameTitle = info.title;
      next.platform = info.platform;
    }
    await this.saveBuilds(this.builds().map((b) => (b.id === id ? next : b)));
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
    const job = this.doHash(id).finally(() => this.hashing.delete(id));
    this.hashing.set(id, job);
    return job;
  }

  async host(options: ModsHostOptions): Promise<void> {
    const build = await this.hashBuild(options.buildId);
    await this.launch(build, {
      host: true,
      build: { hash: build.hash!, name: build.name },
      public: options.public,
      controller: await this.launchController(),
    });
  }

  async join(options: ModsJoinOptions): Promise<void> {
    const code = normalizeRoomCode(options.code);
    if (!code) {
      throw new Error("Enter the room's code.");
    }
    const build = await this.hashBuild(options.buildId);
    await this.launch(build, {
      join: code,
      build: { hash: build.hash!, name: build.name },
      controller: await this.launchController(),
    });
  }

  async request(request: ModRoomRequest): Promise<void> {
    if (!this.running) {
      throw new Error("No Brawl Mods room is open.");
    }
    await writeModRoomRequest(this.dolphin.netplayOnlineDir(), request);
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
    const build = this.findBuild(id);
    await assertFile(build.discPath, "disc image");
    await assertFile(build.launcherPath, "launcher");
    await assertFile(build.sdPath, "SD card image");
    const key = await buildHashKey(build);
    if (build.hash && build.hashKey === key) {
      return build;
    }
    const progress = throttleProgress((done, total) => {
      this.emit({ type: "hashProgress", progress: { id, done, total } });
    }, 200);
    const tool = await this.toolPath();
    const parts = await computeBuildParts(build, (disc) => discContentSha1(tool, disc), progress);
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
      updated = { ...latest, hash, parts, hashKey: key };
      if (updated.autoName) {
        updated.name = autoName(updated);
      }
    }
    await this.saveBuilds(this.builds().map((b) => (b.id === id ? updated : b)));
    return updated;
  }

  private async launch(build: ModBuild, room: ModsRoomArg): Promise<void> {
    if (this.running || this.dolphin.isNetplayRunning()) {
      throw new Error("Dolphin is already open. Close it first.");
    }
    await this.dolphin.launchModsDolphin(build, room, () => this.onDolphinClosed());
    this.running = true;
    this.status = null;
    this.statusText = "";
    this.startPolling();
    this.emitState();
  }

  private onDolphinClosed(): void {
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
