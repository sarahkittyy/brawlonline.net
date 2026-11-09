import { animations, moves } from "@slippi/slippi-js";
import { describe, expect, it } from "vitest";

import type { Timeline } from "../highlights/replay_frames";
import { PAD_KEPT } from "../highlights/replay_frames";
import { computeStats } from "./brawl_stats";

// Brawl status and motion kinds the tests use (BrawlHeaders ft/fighter.h).
const WAIT = 0x00;
const DASH = 0x03;
const TURN = 0x06;
const TURN_DASH = 0x07;
const JUMP_SQUAT = 0x0a;
const JUMP = 0x0b;
const FALL = 0x0e;
const LANDING_ATTACK_AIR = 0x18;
const LANDING_FALL_SPECIAL = 0x19;
const ESCAPE_AIR = 0x21;
const ATTACK_HI4_START = 0x30;
const ATTACK_HI4 = 0x32;
const ATTACK_AIR = 0x33;
const CATCH_WAIT = 0x39;
const THROW = 0x3c;
const CAPTURE_PULLED = 0x3d;
const CAPTURE_THROWN = 0x42;
const DAMAGE_FLY = 0x45;
const DEAD = 0xbd;
const SPECIAL = 0x115; // a fighter-specific status
const MOTION_ATTACK_AIR_F = 0x63;
const MOTION_LANDING_AIR_F = 0x68;
const MOTION_THROW_F = 0x73;
const B = 0x200;
const X = 0x400;

/** A two-player (or n-player) timeline of `frames` frames: everyone waiting at 0%, 4 stocks. */
function timeline(frames: number, players = 2): Timeline {
  const make = <T>(f: () => T) => Array.from({ length: players }, f);
  return {
    firstFrame: 0,
    frames,
    players: make(() => null).map((_, i) => ({
      index: i,
      port: i + 1,
      displayName: `P${i + 1}`,
      connectCode: "",
      characterId: 7,
      costume: 0,
      teamId: null,
      entryId: 0x10000 + i,
    })),
    recorderPort: 1,
    isTeams: false,
    stageId: null,
    damage: make(() => new Float32Array(frames)),
    stocks: make(() => new Int32Array(frames).fill(4)),
    status: make(() => new Int32Array(frames).fill(WAIT)),
    attacker: make(() => new Uint32Array(frames)),
    anim: make(() => new Float32Array(frames)),
    motion: make(() => new Int32Array(frames)),
    x: make(() => new Float32Array(frames)),
    y: make(() => new Float32Array(frames)),
    kind: make(() => new Uint32Array(frames).fill(6)),
    aerialLandings: make(() => new Float32Array(frames)),
    lCancels: make(() => new Float32Array(frames)),
    pads: make(() => new Uint8Array(frames * PAD_KEPT)),
  };
}

/** Sets fighter `p`'s status (and motion) for frames [from, to). */
function state(t: Timeline, p: number, from: number, to: number, status: number, motion = 0) {
  t.status[p].fill(status, from, to);
  t.motion[p].fill(motion, from, to);
}

function press(t: Timeline, p: number, f: number, buttons: number, stick: [number, number] = [0, 0]) {
  const at = f * PAD_KEPT;
  t.pads[p][at] = buttons >>> 24;
  t.pads[p][at + 1] = (buttons >>> 16) & 0xff;
  t.pads[p][at + 2] = (buttons >>> 8) & 0xff;
  t.pads[p][at + 3] = buttons & 0xff;
  t.pads[p][at + 4] = stick[0] & 0xff;
  t.pads[p][at + 5] = stick[1] & 0xff;
}

/** P2's damage goes up by `amount` at frame `f` (and stays). */
function hit(t: Timeline, victim: number, f: number, amount: number) {
  const d = t.damage[victim];
  for (let g = f; g < t.frames; g++) {
    d[g] += amount;
  }
}

describe("L-cancels", () => {
  it("counts P+'s decision at each aerial landing", () => {
    const t = timeline(100);
    // Three landings: L-cancelled, missed, L-cancelled.
    for (const [f, cancelled] of [
      [10, true],
      [40, false],
      [70, true],
    ] as const) {
      for (let g = f; g < 100; g++) {
        t.aerialLandings[0][g] += 1;
        t.lCancels[0][g] += cancelled ? 1 : 0;
      }
    }
    const s = computeStats(t, true);
    expect(s.actionCounts[0].lCancelCount).toEqual({ success: 2, fail: 1 });
    expect(s.actionCounts[1].lCancelCount).toEqual({ success: 0, fail: 0 });
  });

  it("does not count another fighter instance's counters (Zelda to Sheik)", () => {
    const t = timeline(60);
    t.kind[0].fill(0x14, 30); // the other instance from frame 30, its own counters
    t.aerialLandings[0].fill(5, 30);
    t.lCancels[0].fill(3, 30);
    expect(computeStats(t, true).actionCounts[0].lCancelCount).toEqual({ success: 0, fail: 0 });
  });
});

