import type { ReplayCommunication, ReplayPlaybackStatus } from "@dolphin/types";
import { crc32 } from "@replays/replay_format";
import { mkdtemp, readFile, rm } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { deflateSync } from "zlib";

import { frmsLastFrame, LiveGameFile } from "./live_game_file";
import { pickAutoPlay, SpectateClient, startPlace } from "./spectate_client";
import type { LiveGameEntry, LiveList } from "./types";

const ME = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const STREAM = "3fa9c0d1e2b4";
const UID = "00112233445566778899aabbccddeeff";

/** A chunk as the replay writer stores it (ReplayFile EncodeChunk). */
function chunk(type: string, raw: Buffer, deflate = false): Buffer {
  const stored = deflate ? deflateSync(raw) : raw;
  const h = Buffer.alloc(20);
  h.write(type, 0, "latin1");
  h.writeUInt32LE(deflate ? 1 : 0, 4);
  h.writeUInt32LE(stored.length, 8);
  h.writeUInt32LE(raw.length, 12);
  h.writeUInt32LE(crc32(stored), 16);
  return Buffer.concat([h, stored]);
}

/** An FRMS chunk's raw bytes: first frame and count (the rest doesn't matter here). */
function frms(first: number, count: number): Buffer {
  const raw = Buffer.alloc(16);
  raw.writeUInt32LE(first, 0);
  raw.writeUInt16LE(count, 4);
  return chunk("FRMS", raw, true);
}

function entry(id: string, startedAt: string, patch: Partial<LiveGameEntry> = {}): LiveGameEntry {
  return {
    id,
    mode: "direct",
    room: null,
    isTeams: false,
    startedAt,
    spectators: 0,
    game: { number: 1, place: 1, startedAt, stageId: 3, state: "playing" },
    players: [
      { uid: "p1", displayName: "ann", connectCode: "ANN#1", characterId: 2, costume: 0, teamId: null, wins: 0 },
      { uid: "p2", displayName: "bob", connectCode: "BOB#2", characterId: 9, costume: 0, teamId: null, wins: 0 },
    ],
    ...patch,
  };
}

describe("LiveGameFile", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "spectate-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("writes the header, the chunks as relayed and the trailer: a whole replay", async () => {
    const p = path.join(dir, "1.brep");
    const f = await LiveGameFile.create(p, UID);
    const info = chunk("INFO", Buffer.from("{}"));
    const a = frms(600, 60);
    const b = frms(660, 23);
    const endg = chunk("ENDG", Buffer.from('{"endMethod":"game"}'));
    for (const c of [info, a, b, endg]) {
      await f.append(c);
    }
    await f.end();
    const bytes = await readFile(p);
    expect(bytes.subarray(0, 8)).toEqual(Buffer.from([0x89, 0x42, 0x52, 0x50, 0x0d, 0x0a, 0x1a, 0x0a]));
    expect(bytes.readUInt16LE(8)).toBe(1);
    expect(bytes.readUInt32LE(12)).toBe(32);
    expect(bytes.subarray(16, 32).toString("hex")).toBe(UID);
    expect(bytes.subarray(32, bytes.length - 16)).toEqual(Buffer.concat([info, a, b, endg]));
    const t = bytes.subarray(bytes.length - 16);
    expect(t.toString("latin1", 0, 4)).toBe("BRPT");
    expect(t.readUInt32LE(4)).toBe(32 + info.length + a.length + b.length); // where ENDG starts
    expect(t.readUInt32LE(8)).toBe(682); // the last frame
    expect(t.readUInt32LE(12)).toBe(crc32(t.subarray(0, 12)));
  });

  it("has no trailer when the game didn't end, and refuses what isn't a chunk", async () => {
    const p = path.join(dir, "2.brep");
    const f = await LiveGameFile.create(p, UID);
    await f.append(frms(600, 60));
    await expect(f.append(Buffer.from("not a chunk at all, really"))).rejects.toThrow();
    await f.close();
    expect((await readFile(p)).length).toBe(32 + frms(600, 60).length);
    await expect(LiveGameFile.create(path.join(dir, "x.brep"), "../../etc")).rejects.toThrow();
  });

  it("reads an FRMS chunk's last frame, and nothing from a broken one", () => {
    expect(frmsLastFrame(frms(1200, 60))).toBe(1259);
    const broken = frms(1200, 60);
    broken.fill(0, 20);
    expect(frmsLastFrame(broken)).toBeNull();
  });
});

