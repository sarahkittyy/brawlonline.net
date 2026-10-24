import { DETECTOR_VERSION, findConversions, isHighlight } from "./detect_highlights";
import { findLrzClips } from "./lrz_clips";
import type { Timeline } from "./replay_frames";
import { readTimeline } from "./replay_frames";
import type { ReplayHighlights } from "./types";

/** A highlight plays from 1.5 s before its first hit to 1 s after the string (a KO: after the stock is lost). */
export const PRE_FRAMES = 90;
export const POST_FRAMES = 60;

export type AnalyzeOptions = {
  /** Find highlights (Settings > Clips: "Disable automatic highlight detection" turns it off). */
  highlights: boolean;
  /** Find L+R+Z clips. */
  clips: boolean;
  /** Whose L+R+Z presses count: the recording player's only (the default), or every player's. */
  clipPlayers: "local" | "all";
};

export function analyzeTimeline(t: Timeline, options: AnalyzeOptions): ReplayHighlights {
  const port = (index: number) => t.players[index].port;
  const last = t.frames - 1;
  const highlights = options.highlights
    ? findConversions(t)
        .filter(isHighlight)
        .sort((a, b) => b.score - a.score)
        .map((c) => ({
          kind: "highlight" as const,
          attackerPort: port(c.attacker),
          victimPort: port(c.victim),
          firstHitFrame: t.firstFrame + c.hits[0].frame,
          startFrame: t.firstFrame + Math.max(c.hits[0].frame - PRE_FRAMES, 1),
          endFrame: t.firstFrame + Math.min(c.endFrame + POST_FRAMES, last),
          kill: c.kill,
          score: Math.round(c.score * 100) / 100,
        }))
    : [];
  const clipPlayers =
    options.clipPlayers === "all"
      ? t.players.map((p) => p.index)
      : t.players.filter((p) => p.port === t.recorderPort).map((p) => p.index);
  const clips = options.clips
    ? findLrzClips(t, clipPlayers).map((c) => ({
        kind: "clip" as const,
        port: port(c.player),
        frame: t.firstFrame + c.frame,
        startFrame: t.firstFrame + c.start,
        endFrame: t.firstFrame + c.end,
      }))
    : [];
  return {
    detectorVersion: DETECTOR_VERSION,
    players: t.players.map(({ port, displayName, connectCode, characterId, costume, teamId }) => ({
      port,
      displayName,
      connectCode,
      characterId,
      costume,
      teamId,
    })),
    isTeams: t.isTeams,
    recorderPort: t.recorderPort,
    highlights,
    clips,
  };
}

export async function analyzeReplay(fullPath: string, options: AnalyzeOptions): Promise<ReplayHighlights> {
  return analyzeTimeline(await readTimeline(fullPath), options);
}
