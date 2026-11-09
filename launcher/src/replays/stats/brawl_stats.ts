/**
 * Slippi's per-game stats (slippi-js `StatsType`, what the replay stats page shows: overall,
 * kills, openings and conversions) from a `.brep`'s timeline (highlights/replay_frames.ts).
 *
 * slippi-js computes them from Melee's per-frame action states and its L-cancel flag. These are the
 * same computations as its stats computers (conversions, stocks, actions, inputs, and
 * `generateOverallStats` itself) over Brawl's status kinds (BrawlHeaders ft/fighter.h) and P+'s own
 * L-cancel counters, as verified in P+ (docs/brawl-memory-map.md "Per player"):
 *
 * - a wavedash is Jump_Squat → Jump → Escape_Air → Landing_Fall_Special (Melee's shape); a waveland
 *   the same without the jump squat;
 * - a dash dance turn is Dash → Turn_Dash (one frame) → Dash, not Melee's TURN;
 * - L-cancels are P+'s own decision: its count of aerial landings grows on the landing's frame, its
 *   count of L-cancelled ones with it when the landing was cancelled (a missed one with the first
 *   only). Replays from before 2026-11-08 have no counters: no L-cancels counted.
 *
 * The page reads two Melee lookups: a move's name (slippi-js `moves`, `MoveLandedType.moveId`) and
 * a KO's direction (slippi-js `animations.getDeathDirection`, `StockType.deathAnimation`). Their ids
 * are generic (move 14 is "Forward Air", dead state 1 a KO off the left side), so Brawl's moves and
 * KOs are written with them. A special is Neutral / Side / Up / Down B by the stick when B began it
 * (fighter-specific statuses have no common names). Frames are the replay's (playback's and the
 * highlights').
 *
 * Only what the page shows is computed: of the action counts, rolls, air dodges, spot dodges,
 * wavedashes, wavelands, dash dances, ledge grabs and L-cancels (the rest stay 0), and no combos
 * (the page reads none). Like slippi-js, everything is for singles: a game of 3 or 4 players gets
 * empty lists.
 */
import type {
  ActionCountsType,
  ConversionType,
  GameStartType,
  MoveLandedType,
  StatsType,
  StockType,
} from "@slippi/slippi-js";
import { generateOverallStats, Timers } from "@slippi/slippi-js";

import { moveStarts } from "../highlights/detect_highlights";
import type { Timeline } from "../highlights/replay_frames";
import { PAD_KEPT, readTimeline } from "../highlights/replay_frames";
import { readReplayHeader } from "../replay_format";

// Brawl's status kinds (BrawlHeaders ft/fighter.h).
const DASH = 0x03;
const TURN_DASH = 0x07;
const JUMP_SQUAT = 0x0a;
const JUMP = 0x0b;
const FALL_AERIAL = 0x0f;
const SQUAT = 0x11;
const SQUAT_RV = 0x15;
const LANDING_FALL_SPECIAL = 0x19;
const ESCAPE = 0x1e;
const ESCAPE_F = 0x1f;
const ESCAPE_B = 0x20;
const ESCAPE_AIR = 0x21;
const ATTACK = 0x24;
const ATTACK_100 = 0x25;
const ATTACK_DASH = 0x26;
const ATTACK_S3 = 0x27;
const ATTACK_HI3 = 0x28;
const ATTACK_LW3 = 0x29;
const ATTACK_S4_START = 0x2a;
const ATTACK_S4 = 0x2c;
const ATTACK_LW4_START = 0x2d;
const ATTACK_LW4 = 0x2f;
const ATTACK_HI4_START = 0x30;
const ATTACK_HI4 = 0x32;
const ATTACK_AIR = 0x33;
const CATCH = 0x34;
const CATCH_ATTACK = 0x3a;
const THROW = 0x3c;
const CAPTURE_PULLED = 0x3d;
const CAPTURE_THROWN = 0x42;
const DAMAGE = 0x43;
const DAMAGE_FALL = 0x49;
const DOWN_STAND_ATTACK = 0x53;
const DOWN_DAMAGE = 0x54; // hit while down (Melee's jab reset)
const TREAD_DAMAGE = 0x6e; // footstooled
const TREAD_DAMAGE_AIR = 0x70;
const CLIFF_CATCH = 0x74;
const CLIFF_ATTACK = 0x76;
const SLIP_STAND_ATTACK = 0x8f; // trip attack
const DEAD = 0xbd;
const STANDBY = 0x10b; // P+'s state after a KO (before the stock goes)
const SPECIALS = 0x112; // fighter-specific statuses (the specials) from here on

