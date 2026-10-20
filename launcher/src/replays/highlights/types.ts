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
  /** Sorted by score, best first. */
  highlights: ReplayHighlight[];
  /** In game order. */
  clips: ReplayClip[];
  /** Why nothing could be found (an older replay without fighter memory, a damaged file). */
  error?: string;
};

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
