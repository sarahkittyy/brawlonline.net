import type { ClipResolution } from "@common/clip_resolution";
import { clipResolution } from "@common/clip_resolution";
import { pplusReleaseLabel } from "@common/pplus_releases";
import { Preconditions } from "@common/preconditions";
import { DOLPHIN_ONLINE_DIR, PRODUCT_NAME } from "@common/product";
import { readTestMode } from "@common/test_mode";
import { readReplayHeader } from "@replays/replay_format";
import type { SettingsManager } from "@settings/settings_manager";
import { Mutex } from "async-mutex";
import type { MessageBoxOptions } from "electron";
import { app, BrowserWindow, dialog } from "electron";
import electronLog from "electron-log";
import { existsSync } from "fs";
import { mkdir, readdir } from "fs/promises";
import { Observable, Subject } from "observable-fns";
import path from "path";
import { fileExists } from "utils/file_exists";
import { isSubdirectory } from "utils/is_subdirectory";

import { isAbnormalExit, keepCrashLog } from "./crash_log";
import { clearInputDelayRequest, writeInputDelayRequest } from "./input_delay_handoff";
import { brawlSaveDir, ensureBrawlSave, isUsableBrawlSave } from "./install/brawl_save";
import { bundledDolphinSource, installBundledDolphin } from "./install/bundled_dolphin";
import { fetchLatestVersion } from "./install/fetch_latest_version";
import { LocalDolphinInstallation } from "./install/local_installation";
import type { GameFilesTarget } from "./install/move_game_files";
import {
  chooseGameFilesTarget,
  GameFilesMoveError,
  isInInstallFolder,
  moveGameFiles,
  removeGameFiles,
} from "./install/move_game_files";
import type { DolphinPathEnv } from "./install/paths";
import {
  defaultDolphinExecutable,
  defaultNetplayFolder,
  defaultPluginLocation,
  defaultUserTemplate,
  launcherInstallFolder,
  launchTypeFolder,
  netplayFolder,
  patchedSdCardFolder,
  projectPlusStoreFolder,
} from "./install/paths";
import { otherProjectPlusBuild } from "./install/pplus_builds";
import type { PPlusProgress, PPlusRelease, PPlusTarget } from "./install/pplus_release";
import {
  ensureNetplaySave,
  installProjectPlusFiles,
  missingProjectPlusFiles,
  NETPLAY_SAVE_DIR,
  netplaySaveStore,
  PPLUS_RELEASE,
  setAsideProjectPlusCard,
} from "./install/pplus_release";
import type { PPlusVersionState, PrepareProgress } from "./install/pplus_versions";
import {
  findProjectPlusCard,
  PLAYABLE_PPLUS_RELEASES,
  PPlusVersionError,
  PPlusVersions,
} from "./install/pplus_versions";
import { installPluginOnSdCard, loadPlugin, SdCardInUseError } from "./install/sd_card";
import { DolphinInstance, MacOsRosettaRequiredError, PlaybackDolphinInstance } from "./instance";
import type { ModsRoomArg } from "./mods/mods_args";
import { buildModsDolphinArgs } from "./mods/mods_args";
import { clearModRoomFiles } from "./mods/room_files";
import { clearTemplateSeededSaves } from "./mods/template_saves";
import type { ModBuild } from "./mods/types";
import type { RenderDolphinSettings } from "./netplay_args";
import { buildNetplayDolphinArgs, buildPlaybackDolphinArgs, buildRenderDolphinArgs } from "./netplay_args";
import { closeIfIdle } from "./quit_if_idle";
import type { RenderDolphin, RenderKey } from "./render_queue";
import { RenderQueue } from "./render_queue";
import { clearRoomRequest } from "./room_handoff";
import { createProgressThrottle, runWithProgress } from "./setup_progress";
import type {
  DolphinEvent,
  DolphinSetupPhase,
  NetplayLaunchResult,
  ReplayCommunication,
  ReplayPlaybackStatus,
  ReplayPlayChoice,
  ReplayPlayResult,
} from "./types";
import { DolphinErrorType, DolphinEventType, DolphinLaunchType } from "./types";

/** Progress events are sent at most this often per launch type (a 2 GB step has thousands of chunks). */
const PROGRESS_INTERVAL_MS = 100;

const PPLUS_PHASES: Record<PPlusProgress["phase"], DolphinSetupPhase> = {
  download: "downloadProjectPlus",
  verify: "verifyProjectPlus",
  extract: "extractProjectPlus",
  update: "updateProjectPlus",
};

const log = electronLog.scope("dolphin/manager");

// DolphinManager should be in control of all dolphin instances that get opened for actual use.
// This includes playing netplay, viewing replays and configuring Dolphin.
//
// Differences from Slippi: one Dolphin build (ours) serves both netplay and playback; it ships
// inside the launcher package and is installed into userData/netplay from there (so it updates
// with the launcher) instead of being downloaded; P+'s own files (SD card, launcher DOLs, Brawl
// save template) are downloaded from P+'s official release at setup, with Slippi's Dolphin
// download progress; and Play boots P+'s netplay launcher DOL with the Brawl disc as Dolphin's
// default ISO, the way P+ does.
export class DolphinManager {
  private playbackDolphinInstances = new Map<string, PlaybackDolphinInstance>();
  /** The Project+ release each playback Dolphin's card is on (null: the launcher's own). */
  private playbackReleases = new Map<string, string | null>();
  /** A render Dolphin's music and video height (set on its command line). */
  private renderSettings = new Map<string, string>();
  private pplusVersionStore: PPlusVersions | null = null;
  private netplayDolphinInstance: DolphinInstance | null = null;
  private versionCache = new Map<string, string>();
  private bundledInstall: Promise<void> | null = null;
  private projectPlusInstall: Promise<void> | null = null;
  /** True while Settings moves the game files (moveGameFiles): nothing may start Dolphin. */
  private movingGameFiles = false;
  /** Start-up's move of game files out of the launcher's install folder, while it runs. */
  private installFolderRescue: Promise<boolean> | null = null;
  private eventSubject = new Subject<DolphinEvent>();
  private shouldSendProgress = createProgressThrottle(PROGRESS_INTERVAL_MS);
  /** Runs before a new Dolphin starts and throws to stop it (the launcher's update check). */
  private launchGate: () => Promise<void> = async () => undefined;
  /**
   * Play and the playback Dolphin start one at a time: both may copy or patch the SD card copy
   * (pponline-sd/sd.raw). Bug report #19 (2026-10-15): Spectate clicked again during the first
   * start's 2 GB copy started a second copy, which deleted the first one's `.partial`; both failed,
   * every time.
   */
  private sdCardStarts = new Mutex();
  events = Observable.from(this.eventSubject);

  constructor(private settingsManager: SettingsManager) {}

  private renderQueue: RenderQueue | null = null;

  /** The rendered videos' height (Settings > Clips). */
  clipResolution(): ClipResolution {
    return clipResolution(this.settingsManager.get().settings.clipResolution);
  }

  /** Highlights rendered to video (render_queue.ts), into the Clips folder (Settings > Clips). */
  get renders(): RenderQueue {
    if (!this.renderQueue) {
      this.renderQueue = new RenderQueue({
        launch: async (key) => this.launchRenderDolphin(key),
        folder: () => this.settingsManager.getClipsFolder(),
        emit: (job) => this.eventSubject.next({ type: DolphinEventType.RENDER_PROGRESS, job }),
      });
    }
    return this.renderQueue;
  }

  /** No new Dolphin (Play, replays, Configure) starts while `gate` throws. A running one is left alone. */
  setLaunchGate(gate: () => Promise<void>): void {
    this.launchGate = gate;
  }

  /** True while the netplay Dolphin the launcher started (Play, or Configure) is running. */
  isNetplayRunning(): boolean {
    return this.netplayDolphinInstance != null;
  }