// Motion kinds.
const ATTACK_11 = 0x48; // jab 1; 0x49, 0x4a are jabs 2 and 3
const ATTACK_13 = 0x4a;
const ATTACK_AIR_N = 0x62; // N, F, B, Hi, Lw follow
const THROW_B = 0x72;
const THROW_F = 0x73;
const THROW_HI = 0x74;
const THROW_LW = 0x75;
const CLIFF_ATTACK_QUICK = 0xd7;
const CLIFF_ATTACK_SLOW = 0xdc;

// slippi-js move ids (generic names: moves.getMoveName).
const MOVE_MISC = 1;
const MOVE_JAB = [2, 3, 4];
const MOVE_RAPID_JABS = 5;
const MOVE_DASH_ATTACK = 6;
const MOVE_FTILT = 7;
const MOVE_UTILT = 8;
const MOVE_DTILT = 9;
const MOVE_FSMASH = 10;
const MOVE_USMASH = 11;
const MOVE_DSMASH = 12;
const MOVE_AERIALS = [13, 14, 15, 16, 17]; // nair, fair, bair, uair, dair
const MOVE_NEUTRAL_B = 18;
const MOVE_SIDE_B = 19;
const MOVE_UP_B = 20;
const MOVE_DOWN_B = 21;
const MOVE_GETUP_ATTACK = 50;
const MOVE_PUMMEL = 52;
const MOVE_FTHROW = 53;
const MOVE_BTHROW = 54;
const MOVE_UTHROW = 55;
const MOVE_DTHROW = 56;
const MOVE_EDGE_ATTACK_SLOW = 61;
const MOVE_EDGE_ATTACK = 62;

// slippi-js `animations.getDeathDirection`'s ids (Melee's dead states).
const DEAD_DOWN = 0;
const DEAD_LEFT = 1;
const DEAD_RIGHT = 2;
const DEAD_UP = 3;

/** Damage below this between two frames is float noise, not a hit. */
const DAMAGE_EPSILON = 0.05;

// Pads (gfPadStatus, GameCube bits; sticks ±127 at full tilt, triggers 0-255).
const BUTTON_B = 0x200;
const STICK_FULL = 127;
/** slippi-js's stick region threshold (0.2875 of full tilt). */
const STICK_REGION = 0.2875 * STICK_FULL;
/** slippi-js counts a trigger press past 0.3. */
const TRIGGER_PRESS = 0.3 * 255;
/** A special's B press is found at most this many frames before the move began (a buffered press). */
const SPECIAL_LOOKBACK = 10;

const isDamaged = (s: number) =>
  (s >= DAMAGE && s <= DAMAGE_FALL) || s === DOWN_DAMAGE || (s >= TREAD_DAMAGE && s <= TREAD_DAMAGE_AIR);
const isGrabbed = (s: number) => s >= CAPTURE_PULLED && s <= CAPTURE_THROWN;
/** Held by an opponent's special: bitten, clung, swallowed, carried, captured (P+'s Brawl statuses). */
const isCommandGrabbed = (s: number) =>
  (s >= 0xcc && s <= 0xdb) || (s >= 0xe6 && s <= 0xec) || s === 0xee || (s >= 0x102 && s <= 0x109);
const isInControl = (s: number) =>
  s <= JUMP_SQUAT || (s >= SQUAT && s <= SQUAT_RV) || (s > ATTACK && s <= ATTACK_HI4) || s === CATCH;
