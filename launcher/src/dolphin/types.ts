import type { ChatKeyPress } from "@common/chat_key";
import type { HighlightListView, HighlightPlayItem } from "@replays/highlights/types";

import type { SyncedDolphinSettings } from "./config/config";
import type { DolphinVersionResponse } from "./install/fetch_latest_version";
import type { RoomHandOffResult } from "./room_handoff";

export type ReplayCommunication = {
  /**
   * Ours, `highlights`: the queue's replays from each item's startFrame to its endFrame (ReplayPlayback.h);
   * `render`: the same, written to `output` (an MP4) by the render Dolphin (render_queue.ts).
   */
  mode: "normal" | "mirror" | "queue" | "highlights" | "render"; // default normal
  output?: string;
  /** Ours, `render`: the video's height (Settings > Clips). */
  height?: number;
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
  /** Ours, `highlights` from a list (highlight_playlist.ts): its stars on the seek bar and the overlay's list buttons. */
  marks?: HighlightListView["marks"];
  nav?: HighlightListView["nav"];
};

/** What the playback Dolphin writes to `<comm file>.status.json` (Core/Rollback/ReplayPlayback.h). */
export type ReplayPlaybackStatus = {
  commandId?: string;
  /** `between`: a spectated set's next game is coming; `close`: a spectated stream's end asks to be closed. */
  /** `seeking`: highlights, on the way to the clip's first frame (a fast-forward). */
  state?: "idle" | "loading" | "waiting" | "seeking" | "playing" | "between" | "ended" | "close" | "error";
  replay?: string;
  error?: string;
  needsPlayAnyway?: boolean;
  /** Which of the request's replays (or highlights) plays, of how many. */
  index?: number;
  count?: number;
  /** The session's frame (the last one run). */
  frame?: number;
  first_frame?: number;
  last_frame?: number;
  /** Highlights: the playing one's frames, and the frame its fast-forward started from (-1: not yet). */
  highlights?: boolean;
  clipStart?: number;
  clipEnd?: number;
  seekFrom?: number;
  /** Rendering (`render`): the file, the clips written to it so far, and its frames. */
  render?: boolean;
  output?: string;
  rendered?: number;
  framesWritten?: number;
  /**
   * Manual clips saved in this Dolphin's run (the seek bar's Clip, then "Save as highlight"): the
   * replay (a spectated game's file too) and its frames. The launcher keeps each once
   * (replays/highlights/manual_clips.ts).
   */
  savedClips?: { path: string; startFrame: number; endFrame: number }[];
  /**
   * A list's highlights (`nav` asked for), each counted from 1 for the commandId: a list button that
   * needs another replay ("<<", ">>", or "<" / ">" past this replay's stars) at the frame shown, and
   * the last star (or "<" / ">") Dolphin played itself.
   */
  navRequest?: { seq: number; action: "prevReplay" | "prevClip" | "nextClip" | "nextReplay"; frame: number };
  highlightPick?: { seq: number; startFrame: number; endFrame: number };
};

/**
 * Rendering highlights to video (render_queue.ts): `names[i]` is item i's file name (no folder or
 * extension); a `combo` is one file of all the items, named `names[0]`. `music`: off renders with
 * P+'s Music Off.
 */
export type RenderRequest = {
  items: HighlightPlayItem[];
  names: string[];
  music: boolean;
  combo: boolean;
  /** Each item is its whole match (the replays list's Render to video): its frames are ignored and read from the file. */
  wholeMatch?: boolean;
};

/** A render job (one file) and how far it is. */
export type RenderJobInfo = {
  jobId: string;
  /** The file's name (sanitized), and its path once it was chosen. */
  name: string;
  output: string | null;
  /** The replays it renders from. */
  paths: string[];
  /** A whole match (RenderRequest.wholeMatch), not highlights. */
  wholeMatch: boolean;
  /** Its clips, and the one being rendered (1-based; 0 before the first). */
  clips: number;
  clip: number;
  music: boolean;
  /** The video's height. */
  resolution: number;
  release: string | null;
  state: "queued" | "starting" | "rendering" | "done" | "error" | "cancelled";
  /** 0..1 */
  progress: number;
  error?: string;
};