  /**
   * Closes the open netplay Dolphin if it has no game (its main window only, after the game was
   * stopped, or Configure), so Play, a room or Other Games can start one without the player closing
   * it first (quit_if_idle.ts). True when no netplay Dolphin is open (any more); false while one is
   * in a game. A playback Dolphin (a replay, a spectated game) is never asked: it has its own User
   * folder and is not the game.
   */
  async closeNetplayIfIdle(): Promise<boolean> {
    if (!this.netplayDolphinInstance) {
      return true;
    }
    const dir = this.netplayOnlineDir();
    await mkdir(dir, { recursive: true });
    return await closeIfIdle(dir, () => this.netplayDolphinInstance != null, { log: (message) => log.info(message) });
  }

  /**
   * The Input Delay setting changed: a running netplay Dolphin (Play, a room, Other Games) takes
   * it from the next frame of its game (input_delay_handoff.ts). Its Dolphin.ini has it already.
   */
  async sendInputDelayToGame(inputDelay: number): Promise<void> {
    if (!this.netplayDolphinInstance) {
      return;
    }
    const dir = this.netplayOnlineDir();
    await mkdir(dir, { recursive: true });
    const req = await writeInputDelayRequest(dir, inputDelay);
    log.info(`Input delay ${inputDelay} sent to the running game (request ${req.id})`);
  }

  /** The netplay User folder (its `Config` has the controller settings). */
  netplayUserFolder(): string {
    return this.getInstallation(DolphinLaunchType.NETPLAY).userFolder;
  }

  /** The netplay User folder's online folder (`user.json`, the room hand-off files). */
  netplayOnlineDir(): string {
    return path.join(this.getInstallation(DolphinLaunchType.NETPLAY).userFolder, DOLPHIN_ONLINE_DIR);
  }

  /** The netplay folder: the bundled Dolphin, its User folder, P+'s files (paths.ts). */
  netplayFolder(): string {
    return netplayFolder(app.getPath("userData"), this.settingsManager.get().settings.gameFilesFolder);
  }

  private _pathEnv(): DolphinPathEnv {
    return {
      env: process.env,
      isDevelopment: !app.isPackaged,
      devRepoRoot: process.cwd(),
      netplayFolder: this.netplayFolder(),
    };
  }

  /** The configured executable, or the default for this machine. */
  getDolphinExecutablePath(launchType: DolphinLaunchType): string {
    return this.settingsManager.getDolphinPath(launchType) ?? defaultDolphinExecutable(launchType, this._pathEnv());
  }

  getInstallation(launchType: DolphinLaunchType): LocalDolphinInstallation {
    const exePath = this.getDolphinExecutablePath(launchType);
    const portable = existsSync(path.join(path.dirname(exePath), "portable.txt"));
    // Only the netplay User folder is seeded with P+'s files (2 GB SD card). Playback has
    // nothing to play until our replay format exists, so it gets a plain User folder.
    const template = launchType === DolphinLaunchType.NETPLAY ? defaultUserTemplate(this._pathEnv()) : null;
    const dataFolder =
      launchType === DolphinLaunchType.NETPLAY
        ? this.netplayFolder()
        : path.join(app.getPath("userData"), launchTypeFolder(launchType));
    return new LocalDolphinInstallation(launchType, exePath, dataFolder, template, portable);
  }

  /**
   * Slippi downloads or updates its Dolphin here. We check the configured build
   * and prepare its User folder; the update channel hook is a stub for now.
   */
  async installDolphin(dolphinType: DolphinLaunchType): Promise<void> {
    const useBeta = false;
    const update = await fetchLatestVersion(dolphinType, useBeta).catch((err) => {
      log.warn(`Could not check for Dolphin updates: ${err}`);
      return null;
    });
    if (update) {
      log.info(`Dolphin ${update.version} is available, but installing updates is not implemented yet`);
    }

    if (this._gameFilesInInstallFolder() && (await this._moveGameFilesOutOfInstallFolder())) {
      // The move installs both Dolphins again in the new folder.
      return;
    }

    const dolphinInstall = this.getInstallation(dolphinType);
    try {
      await this._installBundledDolphin();
      if (dolphinType === DolphinLaunchType.NETPLAY) {
        await this.ensureNetplayUserFolder();
        await this._ensureProjectPlusFiles(dolphinInstall);
      }
      await dolphinInstall.validate({
        onStart: () => this._onStart(dolphinType),
        onProgress: (current, total) => this._onProgress(dolphinType, current, total),
        onComplete: () => undefined,
        dolphinDownloadInfo: { version: "", downloadUrls: { darwin: "", linux: "", win32: "" } },
      });
    } catch (err) {
      log.error(err);
      // Still mark it as ready so Play shows the real error (missing executable) when pressed.
      this._onComplete(dolphinType, undefined);
      throw err;
    }

    const isoPath = this.settingsManager.get().settings.isoPath;
    if (isoPath) {
      const gameDir = path.dirname(isoPath);
      await dolphinInstall.addGamePath(gameDir);
      if (dolphinType === DolphinLaunchType.NETPLAY) {
        await dolphinInstall.setDefaultIso(isoPath);
      }
    }

    // Ready as soon as the build is found. `--version` can take many seconds (our build
    // initialises Qt first), so report the version when it arrives, cached per executable.
    const exePath = this.getDolphinExecutablePath(dolphinType);
    this._onComplete(dolphinType, this._cachedVersion(dolphinType));
    if (!this.versionCache.has(exePath)) {
      void dolphinInstall.getDolphinVersion().then((version) => {
        if (version) {
          this.versionCache.set(exePath, version);
        }
        this._onComplete(dolphinType, version);
      });
    }
  }

  /**
   * Plays replays (docs/design/replays.md §6.2) in the playback Dolphin: the same executable as
   * Play, with its own User folder (its own graphics settings and window, and a Brawl save of its
   * own), booting the game from the netplay install's P+ files and patched SD card. A replay's
   * game settings come from the replay itself (Dolphin forces them for the run only).
   */
  /** Whether the playback Dolphin `id` is open. */
  isPlaybackOpen(id = "playback"): boolean {
    return this.playbackDolphinInstances.has(id);
  }

  /** What the playback Dolphin `id` plays (its status file), or null when it isn't open. */
  async playbackStatus(id = "playback"): Promise<ReplayPlaybackStatus | null> {
    return (await this.playbackDolphinInstances.get(id)?.readStatus()) ?? null;
  }

  /** Closes the playback Dolphin `id`. */
  closePlaybackDolphin(id = "playback"): void {
    this.playbackDolphinInstances.get(id)?.stop();
  }

  /** Where spectated games are written (docs/design/spectating.md §3.3): the playback User folder's `Spectate`. */
  spectateDir(): string {
    return path.join(this.getInstallation(DolphinLaunchType.PLAYBACK).userFolder, "Spectate");
  }

  /**
   * Plays `replayComm` in the playback Dolphin `id`. `pplusRelease`: the Project+ release the
   * replays were recorded on, when it is not the launcher's own: the Dolphin then boots that
   * release's card (pplus_versions.ts; its files must be here: preparePPlusRelease), and a running
   * one on another release is closed first.
   */
  async launchPlaybackDolphin(
    id: string,
    replayComm: ReplayCommunication,
    { pplusRelease = null, render }: { pplusRelease?: string | null; render?: RenderDolphinSettings } = {},
  ): Promise<void> {
    const release = pplusRelease && pplusRelease !== this.pplusVersions().currentRelease ? pplusRelease : null;
    // A second request waits, then finds the Dolphin the first one opened.
    await this.sdCardStarts.runExclusive(() => this._launchPlaybackDolphin(id, replayComm, release, render));
  }

  /**
   * The render Dolphin (render_queue.ts): the playback Dolphin with no window (`-b --render`),
   * started by the first request, in its own User folder (`<userData>/render/User`) so that it never
   * shares a NAND or a log with a playback Dolphin open at the same time.
   */
  launchRenderDolphin({ release, music, resolution }: RenderKey): RenderDolphin {
    const id = "render";
    return {
      play: (comm) => this.launchPlaybackDolphin(id, comm, { pplusRelease: release, render: { music, resolution } }),
      readStatus: async () => (await this.playbackDolphinInstances.get(id)?.readStatus()) ?? null,
      isOpen: () => this.playbackDolphinInstances.has(id),
      close: async () => {
        const instance = this.playbackDolphinInstances.get(id);
        if (instance) {
          await this._stopPlayback(id, instance);
        }
      },
    };
  }

