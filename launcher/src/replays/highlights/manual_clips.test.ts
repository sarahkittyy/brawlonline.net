import { crc32 } from "@replays/replay_format";
import { mkdir, mkdtemp, readdir, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { ManualClips, SPECTATED_FOLDER, spectatedName, wholeLength } from "./manual_clips";
import type { ManualClip } from "./types";

const MAGIC = Buffer.from([0x89, 0x42, 0x52, 0x50, 0x0d, 0x0a, 0x1a, 0x0a]);

function chunk(type: string, raw: Buffer): Buffer {
  const h = Buffer.alloc(20);
  h.write(type, 0, "ascii");
  h.writeUInt32LE(0, 4);
  h.writeUInt32LE(raw.length, 8);
  h.writeUInt32LE(raw.length, 12);
  h.writeUInt32LE(crc32(raw), 16);
  return Buffer.concat([h, raw]);
}

const INFO = {
  v: 1,
  startTime: "2026-10-12T18:03:12.345Z",
  mode: "ranked",
  matchId: "m",
  gameIndex: 2,
  isTeams: false,
  stageId: 2,
  players: [
    { port: 1, uid: "u1", displayName: "Alice", connectCode: "ALIC#1", characterId: 7, costume: 0, teamId: null },
    { port: 2, uid: "u2", displayName: "Bob", connectCode: "BOB#2", characterId: 0x13, costume: 1, teamId: null },
  ],
  rules: { stocks: 4, timeMinutes: 8 },
  recorder: { port: 2, role: "host" },
  build: { dolphin: "test", sim: 1, game: "RSBE01" },
  session: { startFrame: 120 },
};

/** A replay's parts: the header and chunks so far, and (`end`) its ENDG and trailer. */
function replayParts() {
  const header = Buffer.alloc(32);
  MAGIC.copy(header, 0);
  header.writeUInt16LE(1, 8);
  header.writeUInt32LE(32, 12);
  header.fill(0xab, 16, 32);
  const start = Buffer.concat([
    header,
    chunk("INFO", Buffer.from(JSON.stringify(INFO))),
    chunk("SETP", Buffer.from("{}")),
    chunk("FRMS", Buffer.alloc(300, 1)),
  ]);
  const endg = chunk("ENDG", Buffer.from(JSON.stringify({ endMethod: "game", firstFrame: 0, lastFrame: 5000 })));
  const t = Buffer.alloc(16);
  t.write("BRPT", 0, "ascii");
  t.writeUInt32LE(start.length, 4);
  t.writeUInt32LE(5000, 8);
  t.writeUInt32LE(crc32(t.subarray(0, 12)), 12);
  return { start, end: Buffer.concat([endg, t]) };
}

describe("wholeLength", () => {
  const { start, end } = replayParts();

  it("takes a finished replay whole", () => {
    const file = Buffer.concat([start, end]);
    expect(wholeLength(file)).toEqual({ length: file.length, complete: true });
  });

  it("cuts a replay still being written after its last whole chunk", () => {
    const more = chunk("FRMS", Buffer.alloc(200, 2));
    expect(wholeLength(start)).toEqual({ length: start.length, complete: false });
    expect(wholeLength(Buffer.concat([start, more.subarray(0, 50)]))).toEqual({
      length: start.length,
      complete: false,
    });
    expect(wholeLength(Buffer.concat([start, more.subarray(0, 10)]))).toEqual({
      length: start.length,
      complete: false,
    });
    expect(wholeLength(start.subarray(0, 20))).toEqual({ length: 0, complete: false });
  });
});

describe("spectatedName", () => {
  it("names the game as Dolphin names replays", () => {
    const at = new Date(2026, 9, 24, 15, 30);
    expect(
      spectatedName({ startTime: at.toISOString(), mode: "ranked", gameIndex: 2, players: INFO.players }, new Date()),
    ).toBe("2026-10-24 1530 Spectated Ranked G2 - Alice vs Bob");
    expect(
      spectatedName(
        { startTime: null, mode: "?", gameIndex: 0, players: [{ displayName: 'a/b:"c"', connectCode: "", port: 1 }] },
        at,
      ),
    ).toBe("2026-10-24 1530 Spectated Online G1 - abc");
  });
});

describe("ManualClips", () => {
  let root: string;
  let replays: string;
  let spectate: string;
  let stored: ManualClip[];
  let clips: ManualClips;

  beforeEach(async () => {
    root = await mkdtemp(path.join(os.tmpdir(), "manual-clips-"));
    replays = path.join(root, "Replays");
    spectate = path.join(root, "User", "Spectate");
    await mkdir(replays, { recursive: true });
    await mkdir(path.join(spectate, "s1"), { recursive: true });
    stored = [];
    clips = new ManualClips({
      clips: () => stored,
      save: async (next) => {
        stored = next;
      },
      spectateDir: () => spectate,
      replayDir: () => replays,
    });
  });

  afterEach(async () => {
    await rm(root, { recursive: true, force: true });
  });

  it("keeps each clip of a replay once, with its game's players", async () => {
    const { start, end } = replayParts();
    const file = path.join(replays, "2026-10-12 1403 Ranked G2 - Alice (Fox) vs Bob (Marth).brep");
    await writeFile(file, Buffer.concat([start, end]));
    const clip = { path: file, startFrame: 600, endFrame: 1800 };
    clips.onStatus({ savedClips: [clip] });
    clips.onStatus({ savedClips: [clip, { path: file, startFrame: 900, endFrame: 900 }] });
    await clips.idle();
    expect(stored).toHaveLength(1);
    expect(stored[0]).toMatchObject({
      path: file,
      startFrame: 600,
      endFrame: 1800,
      date: INFO.startTime,
      isTeams: false,
      recorderPort: 2,
    });
    expect(stored[0].spectated).toBeUndefined();
    expect(stored[0].players.map((p) => p.displayName)).toEqual(["Alice", "Bob"]);

    // Another launcher run's Dolphin saving it again: still one.
    const again = new ManualClips({
      clips: () => stored,
      save: async (next) => {
        stored = next;
      },
      spectateDir: () => spectate,
      replayDir: () => replays,
    });
    again.onStatus({ savedClips: [clip] });
    await again.idle();
    expect(stored).toHaveLength(1);
  });

  it("keeps a copy of a spectated game, whole chunks only, and copies it again once it ends", async () => {
    const { start, end } = replayParts();
    const source = path.join(spectate, "s1", "1.brep");
    const half = chunk("FRMS", Buffer.alloc(100, 3)).subarray(0, 30);
    await writeFile(source, Buffer.concat([start, half]));
    clips.onStatus({ savedClips: [{ path: source, startFrame: 100, endFrame: 400 }] });
    await clips.idle();
    const kept = await readdir(path.join(replays, SPECTATED_FOLDER));
    expect(kept).toHaveLength(1);
    expect(kept[0]).toMatch(/^\d{4}-\d{2}-\d{2} \d{4} Spectated Ranked G2 - Alice vs Bob\.brep$/);
    const dest = path.join(replays, SPECTATED_FOLDER, kept[0]);
    expect(await readFile(dest)).toEqual(start);
    expect(stored).toHaveLength(1);
    expect(stored[0]).toMatchObject({
      path: dest,
      startFrame: 100,
      endFrame: 400,
      spectated: true,
      recorderPort: null,
    });

    // The game ended: the next clip of it finds the copy whole.
    await writeFile(source, Buffer.concat([start, end]));
    clips.onStatus({ savedClips: [{ path: source, startFrame: 500, endFrame: 700 }] });
    await clips.idle();
    expect(await readFile(dest)).toEqual(Buffer.concat([start, end]));
    expect(await readdir(path.join(replays, SPECTATED_FOLDER))).toHaveLength(1);
    expect(stored.map((c) => c.path)).toEqual([dest, dest]);
  });
});
