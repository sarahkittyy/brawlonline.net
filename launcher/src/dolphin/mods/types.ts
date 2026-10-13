/**
 * Brawl Mods (docs/design/brawl-mods.md): rooms for whole-machine rollback sessions of any Brawl
 * build (vanilla, Brawl Minus, REX...). The launcher keeps the player's builds, hashes them, starts
 * our Dolphin with a build and the room to open or join (`--mods-room`), and shows the room from
 * the file Dolphin writes (`mods-room.json`).
 */

/** A build the player added in the Brawl Mods tab (design §4). */
export type ModBuild = {
  id: string;
  /** The player's name for it, or the known build's. */
  name: string;
  /** The game's disc image (Brawl). */
  discPath: string;
  /** A `.dol` booted instead of the disc (Minus Launcher, P+ launchers), or null. */
  launcherPath: string | null;
  /** The SD card image the build reads, or null. */
  sdPath: string | null;
  /** SHA-256 (64 hex digits) of the parts, null until hashed. */
  hash: string | null;
  /** The parts' sizes and modification times when hashed: a change hashes them again. */
  hashKey: string | null;
};

export type ModBuildInput = Pick<ModBuild, "name" | "discPath" | "launcherPath" | "sdPath">;

/** What a player plays with (design §6.2): their GameCube port or their Wii Remote. */
export type ModsController = "gc" | "wiimote";

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
  lastRequest: { id: string; result: "ok" | "failed"; error?: string } | null;
  pid: number;
  updatedAt: string;
};

/** A request to the running Dolphin's room (`mods-request.json`). */
export type ModRoomRequest =
  | { op: "leave" }
  | { op: "kick"; slot: number }
  | { op: "slot"; slot: number; open: boolean }
  | { op: "public"; public: boolean };

/** Hashing a build: bytes done of the total. */
export type ModsHashProgress = { id: string; done: number; total: number };

export type ModsState = {
  builds: ModBuild[];
  /** Our Dolphin runs a Brawl Mods room. */
  running: boolean;
  /** The room Dolphin last reported, while it runs. */
  status: ModRoomStatus | null;
};

export type ModsEvent = { type: "state"; state: ModsState } | { type: "hashProgress"; progress: ModsHashProgress };

export type ModsHostOptions = { buildId: string; public: boolean; controller: ModsController };
export type ModsJoinOptions = { buildId: string; code: string; controller: ModsController };

export interface ModsService {
  getState(): Promise<ModsState>;
  addBuild(input: ModBuildInput): Promise<ModBuild>;
  updateBuild(id: string, input: Partial<ModBuildInput>): Promise<ModBuild>;
  removeBuild(id: string): Promise<void>;
  /** Hashes the build's parts (when they changed since): progress comes as events. */
  hashBuild(id: string): Promise<ModBuild>;
  host(options: ModsHostOptions): Promise<void>;
  join(options: ModsJoinOptions): Promise<void>;
  request(request: ModRoomRequest): Promise<void>;
  onEvent(handle: (event: ModsEvent) => void): () => void;
}