  private _renderInstallation(): LocalDolphinInstallation {
    return new LocalDolphinInstallation(
      DolphinLaunchType.PLAYBACK,
      this.getDolphinExecutablePath(DolphinLaunchType.PLAYBACK),
      path.join(app.getPath("userData"), "render"),
    );
  }

  private async _launchPlaybackDolphin(
    id: string,
    replayComm: ReplayCommunication,
    release: string | null,
    render?: RenderDolphinSettings,
  ): Promise<void> {
    const playbackInstallation = render ? this._renderInstallation() : this.getInstallation(DolphinLaunchType.PLAYBACK);
    this._assertNotMoving();
    const netplayInstallation = this.getInstallation(DolphinLaunchType.NETPLAY);
    await this._repairBundledDolphin();
    const dolphinPath = await playbackInstallation.findDolphinExecutable();
    await playbackInstallation.ensureUserFolder();

    const configuring = this.playbackDolphinInstances.get("configure");
    if (configuring) {
      throw new Error("Cannot open dolphin if a configuring dolphin is open.");
    }
    let playbackInstance = this.playbackDolphinInstances.get(id);
    if (playbackInstance && (this.playbackReleases.get(id) ?? null) !== release) {
      // Its card is another Project+ release's: the game's files are read at boot, so it starts again.
      log.info(`Restarting the playback Dolphin for Project+ ${release ?? this.pplusVersions().currentRelease}`);
      playbackInstance.replaced = true;
      await this._stopPlayback(id, playbackInstance);
      playbackInstance = undefined;
    }
    if (playbackInstance && render && this.renderSettings.get(id) !== JSON.stringify(render)) {
      // (Its music and resolution are on its command line.)
      playbackInstance.replaced = true;
      await this._stopPlayback(id, playbackInstance);
      playbackInstance = undefined;
    }
    if (!playbackInstance) {
      await this.launchGate();
      const isoPath = await this._getIsoPath();
      Preconditions.checkExists(isoPath, "No Brawl disc image set. Choose one in Settings > Game.");
      await this.ensureNetplayUserFolder();
      await this._ensureProjectPlusFiles(netplayInstallation);
      await netplayInstallation.assertProjectPlusFiles();
      // The template is in the netplay folder (also when Settings moved it), not in userData
      const storeDir = projectPlusStoreFolder(this.netplayFolder());
      await this._ensureBrawlSave(playbackInstallation, storeDir);
      await playbackInstallation.addGamePath(path.dirname(isoPath));
      await playbackInstallation.setDefaultIso(isoPath);
      // The launcher's Music setting: off applies P+'s Music Off code here too. (A render's is the
      // job's, on its command line.)
      if (!render) {
        await playbackInstallation.updateSettings({ enableMusic: this.settingsManager.getEnableMusic() });
      }
      // A Play Dolphin (a game, a queue, a room) may have the card open: spectating then shares it.
      const inUse = this.netplayDolphinInstance != null || this.playbackDolphinInstances.size > 0;
      const sdCardImage = await this._withProgress(DolphinLaunchType.PLAYBACK, "prepareSdCard", (onProgress) =>
        release
          ? this._releaseCard(netplayInstallation, release, onProgress)
          : this._installOnlinePlugin(netplayInstallation, true, inUse, onProgress),
      );
      this.playbackReleases.set(id, release);
      const params = buildPlaybackDolphinArgs({
        userArgs: playbackInstallation.userArgs(),
        sdCardImage,
        bootFile: netplayInstallation.netplayLauncherDol,
        testMode: readTestMode(process.env, app.isPackaged),
      });
      if (render) {
        params.push(...buildRenderDolphinArgs(render));
        this.renderSettings.set(id, JSON.stringify(render));
      }
      // The playback NAND must have a Brawl save, or P+ asks to create one before every first replay
      await this._checkBrawlSave(playbackInstallation, storeDir);
      log.info(`Launching playback dolphin at path: ${dolphinPath} ${params.join(" ")}`);
      playbackInstance = new PlaybackDolphinInstance(dolphinPath, params);
      const created = playbackInstance;
      playbackInstance.on("close", async (exitCode) => {
        this.eventSubject.next({
          type: DolphinEventType.CLOSED,
          instanceId: id,
          dolphinType: DolphinLaunchType.PLAYBACK,
          exitCode,
          replaced: created.replaced || undefined,
        });

        // Remove the instance from the map on close
        this.playbackDolphinInstances.delete(id);
        this.playbackReleases.delete(id);
        this.renderSettings.delete(id);
      });
      playbackInstance.on("error", (err: Error) => {
        log.error(err);
        throw err;
      });
      // A replay from another version: ask, as §6.5 of docs/design/replays.md says.
      const instance = playbackInstance;
      // (A render gives up instead: the player chose before it was queued.)
      instance.onNeedsPlayAnyway = render
        ? null
        : (status) => {
            log.info(`Replay refused: ${status.error ?? ""}`);
            void dialog
              .showMessageBox({
                type: "question",
                buttons: ["Play anyway", "Cancel"],
                defaultId: 1,
                cancelId: 1,
                title: "Replay",
                message: "This replay may not play as it was played.",
                detail: `${status.error ?? "It was recorded with another version."}\n\nPlay it anyway?`,
              })
              .then(({ response }) => (response === 0 ? instance.playAnyway() : undefined))
              .catch((err) => log.error(err));
          };

      instance.onStatus = (status) => {
        this.eventSubject.next({ type: DolphinEventType.PLAYBACK_STATUS, instanceId: id, status });
      };

      this.playbackDolphinInstances.set(id, playbackInstance);
    }

    try {
      await playbackInstance.play(replayComm);
    } catch (err) {
      if (err instanceof MacOsRosettaRequiredError) {
        this.eventSubject.next({
          type: DolphinEventType.ERROR,
          errorType: DolphinErrorType.ROSETTA_REQUIRED,
          dolphinType: DolphinLaunchType.PLAYBACK,
        });
      }
      throw err;
    }
  }

  /**
   * Starts the netplay Dolphin. It boots P+ when the Play button is set to (`launchGameOnPlay`) or
   * when `bootGame` says so: a hand-off (an invite, a room, Create room) leaves a request only a
   * booted game reads, and closing a Dolphin that never booted takes it back.
   */
  async launchNetplayDolphin(opts: { bootGame?: boolean } = {}): Promise<NetplayLaunchResult> {
    return await this.sdCardStarts.runExclusive(() => this._launchNetplayDolphin(opts));
  }

  /**
   * Puts an SD card of another Project+ release aside (kept as `sd (Project+ <release>).raw`) and
   * gets the launcher's release instead, as on the first start (bug report #74).
   */
  async replaceProjectPlusCard(): Promise<void> {
    await this.sdCardStarts.runExclusive(async () => {
      const installation = this.getInstallation(DolphinLaunchType.NETPLAY);
      const release = this._pplusRelease().version;
      const other = await otherProjectPlusBuild(installation.sdCardImage, release);
      if (other) {
        const kept = await setAsideProjectPlusCard(installation.userFolder, other);
        log.warn(`The SD card was on Project+ ${other}, not ${release}: kept as ${kept}; getting ${release}`);
      }
      try {
        await this._ensureProjectPlusFiles(installation);
      } finally {
        this._onComplete(DolphinLaunchType.NETPLAY, this._cachedVersion(DolphinLaunchType.NETPLAY));
      }
    });
  }

