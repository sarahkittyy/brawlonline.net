import type { ChatKeyPress } from "@common/chat_key";

import type { SyncedDolphinSettings } from "./config/config";
import type { DolphinVersionResponse } from "./install/fetch_latest_version";
import type { RoomHandOffResult } from "./room_handoff";

export type ReplayCommunication = {
  mode: "normal" | "mirror" | "queue"; // default normal
  replay?: string; // path to the replay if in normal or mirror mode
  startFrame?: number; // when to start watching the replay
  endFrame?: number; // when to stop watching the replay
  commandId?: string; // random string, doesn't really matter
  outputOverlayFiles?: boolean; // outputs gameStartAt and gameStation to text files (only works in queue mode)
  isRealTimeMode?: boolean; // default false; keeps dolphin fairly close to real time (about 2-3 frames); only relevant in mirror mode
  shouldResync?: boolean; // default true; disables the resync functionality
  rollbackDisplayMethod?: "off" | "normal" | "visible"; // default off; normal shows like a player experienced it, visible shows ALL frames (normal and rollback)
  gameStation?: string;
  queue?: ReplayQueueItem[];
  /** Ours: play a replay recorded with another version anyway (the player was asked). */
  playAnyway?: boolean;
  /**
   * Ours, `mirror` (spectating, docs/design/spectating.md §6.2): `queue` is the live stream's games, still growing
   * (games are only added at its end, under the same commandId); `streamOver` once no more come; at its end, with
   * `closeOnEnd`, Dolphin counts down and asks to be closed (status `close`), else it shows `banner`.
   */
  streamOver?: boolean;
  closeOnEnd?: boolean;
  banner?: string;
};

/** What the playback Dolphin writes to `<comm file>.status.json` (Core/Rollback/ReplayPlayback.h). */
export type ReplayPlaybackStatus = {
  commandId?: string;
  /** `between`: a spectated set's next game is coming; `close`: a spectated stream's end asks to be closed. */
  state?: "idle" | "loading" | "waiting" | "playing" | "between" | "ended" | "close" | "error";
  replay?: string;
  error?: string;
  needsPlayAnyway?: boolean;
};

export type ReplayQueueItem = {
  path: string;
  /** Ours, `mirror`: the rest of this live game won't come (its upload stopped): what's in the file is all of it. */
  cut?: boolean;
  startFrame?: number;
  endFrame?: number;
  gameStartAt?: string;
  gameStation?: string;
};

export const enum DolphinLaunchType {
  NETPLAY = "netplay",
  PLAYBACK = "playback",
}

export const enum DolphinUseType {
  PLAYBACK = "playback",
  SPECTATE = "spectate",
  CONFIG = "config",
  NETPLAY = "netplay",
}

export type PlayKey = {
  uid: string;
  playKey: string;
  connectCode: string;
  displayName: string;
  latestVersion?: string;
};

export const enum DolphinEventType {
  CLOSED = "CLOSED",
  DOWNLOAD_START = "DOWNLOAD_START",
  DOWNLOAD_PROGRESS = "DOWNLOAD_PROGRESS",
  DOWNLOAD_COMPLETE = "DOWNLOAD_COMPLETE",
  ERROR = "ERROR",
}

export const enum DolphinErrorType {
  NETWORK_ERROR = "NETWORK_ERROR",
  ROSETTA_REQUIRED = "ROSETTA_REQUIRED",
}

export type DolphinNetplayClosedEvent = {
  type: DolphinEventType.CLOSED;
  dolphinType: DolphinLaunchType.NETPLAY;
  exitCode: number | null;
};

export type DolphinPlaybackClosedEvent = {
  type: DolphinEventType.CLOSED;
  dolphinType: DolphinLaunchType.PLAYBACK;
  instanceId: string;
  exitCode: number | null;
};

export type DolphinDownloadStartEvent = {
  type: DolphinEventType.DOWNLOAD_START;
  dolphinType: DolphinLaunchType;
};

/**
 * The step a Dolphin download/progress event belongs to, so the Play button can name it. Slippi
 * had one step (its Dolphin download) and showed "Updating"; ours are:
 * - `installDolphin`: the bundled Dolphin copied into the netplay folder (bytes);
 * - `copyUserFolder`: development only, a new User folder seeded from the template (bytes);
 * - `downloadProjectPlus`, `verifyProjectPlus`, `extractProjectPlus`: P+'s release zip (bytes);
 * - `updateProjectPlus`: an installed SD card brought to a newer P+ release (bytes);
 * - `prepareSdCard`: the first Play's copy of the 2 GB SD card (bytes);
 * - `moveGameFiles`: Settings moving the netplay folder to another drive (bytes; nothing on one drive).
 */
export type DolphinSetupPhase =
  | "installDolphin"
  | "copyUserFolder"
  | "downloadProjectPlus"
  | "verifyProjectPlus"
  | "extractProjectPlus"
  | "updateProjectPlus"
  | "prepareSdCard"
  | "moveGameFiles";

export type DolphinDownloadProgressEvent = {
  type: DolphinEventType.DOWNLOAD_PROGRESS;
  dolphinType: DolphinLaunchType;
  progress: {
    current: number;
    total: number;
    /** Absent from Slippi-style events (Slippi's Dolphin download). */
    phase?: DolphinSetupPhase;
  };
};

