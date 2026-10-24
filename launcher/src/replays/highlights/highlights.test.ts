import { mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "os";
import path from "path";
import { describe, expect, it } from "vitest";
import { deflateSync } from "zlib";

import { crc32 } from "../replay_format";
import { analyzeReplay, analyzeTimeline } from "./analyze_replay";
import { findConversions, isHighlight } from "./detect_highlights";
import { CLIP_AFTER, CLIP_BEFORE, findLrzClips, HOLD_FRAMES, longestLrzHold } from "./lrz_clips";
import { decodeTimeline, ReplayFramesError } from "./replay_frames";

// A replay as Dolphin writes it (ReplayFile.cpp, ReplayRecorder.cpp): header, INFO, FTSP, then
// FRMS + FTRM chunks of 60 frames, each row XORed with the one before in its chunk.
const MAGIC = Buffer.from([0x89, 0x42, 0x52, 0x50, 0x0d, 0x0a, 0x1a, 0x0a]);
const BLOCKS = [
  { name: "entry", offset: 0, size: 128 },
  { name: "owner", offset: 128, size: 256 },
  { name: "motion", offset: 384, size: 368 },
  { name: "damage", offset: 752, size: 172 },
  { name: "status", offset: 924, size: 164 },
];
const BLOCK = 1088;
const L_R_Z = 0x70;

type FighterFrame = { damage: number; stocks: number; status: number; attacker: number; anim: number; motion: number };
type Frame = { fighters: FighterFrame[]; buttons: number[] };

function chunk(type: string, raw: Buffer, deflate = true): Buffer {
  const stored = deflate ? deflateSync(raw) : raw;
  const h = Buffer.alloc(20);
  h.write(type, 0, "ascii");
  h.writeUInt32LE(deflate ? 1 : 0, 4);
  h.writeUInt32LE(stored.length, 8);
  h.writeUInt32LE(raw.length, 12);
  h.writeUInt32LE(crc32(stored), 16);
  return Buffer.concat([h, stored]);
}

const ENTRY_IDS = [0x10001, 0x10002];

function encode(frames: Frame[], recorderPort = 1): Buffer {
  const header = Buffer.alloc(32);
  MAGIC.copy(header, 0);
  header.writeUInt16LE(1, 8);
  header.writeUInt32LE(32, 12);
  const info = {
    players: [
      { port: 1, displayName: "Alice", connectCode: "ALIC#1", characterId: 2, costume: 0, teamId: null },
      { port: 2, displayName: "Bob", connectCode: "BOB#2", characterId: 22, costume: 1, teamId: null },
    ],
    recorder: { port: recorderPort },
    isTeams: false,
  };
  const parts = [
    header,
    chunk("INFO", Buffer.from(JSON.stringify(info)), false),
    chunk("FTSP", Buffer.from(JSON.stringify({ bytes: BLOCK, blocks: BLOCKS })), false),
  ];
  for (let first = 0; first < frames.length; first += 60) {
    const rows = frames.slice(first, first + 60);
    const n = rows.length;
    // FRMS: u32 first, u16 count, u8 ports, u8 0, pads per port, checksums, u16 parts.
    const frms = Buffer.alloc(8 + 2 * n * 0x40 + 4 * n + 2);
    frms.writeUInt32LE(first, 0);
    frms.writeUInt16LE(n, 4);
    frms[6] = 0x3;
    // FTRM: u32 first, u32 count, u8 ports, pad, u32 block, the rows per port.
    const ftrm = Buffer.alloc(16 + 2 * n * BLOCK);
    ftrm.writeUInt32LE(first, 0);
    ftrm.writeUInt32LE(n, 4);
    ftrm[8] = 0x3;
    ftrm.writeUInt32LE(BLOCK, 12);
    for (let p = 0; p < 2; p++) {
      let prevPad = Buffer.alloc(0x40);
      let prevRow = Buffer.alloc(BLOCK);
      rows.forEach((row, f) => {
        const pad = Buffer.alloc(0x40);
        pad.writeUInt32BE(row.buttons[p] >>> 0, 0);
        const r = Buffer.alloc(BLOCK);
        const s = row.fighters[p];
        r.writeUInt32BE(ENTRY_IDS[p], 0 + 0x04);
        r.writeFloatBE(s.damage, 128 + 0x24);
        r.writeInt32BE(s.stocks, 128 + 0x34);
        r.writeFloatBE(s.anim, 384 + 0x40);
        r.writeInt32BE(s.motion, 384 + 0x58);
        r.writeUInt32BE(s.attacker >>> 0, 752 + 0x84);
        r.writeInt32BE(s.status, 924 + 0x34);
        for (let k = 0; k < 0x40; k++) {
          frms[8 + p * n * 0x40 + f * 0x40 + k] = pad[k] ^ prevPad[k];
        }
        for (let k = 0; k < BLOCK; k++) {
          ftrm[16 + p * n * BLOCK + f * BLOCK + k] = r[k] ^ prevRow[k];
        }
        prevPad = pad;
        prevRow = r;
      });
    }
    parts.push(chunk("FRMS", frms), chunk("FTRM", ftrm));
  }
  return Buffer.concat(parts);
}

const idle = (damage = 0): FighterFrame => ({ damage, stocks: 3, status: 0, attacker: 0, anim: 0, motion: 0 });

/**
 * 600 frames: Alice (P1) hits Bob (P2) with `moves` (one motion kind each) every 20 frames from
 * frame 100, 10% each, Bob in hitstun until 30 frames after the last; Alice holds L+R+Z for
 * `hold` frames from frame 400.
 */
function game(moves: number[], hold = 0): Frame[] {
  const frames: Frame[] = [];
  let bobDamage = 0;
  const hitAt = moves.map((_, i) => 100 + i * 20);
  const lastHit = hitAt[hitAt.length - 1];
  for (let f = 0; f < 600; f++) {
    const k = hitAt.indexOf(f);
    if (k >= 0) {
      bobDamage += 10;
    }
    // Alice's move k runs from its hit - 5 to the next one (a new motion: a new move).
    const current = hitAt.findIndex(
      (h, i) => f >= h - 5 && (i + 1 === hitAt.length ? f < h + 10 : f < hitAt[i + 1] - 5),
    );
    const alice: FighterFrame =
      current >= 0
        ? { ...idle(), status: 0x24 + (current % 4), motion: moves[current], anim: f - hitAt[current] + 5 }
        : idle();
    const bob: FighterFrame = {
      ...idle(bobDamage),
      status: f >= 100 && f <= lastHit + 30 ? 0x45 : 0,
      attacker: ENTRY_IDS[0],
    };
    const buttons = [hold && f >= 400 && f < 400 + hold ? L_R_Z : 0, 0];
    frames.push({ fighters: [alice, bob], buttons });
  }
  return frames;
}

describe("replay highlights", () => {
  it("decodes the fighter fields and pads from XOR-delta chunks", () => {
    const t = decodeTimeline(encode(game([1, 2, 3, 4], 40)));
    expect(t.frames).toBe(600);
    expect(t.players.map((p) => [p.port, p.displayName, p.characterId, p.entryId])).toEqual([
      [1, "Alice", 2, ENTRY_IDS[0]],
      [2, "Bob", 22, ENTRY_IDS[1]],
    ]);
    expect(t.recorderPort).toBe(1);
    expect(t.damage[1][99]).toBe(0);
    expect(t.damage[1][100]).toBe(10);
    expect(t.damage[1][599]).toBe(40);
    expect(t.status[1][130]).toBe(0x45);
    expect(t.attacker[1][300]).toBe(ENTRY_IDS[0]);
    expect(t.motion[0][120]).toBe(2);
    expect(longestLrzHold(t, 0)).toBe(40);
    expect(longestLrzHold(t, 1)).toBe(0);
  });

  it("finds a string of four different moves as a combo", () => {
    const t = decodeTimeline(encode(game([1, 2, 3, 4])));
    const conv = findConversions(t).filter(isHighlight);
    expect(conv).toHaveLength(1);
    const [c] = conv;
    expect([c.attacker, c.victim, c.startFrame, c.moves, c.moveKinds, Math.round(c.damage)]).toEqual([
      0, 1, 100, 4, 4, 40,
    ]);
    expect(c.tags).toContain("combo");
    expect(c.kill).toBe(false);
  });

  it("keeps a string of one move repeated (a chain grab), scored below a varied one", () => {
    const varied = findConversions(decodeTimeline(encode(game([1, 2, 3, 4, 5])))).filter(isHighlight);
    const same = findConversions(decodeTimeline(encode(game([7, 7, 7, 7, 7])))).filter(isHighlight);
    expect(same).toHaveLength(1);
    expect(same[0].moves).toBe(5);
    expect(same[0].moveKinds).toBe(1);
    expect(same[0].score).toBeLessThan(varied[0].score);
    // Version 1's score did not tell them apart.
    const v1 = (moves: number[]) => findConversions(decodeTimeline(encode(game(moves))), { noVariety: true })[0].score;
    expect(v1([7, 7, 7, 7, 7])).toBeCloseTo(v1([1, 2, 3, 4, 5]));
  });

  it("marks an L+R+Z press as a clip of 20 s before and 2 s after", () => {
    const t = decodeTimeline(encode(game([1, 2, 3, 4], 40)));
    const clips = findLrzClips(t, [0, 1]);
    expect(clips).toHaveLength(1);
    const done = 400 + HOLD_FRAMES - 1;
    expect(clips[0]).toEqual({ player: 0, frame: done, start: 0, end: Math.min(done + CLIP_AFTER, 599) });
    expect(CLIP_BEFORE).toBe(1200);
    // A press of one frame is one too; no press, none.
    expect(findLrzClips(decodeTimeline(encode(game([1, 2, 3, 4], 1))), [0, 1])).toHaveLength(1);
    expect(findLrzClips(decodeTimeline(encode(game([1, 2, 3, 4], 0))), [0, 1])).toHaveLength(0);
  });

  it("counts only the recording player's L+R+Z by default", () => {
    const options = { highlights: true, clips: true, clipPlayers: "local" as const };
    expect(analyzeTimeline(decodeTimeline(encode(game([1, 2, 3, 4], 40), 1)), options).clips).toHaveLength(1);
    expect(analyzeTimeline(decodeTimeline(encode(game([1, 2, 3, 4], 40), 2)), options).clips).toHaveLength(0);
    const all = analyzeTimeline(decodeTimeline(encode(game([1, 2, 3, 4], 40), 2)), { ...options, clipPlayers: "all" });
    expect(all.clips.map((c) => c.port)).toEqual([1]);
    // (Replays > Highlights > My Highlights: the recorder's.)
    expect(all.recorderPort).toBe(2);
  });

  it("plays a highlight from 1.5 s before its first hit to 1 s after the string", async () => {
    const dir = await mkdtemp(path.join(os.tmpdir(), "highlights-"));
    try {
      const file = path.join(dir, "game.brep");
      await writeFile(file, encode(game([1, 2, 3, 4])));
      const r = await analyzeReplay(file, { highlights: true, clips: false, clipPlayers: "local" });
      expect(r.highlights).toHaveLength(1);
      const h = r.highlights[0];
      expect([h.attackerPort, h.victimPort, h.firstHitFrame, h.startFrame]).toEqual([1, 2, 100, 10]);
      // The string ends with Bob's hitstun (the last hit at 160, 30 frames of it), then 1 s.
      expect(h.endFrame).toBe(160 + 30 + 60);
      expect(r.players.map((p) => p.displayName)).toEqual(["Alice", "Bob"]);
      const off = await analyzeReplay(file, { highlights: false, clips: true, clipPlayers: "local" });
      expect(off.highlights).toHaveLength(0);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("refuses a file without fighter memory, and damaged chunks", () => {
    const frames = encode(game([1, 2, 3, 4]));
    const noFighters = Buffer.concat([frames.subarray(0, 32)]);
    expect(() => decodeTimeline(noFighters)).toThrow(ReplayFramesError);
    expect(() => decodeTimeline(Buffer.from("not a replay at all, not at all"))).toThrow(ReplayFramesError);
    // A flipped byte in a late chunk: the frames before it are still read.
    const damaged = Buffer.from(frames);
    damaged[damaged.length - 10] ^= 0xff;
    const t = decodeTimeline(damaged);
    expect(t.frames).toBe(540);
  });
});