const isDead = (s: number) => s === DEAD || s === STANDBY;
/** Melee's CONTROLLED_JUMP range without the knee bend: jumps and falls. */
const isAirborneJump = (s: number) => s >= JUMP && s <= FALL_AERIAL;
const isAttack = (s: number) =>
  (s >= ATTACK && s <= ATTACK_AIR) ||
  s === CATCH_ATTACK ||
  s === THROW ||
  s === DOWN_STAND_ATTACK ||
  s === CLIFF_ATTACK ||
  s === SLIP_STAND_ATTACK ||
  s >= SPECIALS;

type Pads = Uint8Array;
const buttonsAt = (pads: Pads, f: number) =>
  ((pads[f * PAD_KEPT] << 24) |
    (pads[f * PAD_KEPT + 1] << 16) |
    (pads[f * PAD_KEPT + 2] << 8) |
    pads[f * PAD_KEPT + 3]) >>>
  0;
const s8 = (v: number) => (v << 24) >> 24;
const stickAt = (pads: Pads, f: number, c: boolean): [number, number] => [
  s8(pads[f * PAD_KEPT + (c ? 6 : 4)]),
  s8(pads[f * PAD_KEPT + (c ? 7 : 5)]),
];

/** Neutral / Side / Up / Down B by a stick (Brawl: the larger axis wins). */
function specialFromStick([x, y]: [number, number]): number {
  if (Math.hypot(x, y) < STICK_REGION) {
    return MOVE_NEUTRAL_B;
  }
  if (Math.abs(y) > Math.abs(x)) {
    return y > 0 ? MOVE_UP_B : MOVE_DOWN_B;
  }
  return MOVE_SIDE_B;
}

/** Which special began at `start`: the stick at its B press, else the C-stick (P+'s C-stick specials). */
function specialAt(pads: Pads, start: number): number {
  for (let f = start; f >= Math.max(1, start - SPECIAL_LOOKBACK); f--) {
    if (buttonsAt(pads, f) & BUTTON_B && !(buttonsAt(pads, f - 1) & BUTTON_B)) {
      return specialFromStick(stickAt(pads, f, false));
    }
  }
  const c = stickAt(pads, start, true);
  return Math.hypot(c[0], c[1]) >= STICK_REGION ? specialFromStick(c) : specialFromStick(stickAt(pads, start, false));
}

/** The slippi-js move id of the move a fighter begins at frame `f` (its status and motion). */
function moveIdAt(t: Timeline, p: number, f: number): number {
  const s = t.status[p][f];
  const m = t.motion[p][f];
  if (s >= SPECIALS) {
    return specialAt(t.pads[p], f);
  }
  switch (s) {
    case ATTACK:
      return m >= ATTACK_11 && m <= ATTACK_13 ? MOVE_JAB[m - ATTACK_11] : MOVE_JAB[0];
    case ATTACK_100:
      return MOVE_RAPID_JABS;
    case ATTACK_DASH:
      return MOVE_DASH_ATTACK;
    case ATTACK_S3:
      return MOVE_FTILT;
    case ATTACK_HI3:
      return MOVE_UTILT;
    case ATTACK_LW3:
      return MOVE_DTILT;
    case ATTACK_AIR:
      return m >= ATTACK_AIR_N && m < ATTACK_AIR_N + 5 ? MOVE_AERIALS[m - ATTACK_AIR_N] : MOVE_MISC;
    case CATCH_ATTACK:
      return MOVE_PUMMEL;
    case THROW:
      return m === THROW_F
        ? MOVE_FTHROW
        : m === THROW_B
        ? MOVE_BTHROW
        : m === THROW_HI
        ? MOVE_UTHROW
        : m === THROW_LW
        ? MOVE_DTHROW
        : MOVE_MISC;
    case DOWN_STAND_ATTACK:
    case SLIP_STAND_ATTACK:
      return MOVE_GETUP_ATTACK;
    case CLIFF_ATTACK:
      return m === CLIFF_ATTACK_SLOW ? MOVE_EDGE_ATTACK_SLOW : m === CLIFF_ATTACK_QUICK ? MOVE_EDGE_ATTACK : MOVE_MISC;
    default:
      break;
  }
  if (s >= ATTACK_S4_START && s <= ATTACK_S4) {
    return MOVE_FSMASH;
  }
  if (s >= ATTACK_LW4_START && s <= ATTACK_LW4) {
    return MOVE_DSMASH;
  }
  if (s >= ATTACK_HI4_START && s <= ATTACK_HI4) {
    return MOVE_USMASH;
  }
  return MOVE_MISC;
}