/** What a render request did: queued its jobs, or (as a play request) needs a Project+ release first. */
export type RenderResult =
  | { status: "queued"; jobs: RenderJobInfo[] }
  | Exclude<ReplayPlayResult, { status: "playing" }>;

/**
 * What playing replays needs, from the Project+ release they were recorded on (docs/design/
 * pplus-versions.md): `play` them (on their release's card, or the launcher's own: release null), or
 * that release's files first (`needs-files`), or not at all with this launcher (`unplayable`).
 * A request mixing releases plays the first replay's release only (`skipped` others).
 */
/** A replay played from the Replays tab's list: its request's id and the list's buttons. */
export type ReplayListPlay = { commandId: string; nav: HighlightListView["nav"] };

export type ReplayPlayResult =
  | { status: "playing"; release: string | null; skipped: number; skippedRelease?: string }
  | { status: "needs-files"; release: string; current: string; downloadBytes: number }
  | { status: "unplayable"; release: string; current: string };

/** `auto`: on the replays' own release when it is ready; `current`: on the launcher's release anyway. */
export type ReplayPlayChoice = "auto" | "current";

/** An older Project+ release on this PC (Settings > Replays, the replay version dialog). */
/** A failed prepare says why (`offline`, `card-unknown`, ...: PPlusVersionError's codes). */
export type PPlusPrepareResult = { ok: true; state: PPlusVersionInfo } | { ok: false; code: string; message: string };

export type PPlusVersionInfo = {
  release: string;
  current: boolean;
  playable: boolean;
  filesReady: boolean;
  cardReady: boolean;
  downloadBytes: number;
  filesBytes: number;
  cardBytes: number;
};

export type PPlusPrepareProgressEvent = {
  type: DolphinEventType.PPLUS_PREPARE_PROGRESS;
  release: string;
  phase: "index" | "download" | "card";
  current: number;
  total: number;
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
  /** The playback Dolphin's status changed (`<comm>.status.json`; for highlights' progress). */
  PLAYBACK_STATUS = "PLAYBACK_STATUS",
  /** An older Project+ release is downloaded or its card made (preparePPlusRelease). */
  PPLUS_PREPARE_PROGRESS = "PPLUS_PREPARE_PROGRESS",
  /** A render job was queued, moved on, finished or failed (render_queue.ts). */
  RENDER_PROGRESS = "RENDER_PROGRESS",
}

/** A DLL Dolphin imports that Windows cannot load (missing_dlls.ts). */
export type MissingDll = {
  name: string;
  /** Who imports it (a file beside Dolphin.exe, or Dolphin.exe itself). */
  importedBy: string;
  /** "missing": not found; "not-x64": found but not a 64-bit DLL. */
  problem: "missing" | "not-x64";
};

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
  /** Closed by the launcher to start it again (on another Project+ release's card), not by the player. */
  replaced?: boolean;
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

export type DolphinPlaybackStatusEvent = {
  type: DolphinEventType.PLAYBACK_STATUS;
  instanceId: string;
  status: ReplayPlaybackStatus;
};

export type DolphinRenderProgressEvent = {
  type: DolphinEventType.RENDER_PROGRESS;
  job: RenderJobInfo;
};

export type DolphinEventMap = {
  [DolphinEventType.CLOSED]: DolphinNetplayClosedEvent | DolphinPlaybackClosedEvent;
  [DolphinEventType.DOWNLOAD_START]: DolphinDownloadStartEvent;
  [DolphinEventType.DOWNLOAD_PROGRESS]: DolphinDownloadProgressEvent;
  [DolphinEventType.DOWNLOAD_COMPLETE]: DolphinDownloadCompleteEvent;
  [DolphinEventType.ERROR]: DolphinNetworkErrorEvent | DolphinRosettaRequiredEvent;
  [DolphinEventType.PLAYBACK_STATUS]: DolphinPlaybackStatusEvent;
  [DolphinEventType.PPLUS_PREPARE_PROGRESS]: PPlusPrepareProgressEvent;
  [DolphinEventType.RENDER_PROGRESS]: DolphinRenderProgressEvent;
};

