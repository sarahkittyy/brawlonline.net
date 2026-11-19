import type { SpectatorChatFile, SpectatorListFile } from "@dolphin/types";
import { describe, expect, it } from "vitest";

import { LIST_PAGE, MAX_LINES, SpectatorChat } from "./spectator_chat";

const ME = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const BOB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
const STREAM = "3fa9c0d1e2b4";
const OTHER = "0123456789ab";

const line = (id: number, uid: string, displayName: string, text: string) => ({
  id,
  uid,
  displayName,
  connectCode: `${displayName.slice(0, 3).toUpperCase()}#1`,
  text,
  at: "2026-11-15T20:00:00.000Z",
});

function setup({ answer }: { answer?: (url: string, body: any) => { status: number; body: unknown } } = {}) {
  const writes: (SpectatorChatFile | null)[] = [];
  const lists: (SpectatorListFile | null)[] = [];
  const posts: { url: string; body: any }[] = [];
  const gets: string[] = [];
  // 450 spectators, served a page at a time as accounts does.
  const crowd = Array.from({ length: 450 }, (_, i) => ({ uid: `u${i}`, displayName: `n${i}`, connectCode: `C#${i}` }));
  let hidden: string[] = [];
  let open = true;
  let disabled = false;
  let nextServerId = 100;
  const chat = new SpectatorChat({
    baseUrl: "https://example.test",
    userAgent: "test",
    getToken: async () => "token",
    write: async (file) => {
      if (!open) {
        return false;
      }
      writes.push(file);
      return true;
    },
    writeList: async (list) => {
      lists.push(list);
      return true;
    },
    readHidden: async () => hidden,
    setHidden: async (uid, on) => {
      hidden = on ? [...hidden.filter((u) => u !== uid), uid] : hidden.filter((u) => u !== uid);
    },
    disabled: () => disabled,
    fetch: (async (url: string, init: RequestInit) => {
      if (init.method === "GET") {
        gets.push(url);
        const u = new URL(url);
        const after = Number(u.searchParams.get("after") ?? "0");
        const limit = Number(u.searchParams.get("limit"));
        const page = crowd.slice(after, after + limit);
        const next = after + limit < crowd.length ? String(after + limit) : null;
        return new Response(JSON.stringify({ count: crowd.length, spectators: page, next }), { status: 200 });
      }
      const body = JSON.parse(String(init.body));
      posts.push({ url, body });
      const r = answer?.(url, body) ?? {
        status: 200,
        body: url.endsWith("/chat") ? { line: line(nextServerId++, ME, "me", body.text) } : null,
      };
      return new Response(r.body === null ? null : JSON.stringify(r.body), { status: r.status });
    }) as unknown as typeof fetch,
  });
  const last = async () => {
    await chat.flush();
    return writes[writes.length - 1];
  };
  return {
    chat,
    writes,
    posts,
    gets,
    lists,
    last,
    setOpen: (o: boolean) => (open = o),
    setDisabled: (d: boolean) => (disabled = d),
    hidden: () => hidden,
  };
}

const welcome = {
  type: "welcome",
  you: ME,
  stream: { id: STREAM, spectators: 2 },
  chat: [line(1, BOB, "bob", "hi all")],
  spectators: [
    { uid: BOB, displayName: "bob", connectCode: "BOB#1" },
    { uid: ME, displayName: "me", connectCode: "ME#1" },
  ],
};

