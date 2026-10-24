/**
 * Highlights in a replay's timeline (replay_frames.ts), as the prototype's highlights.py finds them
 * (D:\code\brawl-highlights; DESIGN.md §1).
 *
 * A *conversion* (Slippi's term) is one player's string of hits on another that the victim never
 * escapes: it goes on while the victim is hit, grabbed, tumbling, downed or teching, and ends once
 * the victim has been free for RESET_FRAMES, or dies. Each one gets features (moves, damage, start
 * percent, a KO, speed, the attacker's inputs a second, whiffs) and tags:
 *
 *   combo          4+ moves, 30%+ damage
 *   zero-to-death  a KO in one conversion starting at 0% (low-to-death: at 30% or less)
 *   fast           4+ moves landed within one second, or 3+ moves with the attacker at
 *                  FAST_INPUTS_PER_S+ inputs a second
 *   clean          3+ moves and no whiffed attack from the first hit to the last
 *
 * A move is one attack instance: a new action state, a new animation or the animation restarting
 * (two fairs in a row are two moves); a special's chain of states and an aerial's landing are one.
 * A highlight is a conversion with a tag other than `clean`.
 *
 * The score (never shown: the list shows only who and on whom, sorted by it) rewards moves, damage,
 * a KO (more from lower percents), speed and a clean string, scaled by the share of attacks that
 * hit. Since detector version 2 (the user, 2026-10-20) it also rewards using different moves: a
 * move of a kind the string already used (the same motion) counts half, and every kind past the
 * second adds a bonus. Chain grabs and other repeated strings are kept, ranked below varied ones.
 */
import type { Timeline } from "./replay_frames";
import { PAD_KEPT } from "./replay_frames";

/** Bumped whenever what is found or its score changes: cached results of another version are found again. */
export const DETECTOR_VERSION = 4;

const RESET_FRAMES = 45; // Slippi's COMBO_STRING_RESET_FRAMES
const DEAD = 189; // Status_Dead (then P+'s post-KO 267, seen before every stock loss)
const FPS = 60;
const FAST_INPUTS_PER_S = 12;
const SPECIALS = 0x112; // fighter-specific statuses (specials) start here
const LANDING_AIR_ATTACK = 0x18;
const AIR_ATTACK = 0x33;
/** The weight of a move whose kind the string already used (detector version 2). */
const REPEAT_WEIGHT = 0.5;
/** The bonus of each kind of move past the second (detector version 2). */
const VARIETY_BONUS = 0.75;

const ATTACKS = (() => {
  const s = new Uint8Array(1024);
  s.fill(1, 0x24, 0x39); // jab .. aerials, grabs
  s[0x3a] = s[0x3c] = 1; // pummel, throw
  s[0x53] = s[0x76] = s[0x8f] = 1; // getup, ledge and trip attacks
  s.fill(1, SPECIALS);
  return s;
})();
const isGrab = (st: number) => st >= 0x34 && st < 0x39;
const isCaptured = (st: number) => st >= 0x3d && st < 0x43;

const PUNISHABLE = (() => {
  const s = new Uint8Array(512);
  s.fill(1, 0x3d, 0x65); // captured, damage/hitstun/tumble, down, shield break, dizzy, song, bind, techs
  s.fill(1, 0x6e, 0x71); // footstooled
  s.fill(1, 0xcc, 0xec); // bitten, clung, swallowed, carried, captured by specials
  s[DEAD] = s[267] = 1;
  return s;
})();

type Hit = {
  frame: number;
  damage: number;
  attacker: number; // player index
  move: number; // the attacker's action instance (the frame its move began)
};

export type Conversion = {
  victim: number;
  attacker: number;
  startFrame: number; // timeline index of the first hit
  endFrame: number;
  startPercent: number;
  endPercent: number;
  hits: Hit[];
  kill: boolean;
  killFrame: number | null;
  moves: number;
  moveKinds: number;
  attacks: number;
  whiffs: number;
  damage: number;
  seconds: number;
  attackerInputsPerS: number;
  densestMoves1s: number;
  attackerShare: number;
  tags: string[];
  score: number;
};

export type DetectOptions = {
  /** Version 1's score (no reward for different moves): to compare with the Python prototype. */
  noVariety?: boolean;
};

/** Python's round (half to even). */
function roundHalfEven(x: number): number {
  const r = Math.round(x);
  return Math.abs(x % 1) === 0.5 && r % 2 !== 0 ? r - 1 : r;
}

function stickZone(x: number, y: number): number {
  const sx = (x << 24) >> 24;
  const sy = (y << 24) >> 24;
  if (Math.hypot(sx, sy) <= 40) {
    return 0;
  }
  const z = roundHalfEven(Math.atan2(sy, sx) / (Math.PI / 4));
  return (((z % 8) + 8) % 8) + 1;
}

/**
 * Button presses, main-stick moves into a new direction, C-stick flicks and trigger presses over
 * the pads of frames [from, to] (inclusive).
 */
