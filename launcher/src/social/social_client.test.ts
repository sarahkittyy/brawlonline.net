import { mkdtemp, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import {
  boxAd,
  dmGroup,
  encodePlaintext,
  fromHex,
  identityPublic,
  kxPublic,
  open,
  pairKey,
  parsePlaintext,
  randomNonce,
  randomSecret,
  seal,
  sessionGroup,
  signKx,
  signMessage,
  toHex,
  verifyMessage,
} from "./crypto";
import {
  HIDDEN_FILE,
  IDENTITY_FILE,
  loadIdentitySeed,
  readHidden,
  readHiddenFile,
  syncedHiddenFile,
  writeHidden,
} from "./shared_files";
import { SocialClient } from "./social_client";
import type { SocialEvent } from "./types";

const ME = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const BOB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
const CAROL = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
const DAVE = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";

/** A player on the other end, with real keys. */
function fakePeer(uid: string, name: string, code: string) {
  const seed = randomSecret();
  const kxSecret = randomSecret();
  const kx = kxPublic(kxSecret);
  return {
    uid,
    seed,
    kxSecret,
    seq: 0n,
    member: {
      uid,
      displayName: name,
      connectCode: code,
      idKey: toHex(identityPublic(seed)),
      kx: toHex(kx),
      kxSig: toHex(signKx(seed, sessionGroup(uid), kx)),
    },
  };
}
type FakePeer = ReturnType<typeof fakePeer>;

/** A fake accounts server: the stream is pushed by the test, POSTs are recorded. */
function fakeServer() {
  let push: ((s: string) => void) | undefined;
  let close: (() => void) | undefined;
  const posts: { path: string; body: any }[] = [];
  /** Every request but the stream, in order. */
  const calls: { method: string; path: string; body: any }[] = [];
  /** Answers by `"<METHOD> <path>"`; GET, PUT and DELETE without one are 404. */
  const routes: Record<string, (body: any) => unknown> = {};
  let streamHeaders: Record<string, string> = {};
  let postStatus = 204;
  const fetchFn = (async (url: string, init?: RequestInit) => {
    const u = new URL(url);
    if (u.pathname === "/v1/social/stream") {
      streamHeaders = init?.headers as Record<string, string>;
      const enc = new TextEncoder();
      const body = new ReadableStream<Uint8Array>({
        start(c) {
          push = (s) => c.enqueue(enc.encode(s));
          close = () => c.close();
          init?.signal?.addEventListener("abort", () => {
            try {
              c.error(new Error("aborted"));
            } catch {
              // closed already
            }
          });
        },
      });
      return new Response(body, { status: 200, headers: { "content-type": "text/event-stream" } });
    }
    const method = init?.method ?? "GET";
    const body = init?.body !== undefined ? JSON.parse(String(init.body)) : undefined;
    calls.push({ method, path: u.pathname, body });
    const route = routes[`${method} ${u.pathname}`];
    if (route) {
      return new Response(JSON.stringify(route(body)), { status: 200 });
    }
    if (method !== "POST") {
      return new Response(JSON.stringify({ error: { code: "not_found", message: "Not found" } }), { status: 404 });
    }
    posts.push({ path: u.pathname, body });
    if (postStatus >= 400) {
      return new Response(JSON.stringify({ error: { code: "x", message: "That player is offline." } }), {
        status: postStatus,
      });
    }
    if (u.pathname === "/v1/social/invite") {
      return new Response(JSON.stringify({ kind: "direct" }), { status: 200 });
    }
    return new Response(null, { status: 204 });
  }) as unknown as typeof fetch;
  return {
    fetchFn,
    posts,
    calls,
    routes,
    send: (ev: unknown) => push!(`data: ${JSON.stringify(ev)}\n\n`),
    raw: (s: string) => push!(s),
    close: () => close?.(),
    myKx: () => fromHex(streamHeaders["X-Chat-Kx"], 32)!,
    myIdKey: () => fromHex(streamHeaders["X-Chat-Key"], 32)!,
    setPostStatus: (s: number) => {
      postStatus = s;
    },
  };
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 15));

async function until(pred: () => boolean) {
  for (let i = 0; i < 200 && !pred(); i++) {
    await tick();
  }
  expect(pred()).toBe(true);
}

