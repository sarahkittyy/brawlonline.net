import { mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "os";
import path from "path";
import { describe, expect, it } from "vitest";
import { deflateSync } from "zlib";

import {
  crc32,
  isReplayFileName,
  parseReplayHeader,
  readReplayFileInfo,
  readReplayHeader,
  ReplayFormatError,
  toSlippiShapes,
} from "./replay_format";

// A replay as Dolphin writes it (ReplayFile.cpp): header, chunks, trailer.
const MAGIC = Buffer.from([0x89, 0x42, 0x52, 0x50, 0x0d, 0x0a, 0x1a, 0x0a]);

function chunk(type: string, raw: Buffer, deflate = false): Buffer {
  const stored = deflate ? deflateSync(raw) : raw;
  const h = Buffer.alloc(20);
  h.write(type, 0, "ascii");
  h.writeUInt32LE(deflate ? 1 : 0, 4);
  h.writeUInt32LE(stored.length, 8);
  h.writeUInt32LE(raw.length, 12);
  h.writeUInt32LE(crc32(stored), 16);
  return Buffer.concat([h, stored]);
}

const INFO = {
  v: 1,
  startTime: "2026-10-12T18:03:12.345Z",
  mode: "ranked",
  matchId: "mode.ranked-2026-10-12T18:02:55.120Z-1a",
  gameIndex: 2,
  isTeams: false,
  stageId: 2,
  players: [
    { port: 2, uid: "u2", displayName: "Bob", connectCode: "BOB#2", characterId: 0x13, costume: 1, teamId: null },
    { port: 1, uid: "u1", displayName: "Alice‮", connectCode: "ALIC#1", characterId: 7, costume: 0, teamId: null },
  ],
  rules: { stocks: 4, timeMinutes: 8 },
  recorder: { port: 1, role: "host" },
  build: { dolphin: "test", sim: 1, game: "RSBE01" },
  session: { startFrame: 120 },
};
const ENDG = {
  endMethod: "game",
  lastFrame: 5000,
  finalFrame: 4992,
  winnerPort: 1,
  players: [
    { port: 1, stocks: 2, damage: 40.5, isWinner: true },
    { port: 2, stocks: 0, damage: 0, isWinner: false },
  ],
};

function replay(opts: { endg?: boolean; info?: unknown } = {}): Buffer {
  const header = Buffer.alloc(32);
  MAGIC.copy(header, 0);
  header.writeUInt16LE(1, 8);
  header.writeUInt16LE(0, 10);
  header.writeUInt32LE(32, 12);
  header.fill(0xab, 16, 32);
  const parts = [
    header,
    chunk("INFO", Buffer.from(JSON.stringify(opts.info ?? INFO))),
    chunk("SETP", Buffer.from("{}")),
    chunk("FRMS", Buffer.alloc(300, 1), true),
  ];
  if (opts.endg !== false) {
    const offset = parts.reduce((n, p) => n + p.length, 0);
    parts.push(chunk("ENDG", Buffer.from(JSON.stringify(ENDG))));
    const t = Buffer.alloc(16);
    t.write("BRPT", 0, "ascii");
    t.writeUInt32LE(offset, 4);
    t.writeUInt32LE(5000, 8);
    t.writeUInt32LE(crc32(t.subarray(0, 12)), 12);
    parts.push(t);
  }
  return Buffer.concat(parts);
}

const parse = (b: Buffer) => parseReplayHeader(b, b, b.length);

describe("replay format", () => {
  it("lists our .brep replays only", () => {
    expect(isReplayFileName("2026-10-12 1403 Ranked G2 - Alice (Fox) vs Bob (Marth).brep")).toBe(true);
    expect(isReplayFileName("a.BREP")).toBe(true);
    expect(isReplayFileName("Game_20231030T2041.slp")).toBe(false);
    expect(isReplayFileName("old.rep")).toBe(false);
  });

  it("reads the metadata and the result", () => {
    const h = parse(replay());
    expect(h.complete).toBe(true);
    expect(h.uid).toBe("ab".repeat(16));
    expect(h.meta.mode).toBe("ranked");
    expect(h.meta.players.map((p) => p.port)).toEqual([1, 2]);
    // Text from another player's machine: control and bidi characters are taken out.
    expect(h.meta.players[0].displayName).toBe("Alice");
    expect(h.end?.winnerPort).toBe(1);
    expect(h.end?.endMethod).toBe("game");
    const s = toSlippiShapes(h);
    expect(s.winnerIndices).toEqual([0]);
    expect(s.settings?.matchInfo?.sessionId).toBe(INFO.matchId);
    expect(s.settings?.matchInfo?.gameNumber).toBe(2);
    expect(s.settings?.players.map((p) => p.characterId)).toEqual([7, 0x13]);
    // Slippi's frame numbers (the countdown from -123): the last frame of the game's clock.
    expect(s.metadata?.lastFrame).toBe(5000 + 120 - 123);
  });

  it("reads a game in progress or cut short by a crash: metadata, no result", () => {
    const b = replay({ endg: false });
    const h = parse(b);
    expect(h.complete).toBe(false);
    expect(h.end).toBeNull();
    expect(toSlippiShapes(h).winnerIndices).toEqual([]);
    // Cut in the middle of the frames: still the same.
    expect(parse(b.subarray(0, b.length - 5)).meta.matchId).toBe(INFO.matchId);
  });

  it("ignores a trailer that does not check out", () => {
    const b = replay();
    b[b.length - 1] ^= 0xff;
    const h = parse(b);
    expect(h.complete).toBe(false);
    expect(h.end).toBeNull();
  });

  it("refuses what is not a replay or is damaged", () => {
    expect(() => parse(Buffer.from("not a replay at all, not even close..."))).toThrow(ReplayFormatError);
    const newer = replay();
    newer.writeUInt16LE(2, 8);
    expect(() => parse(newer)).toThrow(/newer/);
    const damaged = replay();
    damaged[60] ^= 0xff; // inside the INFO chunk
    expect(() => parse(damaged)).toThrow(ReplayFormatError);
    const notJson = replay({ info: "x" });
    expect(() => parse(notJson)).toThrow(ReplayFormatError);
  });

  it("bounds what a hostile file can make it do", () => {
    // An INFO chunk that claims more than the limit, or a deflate bomb for ENDG: refused or
    // ignored, never inflated past the limit.
    const big = replay();
    big.writeUInt32LE(10 * 1024 * 1024, 32 + 12);
    expect(() => parse(big)).toThrow(ReplayFormatError);
    const bomb = Buffer.concat([replay({ endg: false }), chunk("ENDG", Buffer.alloc(1 << 20, 0x20), true)]);
    expect(parse(bomb).end).toBeNull();
    // Absurd values in the metadata are dropped, not trusted.
    const odd = parse(
      replay({ info: { ...INFO, gameIndex: 1e30, players: [{ port: 9 }, { port: 1, characterId: -5 }] } }),
    );
    expect(odd.meta.gameIndex).toBe(1);
    expect(odd.meta.players).toEqual([
      { port: 1, uid: "", displayName: "", connectCode: "", characterId: 0xff, costume: 0, teamId: null },
    ]);
  });

  it("reads a file from the disk, and lists a damaged one by name", async () => {
    const dir = await mkdtemp(path.join(os.tmpdir(), "ppo-brep-"));
    try {
      await writeFile(path.join(dir, "a.brep"), replay());
      await writeFile(path.join(dir, "b.brep"), Buffer.alloc(42));
      expect((await readReplayHeader(path.join(dir, "a.brep"))).complete).toBe(true);
      const a = await readReplayFileInfo(dir, "a.brep");
      expect(a.settings?.players).toHaveLength(2);
      const b = await readReplayFileInfo(dir, "b.brep");
      expect(b.sizeBytes).toBe(42);
      expect(b.settings).toBeUndefined();
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });
});
