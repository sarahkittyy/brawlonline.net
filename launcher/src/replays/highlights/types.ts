/**
 * A replay's highlights and L+R+Z clips, as the stats page lists them (highlight_service.ts finds
 * and caches them). Frames are the replay's (session frames, as playback seeks): `startFrame` to
 * `endFrame` is what plays.
 */
export type HighlightPlayer = {
  port: number; // 1-4
  displayName: string;
  connectCode: string;
  characterId: number;
  costume: number;
  teamId: number | null;
};

export type ReplayHighlight = {
  kind: "highlight";
  /** The attacker (the player with most of the string's damage) and the victim, as ports. */
  attackerPort: number;
  victimPort: number;
  firstHitFrame: number;
  startFrame: number;
  endFrame: number;
  kill: boolean;
  /** Private (DESIGN.md): only used to sort. */
  score: number;
};

export type ReplayClip = {
  kind: "clip";
  /** Who held L+R+Z. */
  port: number;
  /** When the hold was complete. */
  frame: number;
  startFrame: number;
  endFrame: number;
};

export type ReplayHighlights = {
  detectorVersion: number;
  players: HighlightPlayer[];
  isTeams: boolean;
  /**
   * This machine's player (the replay's recorder), whose highlights are "mine" in Replays >
   * Highlights; null when the replay doesn't say. (Results cached before it was kept get it from
   * the replay's header as they are read: highlight_service.ts.)
   */
  recorderPort?: number | null;
  /** Sorted by score, best first. */
  highlights: ReplayHighlight[];
  /** In game order. */
  clips: ReplayClip[];
  /** Why nothing could be found (an older replay without fighter memory, a damaged file). */
  error?: string;
};

/** An L+R+Z clip is of the last this many seconds before the hold (lrz_clips.ts). */
export const CLIP_BEFORE_SECONDS = 20;

/** What the stats page asks for. */
export type HighlightsRequest = {
  filePath: string;
};

/** A row the playback Dolphin plays: the replay from `startFrame` to `endFrame`. */
export type HighlightPlayItem = {
  path: string;
  startFrame: number;
  endFrame: number;
};

/** A replay's highlights in the Replays > Highlights tab: which replay, when it was played (ISO). */
export type GameHighlights = {
  path: string;
  date: string;
  highlights: ReplayHighlights;
};

/** The Highlights tab's fill: a replay done (`game` when it has any), and how many of all. */
export type AllHighlightsEvent = {
  game?: GameHighlights;
  done: number;
  total: number;
};