function inputEvents(pads: Uint8Array, from: number, to: number): number {
  if (to - from < 1) {
    return 0;
  }
  let count = 0;
  const at = (f: number, k: number) => pads[f * PAD_KEPT + k];
  const buttons = (f: number) => ((at(f, 0) << 24) | (at(f, 1) << 16) | (at(f, 2) << 8) | at(f, 3)) >>> 0;
  let prevButtons = buttons(from);
  let prevStick = stickZone(at(from, 4), at(from, 5));
  let prevC = stickZone(at(from, 6), at(from, 7));
  let prevTrig = at(from, 8) > 0x50 || at(from, 9) > 0x50;
  for (let f = from + 1; f <= to; f++) {
    const b = buttons(f);
    let pressed = (b & ~prevButtons) >>> 0;
    while (pressed) {
      count += pressed & 1;
      pressed >>>= 1;
    }
    const stick = stickZone(at(f, 4), at(f, 5));
    const c = stickZone(at(f, 6), at(f, 7));
    const trig = at(f, 8) > 0x50 || at(f, 9) > 0x50;
    if (stick !== prevStick && stick !== 0) {
      count++;
    }
    if (c !== 0 && prevC === 0) {
      count++;
    }
    if (trig && !prevTrig) {
      count++;
    }
    prevButtons = b;
    prevStick = stick;
    prevC = c;
    prevTrig = trig;
  }
  return count;
}

/** For each frame, the frame the player's current move (action instance) began. */
function moveStarts(t: Timeline, i: number): Int32Array {
  const st = t.status[i];
  const mo = t.motion[i];
  const an = t.anim[i];
  const out = new Int32Array(t.frames);
  let start = 0;
  for (let f = 1; f < t.frames; f++) {
    let change = st[f] !== st[f - 1] || mo[f] !== mo[f - 1] || an[f] < Math.fround(an[f - 1] - 0.5);
    // One move: a special's chain of states, an aerial into its landing.
    const same =
      (st[f] >= SPECIALS && st[f - 1] >= SPECIALS && st[f] !== st[f - 1]) ||
      (st[f] === LANDING_AIR_ATTACK && st[f - 1] === AIR_ATTACK);
    change = change && !same;
    if (change) {
      start = f;
    }
    out[f] = start;
  }
  return out;
}

/** Every conversion in the timeline, scored and tagged. */
export function findConversions(t: Timeline, options: DetectOptions = {}): Conversion[] {
  const out: Conversion[] = [];
  const n = t.players.length;
  const starts = t.players.map((_, i) => moveStarts(t, i));
  const byEntry = new Map<number, number>();
  for (const p of t.players) {
    if (!byEntry.has(p.entryId)) {
      byEntry.set(p.entryId, p.index);
    }
  }
  for (let v = 0; v < n; v++) {
    const dmg = t.damage[v];
    const stk = t.stocks[v];
    const st = t.status[v];
    const att = t.attacker[v];
    let conv: Conversion | null = null;
    let reset = 0;
    for (let f = 1; f < t.frames; f++) {
      const lost = stk[f] < stk[f - 1];
      const died = st[f] === DEAD && st[f - 1] !== DEAD;
      const up = Math.fround(dmg[f] - dmg[f - 1]);
      if (up > 0.05 && !lost && stk[f] > 0) {
        let who = byEntry.get(att[f]);
        if (who == null && n === 2) {
          who = 1 - v;
        }
        if (who != null && who !== v) {
          if (conv == null) {
            conv = newConversion(v, who, f, dmg[f - 1], dmg[f]);
          }
          conv.hits.push({ frame: f, damage: up, attacker: who, move: starts[who][f] });
          conv.endFrame = f;
          conv.endPercent = dmg[f];
          reset = 0;
        }
      }
      if (conv == null) {
        continue;
      }
      if (died && !conv.kill) {
        // The KO: the stock goes 1 frame later (blast zone) or ~130 (star and screen KOs).
        conv.kill = true;
        conv.killFrame = f;
      }
      if (lost || (conv.kill && conv.killFrame != null && f - conv.killFrame > 200)) {
        conv.kill = true;
        conv.killFrame = conv.killFrame ?? f;
        conv.endFrame = f;
        out.push(conv);
        conv = null;
        continue;
      }
      if (conv.kill) {
        continue;
      }
      if (PUNISHABLE[st[f] & 511]) {
        reset = 0;
        conv.endFrame = f;
      } else {
        reset++;
        if (reset > RESET_FRAMES) {
          out.push(conv);
          conv = null;
        }
      }
    }
    if (conv != null) {
      out.push(conv);
    }
  }
  for (const c of out) {
    features(t, c, starts, options);
  }
  return out;
}