describe("choosing what to watch", () => {
  it("starts at the game being played, or at the next one between games", () => {
    expect(startPlace(entry(STREAM, "2026-10-14T01:00:00Z"))).toBe(1);
    const between = entry(STREAM, "2026-10-14T01:00:00Z");
    between.game = { ...between.game, place: 2, state: "between" };
    expect(startPlace(between)).toBe(3);
    const old = entry(STREAM, "2026-10-14T01:00:00Z");
    old.game = { ...old.game, place: undefined };
    expect(startPlace(old)).toBeNull();
    // An ended stream is watched from its first game.
    const ended = entry(STREAM, "2026-10-14T01:00:00Z");
    ended.game = { ...ended.game, place: 3, state: "over" };
    expect(startPlace(ended)).toBeNull();
  });

  it("Auto play takes the newest live game, leaving out the one watched, the player's own and ended ones", () => {
    const list: LiveList = {
      games: [
        entry("aaaaaaaaaaaa", "2026-10-14T01:00:00Z"),
        entry("bbbbbbbbbbbb", "2026-10-14T01:05:00Z"),
        entry("cccccccccccc", "2026-10-14T01:09:00Z", {
          players: [
            { uid: ME, displayName: "me", connectCode: "ME#1", characterId: 1, costume: 0, teamId: null, wins: 0 },
          ],
        }),
        entry("dddddddddddd", "2026-10-14T01:08:00Z"),
      ],
    };
    list.games[3].game = { ...list.games[3].game, state: "over" };
    expect(pickAutoPlay(list, null, ME)?.id).toBe("bbbbbbbbbbbb");
    expect(pickAutoPlay(list, "bbbbbbbbbbbb", ME)?.id).toBe("aaaaaaaaaaaa");
    expect(pickAutoPlay({ games: [list.games[2]] }, null, ME)).toBeNull();
    // A game being played comes before a newer one waiting for its set's next game.
    const waiting = entry("eeeeeeeeeeee", "2026-10-14T01:20:00Z");
    waiting.game = { ...waiting.game, state: "between" };
    expect(pickAutoPlay({ games: [...list.games, waiting] }, null, ME)?.id).toBe("bbbbbbbbbbbb");
    expect(pickAutoPlay({ games: [list.games[0], waiting] }, "aaaaaaaaaaaa", ME)?.id).toBe("eeeeeeeeeeee");
  });
});

/** A fake accounts server: `/v1/live` and a watch stream whose events the test pushes. */
class FakeServer {
  list: LiveList = { games: [entry(STREAM, "2026-10-14T01:00:00Z")] };
  watchUrls: string[] = [];
  private push?: (ev: unknown) => void;
  private close?: () => void;

  fetch: typeof fetch = (async (url: string) => {
    if (url.endsWith("/v1/live")) {
      return new Response(JSON.stringify(this.list), { status: 200 });
    }
    this.watchUrls.push(url);
    const stream = new ReadableStream<Uint8Array>({
      start: (ctl) => {
        const enc = new TextEncoder();
        this.push = (ev) => ctl.enqueue(enc.encode(`data: ${JSON.stringify(ev)}\n\n`));
        this.close = () => ctl.close();
      },
    });
    return new Response(stream, { status: 200 });
  }) as unknown as typeof fetch;

  send(ev: unknown) {
    this.push!(ev);
  }

  end() {
    this.close!();
  }
}

/** Until the client has opened `n` watch streams. */
const connected = (server: FakeServer, n: number) =>
  vi.waitFor(() => expect(server.watchUrls.length).toBe(n), { timeout: 3000 });