  private async _launchNetplayDolphin(opts: { bootGame?: boolean }): Promise<NetplayLaunchResult> {
    // An open Dolphin without a game closes first (closeNetplayIfIdle).
    if (this.netplayDolphinInstance && !(await this.closeNetplayIfIdle())) {
      throw new Error("Dolphin is already open with a game. Close it first.");
    }
    this._assertNotMoving();
    await this.launchGate();

    const netplayInstallation = this.getInstallation(DolphinLaunchType.NETPLAY);
    await this._repairBundledDolphin();
    const dolphinPath = await netplayInstallation.findDolphinExecutable();
    await netplayInstallation.ensureUserFolder();
    await this._updateDolphinSettings(DolphinLaunchType.NETPLAY);

    const testMode = readTestMode(process.env, app.isPackaged);
    const launchGameOnPlay = opts.bootGame || this.settingsManager.get().settings.launchGameOnPlay;
    let bootFile: string | null = null;
    if (launchGameOnPlay) {
      // Boot P+ the way P+ does: its netplay launcher DOL applies the codeset from the
      // virtual SD card, then boots Dolphin's default ISO (the Brawl disc).
      const isoPath = await this._getIsoPath();
      Preconditions.checkExists(isoPath, "No Brawl disc image set. Choose one in Settings > Game.");
      await netplayInstallation.assertProjectPlusFiles();
      // Players on different Project+ releases desync at once (bug report #74).
      const release = this._pplusRelease().version;
      const other = await otherProjectPlusBuild(netplayInstallation.sdCardImage, release);
      if (other) {
        log.warn(`Not starting the game: the SD card is on Project+ ${other}, not ${release}`);
        return { status: "wrongPPlusBuild", cardRelease: other, release };
      }
      const storeDir = projectPlusStoreFolder(this.netplayFolder());
      await ensureNetplaySave(storeDir, netplayInstallation.userFolder);
      await this._ensureBrawlSave(netplayInstallation, storeDir);
      await netplayInstallation.setDefaultIso(isoPath);
      bootFile = netplayInstallation.netplayLauncherDol;
    }
    // A playback Dolphin (a replay, a spectated game) may have the card open.
    const inUse = this.playbackDolphinInstances.size > 0;
    const sdCardImage = await this._withProgress(DolphinLaunchType.NETPLAY, "prepareSdCard", (onProgress) =>
      this._installOnlinePlugin(netplayInstallation, launchGameOnPlay, inUse, onProgress),
    );
    const params = buildNetplayDolphinArgs({
      userArgs: netplayInstallation.userArgs(),
      sdCardImage,
      bootFile,
      testMode,
    });
    log.info(`Launching dolphin at path: ${dolphinPath} ${params.join(" ")}`);

    // Create the Dolphin instance and start it
    const dolphinInstance = new DolphinInstance(dolphinPath, params);
    dolphinInstance.on("close", async (exitCode: number | null, signal: string | null) => {
      try {
        await this._updateLauncherSettings(DolphinLaunchType.NETPLAY);
      } catch (e) {
        log.error("Error encountered updating launcher settings.", e);
      }
      this.eventSubject.next({
        type: DolphinEventType.CLOSED,
        dolphinType: DolphinLaunchType.NETPLAY,
        exitCode,
      });

      this.netplayDolphinInstance = null;
      // A room request the game never picked up must not act on the next start.
      void clearRoomRequest(this.netplayOnlineDir()).catch(() => undefined);
      log.warn(`Dolphin exit code: ${exitCode?.toString(16)}`);
      log.warn(`Dolphin exit signal: ${signal}`);
      await this._keepCrashLog(netplayInstallation, exitCode, signal);
    });
    dolphinInstance.on("error", (err: Error) => {
      log.error(err);
      throw err;
    });

    try {
      await dolphinInstance.start();
    } catch (err) {
      if (err instanceof MacOsRosettaRequiredError) {
        this.eventSubject.next({
          type: DolphinEventType.ERROR,
          errorType: DolphinErrorType.ROSETTA_REQUIRED,
          dolphinType: DolphinLaunchType.NETPLAY,
        });
      }
      throw err;
    }
    this.netplayDolphinInstance = dolphinInstance;
    return { status: "started" };
  }

  /**
   * Starts our Dolphin for a Brawl Mods room (docs/design/brawl-mods.md §3): the build instead of
   * P+ (no P+ files, no online plugin on its SD card), and the room to open or join. It is the
   * netplay Dolphin (its User folder, the login): Play and a mod room do not run at once.
   * `onClose` runs when it exits.
   */
  async launchModsDolphin(build: ModBuild, room: ModsRoomArg, onClose?: () => void | Promise<void>) {
    Preconditions.checkState(this.netplayDolphinInstance == null, "Dolphin is already open. Close it first.");
    await this.launchGate();
    const installation = this.getInstallation(DolphinLaunchType.NETPLAY);
    await this._repairBundledDolphin();
    const dolphinPath = await installation.findDolphinExecutable();
    await installation.ensureUserFolder();
    await this._updateDolphinSettings(DolphinLaunchType.NETPLAY);
    await clearModRoomFiles(this.netplayOnlineDir());
    // Builds' saves that started from P+'s save go, once (mods/template_saves.ts).
    await clearTemplateSeededSaves(installation.userFolder, (message) => log.warn(message)).catch((err) =>
      log.error(`Could not clear the builds' saves made from P+'s save: ${err}`),
    );
    const params = buildModsDolphinArgs({
      userArgs: installation.userArgs(),
      build,
      room,
      testMode: readTestMode(process.env, app.isPackaged),
    });
    log.info(`Launching dolphin for a Brawl Mods room at path: ${dolphinPath} ${params.join(" ")}`);
    const dolphinInstance = new DolphinInstance(dolphinPath, params);
    dolphinInstance.on("close", async (exitCode: number | null, signal: string | null) => {
      this.netplayDolphinInstance = null;
      this.eventSubject.next({ type: DolphinEventType.CLOSED, dolphinType: DolphinLaunchType.NETPLAY, exitCode });
      log.warn(`Dolphin (Brawl Mods) exit code: ${exitCode?.toString(16)}, signal: ${signal}`);
      // The room file's last words first (why a join failed), then it goes.
      await Promise.resolve(onClose?.()).catch((err) => log.error(err));
      void clearModRoomFiles(this.netplayOnlineDir()).catch(() => undefined);
      await this._keepCrashLog(installation, exitCode, signal);
    });
    dolphinInstance.on("error", (err: Error) => {
      log.error(err);
    });
    try {
      await dolphinInstance.start();
    } catch (err) {
      if (err instanceof MacOsRosettaRequiredError) {
        this.eventSubject.next({
          type: DolphinEventType.ERROR,
          errorType: DolphinErrorType.ROSETTA_REQUIRED,
          dolphinType: DolphinLaunchType.NETPLAY,
        });
      }
      throw err;
    }
    this.netplayDolphinInstance = dolphinInstance;
  }

  /** `extraArgs` (netplay only): more arguments (`--controllers`); `onClose`: after it closed. */
  async configureDolphin(launchType: DolphinLaunchType, options: { extraArgs?: string[]; onClose?: () => void } = {}) {
    log.debug(`configuring ${launchType} dolphin...`);
    this._assertNotMoving();

    const installation = this.getInstallation(launchType);
    const dolphinPath = await installation.findDolphinExecutable();
    await installation.ensureUserFolder();
    await this._updateDolphinSettings(launchType);
    if (launchType === DolphinLaunchType.NETPLAY && !this.netplayDolphinInstance) {
      await this.launchGate();
      const instance = new DolphinInstance(dolphinPath, [...installation.userArgs(), ...(options.extraArgs ?? [])]);
      instance.on("close", async (exitCode: number | null, signal: string | null) => {
        try {
          await this._updateLauncherSettings(launchType);
        } catch (e) {
          log.error("Error encountered updating launcher settings.", e);
        }
        options.onClose?.();
        this.eventSubject.next({
          type: DolphinEventType.CLOSED,
          dolphinType: DolphinLaunchType.NETPLAY,
          exitCode,
        });
        this.netplayDolphinInstance = null;
        await this._keepCrashLog(installation, exitCode, signal);
      });
      instance.on("error", (err: Error) => {
        log.error(err);
        throw err;
      });
      await instance.start();
      this.netplayDolphinInstance = instance;
    } else if (launchType === DolphinLaunchType.PLAYBACK && this.playbackDolphinInstances.size === 0) {
      await this.launchGate();
      const instanceId = "configure";
      const instance = new PlaybackDolphinInstance(dolphinPath, installation.userArgs());
      instance.on("close", async (exitCode) => {
        this.eventSubject.next({
          type: DolphinEventType.CLOSED,
          dolphinType: DolphinLaunchType.PLAYBACK,
          instanceId,
          exitCode,
        });

        // Remove the instance from the map on close
        this.playbackDolphinInstances.delete(instanceId);
      });
      instance.on("error", (err: Error) => {
        log.error(err);
        throw err;
      });
      await instance.start();
      this.playbackDolphinInstances.set(instanceId, instance);
    }
  }