/**
 * For each frame, the move id of the newest attack the fighter had begun by then (Melee's "last
 * attack landed" for a hit: a projectile hits after its thrower has moved on).
 */
function lastAttacks(t: Timeline, p: number, starts: Int32Array): Int32Array {
  const out = new Int32Array(t.frames).fill(MOVE_MISC);
  let current = MOVE_MISC;
  for (let f = 0; f < t.frames; f++) {
    // A move's id is taken when it begins (a special's later states, an aerial's landing: the same move).
    if (isAttack(t.status[p][f]) && (f === 0 || starts[f] !== starts[f - 1])) {
      current = moveIdAt(t, p, f);
    }
    out[f] = current;
  }
  return out;
}

/**
 * Where a KO went, as slippi-js's dead states: the fighter's position on the frame it was KO'd
 * against typical P+ blast zones (left/right ~224, top ~190, bottom ~115), the farthest past wins.
 */
function deathDirection(t: Timeline, p: number, lostAt: number): number {
  let f = lostAt;
  for (let g = lostAt; g >= Math.max(0, lostAt - 300); g--) {
    if (t.status[p][g] === DEAD && (g === 0 || t.status[p][g - 1] !== DEAD)) {
      f = g;
      break;
    }
  }
  const x = t.x[p][f];
  const y = t.y[p][f];
  const side = Math.abs(x) / 224;
  const up = y / 190;
  const down = -y / 115;
  if (up >= side && up >= down) {
    return DEAD_UP;
  }
  if (down >= side) {
    return DEAD_DOWN;
  }
  return x < 0 ? DEAD_LEFT : DEAD_RIGHT;
}

type Pair = { player: number; opponent: number }; // fighter indices in the timeline

function pairs(t: Timeline): Pair[] {
  return t.players.length === 2
    ? [
        { player: 0, opponent: 1 },
        { player: 1, opponent: 0 },
      ]
    : [];
}

/** slippi-js ConversionComputer. */
function conversions(t: Timeline, starts: Int32Array[], attacks: Int32Array[]): ConversionType[] {
  const out: ConversionType[] = [];
  const frame = (f: number) => t.firstFrame + f;
  const index = (i: number) => t.players[i].port - 1;
  for (const { player: a, opponent: v } of pairs(t)) {
    let current: ConversionType | undefined;
    let move: MoveLandedType | undefined;
    let resetCounter = 0;
    let lastHitMove: number | undefined; // the attacker's move (its start frame) that hit last
    const damage = t.damage[v];
    const status = t.status[v];
    for (let f = 0; f < t.frames; f++) {
      const s = status[f];
      const held = isDamaged(s) || isGrabbed(s) || isCommandGrabbed(s);
      const taken = f > 0 ? damage[f] - damage[f - 1] : 0;
      if (lastHitMove !== undefined && starts[a][f] !== lastHitMove) {
        lastHitMove = undefined;
      }
      if (held) {
        if (!current) {
          current = {
            playerIndex: index(v),
            lastHitBy: index(a),
            startFrame: frame(f),
            endFrame: undefined,
            startPercent: f > 0 ? damage[f - 1] : 0,
            currentPercent: damage[f],
            endPercent: undefined,
            moves: [],
            didKill: false,
            openingType: "unknown",
          };
          out.push(current);
        }
        if (taken > DAMAGE_EPSILON) {
          const hitBy = f > 0 ? f - 1 : f; // the move that connected
          if (lastHitMove === undefined) {
            move = { playerIndex: index(a), frame: frame(f), moveId: attacks[a][hitBy], hitCount: 0, damage: 0 };
            current.moves.push(move);
          }
          if (move) {
            move.hitCount += 1;
            move.damage += taken;
          }
          lastHitMove = starts[a][hitBy];
        }
      }
      if (!current) {
        continue;
      }
      const lostStock = f > 0 && t.stocks[v][f] < t.stocks[v][f - 1];
      // (A star or screen KO loses the stock ~130 frames after the KO: the percent stays the KO's.)
      if (!lostStock && !isDead(s)) {
        current.currentPercent = damage[f];
      }
      if (held) {
        resetCounter = 0;
      }
      if ((resetCounter === 0 && isInControl(s)) || resetCounter > 0) {
        resetCounter += 1;
      }
      if (lostStock || resetCounter > Timers.PUNISH_RESET_FRAMES) {
        current.didKill = lostStock;
        current.endFrame = frame(f);
        current.endPercent = lostStock ? current.currentPercent : f > 0 ? damage[f - 1] : 0;
        current = undefined;
        move = undefined;
        resetCounter = 0;
      }
    }
  }
  return out;
}

