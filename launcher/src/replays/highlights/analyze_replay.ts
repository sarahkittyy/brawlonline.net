import { DETECTOR_VERSION, findConversions, isHighlight } from "./detect_highlights";
import { findLrzClips } from "./lrz_clips";
import type { Timeline } from "./replay_frames";
import { readTimeline } from "./replay_frames";
import type { ReplayHighlights, StockChange } from "./types";

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
  /** The clip hotkey's buttons (@common/clip_hotkey; L+R+Z when not given). */
  clipButtons?: number;
};

// Zelda and Sheik, the one pair of forms a P+ player switches between in a game (Down-B; P+'s Pokémon
// and Samus don't switch): their gmCharacterKind (the replay's characterId) and the live fighter's
// ftKind (Timeline.kind; docs/brawl-memory-map.md, harness brawl.py FT_KIND).
const ZELDA = 0x0e;
const SHEIK = 0x0f;
const FT_ZELDA = 0x0d;
const FT_SHEIK = 0x0e;

/** A player picked Zelda or Sheik (so a highlight of theirs may show the other: formsDuring). */
export const hasForms = (characterId: number) => characterId === ZELDA || characterId === SHEIK;

/**
 * The players (all of them: a teams row shows teammates too) who were their other form for most
 * of the timeline's frames `from` to `to`, with that form's characterId (ReplayHighlight.forms).
 */
export function formsDuring(t: Timeline, from: number, to: number): { port: number; characterId: number }[] {
  const out: { port: number; characterId: number }[] = [];
  t.players.forEach(({ port, characterId }, i) => {
    if (!hasForms(characterId)) {
      return;
    }
    const kind = t.kind[i];
    let zelda = 0;
    let sheik = 0;
    for (let f = Math.max(from, 0); f <= Math.min(to, t.frames - 1); f++) {
      if (kind[f] === FT_ZELDA) {
        zelda += 1;
      } else if (kind[f] === FT_SHEIK) {
        sheik += 1;
      }
    }
    const shown = sheik > zelda ? SHEIK : zelda > sheik ? ZELDA : characterId;
    if (shown !== characterId) {
      out.push({ port, characterId: shown });
    }
  });
  return out;
}

/** Each player's stocks at the first frame, then on each frame they change (game order). */
function stockChanges(t: Timeline): StockChange[] {
  const out: StockChange[] = [];
  t.players.forEach(({ port }, i) => {
    const stocks = t.stocks[i];
    for (let f = 0; f < t.frames; f++) {
      if (f === 0 || stocks[f] !== stocks[f - 1]) {
        out.push({ frame: t.firstFrame + f, port, stocks: stocks[f] });
      }
    }
  });
  return out.sort((a, b) => a.frame - b.frame || a.port - b.port);
}

export function analyzeTimeline(t: Timeline, options: AnalyzeOptions): ReplayHighlights {
  const port = (index: number) => t.players[index].port;
  const last = t.frames - 1;
  const highlights = options.highlights
    ? findConversions(t)
        .filter(isHighlight)
        .sort((a, b) => b.score - a.score)
        .map((c) => {
          const forms = formsDuring(t, c.hits[0].frame, c.endFrame);
          return {
            kind: "highlight" as const,
            attackerPort: port(c.attacker),
            victimPort: port(c.victim),
            firstHitFrame: t.firstFrame + c.hits[0].frame,
            startFrame: t.firstFrame + Math.max(c.hits[0].frame - PRE_FRAMES, 1),
            endFrame: t.firstFrame + Math.min(c.endFrame + POST_FRAMES, last),
            kill: c.kill,
            ...(forms.length ? { forms } : {}),
            score: Math.round(c.score * 100) / 100,
          };
        })
    : [];
  const clipPlayers =
    options.clipPlayers === "all"
      ? t.players.map((p) => p.index)
      : t.players.filter((p) => p.port === t.recorderPort).map((p) => p.index);
  const clips = options.clips
    ? findLrzClips(t, clipPlayers, options.clipButtons).map((c) => ({
        kind: "clip" as const,
        port: port(c.player),
        frame: t.firstFrame + c.frame,
        startFrame: t.firstFrame + c.start,
        endFrame: t.firstFrame + c.end,
      }))
    : [];
  return {
    detectorVersion: DETECTOR_VERSION,
    stageId: t.stageId,
    stocks: stockChanges(t),
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
