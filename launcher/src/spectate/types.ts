/**
 * Spectating (docs/design/spectating.md §3.3, §6; docs/spectate-protocol.md §3): the live games
 * (`GET /v1/live`), and watching one in the playback Dolphin.
 */

export type LiveMode = "ranked" | "unranked" | "direct" | "room";

export type LivePlayer = {
  uid: string;
  displayName: string;
  connectCode: string;
  /** gmCharacterKind, as replays store it. */
  characterId: number;
  costume: number;
  teamId: number | null;
  /** Games won in the set so far. */
  wins: number;
};

export type LiveGameEntry = {
  /** The stream: a Direct, Unranked or Ranked set, or a room's games with the same players. */
  id: string;
  mode: LiveMode;
  room: string | null;
  isTeams: boolean;
  /** The set's start (UTC, ISO 8601): the list's order, newest first. */
  startedAt: string;
  spectators?: number;
  game: {
    /** The game number the players see. */
    number: number;
    /** Its place in the stream (1-based): watching from it starts with this game. */
    place?: number;
    startedAt: string;
    stageId: number;
    /** `between`: waiting for the set's next game. */
    state: "playing" | "between" | "over";
  };
  players: LivePlayer[];
};

export type LiveList = { games: LiveGameEntry[] };

export type SpectateStatus =
  /** Nothing watched. */
  | "idle"
  /** Opening the watch stream (or getting the game's first frames). */
  | "connecting"
  | "watching"
  /** The watch stream dropped; reconnecting from the newest chunk. */
  | "reconnecting"
  /** The stream is over (playback may still be going: everything is on disk). */
  | "over"
  | "error";

export type SpectateState = {
  status: SpectateStatus;
  /** The stream watched (or last watched). */
  stream: string | null;
  /** Its /v1/live entry, from the watch stream's welcome. */
  entry: LiveGameEntry | null;
  spectators: number;
  /** An error, or why the stream ended. */
  message?: string;
  /** Auto play (docs/design/spectating.md §6.2): at a stream's end, watch the newest live game. */
  autoPlay: boolean;
  /** Auto play found no other live game: waiting for one to start. */
  waitingForGame: boolean;
};

export type WatchResult = { ok: true } | { ok: false; message: string };

export type SpectateApi = {
  list(uid: string): Promise<LiveList>;
  /** Watches `stream` in the playback Dolphin (switching what it plays, if it is open). */
  watch(uid: string, stream: string): Promise<WatchResult>;
  stop(): Promise<void>;
  getState(): Promise<SpectateState>;
  setAutoPlay(on: boolean): Promise<void>;
  onStateChange(handle: (state: SpectateState) => void): () => void;
};