describe("social client", () => {
  let dir: string;
  let server: ReturnType<typeof fakeServer>;
  let client: SocialClient;
  let events: SocialEvent[];
  let bob: FakePeer;

  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "social-"));
    server = fakeServer();
    client = new SocialClient({
      baseUrl: "http://accounts",
      userAgent: "test",
      getToken: async () => "token",
      onlineDir: () => dir,
      fetch: server.fetchFn,
      sleep: async () => undefined,
    });
    events = [];
    client.onEvent((e) => events.push(e));
    bob = fakePeer(BOB, "bob", "BO#7");
    await client.connect(ME);
    await until(() => client.getState().status === "connecting");
    await tick();
    server.send({ type: "welcome", you: ME, online: [bob.member], room: null });
    await until(() => client.getState().status === "online");
  });

  afterEach(async () => {
    client.disconnect();
    await rm(dir, { recursive: true, force: true });
  });

  /** A DM from `peer`, sealed to the client's session key. */
  function dmFrom(peer: FakePeer, text: string, opts: { seq?: bigint; tamper?: boolean } = {}) {
    const group = dmGroup(ME, peer.uid);
    const seq = opts.seq ?? ++peer.seq;
    const bytes = new TextEncoder().encode(text);
    const sig = signMessage(peer.seed, group, peer.uid, seq, bytes);
    const key = pairKey(group, peer.kxSecret, server.myKx())!;
    const sealed = seal(key, randomNonce(), boxAd(group, peer.uid, ME), encodePlaintext(seq, sig, bytes));
    if (opts.tamper) {
      sealed[sealed.length - 1] ^= 1;
    }
    return { type: "dm", from: peer.uid, kx: peer.member.kx, box: toHex(sealed) };
  }

  it("lists players and signs its session key with the identity key it stored", async () => {
    const state = client.getState();
    expect(state.online).toEqual([{ uid: BOB, displayName: "bob", connectCode: "BO#7" }]);
    const seed = fromHex((await readFile(path.join(dir, IDENTITY_FILE), "utf8")).trim(), 32)!;
    expect(toHex(identityPublic(seed))).toBe(toHex(server.myIdKey()));
    const carol = fakePeer(CAROL, "carol", "CA#1");
    server.send({ type: "online", member: carol.member });
    await until(() => client.getState().online.length === 2);
    expect(
      client
        .getState()
        .online.map((p) => p.displayName)
        .sort(),
    ).toEqual(["bob", "carol"]);
    server.send({ type: "offline", uid: BOB });
    await until(() => client.getState().online.length === 1);
    // Garbage is skipped.
    server.send({ type: "online", member: { ...carol.member, uid: "nope" } });
    server.send({ type: "online", member: { ...carol.member, uid: BOB, displayName: "\u200b" } });
    server.raw("data: {not json\n\n");
    server.send({ type: "room", room: { code: "KAQB" } });
    await tick();
    expect(client.getState().online.map((p) => p.uid)).toEqual([CAROL]);
    expect(client.getState().room).toBeNull();
    server.send({
      type: "room",
      room: { code: "KFQB", players: 1, openSlots: 2, mode: "1v1", status: "waiting", joinable: true },
    });
    await until(() => client.getState().room?.code === "KFQB");
  });

  it("keeps a player's flag and main when they are known ones, and drops anything else", async () => {
    const carol = fakePeer(CAROL, "carol", "CA#1");
    server.send({ type: "online", member: { ...carol.member, country: "gb-eng", main: "random" } });
    await until(() => client.getState().online.some((p) => p.uid === CAROL));
    expect(client.getState().online.find((p) => p.uid === CAROL)).toEqual({
      uid: CAROL,
      displayName: "carol",
      connectCode: "CA#1",
      country: "gb-eng",
      main: "random",
    });
    // Listed again (a profile change): an unknown flag and character are left out.
    server.send({ type: "online", member: { ...carol.member, country: "zz", main: "<img>" } });
    await until(() => client.getState().online.find((p) => p.uid === CAROL)?.country === undefined);
    expect(client.getState().online.find((p) => p.uid === CAROL)?.main).toBeUndefined();
  });

  it("lists players in a random order that holds until they leave", async () => {
    const carol = fakePeer(CAROL, "carol", "CA#1");
    const dave = fakePeer(DAVE, "_dave", "DA#2");
    const draws = [0.9, 0.5, 0.2];
    const random = vi.spyOn(Math, "random").mockImplementation(() => draws.shift() ?? 0.99);
    try {
      server.send({ type: "online", member: carol.member });
      await until(() => client.getState().online.length === 2);
      server.send({ type: "online", member: dave.member });
      await until(() => client.getState().online.length === 3);
      // Bob's place was drawn at the welcome; carol 0.9, dave 0.5.
      const names = () =>
        client
          .getState()
          .online.map((p) => p.displayName)
          .filter((n) => n !== "bob");
      expect(names()).toEqual(["_dave", "carol"]);
      // A renamed player keeps their place.
      server.send({ type: "online", member: { ...carol.member, displayName: "_carol" } });
      await until(() => names().includes("_carol"));
      expect(names()).toEqual(["_dave", "_carol"]);
      // One who leaves and comes back draws a new one (0.2).
      server.send({ type: "offline", uid: CAROL });
      await until(() => client.getState().online.length === 2);
      server.send({ type: "online", member: carol.member });
      await until(() => client.getState().online.length === 3);
      expect(names()).toEqual(["carol", "_dave"]);
    } finally {
      random.mockRestore();
    }
  });

  it("opens DMs and refuses replays, tampering and other keys", async () => {
    server.send(dmFrom(bob, "hi \u202ethere"));
    await until(() => (client.getState().conversations[BOB]?.lines.length ?? 0) === 1);
    const conv = client.getState().conversations[BOB];
    expect(conv.lines[0]).toMatchObject({ own: false, text: "hi there" });
    expect(conv.unread).toBe(1);
    expect(events).toEqual([{ type: "message", uid: BOB }]);

    server.send(dmFrom(bob, "again", { seq: 1n }));
    server.send(dmFrom(bob, "bad", { tamper: true }));
    server.send({ ...dmFrom(bob, "wrong key"), kx: "11".repeat(32) });
    const carol = fakePeer(CAROL, "carol", "CA#1");
    server.send(dmFrom(carol, "not listed"));
    server.send(dmFrom(bob, "ok"));
    await until(() => client.getState().conversations[BOB].lines.length === 2);
    expect(client.getState().conversations[BOB].lines.map((l) => l.text)).toEqual(["hi there", "ok"]);
    client.markRead(BOB);
    await tick();
    expect(client.getState().conversations[BOB].unread).toBe(0);
  });

  it("sends DMs the other player can open and verify", async () => {
    expect(await client.sendMessage(BOB, "  hello  bob ")).toEqual({ ok: true });
    const post = server.posts.find((p) => p.path === "/v1/social/dm")!;
    expect(post.body.to).toBe(BOB);
    const group = dmGroup(ME, BOB);
    const key = pairKey(group, bob.kxSecret, server.myKx())!;
    const plain = parsePlaintext(open(key, boxAd(group, ME, BOB), fromHex(post.body.box)!)!)!;
    expect(new TextDecoder().decode(plain.text)).toBe("hello bob");
    expect(plain.seq).toBe(1n);
    expect(verifyMessage(server.myIdKey(), group, ME, 1n, plain.text, plain.sig)).toBe(true);
    await until(() => client.getState().conversations[BOB]?.lines.length === 1);
    expect(client.getState().conversations[BOB].lines[0]).toMatchObject({ own: true, text: "hello bob" });

    expect(await client.sendMessage(CAROL, "hi")).toEqual({ ok: false, message: "That player is offline." });
    expect(await client.sendMessage(BOB, "\u200b ")).toEqual({ ok: false, message: "Nothing to send." });
    server.setPostStatus(404);
    expect(await client.sendMessage(BOB, "gone")).toEqual({ ok: false, message: "That player is offline." });
    expect(client.getState().conversations[BOB].lines).toHaveLength(1);
  });

  it("ignores: lines stay, no unread, no sound, no invites; shared with the game's file", async () => {
    await client.setIgnored(BOB, true);
    expect(JSON.parse(await readFile(path.join(dir, HIDDEN_FILE), "utf8"))).toEqual({ hidden: [BOB] });
    await until(() => client.getState().ignored.includes(BOB));
    server.send(dmFrom(bob, "psst"));
    server.send({ type: "invite", from: BOB, kind: "direct", connectCode: "BO#7" });
    await until(() => client.getState().conversations[BOB]?.lines.length === 1);
    expect(client.getState().conversations[BOB].unread).toBe(0);
    expect(client.getState().invites).toEqual([]);
    expect(events).toEqual([]);

    // The game unignores bob in its own file; the launcher picks it up.
    await new Promise((resolve) => setTimeout(resolve, 20));
    await writeFile(path.join(dir, HIDDEN_FILE), JSON.stringify({ hidden: [] }));
    await until(() => client.getState().ignored.length === 0);
  }, 10_000);

  it("takes valid invites and answers them", async () => {
    server.send({ type: "invite", from: BOB, kind: "direct", connectCode: "XX#1" });
    server.send({ type: "invite", from: CAROL, kind: "direct", connectCode: "CA#1" });
    server.send({ type: "invite", from: BOB, kind: "room", room: { code: "KFQB" } });
    server.send({
      type: "invite",
      from: BOB,
      kind: "room",
      room: { code: "KFQB", players: 1, openSlots: 4, mode: "ffa", status: "waiting", joinable: true },
    });
    await until(() => client.getState().invites.length === 1);
    const inv = client.getState().invites[0];
    expect(inv).toMatchObject({ kind: "room", from: { uid: BOB }, room: { code: "KFQB", mode: "ffa" } });
    expect(events.map((e) => e.type)).toEqual(["invite"]);
    server.send({ type: "invite", from: BOB, kind: "direct", connectCode: "BO#7" });
    await until(() => client.getState().invites[0]?.kind === "direct");
    expect(client.getState().invites).toHaveLength(1);
    expect(await client.answerInvite(client.getState().invites[0].id, true)).toEqual({ ok: true });
    expect(server.posts.at(-1)).toEqual({ path: "/v1/social/invite/answer", body: { from: BOB, accept: true } });
    await until(() => client.getState().invites.length === 0);

    server.send({ type: "invite-answer", by: BOB, kind: "direct", accept: true, connectCode: "BO#7" });
    await until(() => events.length === 3);
    expect(events[2]).toEqual({
      type: "invite-answer",
      player: { uid: BOB, displayName: "bob", connectCode: "BO#7" },
      kind: "direct",
      accept: true,
    });
    expect(await client.invite(BOB)).toEqual({ ok: true, kind: "direct" });
  });

  it("stops when another launcher takes over, reconnects after a drop", async () => {
    server.close();
    await until(() => client.getState().status === "connecting" || client.getState().status === "reconnecting");
    await tick();
    server.send({ type: "welcome", you: ME, online: [], room: null });
    await until(() => client.getState().status === "online");
    server.send({ type: "replaced" });
    await until(() => client.getState().status === "off");
    expect(client.getState().message).toMatch(/another launcher/);
    expect(events.at(-1)).toEqual({ type: "replaced" });
  });

  it("passes an account change on, and stops for good on a ban", async () => {
    server.send({ type: "account" });
    await until(() => events.some((e) => e.type === "account"));
    expect(client.getState().status).toBe("online");
    server.send({ type: "banned" });
    await until(() => client.getState().status === "off");
    expect(client.getState().message).toBe("This account is banned.");
    expect(events.at(-1)).toEqual({ type: "banned" });
  });
});