describe("movement", () => {
  it("tells wavedashes from wavelands and late air dodges, and takes their dodges out", () => {
    const t = timeline(200);
    // Wavedash: jump squat 3 frames, 2 in the air, the dodge, the landing.
    state(t, 0, 10, 13, JUMP_SQUAT);
    state(t, 0, 13, 15, JUMP);
    state(t, 0, 15, 17, ESCAPE_AIR);
    state(t, 0, 17, 27, LANDING_FALL_SPECIAL);
    // Waveland: falling, a dodge, the landing (no jump squat in the last 15 frames).
    state(t, 0, 60, 80, FALL);
    state(t, 0, 80, 82, ESCAPE_AIR);
    state(t, 0, 82, 92, LANDING_FALL_SPECIAL);
    // A late air dodge: only the dodge and the landing in the last 15 frames.
    state(t, 0, 120, 140, ESCAPE_AIR);
    state(t, 0, 140, 150, LANDING_FALL_SPECIAL);
    const c = computeStats(t, true).actionCounts[0];
    expect(c.wavedashCount).toBe(1);
    expect(c.wavelandCount).toBe(1);
    expect(c.airDodgeCount).toBe(1); // three dodges, two of them a wavedash and a waveland
  });

  it("counts Brawl's dash back (Dash, one frame of Turn_Dash, Dash) as a dash dance, not a turn", () => {
    const t = timeline(100);
    state(t, 0, 10, 16, DASH);
    state(t, 0, 16, 17, TURN_DASH);
    state(t, 0, 17, 23, DASH);
    state(t, 0, 23, 24, TURN_DASH);
    state(t, 0, 24, 30, DASH);
    // Melee's pattern (a TURN) is a turn around in Brawl, not a dash dance.
    state(t, 0, 50, 56, DASH);
    state(t, 0, 56, 57, TURN);
    state(t, 0, 57, 63, DASH);
    expect(computeStats(t, true).actionCounts[0].dashDanceCount).toBe(2);
  });
});