export type DolphinDownloadCompleteEvent = {
  type: DolphinEventType.DOWNLOAD_COMPLETE;
  dolphinType: DolphinLaunchType;
  dolphinVersion: string | undefined;
};

export type DolphinNetworkErrorEvent = {
  type: DolphinEventType.ERROR;
  errorType: DolphinErrorType.NETWORK_ERROR;
  dolphinType: DolphinLaunchType;
};

export type DolphinRosettaRequiredEvent = {
  type: DolphinEventType.ERROR;
  errorType: DolphinErrorType.ROSETTA_REQUIRED;
  dolphinType?: DolphinLaunchType;
};

export type DolphinEventMap = {
  [DolphinEventType.CLOSED]: DolphinNetplayClosedEvent | DolphinPlaybackClosedEvent;
  [DolphinEventType.DOWNLOAD_START]: DolphinDownloadStartEvent;
  [DolphinEventType.DOWNLOAD_PROGRESS]: DolphinDownloadProgressEvent;
  [DolphinEventType.DOWNLOAD_COMPLETE]: DolphinDownloadCompleteEvent;
  [DolphinEventType.ERROR]: DolphinNetworkErrorEvent | DolphinRosettaRequiredEvent;
};

export type DolphinEvent = DolphinEventMap[DolphinEventType];

/** What Settings > Dolphin's game files move did; `error` carries the reason to show. */
export type GameFilesMoveOutcome =
  | { action: "moved" | "adopted"; folder: string; leftBehind: string | null }
  | { action: "cancelled"; folder: string }
  | { action: "error"; message: string };

export interface DolphinService {
  installRosetta(): Promise<{ exitCode: number }>;
  downloadDolphin(dolphinType: DolphinLaunchType): Promise<void>;
  /**
   * The Dolphin executable in use (configured or default), its User folder, the user.json path and
   * the netplay folder (where the game files are).
   */
  getDolphinPaths(
    dolphinType: DolphinLaunchType,
  ): Promise<{ executable: string; userFolder: string; playKeyFile: string; gameFilesFolder: string }>;
  /** Moves the game files (the netplay folder) to `folder`, or back to the default with null. */
  moveGameFiles(folder: string | null): Promise<GameFilesMoveOutcome>;
  configureDolphin(dolphinType: DolphinLaunchType): Promise<void>;
  softResetDolphin(dolphinType: DolphinLaunchType): Promise<void>;
  hardResetDolphin(dolphinType: DolphinLaunchType): Promise<void>;
  openDolphinSettingsFolder(dolphinType: DolphinLaunchType): Promise<void>;
  storePlayKeyFile(key: PlayKey): Promise<void>;
  checkPlayKeyExists(key: PlayKey): Promise<boolean>;
  removePlayKeyFile(): Promise<void>;
  viewSlpReplay(files: ReplayQueueItem[]): Promise<void>;
  /** `bootGame`: boot P+ even when the Play button is set to open Dolphin only. */
  launchNetplayDolphin(opts?: { bootGame?: boolean }): Promise<void>;
  /**
   * Hands a room code to the running game; `not-running` when no netplay Dolphin is open. `ruleset`:
   * a create request's ruleset code.
   */
  joinRoomInGame(code: string, ruleset?: string): Promise<RoomHandOffResult>;
  /** Closes the netplay Dolphin if it is open without a game; true when none is open now. */
  closeNetplayIfIdle(): Promise<boolean>;
  /** Leaves a room request (a create with its ruleset code, if any) for the game that Play is about to start. */
  prepareRoomLaunch(code: string, ruleset?: string): Promise<{ id: string }>;
  /** Takes a prepared room request back. */
  cancelRoomLaunch(id: string): Promise<void>;
  /** The key that opens the chat box, as an expression (common/chat_key.ts). */
  getChatKey(): Promise<string>;
  /** Sets the chat key to the pressed key; null (nothing changed) for a key that can't open chat. */
  setChatKey(press: ChatKeyPress): Promise<string | null>;
  onEvent<T extends DolphinEventType>(eventType: T, handle: (event: DolphinEventMap[T]) => void): () => void;
}

export interface DolphinInstallation {
  readonly installationFolder: string;
  get userFolder(): string;
  get sysFolder(): string;

  findDolphinExecutable(): Promise<string>;
  clearCache(): Promise<void>;
  importConfig(fromPath: string): Promise<void>;
  validate(options: {
    onStart: () => void;
    onProgress: (current: number, total: number) => void;
    onComplete: () => void;
    dolphinDownloadInfo: DolphinVersionResponse;
  }): Promise<void>;
  downloadAndInstall(options: {
    dolphinDownloadInfo: DolphinVersionResponse;
    onProgress?: (current: number, total: number) => void;
    onComplete?: () => void;
    cleanInstall?: boolean;
  }): Promise<void>;
  addGamePath(gameDir: string): Promise<void>;
  getSettings(): Promise<SyncedDolphinSettings>;
  updateSettings(options: Partial<SyncedDolphinSettings>): Promise<void>;
  getDolphinVersion(): Promise<string | undefined>;
  findPlayKey(): Promise<string>;
}