describe("shared chat files", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "chatfiles-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("makes the identity key once and reads the game's", async () => {
    const a = await loadIdentitySeed(dir);
    expect(toHex(await loadIdentitySeed(dir))).toBe(toHex(a));
    expect(await readFile(path.join(dir, IDENTITY_FILE), "utf8")).toBe(`${toHex(a)}\n`);
    await writeFile(path.join(dir, IDENTITY_FILE), `${"AB".repeat(32)}\r\n`);
    expect(toHex(await loadIdentitySeed(dir))).toBe("ab".repeat(32));
    await writeFile(path.join(dir, IDENTITY_FILE), "garbage");
    const fresh = await loadIdentitySeed(dir);
    expect(fresh).toHaveLength(32);
    expect(await readFile(path.join(dir, `${IDENTITY_FILE}.bad`), "utf8")).toBe("garbage");
  });

  it("reads and writes the ignore list", async () => {
    expect(await readHidden(dir)).toEqual([]);
    await writeHidden(dir, [CAROL, BOB, "junk", BOB]);
    expect(await readHidden(dir)).toEqual([BOB, CAROL]);
    await writeFile(path.join(dir, HIDDEN_FILE), JSON.stringify({ hidden: [BOB, 5, "x"] }));
    expect(await readHidden(dir)).toEqual([BOB]);
    await writeFile(path.join(dir, HIDDEN_FILE), "{");
    expect(await readHidden(dir)).toEqual([]);
  });
});

