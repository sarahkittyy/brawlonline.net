import { mkdtemp, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

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
import { HIDDEN_FILE, IDENTITY_FILE, loadIdentitySeed, readHidden, writeHidden } from "./shared_files";
import { SocialClient } from "./social_client";
import type { SocialEvent } from "./types";

const ME = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const BOB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
const CAROL = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

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
    const body = JSON.parse(String(init?.body));
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
    expect(client.getState().online.map((p) => p.displayName)).toEqual(["bob", "carol"]);
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