/** slippi-js ConversionComputer._populateConversionTypes: trades, counter-attacks, neutral wins. */
function setOpeningTypes(conversions: ConversionType[]): ConversionType[] {
  const byStart = new Map<number, ConversionType[]>();
  for (const c of conversions) {
    const group = byStart.get(c.startFrame);
    if (group) {
      group.push(c);
    } else {
      byStart.set(c.startFrame, [c]);
    }
  }
  const lastEndFrameByOppIdx: Record<number, number | undefined> = {};
  for (const [, group] of [...byStart].sort((x, y) => x[0] - y[0])) {
    const isTrade = group.length >= 2;
    for (const c of group) {
      lastEndFrameByOppIdx[c.playerIndex] = c.endFrame;
      if (isTrade) {
        c.openingType = "trade";
        continue;
      }
      const lastMove = c.moves[c.moves.length - 1];
      const oppEndFrame = lastEndFrameByOppIdx[lastMove ? lastMove.playerIndex : c.playerIndex];
      c.openingType = oppEndFrame && oppEndFrame > c.startFrame ? "counter-attack" : "neutral-win";
    }
  }
  return conversions;
}

/** slippi-js StockComputer. */
function stocks(t: Timeline): StockType[] {
  const out: StockType[] = [];
  for (const { player: p } of pairs(t)) {
    let stock: StockType | undefined;
    for (let f = 0; f < t.frames; f++) {
      const s = t.status[p][f];
      if (!stock) {
        if (isDead(s)) {
          continue;
        }
        stock = {
          playerIndex: t.players[p].port - 1,
          startFrame: t.firstFrame + f,
          endFrame: undefined,
          startPercent: 0,
          endPercent: undefined,
          currentPercent: 0,
          count: t.stocks[p][f],
          deathAnimation: undefined,
        };
        out.push(stock);
      } else if (f > 0 && t.stocks[p][f] < t.stocks[p][f - 1]) {
        stock.endFrame = t.firstFrame + f;
        stock.endPercent = stock.currentPercent;
        stock.deathAnimation = deathDirection(t, p, f);
        stock = undefined;
      } else if (!isDead(s)) {
        // (Kept from the KO on: a star KO loses the stock ~130 frames later.)
        stock.currentPercent = t.damage[p][f];
      }
    }
  }
  return out;
}

