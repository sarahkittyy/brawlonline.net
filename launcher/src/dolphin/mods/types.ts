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
  /** Brawl Online's own Project+ (ModsManager.refreshBuiltIn): the Brawl disc set in Settings and
   * the launcher's P+ files. Always in the list, its files not the player's to change, and its SD
   * card never overwritten by a room's. */
  builtIn?: boolean;
  /** The codes the player's rooms of this game run (docs/design/room-codes.md); absent until the
   * launcher first looked (it starts with the ones on in the player's own Dolphin). */
  codes?: ModBuildCodes;
};

/** A Gecko or Action Replay code (docs/design/room-codes.md). `lines`: "XXXXXXXX YYYYYYYY", or an
 * encrypted Action Replay code's "XXXX-XXXX-XXXXX". */
export type ModCode = { name: string; kind: "gecko" | "ar"; lines: string[]; author?: string; notes?: string };
/** Where a code in the Codes dialog comes from: added there, the player's own Dolphin, Dolphin's
 * list for the game, or the code database (codes.rc24.xyz, Dolphin's Download Codes). */
export type ModCodeSource = "yours" | "own" | "dolphin" | "database";
/** A code a room runs, with its own copy of the lines: the list it came from changing later does
 * not change the room's. */
export type ModOnCode = ModCode & { source: ModCodeSource };
/** A game's codes: the ones on, and the ones the player added. */
export type ModBuildCodes = { on: ModOnCode[]; yours: ModCode[] };
/** The Codes dialog's lists of a game (the database's are fetched when that section opens).
 * `ownFiles`: the ini files Your Dolphin's came from. */
export type ModCodeLists = { own: ModCode[]; ownFiles: string[]; dolphin: ModCode[] };

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
export type ModRoomBuild = {
  hash: string;
  name: string;
  disc?: string;
  discLabel?: string;
  sd?: string;
  /** The names of the codes the host's game runs (room-codes.md); absent: none, or an older host. */
  codes?: string[];
};

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
  /** The game the running room plays (absent from an older launcher's state). */
  runningBuildId?: string | null;
  /** The build hash of each of the player's discs played alone (`matchRoom`'s `discAlone`), by
   * the disc's part. */
  discAlone?: Record<string, string>;
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
  /** A save of the game in the player's own Dolphin that is not the one here (a Wii game's;
   * `findOwnDolphinSave`), to import with one click. */
  ownDolphin?: { folder: string; files: number; modified: string | null } | null;
};

/** The save imported, and where the one it replaced went (null: there was none). */
export type ModSaveImportResult = { save: ModSaveInfo; backup: string | null };

/** `uid`: the account that hosts, whose launcher puts the game's SD card listing on the server
 * (docs/design/brawl-mods.md §6.5), so joiners see what differs before their Dolphin starts. */
export type ModsHostOptions = { buildId: string; public: boolean; uid?: string };

/** What a joiner would download of a room's SD card, asked in the launcher before Dolphin starts
 * (docs/design/brawl-mods.md §6.5; the user's, 2026-10-19). */
export type ModSdCardCheck =
  /** The player's card is the room's (or one side has none): nothing to ask. */
  | { kind: "same" }
  /** From the host's listing: the bytes and files that differ, the largest named first
   * (`filesKnown` false when the host's card is not a FAT volume the tool reads). */
  | { kind: "differs"; bytes: number; files: number; names: string[]; filesKnown: boolean }
  /** The host's listing is not on the server (a host on an older launcher): asked without sizes. */
  | { kind: "unknown" };
/** The player's answer to the SD card question: download it, or download it and write it into
 * their own card. */
export type ModsSdChoice = "yes" | "overwrite";

/** `sdSync`: the player's answer to the SD card question, once asked; without it a join whose card
 * differs answers `sd-card` instead of starting Dolphin. `uid`: the account that joins. */
/** `discOnly`: the room plays the disc alone (room_match.ts), joined with `buildId`'s disc as its
 * own game. */
export type ModsJoinOptions = {
  buildId: string;
  code: string;
  uid?: string;
  sdSync?: ModsSdChoice;
  discOnly?: boolean;
};
export type ModsJoinResult =
  | { outcome: "joining" }
  /** The room's SD card differs: ask the player, then join again with `sdSync`. `roomName`: the
   * room's game, as its host named it. */
  | { outcome: "sd-card"; buildId: string; code: string; check: ModSdCardCheck; roomName?: string };
/** A room code typed in: the room's game is looked up (`uid`: the account that asks). */
export type ModsJoinByCodeOptions = { uid: string; code: string };
export type ModsJoinByCodeResult =
  | ModsJoinResult
  /** No game in the list plays the room. `closest`: the player's game of it whose `part` keeps it
   * out (the disc differs, or an SD card is on one side only), to change; none when they have no
   * such game. */
  | {
      outcome: "no-game";
      name: string;
      hash: string;
      discLabel?: string;
      closest?: { buildId: string; part: "disc" | "sd"; roomHasCard: boolean };
    };

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
  join(options: ModsJoinOptions): Promise<ModsJoinResult>;
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
  /** The game's codes and the Codes dialog's lists (room-codes.md). */
  codeLists(id: string): Promise<{ codes: ModBuildCodes; lists: ModCodeLists }>;
  /** Keeps the game's codes: its next room runs the ones on. */
  setCodes(id: string, codes: ModBuildCodes): Promise<ModBuildCodes>;
  /** The code database's codes for the game (fetched once, then from the cache). */
  databaseCodes(id: string): Promise<ModCode[]>;
  onEvent(handle: (event: ModsEvent) => void): () => void;
}
