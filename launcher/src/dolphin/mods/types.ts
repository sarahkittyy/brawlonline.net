/**
 * Other Games (docs/design/brawl-mods.md): rooms for whole-machine rollback sessions of any Wii or
 * GameCube game and any Brawl mod (vanilla Brawl, Brawl Minus, REX...). The launcher keeps the
 * player's games ("builds": a disc, maybe a mod's launcher and SD card), hashes them, starts our
 * Dolphin with one and the room to open or join (`--mods-room`), and shows the room from the
 * file Dolphin writes (`mods-room.json`).
 */

/** What a player plays with (design §6.2): a GameCube controller or a Wii Remote. Dolphin plays
 * the first one of the kind plugged in, of the ones set up in its Controllers settings. */
export type ModsController = "gc" | "wiimote";

/** A GameCube port or Wii Remote set up in Dolphin's Controllers settings (`controllers.ts`). */
export type SetUpController = {
  /** The port or Wii Remote number (1-4). */
  index: number;
  /** Dolphin's device ("XInput/0/Gamepad"); "" for a real Wii Remote. */
  device: string;
  /** What a player calls it ("Xbox controller", "Real Wii Remote", "Keyboard + Nunchuk"). */
  label: string;
  real?: boolean;
};

/** The controllers set up in Dolphin, folded by kind. */
export type DolphinControllers = { gc: SetUpController[]; wiimote: SetUpController[] };

export type DiscPlatform = "wii" | "gamecube";

/** Each part's hash (`build_hash.ts`): the disc's contents' SHA-1, the files' SHA-256s. */
export type ModBuildParts = { disc: string; launcher: string | null; sd: string | null };

/** A game or mod the player added in the tab (design §4). */
export type ModBuild = {
  id: string;
  /** The player's name for it, or the known game's or mod's. */
  name: string;
  /** The player gave no name: `name` follows what the build turns out to be (a known mod). */
  autoName?: boolean;
  /** The game's disc image (any format Dolphin reads). */
  discPath: string;
  /** A `.dol` or `.elf` booted instead of the disc (Minus Launcher, REX, P+ launchers), or null. */
  launcherPath: string | null;
  /** The SD card image the build reads, or null. */
  sdPath: string | null;
  /** From the disc's header (DolphinTool), null until read. */
  gameId?: string | null;
  gameTitle?: string | null;
  platform?: DiscPlatform | null;
  /** SHA-256 (64 hex digits) of the parts, null until hashed. */
  hash: string | null;
  /** The parts' hashes, with `hash`. */
  parts?: ModBuildParts | null;
  /** The parts' sizes and modification times when hashed: a change hashes them again. */
  hashKey: string | null;
};

export type ModBuildInput = Pick<ModBuild, "name" | "discPath" | "launcherPath" | "sdPath">;

/** What a game is made for: a Wii game a Wii Remote (Brawl and its mods take either: a GameCube
 * controller first), a GameCube game a GameCube controller. */
export function defaultController(build: Pick<ModBuild, "gameId" | "platform">): ModsController {
  const brawl = !!build.gameId && build.gameId.startsWith("RSB");
  if (build.platform === "wii" && !brawl) {
    return "wiimote";
  }
  return "gc";
}

/** What "Add a mod folder" found (`folder_import.ts`). */
export type ModFolderParts = { name: string; launcherPath: string | null; sdPath: string | null };

/** A slot's player as Dolphin's `mods-room.json` shows it. */
export type ModRoomPlayer = {
  displayName: string;
  connectCode: string;
  controller: ModsController | string;
  /** Its session reported where its peers reach it. */
  connected: boolean;
};

export type ModRoomSlot = {
  slot: number;
  open: boolean;
  player?: ModRoomPlayer;
};

export type ModRoom = {
  code: string;
  public: boolean;
  build: { hash: string; name: string };
  /** The host's slot (1-4). */
  host: number;
  /** This player's slot (1-4). */
  you: number;
  slots: ModRoomSlot[];
};

export type ModSessionPeer = { slot: number; connected: boolean; rttMs: number };

/** What this player plays with right now, as Dolphin picked it. */
export type ModLocalInput = {
  controller: ModsController;
  /** The GameCube port or Wii Remote (1-4) it is set up as. */
  index: number;
  /** Dolphin's device; "" when none of the kind is set up. */
  device: string;
  plugged: boolean;
  real: boolean;
};

export type ModSession = {
  phase?: string;
  error?: string;
  players?: number;
  epoch?: number;
  frame?: number;
  slot?: number;
  peers?: ModSessionPeer[];
};

/** `<User>/Online/mods-room.json`, written by Dolphin (design §3). */
export type ModRoomStatus = {
  version: number;
  kind: "host" | "join" | "none";
  state: "joining" | "in" | "left";
  error: string;
  room: ModRoom | null;
  session: ModSession;
  /** Absent from an older Dolphin. */
  input?: ModLocalInput;
  lastRequest: { id: string; result: "ok" | "failed"; error?: string } | null;
  pid: number;
  updatedAt: string;
};

/** A request to the running Dolphin's room (`mods-request.json`). */
export type ModRoomRequest =
  | { op: "leave" }
  | { op: "kick"; slot: number }
  | { op: "slot"; slot: number; open: boolean }
  | { op: "public"; public: boolean }
  /** This player's own controller, mid-game too. */
  | { op: "controller"; controller: ModsController }
  /** Opens Dolphin's Controllers settings over the game. */
  | { op: "configure" };

/** Hashing a build: bytes done of the total. */
export type ModsHashProgress = { id: string; done: number; total: number };

export type ModsState = {
  builds: ModBuild[];
  /** Our Dolphin runs a Brawl Mods room. */
  running: boolean;
  /** The room Dolphin last reported, while it runs. */
  status: ModRoomStatus | null;
  /** Why the last room ended, once Dolphin closed (a join that failed: "Room not found."), until
   * the next host or join. */
  lastError: string | null;
  /** What the player plays with (the launcher's controller menu). */
  controller: ModsController;
  /** The controllers set up in Dolphin, as last read. */
  controllers: DolphinControllers;
};

export type ModsEvent =
  | { type: "state"; state: ModsState }
  | { type: "hashProgress"; progress: ModsHashProgress }
  /** A game added twice (the same hash as one already in the list): it was taken out again. */
  | { type: "duplicate"; name: string };

export type ModsHostOptions = { buildId: string; public: boolean };
export type ModsJoinOptions = { buildId: string; code: string };

export interface ModsService {
  getState(): Promise<ModsState>;
  addBuild(input: ModBuildInput): Promise<ModBuild>;
  updateBuild(id: string, input: Partial<ModBuildInput>): Promise<ModBuild>;
  removeBuild(id: string): Promise<void>;
  /** A mod's Dolphin folder: its launcher and SD card, for the add dialog. */
  inspectFolder(folder: string): Promise<ModFolderParts>;
  /** Hashes the build's parts (when they changed since): progress comes as events. */
  hashBuild(id: string): Promise<ModBuild>;
  host(options: ModsHostOptions): Promise<void>;
  join(options: ModsJoinOptions): Promise<void>;
  request(request: ModRoomRequest): Promise<void>;
  /** Plays with this kind of controller from now on (the running room's at once). */
  setController(controller: ModsController): Promise<void>;
  /** Reads Dolphin's controller settings again (they may have changed in Dolphin). */
  refreshControllers(): Promise<void>;
  /** Opens Dolphin's Controllers settings: over the game while one runs, else in a Dolphin of its own. */
  configureControllers(): Promise<void>;
  onEvent(handle: (event: ModsEvent) => void): () => void;
}