describe("social client: friends and the account's ignore list", () => {
  let dir: string;
  let server: ReturnType<typeof fakeServer>;
  let client: SocialClient;
  let events: SocialEvent[];

  const carol = { uid: CAROL, displayName: "carol", connectCode: "CA#1", since: "2026-11-14T00:00:00Z" };
  const dave = { uid: DAVE, displayName: "dave", connectCode: "DA#2", main: "fox", since: "2026-11-14T00:00:00Z" };

  /** Connects with the server's answers in place before the welcome. */
  async function start(routes: Record<string, (body: any) => unknown>, file?: string[], synced?: string[]) {
    dir = await mkdtemp(path.join(os.tmpdir(), "social-friends-"));
    if (file) {
      await writeHidden(dir, file);
    }
    if (synced) {
      await writeHidden(dir, synced, syncedHiddenFile(ME));
    }
    server = fakeServer();
    Object.assign(server.routes, routes);
    client = new SocialClient({
      baseUrl: "http://accounts",
      userAgent: "test",
      getToken: async () => "token",
      onlineDir: () => dir,
      fetch: server.fetchFn,
      sleep: async () => undefined,
    });
    events = [];
    client.onEvent((e) => events.push(e));
    await client.connect(ME);
    await until(() => client.getState().status === "connecting");
    await tick();
    server.send({ type: "welcome", you: ME, online: [], room: null });
    await until(() => client.getState().status === "online");
  }

  afterEach(async () => {
    client.disconnect();
    await rm(dir, { recursive: true, force: true });
  });

  const lists = (over: Partial<Record<"friends" | "incoming" | "outgoing", unknown[]>> = {}) => ({
    friends: [],
    incoming: [],
    outgoing: [],
    ...over,
  });

  /** Waits for a file read to hold. */
  async function untilAsync(pred: () => Promise<boolean>) {
    for (let i = 0; i < 300 && !(await pred()); i++) {
      await tick();
    }
    expect(await pred()).toBe(true);
  }

  it("reads the lists after the welcome, checks them, and pops up a request that arrives", async () => {
    let answer = lists({
      friends: [carol, { ...carol, uid: "nope" }, { ...dave, connectCode: "" }],
      outgoing: [dave],
    });
    await start({ "GET /v1/friends": () => answer });
    await until(() => client.getState().friends.loaded);
    expect(client.getState().friends.friends.map((f) => f.uid)).toEqual([CAROL]);
    expect(client.getState().friends.outgoing[0]).toEqual({ ...dave });

    answer = lists({ friends: [carol], incoming: [dave] });
    server.send({ type: "friends", change: "request", player: dave });
    await until(() => client.getState().requestPopups.length === 1);
    expect(events).toContainEqual({ type: "friend-request", player: dave });
    await until(() => client.getState().friends.incoming.length === 1);
    // Closing the pop-up keeps the request.
    client.dismissRequestPopup(DAVE);
    expect(client.getState().requestPopups).toEqual([]);
    expect(client.getState().friends.incoming.map((f) => f.uid)).toEqual([DAVE]);

    // Accepting answers with the new lists.
    server.routes[`POST /v1/friends/requests/${DAVE}/accept`] = () => lists({ friends: [carol, dave] });
    expect(await client.acceptFriend(DAVE)).toEqual({ ok: true });
    expect(client.getState().friends.friends.map((f) => f.uid)).toEqual([CAROL, DAVE]);
    // The player's own request accepted is an event (a toast).
    server.send({ type: "friends", change: "accepted", player: carol });
    await until(() => events.some((e) => e.type === "friend-accepted"));
  });

  it("sends a request by code and says whether they are friends now", async () => {
    await start({
      "GET /v1/friends": () => lists(),
      "POST /v1/friends/requests": (body) => ({
        status: body.to === "CA#1" ? "friends" : "sent",
        player: body.to === "CA#1" ? carol : dave,
        ...lists({ outgoing: [dave] }),
      }),
    });
    expect(await client.sendFriendRequest("  DA#2 ")).toEqual({ ok: true, status: "sent", player: dave });
    expect(server.calls.find((c) => c.path === "/v1/friends/requests")?.body).toEqual({ to: "DA#2" });
    expect(client.getState().friends.outgoing.map((f) => f.uid)).toEqual([DAVE]);
    expect(await client.sendFriendRequest("CA#1")).toMatchObject({ ok: true, status: "friends" });
    expect(await client.sendFriendRequest("  ")).toMatchObject({ ok: false });
  });

  it("uploads the install's ignore list once, then keeps the account's in the game's file", async () => {
    let account: string[] = [];
    await start(
      {
        "GET /v1/friends": () => lists(),
        "GET /v1/me/ignores": () => ({ ignored: account, imported: false }),
        "POST /v1/me/ignores/import": (body) => {
          account = body.uids;
          return { ignored: account, imported: true };
        },
        [`PUT /v1/me/ignores/${CAROL}`]: () => {
          account = [...account, CAROL];
          return { ignored: account, imported: true };
        },
      },
      [BOB],
    );
    await until(() => server.calls.some((c) => c.path === "/v1/me/ignores/import"));
    expect(server.calls.find((c) => c.path === "/v1/me/ignores/import")?.body).toEqual({ uids: [BOB] });
    await untilAsync(async () => (await readHiddenFile(dir, syncedHiddenFile(ME))) !== null);
    expect(await readHiddenFile(dir, syncedHiddenFile(ME))).toEqual([BOB]);

    // The game ignores carol: the account follows (the file is looked at every 2 s).
    await writeHidden(dir, [BOB, CAROL]);
    await untilAsync(async () => server.calls.some((c) => c.method === "PUT" && c.path === `/v1/me/ignores/${CAROL}`));
    await until(() => client.getState().ignored.includes(CAROL));
  });

  it("with the install's copy, sends only what the game changed since", async () => {
    // The account has bob and dave; this install's game unignored bob and ignored carol while the
    // launcher was closed.
    await start(
      {
        "GET /v1/friends": () => lists(),
        "GET /v1/me/ignores": () => ({ ignored: [BOB, DAVE], imported: true }),
        [`PUT /v1/me/ignores/${CAROL}`]: () => ({ ignored: [BOB, DAVE, CAROL], imported: true }),
        [`DELETE /v1/me/ignores/${BOB}`]: () => ({ ignored: [DAVE, CAROL], imported: true }),
      },
      [CAROL],
      [BOB],
    );
    await until(() => client.getState().ignored.length === 2);
    const changes = server.calls
      .filter((c) => c.method === "PUT" || c.method === "DELETE")
      .map((c) => `${c.method} ${c.path}`);
    expect(changes).toEqual([`PUT /v1/me/ignores/${CAROL}`, `DELETE /v1/me/ignores/${BOB}`]);
    expect([...client.getState().ignored].sort()).toEqual([CAROL, DAVE].sort());
    expect([...(await readHidden(dir))].sort()).toEqual([CAROL, DAVE].sort());
  });

  it("without a copy (another computer), only adds what this file has", async () => {
    await start(
      {
        "GET /v1/friends": () => lists(),
        "GET /v1/me/ignores": () => ({ ignored: [DAVE], imported: true }),
        [`PUT /v1/me/ignores/${CAROL}`]: () => ({ ignored: [DAVE, CAROL], imported: true }),
      },
      [CAROL],
    );
    await until(() => client.getState().ignored.length === 2);
    expect(server.calls.filter((c) => c.method === "DELETE")).toEqual([]);
    expect([...(await readHidden(dir))].sort()).toEqual([CAROL, DAVE].sort());
  });
});