function newConversion(victim: number, attacker: number, f: number, before: number, after: number): Conversion {
  return {
    victim,
    attacker,
    startFrame: f,
    endFrame: f,
    startPercent: before,
    endPercent: after,
    hits: [],
    kill: false,
    killFrame: null,
    moves: 0,
    moveKinds: 0,
    attacks: 0,
    whiffs: 0,
    damage: 0,
    seconds: 0,
    attackerInputsPerS: 0,
    densestMoves1s: 0,
    attackerShare: 1,
    tags: [],
    score: 0,
  };
}

function features(t: Timeline, c: Conversion, starts: Int32Array[], options: DetectOptions) {
  const by = new Map<number, number>();
  for (const h of c.hits) {
    by.set(h.attacker, (by.get(h.attacker) ?? 0) + h.damage);
  }
  let total = 0;
  let best = -1;
  let bestDamage = -Infinity;
  for (const [a, d] of by) {
    total += d;
    if (d > bestDamage) {
      best = a;
      bestDamage = d;
    }
  }
  c.attacker = best;
  c.attackerShare = bestDamage / Math.max(total, 1e-6);
  const moveFirst = new Map<string, number>();
  for (const h of c.hits) {
    const key = `${h.attacker}:${h.move}`;
    if (!moveFirst.has(key)) {
      moveFirst.set(key, h.frame);
    }
  }
  c.moves = moveFirst.size;
  // Kinds of move: the motion (animation) each move began with, per attacker.
  const kinds = new Set<string>();
  for (const key of moveFirst.keys()) {
    const [a, m] = key.split(":").map(Number);
    kinds.add(`${a}:${t.motion[a][m]}`);
  }
  c.moveKinds = kinds.size;
  c.damage = c.hits.reduce((s, h) => s + h.damage, 0);
  const first = c.hits[0].frame;
  const last = c.hits[c.hits.length - 1].frame;
  c.seconds = (last - first) / FPS;
  c.attackerInputsPerS = inputEvents(t.pads[c.attacker], first, last) / Math.max(c.seconds, 0.5);
  const moveFrames = [...new Set(moveFirst.values())].sort((x, y) => x - y);
  c.densestMoves1s = Math.max(...moveFrames.map((f) => moveFrames.filter((g) => f <= g && g < f + FPS).length));

  // Whiffs: the attacker's attacks from the first hit's move to the last hit that hit nothing (a
  // grab counts when the victim is held during it).
  const a = c.attacker;
  const ms = starts[a];
  const sa = t.status[a];
  const sv = t.status[c.victim];
  const hitMoves = new Set(c.hits.filter((h) => h.attacker === a).map((h) => h.move));
  const tried = new Set<number>();
  for (let f = ms[first]; f <= last; f++) {
    if (ATTACKS[sa[f] & 1023]) {
      tried.add(ms[f]);
    }
  }
  let whiffs = 0;
  for (const m of tried) {
    if (hitMoves.has(m)) {
      continue;
    }
    let end = m;
    while (end + 1 <= last && ms[end + 1] === m) {
      end++;
    }
    if (isGrab(sa[m])) {
      let held = false;
      for (let f = m; f < Math.min(end + 12, t.frames); f++) {
        if (isCaptured(sv[f])) {
          held = true;
          break;
        }
      }
      if (held) {
        continue;
      }
    }
    whiffs++;
  }
  const all = new Set([...tried, ...hitMoves]);
  c.attacks = all.size;
  c.whiffs = whiffs;

  const tags: string[] = [];
  if (c.moves >= 4 && c.damage >= 30) {
    tags.push("combo");
  }
  if (c.kill && c.startPercent < 1) {
    tags.push("zero-to-death");
  } else if (c.kill && c.startPercent <= 30) {
    tags.push("low-to-death");
  }
  if (c.densestMoves1s >= 4 || (c.moves >= 3 && c.attackerInputsPerS >= FAST_INPUTS_PER_S)) {
    tags.push("fast");
  }
  if (tags.length && c.moves >= 3 && c.whiffs === 0) {
    tags.push("clean");
  }
  c.tags = tags;
  const speed = Math.max(c.densestMoves1s - 2, 0) * 1.5 + Math.max(c.attackerInputsPerS - 8, 0) * 0.4;
  const killBonus = tags.includes("zero-to-death") ? 6 : tags.includes("low-to-death") ? 3 : c.kill ? 1.5 : 0;
  const accuracy = 1 - c.whiffs / Math.max(c.attacks, 1);
  const moveValue = options.noVariety ? c.moves : c.moveKinds + REPEAT_WEIGHT * (c.moves - c.moveKinds);
  const variety = options.noVariety ? 0 : VARIETY_BONUS * Math.max(c.moveKinds - 2, 0);
  const cleanBonus = c.whiffs === 0 && c.moves >= 3 ? moveValue : 0;
  c.score =
    (moveValue + c.damage / 15 + killBonus + Math.min(speed, 8) + cleanBonus + variety) *
    (0.5 + 0.5 * accuracy) *
    c.attackerShare;
}

/** A conversion worth showing: one with a tag other than `clean`. */
export const isHighlight = (c: Conversion): boolean => c.tags.some((tag) => tag !== "clean");