describe("SpectatorChat", () => {
  it("writes the watch stream's chat for the playback Dolphin", async () => {
    const t = setup();
    t.chat.start(ME, STREAM, 1);
    t.chat.onEvent(STREAM, welcome);
    t.chat.onEvent(STREAM, { type: "chat", line: line(2, BOB, "bob", "nice") });
    // The same line again (a reconnect's welcome): once.
    t.chat.onEvent(STREAM, { type: "chat", line: line(2, BOB, "bob", "nice") });
    // Another stream's events: not this chat.
    t.chat.onEvent(OTHER, { type: "chat", line: line(3, BOB, "bob", "elsewhere") });
    t.chat.onEvent(STREAM, { type: "chunk", game: 1, i: 0, b: "" });
    const f = (await t.last())!;
    expect(f.stream).toBe(STREAM);
    expect(f.you).toBe(ME);
    expect(f.over).toBe(false);
    expect(f.count).toBe(2);
    expect(f.spectators).toEqual([
      { uid: BOB, name: "bob", code: "BOB#1" },
      { uid: ME, name: "me", code: "ME#1" },
    ]);
    expect(f.lines).toEqual([
      { id: 1, uid: BOB, name: "bob", code: "BOB#1", text: "hi all" },
      { id: 2, uid: BOB, name: "bob", code: "BOB#1", text: "nice" },
    ]);
    // Who watches changes.
    t.chat.onEvent(STREAM, {
      type: "spectators",
      count: 5,
      names: [{ uid: BOB, displayName: "bob", connectCode: "" }],
    });
    const g = (await t.last())!;
    expect(g.spectators).toEqual([{ uid: BOB, name: "bob", code: "" }]);
    expect(g.count).toBe(5);
  });

  it("sends what was typed and says it took it", async () => {
    const t = setup();
    t.chat.start(ME, STREAM);
    t.chat.onEvent(STREAM, welcome);
    await t.chat.onOutbox({ run: "r1", out: [{ seq: 1, stream: STREAM, send: "gg" }] });
    expect(t.posts).toEqual([{ url: `https://example.test/v1/live/${STREAM}/chat`, body: { text: "gg" } }]);
    let f = (await t.last())!;
    expect(f.done).toEqual({ run: "r1", seq: 1 });
    expect(f.lines.at(-1)).toEqual({ id: 2, uid: ME, name: "me", code: "ME#1", text: "gg" });
    // Its echo on the watch stream: already there.
    t.chat.onEvent(STREAM, { type: "chat", line: line(100, ME, "me", "gg") });
    f = (await t.last())!;
    expect(f.lines).toHaveLength(2);
    // The same outbox read again: nothing sent twice.
    await t.chat.onOutbox({ run: "r1", out: [{ seq: 1, stream: STREAM, send: "gg" }] });
    expect(t.posts).toHaveLength(1);
    // A new Dolphin (another run) counts from 1 again.
    await t.chat.onOutbox({ run: "r2", out: [{ seq: 1, stream: STREAM, send: "again" }] });
    expect(t.posts).toHaveLength(2);
    expect((await t.last())!.done).toEqual({ run: "r2", seq: 1 });
  });

  it("a refused line comes back as the server's notice", async () => {
    const t = setup({
      answer: () => ({
        status: 429,
        body: { error: { code: "busy", message: "Chat is busy. Try again in a moment." } },
      }),
    });
    t.chat.start(ME, STREAM);
    await t.chat.onOutbox({ run: "r", out: [{ seq: 1, stream: STREAM, send: "hello" }] });
    const f = (await t.last())!;
    expect(f.lines).toEqual([{ id: 1, notice: "Chat is busy. Try again in a moment." }]);
    expect(f.done).toEqual({ run: "r", seq: 1 });
  });

  it("drops what was typed for a game no longer watched", async () => {
    const t = setup();
    t.chat.start(ME, OTHER);
    await t.chat.onOutbox({ run: "r", out: [{ seq: 1, stream: STREAM, send: "late" }] });
    expect(t.posts).toHaveLength(0);
    expect((await t.last())!.done).toEqual({ run: "r", seq: 1 });
  });

  it("ignores in the shared list and reports a line by the server's id", async () => {
    const t = setup();
    t.chat.start(ME, STREAM);
    t.chat.onEvent(STREAM, welcome);
    await t.chat.onOutbox({ run: "r", out: [{ seq: 1, stream: STREAM, hide: BOB, hidden: true }] });
    expect(t.hidden()).toEqual([BOB]);
    expect((await t.last())!.hidden).toEqual([BOB]);
    await t.chat.onOutbox({
      run: "r",
      out: [
        { seq: 1, stream: STREAM, hide: BOB, hidden: true },
        { seq: 2, stream: STREAM, report: 1 },
      ],
    });
    expect(t.posts).toEqual([{ url: `https://example.test/v1/live/${STREAM}/report`, body: { line: 1, reason: "" } }]);
    expect((await t.last())!.lines.at(-1)).toEqual({ id: 2, notice: "Reported bob." });
    await t.chat.onOutbox({ run: "r", out: [{ seq: 3, stream: STREAM, hide: BOB, hidden: false }] });
    expect(t.hidden()).toEqual([]);
  });

  it("the stream's end, stopping, Disable Chat, and a Dolphin not open yet", async () => {
    const t = setup();
    t.setOpen(false);
    t.chat.start(ME, STREAM);
    t.chat.onEvent(STREAM, welcome);
    await t.chat.flush();
    expect(t.writes).toHaveLength(0);
    t.setOpen(true);
    expect((await t.last())!.lines).toHaveLength(1);
    t.chat.end();
    expect((await t.last())!.over).toBe(true);
    t.setDisabled(true);
    t.chat.refresh();
    expect(await t.last()).toBeNull();
    t.setDisabled(false);
    t.chat.refresh();
    expect((await t.last())!.stream).toBe(STREAM);
    t.chat.stop();
    expect(await t.last()).toBeNull();
    // Another stream: its own chat.
    t.chat.start(ME, OTHER);
    expect((await t.last())!.lines).toEqual([]);
  });

  it("keeps the newest lines", async () => {
    const t = setup();
    t.chat.start(ME, STREAM);
    for (let i = 1; i <= MAX_LINES + 20; i++) {
      t.chat.onEvent(STREAM, { type: "chat", line: line(i, BOB, "bob", `line ${i}`) });
    }
    const f = (await t.last())!;
    expect(f.lines).toHaveLength(MAX_LINES);
    expect(f.lines.at(-1)).toMatchObject({ text: `line ${MAX_LINES + 20}` });
  });

  it("loads the Spectators window's list a page at a time", async () => {
    const t = setup();
    t.chat.start(ME, STREAM);
    // Scrolled to the end before it was ever opened: nothing to load yet.
    await t.chat.onOutbox({ run: "r", out: [{ seq: 1, stream: STREAM, list: "next" }] });
    expect(t.gets).toHaveLength(0);
    await t.chat.onOutbox({ run: "r", out: [{ seq: 2, stream: STREAM, list: "first" }] });
    expect(t.gets).toEqual([`https://example.test/v1/live/${STREAM}/spectators?limit=${LIST_PAGE}`]);
    let list = t.lists.at(-1)!;
    expect(list.more).toBe(true);
    expect(list.names).toHaveLength(LIST_PAGE);
    expect(list.names[0]).toEqual({ uid: "u0", name: "n0", code: "C#0" });
    expect(list.count).toBe(450);
    await t.chat.onOutbox({ run: "r", out: [{ seq: 3, stream: STREAM, list: "next" }] });
    expect(t.gets.at(-1)).toContain(`&after=${LIST_PAGE}`);
    await t.chat.onOutbox({ run: "r", out: [{ seq: 4, stream: STREAM, list: "next" }] });
    list = t.lists.at(-1)!;
    expect(list.more).toBe(false);
    expect(list.names).toHaveLength(450);
    // No more pages: nothing asked.
    await t.chat.onOutbox({ run: "r", out: [{ seq: 5, stream: STREAM, list: "next" }] });
    expect(t.gets).toHaveLength(3);
    expect((await t.last())!.done).toEqual({ run: "r", seq: 5 });
    // Opened again: from the first page.
    await t.chat.onOutbox({ run: "r", out: [{ seq: 6, stream: STREAM, list: "first" }] });
    expect(t.lists.at(-1)!.names).toHaveLength(LIST_PAGE);
    // Another stream: the list goes.
    t.chat.start(ME, OTHER);
    expect(t.lists.at(-1)).toBeNull();
  });
});