export type DolphinEvent = DolphinEventMap[DolphinEventType];

/** What Settings > Dolphin's game files move did; `error` carries the reason to show. */
/**
 * What Play did: started the netplay Dolphin, or not because the SD card is on another Project+
 * release (`cardRelease`) than the launcher's (`release`; install/pplus_builds.ts).
 */
export type NetplayLaunchResult =
  | { status: "started" }
  | { status: "wrongPPlusBuild"; cardRelease: string; release: string };

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
  /** `list`: one replay of the Replays tab's list, with its place in it (highlight_nav.ts). */
  viewSlpReplay(files: ReplayQueueItem[], choice?: ReplayPlayChoice, list?: ReplayListPlay): Promise<ReplayPlayResult>;
  /**
   * Plays highlights (or L+R+Z clips) one after another in the playback Dolphin, each from its
   * startFrame to its endFrame; one of the replay already playing goes on in its running match.
   * Progress: PLAYBACK_STATUS events with this `commandId`.
   */
  playHighlights(
    items: HighlightPlayItem[],
    commandId: string,
    choice?: ReplayPlayChoice,
    list?: HighlightListView,
  ): Promise<ReplayPlayResult>;
  /** The list changed while its highlight plays: the same request (`commandId`) with its new stars and count, if Dolphin is open. */
  updateHighlightList(item: HighlightPlayItem, commandId: string, list: HighlightListView): Promise<void>;
  /**
   * Renders highlights to video in the Clips folder (Settings > Clips), with no Dolphin window:
   * queued jobs, their progress as RENDER_PROGRESS events. `choice` as for playing (Project+ versions).
   */
  renderHighlights(request: RenderRequest, choice?: ReplayPlayChoice): Promise<RenderResult>;
  /** Stops a render job (a finished file is kept). */
  cancelRender(jobId: string): Promise<void>;
  /** This launcher run's render jobs. */
  getRenderJobs(): Promise<RenderJobInfo[]>;
  /** Windows: the DLLs that kept this Dolphin from starting (exit 0xC0000135 / 0xC000007B). */
  findMissingDlls(dolphinType: DolphinLaunchType): Promise<MissingDll[]>;
  /** The launcher's Project+ release and the older ones for replays. */
  getPPlusVersions(): Promise<{ current: string; states: PPlusVersionInfo[] }>;
  /**
   * Gets an older release ready for replays: downloaded, or taken from `importFrom` (a Project+
   * folder or sd.raw of that release on this PC). Progress: PPLUS_PREPARE_PROGRESS events.
   */
  preparePPlusRelease(release: string, importFrom?: string): Promise<PPlusPrepareResult>;
  removePPlusRelease(release: string): Promise<void>;
  /**
   * `bootGame`: boot P+ even when the Play button is set to open Dolphin only. Not started when the
   * SD card is on another Project+ release (`wrongPPlusBuild`): replaceProjectPlusCard first.
   */
  launchNetplayDolphin(opts?: { bootGame?: boolean }): Promise<NetplayLaunchResult>;
  /**
   * Replaces an SD card of another Project+ release with the launcher's release (downloaded; the
   * old card is kept next to it). Progress on the Play button, as P+'s first download.
   */
  replaceProjectPlusCard(): Promise<void>;
  /**
   * Hands a room code to the running game; `not-running` when no netplay Dolphin is open. `ruleset`:
   * a create request's ruleset code; `isPrivate`: the create makes a private room.
   */
  joinRoomInGame(code: string, ruleset?: string, isPrivate?: boolean): Promise<RoomHandOffResult>;
  /** Closes the netplay Dolphin if it is open without a game; true when none is open now. */
  closeNetplayIfIdle(): Promise<boolean>;
  /** Leaves a room request (a create with its ruleset code, if any) for the game that Play is about to start. */
  prepareRoomLaunch(code: string, ruleset?: string, isPrivate?: boolean): Promise<{ id: string }>;
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