  /**
   * Slippi's "Reset Dolphin" re-downloads its build. Ours is not downloaded, so a
   * soft reset clears the cache and a hard reset also recreates the User folder
   * (the online `user.json` is rewritten on the next Play).
   */
  async reinstallDolphin(launchType: DolphinLaunchType, cleanInstall?: boolean) {
    switch (launchType) {
      case DolphinLaunchType.NETPLAY: {
        if (this.netplayDolphinInstance !== null) {
          log.warn("A netplay dolphin is open");
          return;
        }
        break;
      }
      case DolphinLaunchType.PLAYBACK: {
        if (this.playbackDolphinInstances.size > 0) {
          log.warn("A playback dolphin is open");
          return;
        }
        break;
      }
    }

    this._assertNotMoving();
    const installation = this.getInstallation(launchType);
    this._onStart(launchType);
    await installation.clearCache();
    if (cleanInstall) {
      await installation.resetUserFolder();
    }
    await this.installDolphin(launchType);
  }

  /**
   * Settings > Dolphin's game files folder: moves the netplay folder to the folder the player
   * picked (null: back to `<userData>/netplay`), see move_game_files.ts, with the Play button
   * showing the progress. A folder that already holds game files (a copy the player made) is used
   * as it is once the player agrees. The old files are deleted afterwards and the bundled Dolphin
   * is installed into the new folder.
   */
  async moveGameFiles(picked: string | null): Promise<GameFilesMoveResult> {
    if (this.movingGameFiles) {
      throw new GameFilesMoveError("The game files are already being moved.");
    }
    this.movingGameFiles = true;
    try {
      return await this._moveGameFiles(picked);
    } finally {
      this.movingGameFiles = false;
    }
  }

  private async _moveGameFiles(picked: string | null): Promise<GameFilesMoveResult> {
    if (this.netplayDolphinInstance || this.playbackDolphinInstances.size > 0) {
      throw new GameFilesMoveError("Close Dolphin first.");
    }
    if (this.projectPlusInstall) {
      throw new GameFilesMoveError("Wait for the Project+ download to finish first.");
    }
    const from = this.netplayFolder();
    const defaultFolder = defaultNetplayFolder(app.getPath("userData"));
    const target: GameFilesTarget =
      picked == null
        ? await defaultGameFilesTarget(defaultFolder, from)
        : await chooseGameFilesTarget(picked, from, PRODUCT_NAME, this._launcherInstallFolder());
    if (target.kind === "adopt" && !(await confirmAdoptGameFiles(target.folder, from))) {
      return { action: "cancelled", folder: from, leftBehind: null };
    }
    // The bundled Dolphin's install must not write into the old folder while it is moved.
    await this.bundledDolphinReady();

    this._onStart(DolphinLaunchType.NETPLAY);
    try {
      if (target.kind === "move") {
        await moveGameFiles({
          from,
          to: target.folder,
          onProgress: (current, total) => this._onProgress(DolphinLaunchType.NETPLAY, current, total, "moveGameFiles"),
          log: (message) => log.info(message),
        });
      } else {
        log.info(`Using the game files already in ${target.folder}; deleting the ones in ${from}`);
      }
      const isDefault = path.relative(target.folder, defaultFolder) === "";
      await this.settingsManager.updateSetting("gameFilesFolder", isDefault ? null : target.folder);
    } catch (err) {
      this._onComplete(DolphinLaunchType.NETPLAY, this._cachedVersion(DolphinLaunchType.NETPLAY));
      throw err;
    }

    // An executable set to the old folder is gone, and one set to the new folder is the default now.
    const settings = this.settingsManager.get().settings;
    const inMovedFolder = (p: string | null) =>
      p != null && [from, target.folder].some((dir) => isSubdirectory(dir, p));
    if (inMovedFolder(settings.netplayDolphinPath)) {
      await this.settingsManager.updateSetting("netplayDolphinPath", null);
    }
    if (inMovedFolder(settings.playbackDolphinPath)) {
      await this.settingsManager.updateSetting("playbackDolphinPath", null);
    }

    let leftBehind: string | null = null;
    try {
      await removeGameFiles(from);
    } catch (err) {
      leftBehind = from;
      log.warn(`Could not delete the old game files in ${from}: ${err instanceof Error ? err.message : String(err)}`);
    }

    this.bundledInstall = null;
    for (const dolphinType of [DolphinLaunchType.NETPLAY, DolphinLaunchType.PLAYBACK]) {
      void this.installDolphin(dolphinType).catch((err) => log.error(err));
    }
    return { action: target.kind === "move" ? "moved" : "adopted", folder: target.folder, leftBehind };
  }

  /** The folder every launcher update deletes or replaces (paths.ts launcherInstallFolder). */
  private _launcherInstallFolder(): string | null {
    return launcherInstallFolder({
      platform: process.platform,
      isPackaged: app.isPackaged,
      exePath: app.getPath("exe"),
    });
  }

  private _gameFilesInInstallFolder(): boolean {
    const folder = this.settingsManager.get().settings.gameFilesFolder;
    return folder != null && isInInstallFolder(folder, this._launcherInstallFolder());
  }

  /**
   * Game files moved into the launcher's install folder (before Settings refused it) are deleted
   * by every launcher update: the User folder with the player's Dolphin settings, hotkeys,
   * controller settings and custom textures, and the SD card (LUCA#19, bug report #42 and
   * 2026-10-19: the launcher in `D:\Emulation\...\Brawl Online`, the game files moved to its
   * `netplay` folder for the second SSD's space). At start-up they are moved next to the install
   * folder, on the same drive (`<parent>\Brawl Online Game Files`), else to `<userData>/netplay`
   * (or `<userData>/game-files` when that already has game files of its own). True if they were
   * moved; when no move works they stay where they are for this run.
   */
  private _moveGameFilesOutOfInstallFolder(): Promise<boolean> {
    if (!this.installFolderRescue) {
      this.installFolderRescue = (async () => {
        const installFolder = this._launcherInstallFolder();
        const userData = app.getPath("userData");
        const fallback = (await fileExists(path.join(defaultNetplayFolder(userData), "User")))
          ? path.join(userData, "game-files")
          : null;
        const targets: (string | null)[] = [
          ...(installFolder ? [path.join(path.dirname(installFolder), `${PRODUCT_NAME} Game Files`)] : []),
          fallback,
        ];
        for (const picked of targets) {
          log.warn(
            `The game files (${this.netplayFolder()}) are inside the launcher's install folder ` +
              `(${installFolder}), which every update deletes: moving them to ` +
              `${picked ?? defaultNetplayFolder(userData)}`,
          );
          try {
            const result = await this.moveGameFiles(picked);
            return result.action !== "cancelled";
          } catch (err) {
            log.error(`Could not move the game files out of the install folder: ${err}`);
          }
        }
        return false;
      })().finally(() => {
        this.installFolderRescue = null;
      });
    }
    return this.installFolderRescue;
  }

  private _assertNotMoving() {
    if (this.movingGameFiles) {
      throw new Error("The game files are being moved. Try again when the move is done.");
    }
  }

  /**
   * The netplay User folder, seeded from the development template the first time (2 GB, with the
   * Play button's progress). Start-up's theme extraction waits for the same seed.
   */
  ensureNetplayUserFolder(): Promise<void> {
    const installation = this.getInstallation(DolphinLaunchType.NETPLAY);
    let started = false;
    return installation.ensureUserFolder((current, total) => {
      if (!started) {
        started = true;
        this._onStart(DolphinLaunchType.NETPLAY);
      }
      this._onProgress(DolphinLaunchType.NETPLAY, current, total, "copyUserFolder");
    });
  }

