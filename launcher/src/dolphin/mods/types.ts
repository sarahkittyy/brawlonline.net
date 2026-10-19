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

/** Each part's hash (`build_hash.ts`): the disc's contents' SHA-1, the launcher's SHA-256, and
 * whether there is an SD card (its contents are the room host's: not part of the build hash).
 * `sdCard` is the card's root (DolphinTool sdhash: BLAKE2b-256 of its manifest), which rooms
 * compare (docs/design/brawl-mods.md §6.5); null for no card, absent until it was hashed. */
export type ModBuildParts = { disc: string; launcher: string | null; sd: boolean; sdCard?: string | null };

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
  /** The disc's revision (Brawl: 0, 1 or 2), from its header; absent from older settings. */
  revision?: number | null;
  gameTitle?: string | null;
  platform?: DiscPlatform | null;
  /** A Wii disc's title ID (16 hex digits), which names its save; absent from older settings. */
  titleId?: string | null;
  /** SHA-256 (64 hex digits) of the parts, null until hashed. */
  hash: string | null;
  /** The parts' hashes, with `hash`. */
  parts?: ModBuildParts | null;
  /** The parts' sizes and modification times when hashed: a change hashes them again. */
  hashKey: string | null;
  /** The SD card's size and modification time when its root (`parts.sdCard`) was taken. */
  sdHashKey?: string | null;
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

/** A room's build as the server says it (docs/design/brawl-mods.md §6.5): with its parts, from a
 * launcher that sends them: the disc's SHA-1 and what it is ("RSBE01 Rev 1"), and the SD card's
 * root ("" for none). */
export type ModRoomBuild = { hash: string; name: string; disc?: string; discLabel?: string; sd?: string };

export type ModRoom = {
  code: string;
  public: boolean;
  build: ModRoomBuild;
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

/** A joiner's SD card before its join (Dolphin's SDSyncStep): checking the player's own card,
 * asking the player (the dialog), downloading the host's. */
export type ModSdSync = {
  step: "checking" | "ask" | "downloading" | "ready" | "failed";
  /** The share of the player's own card checked (0-1). */
  checked: number;
  /** What differs from the host's card: bytes and files, and some of those files, largest first. */
  bytes: number;
  files: number;
  names: string[];
  /** Seconds the download should take (a guess from the link). */
  eta_s: number;
  /** While downloading: bytes done of the total, and the rate over the last few seconds (bytes a
   * second; the players pace what they send so their game keeps its link). */
  done?: number;
  total?: number;
  rate?: number;
};

export type ModSession = {
  phase?: string;
  /** A joiner whose SD card differs from the host's. */
  sd_sync?: ModSdSync;
  error?: string;
  players?: number;
  epoch?: number;
  frame?: number;
  slot?: number;
  peers?: ModSessionPeer[];
  /** How the session runs, for logs and support (absent from an older Dolphin): passes that
   * waited for the others, frames ahead of them, the speed that sets, rollbacks. */
  stall_polls?: number;
  frames_ahead?: number;
  speed?: number;
  rollbacks?: number;
  frames_resimulated?: number;
  desyncs?: number;
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
  /** The host: two slots (1-4) swap players, mid-game too (the session swaps them at an epoch). */
  | { op: "move"; from: number; to: number }
  /** This player's own controller, mid-game too. */
  | { op: "controller"; controller: ModsController }
  /** Opens Dolphin's Controllers settings over the game. */
  | { op: "configure" }
  /** A joiner's answer to the SD card's dialog (session.sd_sync.step "ask"). */
  | { op: "sd-sync"; choice: "yes" | "overwrite" | "no" };

/** Hashing a build: the share done (0-1), or null while the step running reports none
 * (DolphinTool hashing a compressed disc image). */
export type ModsHashProgress = { id: string; fraction: number | null };

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
  /** The builds being hashed, by id, with their progress (`ModsHashProgress.fraction`): kept here,
   * so a tab opened again mid-hash shows it. */
  hashing: Record<string, number | null>;
};

export type ModsEvent =
  | { type: "state"; state: ModsState }
  | { type: "hashProgress"; progress: ModsHashProgress }
  /** A game added twice (the same hash as one already in the list): it was taken out again. */
  | { type: "duplicate"; name: string }
  /** The check started when a game was added failed (Host checks it again). */
  | { type: "hashFailed"; name: string; error: string };

/** A Wii game's save in its build's store (`save_import.ts`), which the game plays in rooms. */
export type ModSaveInfo = {
  /** The save's folder. */
  folder: string;
  files: number;
  bytes: number;
  /** The newest file's change (ISO), or null when the game has no save yet. */
  modified: string | null;
};

/** The save imported, and where the one it replaced went (null: there was none). */
export type ModSaveImportResult = { save: ModSaveInfo; backup: string | null };

export type ModsHostOptions = { buildId: string; public: boolean };
export type ModsJoinOptions = { buildId: string; code: string };
/** A room code typed in: the room's game is looked up (`uid`: the account that asks). */
export type ModsJoinByCodeOptions = { uid: string; code: string };
export type ModsJoinByCodeResult =
  | { outcome: "joining" }
  /** No game in the list has the room's hash. */
  | { outcome: "no-game"; name: string; hash: string };

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
  /** Joins the room with this code with the game whose hash it needs, if the player has it. */
  joinByCode(options: ModsJoinByCodeOptions): Promise<ModsJoinByCodeResult>;
  request(request: ModRoomRequest): Promise<void>;
  /** Plays with this kind of controller from now on (the running room's at once). */
  setController(controller: ModsController): Promise<void>;
  /** Reads Dolphin's controller settings again (they may have changed in Dolphin). */
  refreshControllers(): Promise<void>;
  /** Opens Dolphin's Controllers settings: over the game while one runs, else in a Dolphin of its own. */
  configureControllers(): Promise<void>;
  /** The game's save in rooms (a Wii game, once hashed), or null when it has none to show. */
  saveInfo(id: string): Promise<ModSaveInfo | null>;
  /** Puts a save (a data.bin or a folder with one) in the game's store, the old one kept aside. */
  importSave(id: string, source: string): Promise<ModSaveImportResult>;
  /** Opens the game's save folder (made if it is not there yet). */
  openSaveFolder(id: string): Promise<void>;
  onEvent(handle: (event: ModsEvent) => void): () => void;
}