describe("SpectateClient", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "spectate-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  function make(
    server: FakeServer,
    opts: { closeOnEnd?: boolean; play?: (comm: ReplayCommunication) => Promise<void> } = {},
  ) {
    const comms: ReplayCommunication[] = [];
    let status: ReplayPlaybackStatus | null = null;
    let open = false;
    let closed = 0;
    const client = new SpectateClient({
      baseUrl: "http://accounts",
      userAgent: "test",
      getToken: async () => "token",
      spectateDir: () => dir,
      play: async (comm) => {
        comms.push(JSON.parse(JSON.stringify(comm)));
        await opts.play?.(comm);
        open = true;
      },
      playbackStatus: async () => status,
      isPlaybackOpen: () => open,
      closePlayback: async () => {
        closed++;
        open = false;
      },
      closeOnEnd: () => opts.closeOnEnd ?? true,
      fetch: server.fetch,
      sleep: async () => undefined,
    });
    return {
      client,
      comms,
      setStatus: (s: ReplayPlaybackStatus) => (status = s),
      closed: () => closed,
    };
  }

  it("writes the games as they come and keeps the playback Dolphin's mirror request up to date", async () => {
    const server = new FakeServer();
    const { client, comms } = make(server);
    const watching = client.watch(ME, STREAM);
    await connected(server, 1);
    server.send({ type: "welcome", stream: { ...server.list.games[0], spectators: 3 }, you: ME, chat: [] });
    expect(await watching).toEqual({ ok: true });
    expect(server.watchUrls[0]).toBe(`http://accounts/v1/live/${STREAM}/watch?game=1&chunk=0`);
    expect(client.getState()).toMatchObject({ status: "watching", stream: STREAM, spectators: 3 });

    server.send({ type: "game", game: 1, number: 1, uid: UID });
    server.send({ type: "chunk", game: 1, i: 0, b: chunk("INFO", Buffer.from("{}")).toString("base64") });
    server.send({ type: "chunk", game: 1, i: 1, b: frms(600, 60).toString("base64") });
    server.send({ type: "spectators", count: 4 });
    const first = path.join(dir, STREAM, "1.brep");
    await vi.waitFor(() => {
      expect(comms.at(-1)).toMatchObject({
        mode: "mirror",
        queue: [{ path: first }],
        streamOver: false,
        closeOnEnd: true,
      });
      expect(client.getState().spectators).toBe(4);
    });

    // The set's next game is added under the same request; the first one ended whole.
    server.send({ type: "chunk", game: 1, i: 2, b: chunk("ENDG", Buffer.from("{}")).toString("base64") });
    server.send({ type: "end", game: 1 });
    server.send({ type: "game", game: 2, number: 2, uid: UID });
    const second = path.join(dir, STREAM, "2.brep");
    await vi.waitFor(() => expect(comms.at(-1)!.queue).toEqual([{ path: first }, { path: second }]));
    const id = comms.at(-1)!.commandId;
    expect((await readFile(first)).subarray(-16).toString("latin1", 0, 4)).toBe("BRPT");

    // An uploader that stopped: the game is cut, the stream goes on; then the stream is over.
    server.send({ type: "bad", game: 2, error: "The rest of this game wasn't streamed." });
    await vi.waitFor(() => expect(comms.at(-1)!.queue).toEqual([{ path: first }, { path: second, cut: true }]));
    server.send({ type: "over" });
    await vi.waitFor(() => expect(comms.at(-1)).toMatchObject({ commandId: id, streamOver: true }));
    expect(client.getState().status).toBe("over");
  });

  it("reconnects from the chunk after the newest one", async () => {
    const server = new FakeServer();
    const { client } = make(server);
    const watching = client.watch(ME, STREAM);
    await connected(server, 1);
    server.send({ type: "welcome", stream: server.list.games[0], you: ME, chat: [] });
    await watching;
    server.send({ type: "game", game: 1, number: 1, uid: UID });
    server.send({ type: "chunk", game: 1, i: 0, b: chunk("INFO", Buffer.from("{}")).toString("base64") });
    server.send({ type: "chunk", game: 1, i: 1, b: frms(600, 60).toString("base64") });
    server.end();
    await connected(server, 2);
    expect(server.watchUrls.at(-1)).toBe(`http://accounts/v1/live/${STREAM}/watch?game=1&chunk=2`);
    client.stop();
  });

  it("refuses a game the player plays in, and one that has ended", async () => {
    const server = new FakeServer();
    const { client } = make(server);
    server.list.games[0].players[0].uid = ME;
    expect(await client.watch(ME, STREAM)).toEqual({ ok: false, message: "You're playing in this game." });
    expect(await client.watch(ME, "000000000000")).toEqual({ ok: false, message: "This game has ended." });
    expect(server.watchUrls).toEqual([]);
  });

  it("closes the playback Dolphin when it asks at the stream's end; Auto play turns that off", async () => {
    const server = new FakeServer();
    const { client, comms, setStatus, closed } = make(server);
    const watching = client.watch(ME, STREAM);
    await connected(server, 1);
    server.send({ type: "welcome", stream: server.list.games[0], you: ME, chat: [] });
    await watching;
    await vi.waitFor(() => expect(comms.length).toBeGreaterThan(0));
    client.setAutoPlay(true);
    await vi.waitFor(() => expect(comms.at(-1)!.closeOnEnd).toBe(false));
    client.setAutoPlay(false);
    await vi.waitFor(() => expect(comms.at(-1)!.closeOnEnd).toBe(true));
    setStatus({ commandId: comms.at(-1)!.commandId, state: "close" });
    await vi.waitFor(() => expect(closed()).toBe(1), { timeout: 3000 });
    expect(client.getState().status).toBe("idle");
  });

  it("says why the playback Dolphin can't open, and tries no more (bug report #19)", async () => {
    const server = new FakeServer();
    const why = "Another Dolphin is using the game files. Close it, then try again.";
    const { client, comms } = make(server, {
      play: async () => {
        throw new Error(why);
      },
    });
    const watching = client.watch(ME, STREAM);
    await connected(server, 1);
    server.send({ type: "welcome", stream: server.list.games[0], you: ME, chat: [] });
    expect(await watching).toEqual({ ok: false, message: why });
    expect(client.getState()).toMatchObject({ status: "error", message: why });
    expect(comms).toHaveLength(1);
  });

  it("a watch another click replaced never opens or switches the playback Dolphin (bug report #19)", async () => {
    const server = new FakeServer();
    const OTHER = "bbbbbbbbbbbb";
    server.list.games.push(entry(OTHER, "2026-10-14T01:05:00Z"));
    let release!: () => void;
    const firstOpen = new Promise<void>((resolve) => (release = resolve));
    const { client, comms } = make(server, { play: () => (comms.length === 1 ? firstOpen : Promise.resolve()) });

    // The first game's Dolphin is still opening (copying the SD card) when its game event comes.
    const first = client.watch(ME, STREAM);
    await connected(server, 1);
    server.send({ type: "welcome", stream: server.list.games[0], you: ME, chat: [] });
    await vi.waitFor(() => expect(comms).toHaveLength(1));
    server.send({ type: "game", game: 2, number: 2, uid: UID });

    // Another game clicked meanwhile: only it may play once the first start is done.
    const second = client.watch(ME, OTHER);
    await connected(server, 2);
    server.send({ type: "welcome", stream: server.list.games[1], you: ME, chat: [] });
    expect(await second).toEqual({ ok: true });
    release();
    expect(await first).toEqual({ ok: true });
    await new Promise((resolve) => setTimeout(resolve, 50));
    expect(comms.map((c) => c.commandId!.split("-")[1])).toEqual([STREAM, OTHER]);
    expect(client.getState()).toMatchObject({ status: "watching", stream: OTHER });
    client.stop();
  });
});