  /** A step shown on the Play button from its first progress report until it ends (see runWithProgress). */
  private _withProgress<T>(
    dolphinType: DolphinLaunchType,
    phase: DolphinSetupPhase,
    step: (onProgress: (current: number, total: number) => void) => Promise<T>,
  ): Promise<T> {
    return runWithProgress(step, {
      start: () => this._onStart(dolphinType),
      progress: (current, total) => this._onProgress(dolphinType, current, total, phase),
      complete: () => this._onComplete(dolphinType, this._cachedVersion(dolphinType)),
    });
  }

  /** The version `--version` printed for this Dolphin, once it has (undefined before). */
  cachedDolphinVersion(dolphinType: DolphinLaunchType): string | undefined {
    return this._cachedVersion(dolphinType);
  }

  private _cachedVersion(dolphinType: DolphinLaunchType): string | undefined {
    return this.versionCache.get(this.getDolphinExecutablePath(dolphinType));
  }

  /**
   * Installs the Dolphin build bundled with the launcher into `<userData>/netplay` when it is
   * missing or older (after a launcher update). Shared by the netplay and playback set-up, which
   * run at the same time on start-up. Progress is reported like Slippi's Dolphin download.
   */
  private _installBundledDolphin(): Promise<void> {
    const source = bundledDolphinSource({
      env: process.env,
      isPackaged: app.isPackaged,
      resourcesPath: process.resourcesPath,
    });
    if (!source) {
      return Promise.resolve();
    }
    if (!this.bundledInstall) {
      let started = false;
      this.bundledInstall = installBundledDolphin({
        sourceDir: source,
        destDir: this.netplayFolder(),
        onProgress: (current, total) => {
          if (!started) {
            started = true;
            this._onStart(DolphinLaunchType.NETPLAY);
          }
          this._onProgress(DolphinLaunchType.NETPLAY, current, total, "installDolphin");
        },
        log: (message) => log.info(message),
      })
        .then(({ action, manifest }) => {
          log.info(`Bundled Dolphin ${manifest.version}: ${action}`);
        })
        .catch((err) => {
          this.bundledInstall = null;
          throw err;
        });
    }
    return this.bundledInstall;
  }

  /**
   * Waits for the bundled Dolphin's install, starting it if it has not started. The theme
   * extraction runs DolphinTool from the install folder, and the install cannot replace a running
   * DolphinTool on Windows. Never throws: a failed install is reported by the Dolphin set-up.
   */
  async bundledDolphinReady(): Promise<void> {
    await this._installBundledDolphin().catch(() => undefined);
  }

  /**
   * Start-up's install of the bundled Dolphin removes the old build before it copies the new one,
   * so one that stopped half-way (a file in use) leaves no executable. Play then installs it
   * again, with the set-up progress, instead of failing until the launcher restarts.
   */
  private async _repairBundledDolphin(): Promise<void> {
    if (await fileExists(this.getDolphinExecutablePath(DolphinLaunchType.NETPLAY))) {
      return;
    }
    try {
      await this._installBundledDolphin();
    } finally {
      this._onComplete(DolphinLaunchType.NETPLAY, this._cachedVersion(DolphinLaunchType.NETPLAY));
    }
  }

  /**
   * Downloads P+'s files from P+'s official release (pinned, sha256-checked) into the netplay
   * User folder when they are missing, and puts the Brawl save template into the User folder
   * (`<User>/NetplaySave`). Nothing is downloaded when they are in place (also when a development User
   * template provided them and the Dolphin build has its own save template).
   */
  private _ensureProjectPlusFiles(installation: LocalDolphinInstallation): Promise<void> {
    if (!this.projectPlusInstall) {
      const storeDir = projectPlusStoreFolder(this.netplayFolder());
      const target: PPlusTarget = {
        userFolder: installation.userFolder,
        storeDir,
        downloadDir: path.join(storeDir, "downloads"),
        // The release before the update keeps its files for its replays: no download for them later.
        beforeCardUpdate: (card, paths) => this.pplusVersions().keepReplacedFiles(card, paths),
      };
      const release = this._pplusRelease();
      this.projectPlusInstall = (async () => {
        let missing = await missingProjectPlusFiles(target, release);
        // A development build may carry its own template in Sys (Dolphin falls back to it).
        if (missing.includes(NETPLAY_SAVE_DIR) && existsSync(path.join(installation.sysFolder, NETPLAY_SAVE_DIR))) {
          missing = missing.filter((m) => m !== NETPLAY_SAVE_DIR);
        }
        if (missing.length > 0) {
          // The button changes with the first progress report (installDolphin ends it).
          let started = false;
          await installProjectPlusFiles({
            target,
            release,
            onProgress: ({ phase, current, total }) => {
              if (!started) {
                started = true;
                this._onStart(DolphinLaunchType.NETPLAY);
              }
              this._onProgress(DolphinLaunchType.NETPLAY, current, total, PPLUS_PHASES[phase]);
            },
            log: (message) => log.info(message),
          });
        }
        await ensureNetplaySave(storeDir, installation.userFolder);
      })().finally(() => {
        this.projectPlusInstall = null;
      });
    }
    return this.projectPlusInstall;
  }

  /** The Project+ release Play uses (a test may pin another). */
  private _pplusRelease(): PPlusRelease {
    return readTestMode(process.env, app.isPackaged).pplusRelease ?? PPLUS_RELEASE;
  }

  /** After a run that ended abnormally, keeps its log out of Dolphin's two-run rotation (crash_log.ts). */
  private async _keepCrashLog(
    installation: LocalDolphinInstallation,
    exitCode: number | null,
    signal: string | null,
  ): Promise<void> {
    if (!isAbnormalExit(exitCode, signal)) {
      return;
    }
    try {
      const kept = await keepCrashLog(path.join(installation.userFolder, "Logs"));
      if (kept) {
        log.warn(`Kept the crashed run's log as ${kept}`);
      }
    } catch (e) {
      log.error("Could not keep the crashed run's log.", e);
    }
  }

  /**
   * Seeds the NAND Play boots with P+'s Brawl save template when it has no Brawl save, so P+
   * never asks "Create save file for Project+?" (install/brawl_save.ts). An existing save is never
   * touched. A failure is logged and Play goes on: the game then asks, as P+ does.
   */
  private async _ensureBrawlSave(installation: LocalDolphinInstallation, storeDir: string): Promise<void> {
    try {
      const result = await ensureBrawlSave({
        nandRoot: await installation.nandRoot(),
        templateRoots: [
          netplaySaveStore(storeDir),
          path.join(installation.userFolder, NETPLAY_SAVE_DIR),
          path.join(this.getInstallation(DolphinLaunchType.NETPLAY).userFolder, NETPLAY_SAVE_DIR),
          path.join(installation.sysFolder, NETPLAY_SAVE_DIR),
        ],
        log: (message) => log.info(message),
      });
      if (result.action !== "present" && result.removed.length > 0) {
        log.warn(`Brawl save: replaced one without its banner in ${result.dir} (${result.removed.join(", ")})`);
      }
      if (result.action === "no-template") {
        log.warn(`Brawl save: no template for ${result.dir}`);
      } else if (result.action !== "seeded") {
        log.info(`Brawl save: ${result.action} (${result.dir})`);
      }
    } catch (err) {
      log.error(`Could not seed the Brawl save: ${err instanceof Error ? err.message : String(err)}`);
    }
  }

  /**
   * Right before a Dolphin starts: its NAND has a Brawl save, else it is seeded again (a playback
   * Dolphin on the user's prod, 2026-10-12, met P+'s "Create save file" prompt although the seed ran
   * earlier in the same launch). Logged as warnings, which a release's log keeps.
   */
  private async _checkBrawlSave(installation: LocalDolphinInstallation, storeDir: string): Promise<void> {
    const hasSave = async () => {
      const dir = brawlSaveDir(await installation.nandRoot());
      try {
        const files = (await readdir(dir, { withFileTypes: true })).filter((e) => e.isFile());
        return { dir, ok: isUsableBrawlSave(files.map((e) => e.name)) };
      } catch {
        return { dir, ok: false };
      }
    };
    const before = await hasSave();
    if (before.ok) {
      return;
    }
    log.warn(`No Brawl save in ${before.dir} before starting Dolphin (${installation.userFolder}); seeding it`);
    await this._ensureBrawlSave(installation, storeDir);
    const after = await hasSave();
    log.warn(`Brawl save in ${after.dir}: ${after.ok ? "seeded" : "still missing (P+ will ask to create one)"}`);
  }