function emptyActionCounts(playerIndex: number): ActionCountsType {
  return {
    playerIndex,
    wavedashCount: 0,
    wavelandCount: 0,
    airDodgeCount: 0,
    dashDanceCount: 0,
    spotDodgeCount: 0,
    ledgegrabCount: 0,
    rollCount: 0,
    tauntCount: 0,
    edgeCancelCount: { success: 0, slow: 0 },
    lCancelCount: { success: 0, fail: 0 },
    attackCount: {
      jab1: 0,
      jab2: 0,
      jab3: 0,
      jabm: 0,
      dash: 0,
      ftilt: 0,
      utilt: 0,
      dtilt: 0,
      fsmash: 0,
      usmash: 0,
      dsmash: 0,
      nair: 0,
      fair: 0,
      bair: 0,
      uair: 0,
      dair: 0,
    },
    grabCount: { success: 0, fail: 0 },
    throwCount: { up: 0, forward: 0, back: 0, down: 0 },
    groundTechCount: { away: 0, in: 0, neutral: 0, fail: 0 },
    wallTechCount: { success: 0, fail: 0 },
  };
}

/**
 * slippi-js ActionsComputer over Brawl's statuses, for the counts the page shows (each counted when
 * its status begins), and L-cancels from P+'s counters.
 */
function actionCounts(t: Timeline, p: number): ActionCountsType {
  const c = emptyActionCounts(t.players[p].port - 1);
  const st = t.status[p];
  const wavedash = { lastAirDodgeEnd: -1 };
  for (let f = 1; f < t.frames; f++) {
    // L-cancels: P+'s counters, on the landing's frame (one fighter instance: Zelda and Sheik count
    // apart). A file from before the counters has zeros: none counted.
    if (t.kind[p][f] === t.kind[p][f - 1]) {
      const landed = t.aerialLandings[p][f] - t.aerialLandings[p][f - 1];
      if (landed > 0 && landed < 2) {
        if (t.lCancels[p][f] > t.lCancels[p][f - 1]) {
          c.lCancelCount.success += 1;
        } else {
          c.lCancelCount.fail += 1;
        }
      }
    }
    const s = st[f];
    const prev = st[f - 1];
    if (s === prev) {
      continue;
    }
    if (f >= 2 && s === DASH && prev === TURN_DASH && st[f - 2] === DASH) {
      c.dashDanceCount += 1;
    }
    if (s === ESCAPE_F || s === ESCAPE_B) {
      c.rollCount += 1;
    }
    if (s === ESCAPE) {
      c.spotDodgeCount += 1;
    }
    if (s === ESCAPE_AIR) {
      c.airDodgeCount += 1;
    }
    if (s === CLIFF_CATCH) {
      c.ledgegrabCount += 1;
    }
    handleWavedash(c, st, t.y[p], f, wavedash);
  }
  return c;
}

/**
 * slippi-js handleActionWavedash on frame `f` (a new status): a Landing_Fall_Special after an air
 * dodge or a jump is a wavedash when a jump squat came within the last 15 frames and the fighter
 * did not travel through the air to another height, else a waveland; its air dodge is not counted
 * as one (`state.lastAirDodgeEnd`: the last dodge a landing took, so no dodge is taken twice).
 */
function handleWavedash(
  c: ActionCountsType,
  st: Int32Array,
  ys: Float32Array,
  f: number,
  state: { lastAirDodgeEnd: number },
) {
  const prev = st[f - 1];
  if (st[f] !== LANDING_FALL_SPECIAL || !(prev === ESCAPE_AIR || prev === JUMP_SQUAT || isAirborneJump(prev))) {
    return;
  }
  const LOOKBACK = 15;
  const from = Math.max(0, f - LOOKBACK + 1);
  const recent = Array.from(st.subarray(from, f + 1));
  const dodge = recent.lastIndexOf(ESCAPE_AIR);
  if (new Set(recent).size === 2 && dodge >= 0) {
    // Only the dodge and the landing: a late air dodge, not a wavedash.
    state.lastAirDodgeEnd = from + dodge;
    return;
  }
  if (dodge >= 0 && from + dodge > state.lastAirDodgeEnd) {
    c.airDodgeCount -= 1;
    state.lastAirDodgeEnd = from + dodge;
  }
  const squat = recent.lastIndexOf(JUMP_SQUAT);
  if (squat < 0) {
    c.wavelandCount += 1;
    return;
  }
  const jumpFrames = recent.filter(isAirborneJump).length;
  const changedY = Math.abs(ys[f] - ys[from + squat]) > 0.1;
  if (jumpFrames >= 5 && changedY) {
    c.wavelandCount += 1;
  } else {
    c.wavedashCount += 1;
  }
}