describe("conversions, kills and moves", () => {
  it("names each move by its generic slippi-js id, and the kill's direction", () => {
    const t = timeline(400);
    // P1: a forward air, then an up smash that KOs P2 off the top; P2 is in hitstun throughout.
    state(t, 0, 20, 40, ATTACK_AIR, MOTION_ATTACK_AIR_F);
    state(t, 0, 40, 46, LANDING_ATTACK_AIR, MOTION_LANDING_AIR_F);
    state(t, 0, 60, 66, ATTACK_HI4_START);
    state(t, 0, 66, 90, ATTACK_HI4);
    hit(t, 1, 30, 12);
    hit(t, 1, 70, 17);
    state(t, 1, 30, 120, DAMAGE_FLY);
    state(t, 1, 120, 130, DEAD);
    t.y[1].fill(260, 120, 130); // past the top blast zone
    t.stocks[1].fill(3, 121);
    for (let f = 121; f < 400; f++) {
      t.damage[1][f] = 0;
    }
    const s = computeStats(t, true);
    expect(s.conversions).toHaveLength(1);
    const c = s.conversions[0];
    expect(c.playerIndex).toBe(1);
    expect(c.didKill).toBe(true);
    expect(c.startFrame).toBe(30);
    expect(c.endFrame).toBe(121);
    expect(c.openingType).toBe("neutral-win");
    expect(c.moves.map((m) => moves.getMoveName(m.moveId))).toEqual(["Forward Air", "Up Smash"]);
    expect(c.moves.map((m) => m.damage)).toEqual([12, 17]);
    const lost = s.stocks.find((st) => st.playerIndex === 1 && st.endFrame != null);
    expect(lost?.endFrame).toBe(121);
    expect(animations.getDeathDirection(lost?.deathAnimation ?? -1)).toBe("up");
    expect(s.overall[0].killCount).toBe(1);
    expect(s.overall[0].totalDamage).toBe(29);
  });

  it("keeps a star KO's percent while the stock goes ~130 frames later", () => {
    const t = timeline(400);
    state(t, 0, 20, 40, ATTACK_HI4);
    hit(t, 1, 30, 120);
    state(t, 1, 30, 100, DAMAGE_FLY);
    state(t, 1, 100, 240, DEAD); // the stock goes while the fighter is still dead (as recorded)
    t.y[1].fill(300, 100, 240);
    for (let f = 150; f < 400; f++) {
      t.damage[1][f] = 0; // reset before the stock goes
    }
    t.stocks[1].fill(3, 231);
    const s = computeStats(t, true);
    const lost = s.stocks.find((st) => st.playerIndex === 1 && st.endFrame != null);
    expect([lost?.endFrame, lost?.currentPercent, lost?.endPercent]).toEqual([231, 120, 120]);
    const c = s.conversions[0];
    expect([c.didKill, c.startPercent, c.currentPercent, c.endPercent]).toEqual([true, 0, 120, 120]);
  });

  it("names a special by the stick when B began it, and counts one move for its chain of states", () => {
    const t = timeline(200);
    press(t, 0, 20, B, [0, 127]); // up and B
    state(t, 0, 20, 30, SPECIAL);
    state(t, 0, 30, 50, SPECIAL + 1);
    hit(t, 1, 25, 4);
    hit(t, 1, 35, 4);
    state(t, 1, 25, 60, DAMAGE_FLY);
    const c = computeStats(t, true).conversions[0];
    expect(c.moves.map((m) => [moves.getMoveName(m.moveId), m.hitCount])).toEqual([["Up B", 2]]);
  });

  it("names a throw by its motion", () => {
    const t = timeline(200);
    state(t, 0, 20, 30, CATCH_WAIT);
    state(t, 0, 30, 50, THROW, MOTION_THROW_F);
    state(t, 1, 20, 30, CAPTURE_PULLED);
    state(t, 1, 30, 50, CAPTURE_THROWN);
    state(t, 1, 50, 80, DAMAGE_FLY);
    hit(t, 1, 40, 9);
    const s = computeStats(t, true);
    expect(s.conversions[0].moves.map((m) => moves.getMoveName(m.moveId))).toEqual(["Forward Throw"]);
  });

  it("ends a punish 45 frames after the victim is back in control, and calls a reply before that a counter-attack", () => {
    const t = timeline(300);
    // P1 up smashes P2 (hitstun 20-40, then waiting: in control); P2 hits back at 60, inside P1's
    // punish, which ends at 85 (the 46th frame in control).
    state(t, 0, 10, 30, ATTACK_HI4);
    hit(t, 1, 20, 10);
    state(t, 1, 20, 40, DAMAGE_FLY);
    state(t, 1, 50, 70, ATTACK_HI4);
    hit(t, 0, 60, 10);
    state(t, 0, 60, 80, DAMAGE_FLY);
    const s = computeStats(t, true);
    expect(s.conversions.map((c) => [c.playerIndex, c.startFrame, c.endFrame, c.openingType])).toEqual([
      [1, 20, 85, "neutral-win"],
      [0, 60, 125, "counter-attack"],
    ]);
  });

  it("calls two openings on the same frame a trade", () => {
    const t = timeline(200);
    for (const [a, v] of [
      [0, 1],
      [1, 0],
    ]) {
      state(t, a, 10, 30, ATTACK_HI4);
      state(t, v, 30, 60, DAMAGE_FLY);
      hit(t, v, 30, 10);
    }
    expect(computeStats(t, true).conversions.map((c) => c.openingType)).toEqual(["trade", "trade"]);
  });
});

describe("inputs and the game", () => {
  it("counts button presses and stick moves into a new region per minute", () => {
    const t = timeline(3600);
    for (let f = 100; f < 3600; f += 100) {
      press(t, 0, f, X, [127, 0]); // X and the stick right on one frame, then back to neutral
    }
    const o = computeStats(t, true).overall[0];
    expect(o.inputCounts.buttons).toBe(35);
    expect(o.inputCounts.joystick).toBe(35);
    expect(o.inputsPerMinute.ratio).toBeCloseTo(70);
    expect(o.digitalInputsPerMinute.ratio).toBeCloseTo(35);
  });

  it("gives frames as the replay's (playback's) frames and says whether the game ended", () => {
    const t = timeline(500);
    t.firstFrame = 100; // the timeline's first frame is the replay's frame 100
    state(t, 0, 10, 30, ATTACK_HI4);
    hit(t, 1, 20, 10);
    state(t, 1, 20, 40, DAMAGE_FLY);
    const s = computeStats(t, false);
    expect([s.lastFrame, s.playableFrameCount, s.gameComplete]).toEqual([599, 500, false]);
    expect(s.stocks.map((st) => [st.playerIndex, st.startFrame])).toEqual([
      [0, 100],
      [1, 100],
    ]);
    expect([s.conversions[0].startFrame, s.conversions[0].moves[0].frame]).toEqual([120, 120]);
  });

  it("has nothing for 3 or 4 players (as slippi-js: singles only)", () => {
    const s = computeStats(timeline(100, 3), true);
    expect(s.stocks).toEqual([]);
    expect(s.conversions).toEqual([]);
    expect(s.actionCounts).toEqual([]);
  });
});