  /**
   * Older Project+ releases for replays (pplus_versions.ts), in `<netplay>/pplus-versions`. The
   * launcher's own release is the pinned one (or the test mode's).
   */
  pplusVersions(): PPlusVersions {
    const root = path.join(this.netplayFolder(), "pplus-versions");
    const test = readTestMode(process.env, app.isPackaged);
    const currentRelease = (test.pplusRelease ?? PPLUS_RELEASE).version;
    if (!this.pplusVersionStore || this.pplusVersionStore.cardDir("") !== path.join(root, "cards")) {
      const url = test.active ? process.env.PPO_PPLUS_VERSIONS_URL?.trim() : undefined;
      this.pplusVersionStore = new PPlusVersions({
        root,
        currentRelease,
        baseUrl: url || undefined,
        log: (message) => log.info(message),
      });
    }
    return this.pplusVersionStore;
  }

  /** Closes a playback Dolphin and waits until it has gone (10 s at most). */
  private async _stopPlayback(id: string, instance: PlaybackDolphinInstance): Promise<void> {
    await new Promise<void>((resolve) => {
      const timer = setTimeout(resolve, 10_000);
      instance.once("close", () => {
        clearTimeout(timer);
        // (after the instance's own close handler, which removes it)
        setImmediate(resolve);
      });
      instance.stop();
    });
    this.playbackDolphinInstances.delete(id);
    this.playbackReleases.delete(id);
    this.renderSettings.delete(id);
  }

  /** The playback card of another Project+ release, with the plugin on it (made or checked). */
  private async _releaseCard(
    installation: LocalDolphinInstallation,
    release: string,
    onProgress?: (current: number, total: number) => void,
  ): Promise<string> {
    const env = this._pathEnv();
    const template = defaultUserTemplate(env);
    const plugin = await loadPlugin(defaultPluginLocation({ ...env, resourcesPath: process.resourcesPath }));
    const result = await this.pplusVersions().card(release, installation.sdCardImage, plugin, {
      protectedPaths: template ? [template] : [],
      onProgress,
    });
    log.info(`Project+ ${release} card for replays: ${result.image} (${result.action})`);
    return result.image;
  }

  /** Every Project+ release Brawl Online knows and what of it is on this PC. */
  async pplusVersionStates(): Promise<{ current: string; states: PPlusVersionState[] }> {
    const versions = this.pplusVersions();
    return { current: versions.currentRelease, states: await versions.states() };
  }

  /**
   * Gets `release` ready for replays: its files (from `importFrom`, a card or a Project+ folder of
   * that release on this PC, else downloaded), then its card. Progress through `onProgress`.
   */
  async preparePPlusRelease(
    release: string,
    { importFrom, onProgress }: { importFrom?: string; onProgress?: (p: PrepareProgress) => void } = {},
  ): Promise<PPlusVersionState> {
    const versions = this.pplusVersions();
    const netplayInstallation = this.getInstallation(DolphinLaunchType.NETPLAY);
    // The player's card first: an older release is made from it.
    await this._ensureProjectPlusFiles(netplayInstallation);
    onProgress?.({ phase: "index", current: 0, total: 1 });
    await versions.index({ refresh: true });
    if (importFrom) {
      const card = await findProjectPlusCard(importFrom);
      if (!card) {
        throw new PPlusVersionError("damaged", `No Project+ SD card (sd.raw) was found in ${importFrom}.`);
      }
      const { missing } = await versions.importFromCard(release, card);
      if (missing > 0) {
        throw new PPlusVersionError(
          "card-unknown",
          `That card is not Project+ ${pplusReleaseLabel(release)}: ${missing} of its files are missing or different.`,
        );
      }
    } else {
      await versions.download(release, (current, total) => onProgress?.({ phase: "download", current, total }));
    }
    // The card now (2 GB copied once), unless a playback Dolphin has it open.
    const running = [...this.playbackReleases.entries()].some(([, r]) => r === release);
    if (!running) {
      await this.sdCardStarts.runExclusive(() =>
        this._releaseCard(netplayInstallation, release, (current, total) =>
          onProgress?.({ phase: "card", current, total }),
        ),
      );
    }
    return versions.state(release);
  }

  /**
   * Which Project+ release replays play on (docs/design/pplus-versions.md): their own when it is
   * ready (`playing`, with the paths of the first replay's release: a request mixing releases plays
   * those only), else what it needs first. `current` plays them on the launcher's release anyway
   * (as recorded with another version: `playAnyway`).
   */
  async decideReplayRelease(
    paths: string[],
    choice: ReplayPlayChoice,
  ): Promise<{ result: ReplayPlayResult; paths: string[]; release: string | null; playAnyway: boolean }> {
    const versions = this.pplusVersions();
    const current = versions.currentRelease;
    const releases = await Promise.all(
      paths.map((p) =>
        readReplayHeader(p).then(
          (h) => (h.meta.pplusRelease === current ? null : h.meta.pplusRelease),
          () => null,
        ),
      ),
    );
    const first = releases[0] ?? null;
    const group = paths.filter((_, i) => releases[i] === first);
    const skippedAt = releases.findIndex((r) => r !== first);
    const skipped = paths.length - group.length;
    const skippedRelease = skippedAt >= 0 ? releases[skippedAt] ?? current : undefined;
    const playing = (release: string | null, playAnyway: boolean) => ({
      result: { status: "playing" as const, release, skipped, skippedRelease },
      paths: group,
      release,
      playAnyway,
    });
    if (first == null) {
      return playing(null, false);
    }
    if (choice === "current") {
      return playing(null, true);
    }
    if (!PLAYABLE_PPLUS_RELEASES.includes(first)) {
      return {
        result: { status: "unplayable", release: first, current },
        paths: group,
        release: null,
        playAnyway: false,
      };
    }
    await versions.index();
    const state = await versions.state(first);
    if (state.filesReady) {
      return playing(first, false);
    }
    return {
      result: { status: "needs-files", release: first, current, downloadBytes: state.downloadBytes },
      paths: group,
      release: null,
      playAnyway: false,
    };
  }

  /**
   * What rendering `paths` needs (render_queue.ts): each replay's Project+ release (null: the
   * launcher's own) once all of them are here (`result` null); else, as for playing, the first
   * release whose files are not here yet (`needs-files`) or that this launcher cannot play
   * (`unplayable`). `current`: every one on the launcher's release, played anyway.
   */
  async decideRenderReleases(
    paths: string[],
    choice: ReplayPlayChoice,
  ): Promise<{
    result: Exclude<ReplayPlayResult, { status: "playing" }> | null;
    releases: (string | null)[];
    playAnyway: boolean;
  }> {
    const versions = this.pplusVersions();
    const current = versions.currentRelease;
    const releases = await Promise.all(
      paths.map((p) =>
        readReplayHeader(p).then(
          (h) => (h.meta.pplusRelease === current ? null : h.meta.pplusRelease),
          () => null,
        ),
      ),
    );
    if (choice === "current") {
      return { result: null, releases: paths.map(() => null), playAnyway: releases.some((r) => r != null) };
    }
    const others = [...new Set(releases.filter((r): r is string => r != null))];
    for (const release of others) {
      if (!PLAYABLE_PPLUS_RELEASES.includes(release)) {
        return { result: { status: "unplayable", release, current }, releases, playAnyway: false };
      }
    }
    if (others.length) {
      await versions.index();
    }
    for (const release of others) {
      const state = await versions.state(release);
      if (!state.filesReady) {
        return {
          result: { status: "needs-files", release, current, downloadBytes: state.downloadBytes },
          releases,
          playAnyway: false,
        };
      }
    }
    return { result: null, releases, playAnyway: false };
  }