/** slippi-js InputComputer's per-player result (what generateOverallStats reads). */
type PlayerInput = Parameters<typeof generateOverallStats>[0]["inputs"][number];

function stickRegion(x: number, y: number): number {
  const r = STICK_REGION;
  if (x >= r && y >= r) {
    return 1;
  }
  if (x >= r && y <= -r) {
    return 2;
  }
  if (x <= -r && y <= -r) {
    return 3;
  }
  if (x <= -r && y >= r) {
    return 4;
  }
  if (y >= r) {
    return 5;
  }
  if (x >= r) {
    return 6;
  }
  if (y <= -r) {
    return 7;
  }
  if (x <= -r) {
    return 8;
  }
  return 0;
}

/** slippi-js InputComputer over the recorded pads. */
function inputs(t: Timeline, p: number, opp: number): PlayerInput {
  const pads = t.pads[p];
  const out: PlayerInput = {
    playerIndex: t.players[p].port - 1,
    opponentIndex: t.players[opp].port - 1,
    inputCount: 0,
    joystickInputCount: 0,
    cstickInputCount: 0,
    buttonInputCount: 0,
    triggerInputCount: 0,
  };
  for (let f = 1; f < t.frames; f++) {
    let pressed = ~buttonsAt(pads, f - 1) & buttonsAt(pads, f) & 0xfff;
    let n = 0;
    while (pressed) {
      pressed &= pressed - 1;
      n += 1;
    }
    out.inputCount += n;
    out.buttonInputCount += n;
    for (const c of [false, true]) {
      const before = stickRegion(...stickAt(pads, f - 1, c));
      const now = stickRegion(...stickAt(pads, f, c));
      if (now !== before && now !== 0) {
        out.inputCount += 1;
        if (c) {
          out.cstickInputCount += 1;
        } else {
          out.joystickInputCount += 1;
        }
      }
    }
    for (const k of [8, 9]) {
      if (pads[(f - 1) * PAD_KEPT + k] < TRIGGER_PRESS && pads[f * PAD_KEPT + k] >= TRIGGER_PRESS) {
        out.inputCount += 1;
        out.triggerInputCount += 1;
      }
    }
  }
  return out;
}

/** The game's stats, as slippi-js's `SlippiGame.getStats()` gives a Melee game's. */
export function computeStats(t: Timeline, gameComplete: boolean): StatsType {
  const starts = t.players.map((_, i) => moveStarts(t, i));
  const attacks = t.players.map((_, i) => lastAttacks(t, i, starts[i]));
  const openings = setOpeningTypes(conversions(t, starts, attacks));
  const playerInputs = pairs(t).map(({ player, opponent }) => inputs(t, player, opponent));
  const playableFrameCount = t.frames;
  // generateOverallStats reads only the players (index, team) and whether it is a teams game.
  const settings = {
    isTeams: t.isTeams,
    players: pairs(t).map(({ player }) => ({
      playerIndex: t.players[player].port - 1,
      teamId: t.players[player].teamId ?? undefined,
    })),
  } as unknown as GameStartType;
  return {
    gameComplete,
    lastFrame: t.firstFrame + t.frames - 1,
    playableFrameCount,
    stocks: stocks(t),
    conversions: openings,
    combos: [],
    actionCounts: pairs(t).map(({ player }) => actionCounts(t, player)),
    overall: generateOverallStats({ settings, inputs: playerInputs, conversions: openings, playableFrameCount }),
  };
}

/** A `.brep`'s stats (ReplayFramesError: no fighter memory, an older or damaged file). */
export async function readStats(fullPath: string): Promise<StatsType> {
  const [t, header] = await Promise.all([readTimeline(fullPath), readReplayHeader(fullPath).catch(() => null)]);
  return computeStats(t, header?.end != null);
}