  /** Deletes an older release's files and card (closing a playback Dolphin that has it open). */
  async removePPlusRelease(release: string): Promise<void> {
    for (const [id, r] of [...this.playbackReleases.entries()]) {
      const instance = this.playbackDolphinInstances.get(id);
      if (r === release && instance) {
        await this._stopPlayback(id, instance);
      }
    }
    await this.pplusVersions().remove(release);
  }

  /**
   * Puts our game plugin on a launcher-managed copy of the user's P+ SD card
   * (`<userData>/netplay/pponline-sd/sd.raw`) and returns its path for Dolphin. The user's
   * card (`<User>/Wii/sd.raw`) and the User template are never written. Without a card,
   * nothing is installed unless Play boots the game (`required`), which then fails. While another
   * of our Dolphins runs (`inUse`), the copy it booted is shared as it is and never written.
   */
  private async _installOnlinePlugin(
    installation: LocalDolphinInstallation,
    required: boolean,
    inUse: boolean,
    onProgress?: (current: number, total: number) => void,
  ): Promise<string | null> {
    const sourceImage = installation.sdCardImage;
    if (!required && !(await fileExists(sourceImage))) {
      return null;
    }
    const env = this._pathEnv();
    const template = defaultUserTemplate(env);
    try {
      const plugin = await loadPlugin(defaultPluginLocation({ ...env, resourcesPath: process.resourcesPath }));
      const result = await installPluginOnSdCard({
        sourceImage,
        outputDir: patchedSdCardFolder(env.netplayFolder),
        plugin,
        protectedPaths: template ? [template] : [],
        log: (message) => log.info(message),
        onProgress,
        inUse,
      });
      log.info(`Online plugin ${plugin.sha256.slice(0, 12)} on ${result.image}: ${result.action}`);
      return result.image;
    } catch (err) {
      log.error(err);
      if (err instanceof SdCardInUseError) {
        // The launcher updated (a new plugin) or the P+ card changed while the other Dolphin ran.
        throw new Error("The game's SD card needs an update. Close the other Dolphin window, then try again.");
      }
      const code = (err as NodeJS.ErrnoException | undefined)?.code;
      if (code === "EBUSY" || code === "EPERM") {
        // Windows: a Dolphin started before a launcher update still has the card open
        throw new Error("Another Dolphin is using the game files. Close it, then try again.");
      }
      throw new Error(
        `Could not install the online game plugin on the Project+ SD card: ${
          err instanceof Error ? err.message : String(err)
        }`,
      );
    }
  }

  private async _getIsoPath(): Promise<string | undefined> {
    const isoPath = this.settingsManager.get().settings.isoPath ?? undefined;
    if (isoPath) {
      // Make sure the file actually exists
      if (!(await fileExists(isoPath))) {
        throw new Error(`Could not find ISO file: ${isoPath}`);
      }
    }
    return isoPath;
  }

  async importConfig(launchType: DolphinLaunchType, dolphinPath: string): Promise<void> {
    const installation = this.getInstallation(launchType);
    await installation.importConfig(dolphinPath);
    if (launchType === DolphinLaunchType.NETPLAY) {
      await this._updateLauncherSettings(launchType);
    }
  }

  private async _updateDolphinSettings(launchType: DolphinLaunchType) {
    const installation = this.getInstallation(launchType);
    await installation.updateSettings({
      replayPath: this.settingsManager.getRootSlpPath(),
      enableNetplayReplays: this.settingsManager.getEnableNetplayReplays(),
      enableMonthlySubfolders: this.settingsManager.getEnableMonthlySubfolders(),
      inputDelay: this.settingsManager.getInputDelay(),
      enableMusic: this.settingsManager.getEnableMusic(),
      disableChat: this.settingsManager.getDisableChat(),
      enableStreamGames: this.settingsManager.getEnableStreamGames(),
      pplusRelease: this.pplusVersions().currentRelease,
    });
    if (launchType === DolphinLaunchType.NETPLAY && !this.netplayDolphinInstance) {
      // A change the last Dolphin never took is in Dolphin.ini now.
      await clearInputDelayRequest(this.netplayOnlineDir());
    }
  }

  private async _updateLauncherSettings(launchType: DolphinLaunchType) {
    const installation = this.getInstallation(launchType);
    const newSettings = await installation.getSettings();

    await this._updateLauncherSetting(
      this.settingsManager.getRootSlpPath(),
      path.normalize(newSettings.replayPath),
      (val) => this.settingsManager.updateSetting("rootSlpPath", val),
    );
    await this._updateLauncherSetting(
      this.settingsManager.getEnableNetplayReplays(),
      newSettings.enableNetplayReplays,
      (val) => this.settingsManager.updateSetting("enableNetplayReplays", val),
    );
    await this._updateLauncherSetting(
      this.settingsManager.getEnableMonthlySubfolders(),
      newSettings.enableMonthlySubfolders,
      (val) => this.settingsManager.updateSetting("useMonthlySubfolders", val),
    );
    // Not Input Delay: only the launcher sets it, also while Dolphin runs, and a Dolphin that never
    // took the change (Configure, no game booted) saves its old value when it closes.
    await this._updateLauncherSetting(this.settingsManager.getEnableMusic(), newSettings.enableMusic, (val) =>
      this.settingsManager.updateSetting("enableMusic", val),
    );
    await this._updateLauncherSetting(this.settingsManager.getDisableChat(), newSettings.disableChat, (val) =>
      this.settingsManager.updateSetting("disableChat", val),
    );
    await this._updateLauncherSetting(
      this.settingsManager.getEnableStreamGames(),
      newSettings.enableStreamGames,
      (val) => this.settingsManager.updateSetting("enableStreamGames", val),
    );
  }

  private async _updateLauncherSetting<T>(currentVal: T, newVal: T, update: (val: T) => Promise<void>) {
    if (currentVal === newVal) {
      return;
    }
    await update(newVal);
  }

  private _onStart(dolphinType: DolphinLaunchType) {
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_START,
      dolphinType,
    });
  }

  /**
   * Sends progress, at most every PROGRESS_INTERVAL_MS per launch type; the start and end of a
   * step and a change of phase are always sent.
   */
  private _onProgress(dolphinType: DolphinLaunchType, current: number, total: number, phase?: DolphinSetupPhase) {
    if (!this.shouldSendProgress(dolphinType, current, total, phase)) {
      return;
    }
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_PROGRESS,
      dolphinType,
      progress: { current, total, phase },
    });
  }

  private _onComplete(dolphinType: DolphinLaunchType, dolphinVersion: string | undefined) {
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_COMPLETE,
      dolphinType,
      dolphinVersion,
    });
  }
}

export type GameFilesMoveResult = {
  action: "moved" | "adopted" | "cancelled";
  /** The netplay folder now in use. */
  folder: string;
  /** The old folder, when some of its files could not be deleted. */
  leftBehind: string | null;
};

/** Moving back to `<userData>/netplay`: files left there (the launcher's own) are replaced. */
async function defaultGameFilesTarget(defaultFolder: string, current: string): Promise<GameFilesTarget> {
  if (path.relative(defaultFolder, current) === "") {
    throw new GameFilesMoveError(`The game files are already in ${defaultFolder}.`);
  }
  if (await fileExists(path.join(defaultFolder, "User"))) {
    return { folder: defaultFolder, kind: "adopt" };
  }
  await removeGameFiles(defaultFolder);
  return { folder: defaultFolder, kind: "move" };
}

async function confirmAdoptGameFiles(folder: string, current: string): Promise<boolean> {
  // On the launcher's window even when it is not focused, never a free-floating box over a game.
  const window = BrowserWindow.getFocusedWindow() ?? BrowserWindow.getAllWindows()[0];
  const options: MessageBoxOptions = {
    type: "question",
    buttons: ["Use these files", "Cancel"],
    defaultId: 0,
    cancelId: 1,
    message: `${folder} already has game files.`,
    detail: `Use them? The game files in ${current} are deleted.`,
  };
  const { response } = window ? await dialog.showMessageBox(window, options) : await dialog.showMessageBox(options);
  return response === 0;
}
