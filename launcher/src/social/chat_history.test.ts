import { mkdtemp, readdir, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { ChatHistory, parseStoredLine } from "./chat_history";
import type { ChatLine } from "./types";

const ME = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const BOB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
const CAROL = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
const OTHER_ME = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";

const none = () => new Set<string>();
const line = (n: number, own = false): ChatLine => ({ id: `id-${n}`, own, text: `message ${n}`, at: 1000 + n });
const texts = (lines: ChatLine[]) => lines.map((l) => l.text);

describe("ChatHistory", () => {
  let dir: string;
  let clock: number;
  const now = () => clock++;

  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "chat-history-"));
    clock = Date.now();
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("reads a conversation back a page at a time, newest page first", async () => {
    const h = new ChatHistory(dir, { chunkBytes: 400, now });
    for (let i = 1; i <= 23; i++) {
      h.append(ME, BOB, line(i, i % 2 === 0));
    }
    const pages: string[][] = [];
    let before: string | null = null;
    do {
      const page = await h.page(ME, BOB, before, none, 10);
      pages.push(texts(page.lines));
      before = page.before;
    } while (before !== null);
    expect(pages).toEqual([
      Array.from({ length: 10 }, (_, i) => `message ${14 + i}`),
      Array.from({ length: 10 }, (_, i) => `message ${4 + i}`),
      ["message 1", "message 2", "message 3"],
    ]);
    // The pages crossed chunks.
    const files = await readdir(path.join(dir, ME, BOB));
    expect(files.length).toBeGreaterThanOrEqual(3);
    const page = await h.page(ME, BOB, null, none, 1);
    expect(page.lines[0]).toEqual({ id: "id-23", own: false, text: "message 23", at: 1023 });
  });

  it("leaves out the lines the conversation already has", async () => {
    const h = new ChatHistory(dir, { now });
    for (let i = 1; i <= 6; i++) {
      h.append(ME, BOB, line(i));
    }
    const page = await h.page(ME, BOB, null, () => new Set(["id-5", "id-6"]), 3);
    expect(texts(page.lines)).toEqual(["message 2", "message 3", "message 4"]);
    const rest = await h.page(ME, BOB, page.before, () => new Set(["id-5", "id-6"]), 3);
    expect(texts(rest.lines)).toEqual(["message 1"]);
    expect(rest.before).toBeNull();
  });

  it("ends without a cursor when the last page reaches the start exactly", async () => {
    const h = new ChatHistory(dir, { now });
    for (let i = 1; i <= 4; i++) {
      h.append(ME, BOB, line(i));
    }
    const page = await h.page(ME, BOB, null, none, 4);
    expect(page.lines).toHaveLength(4);
    expect(page.before).toBeNull();
  });

  it("keeps each account's and each player's conversations apart", async () => {
    const h = new ChatHistory(dir, { now });
    h.append(ME, BOB, line(1));
    h.append(ME, CAROL, line(2));
    h.append(OTHER_ME, BOB, line(3));
    expect(texts((await h.page(ME, BOB, null, none)).lines)).toEqual(["message 1"]);
    expect(texts((await h.page(ME, CAROL, null, none)).lines)).toEqual(["message 2"]);
    expect(texts((await h.page(OTHER_ME, BOB, null, none)).lines)).toEqual(["message 3"]);
    expect((await h.page(OTHER_ME, CAROL, null, none)).lines).toEqual([]);
  });

  it("deletes the oldest messages first once over its size, across conversations and launches", async () => {
    const h = new ChatHistory(dir, { chunkBytes: 200, maxBytes: 1000, now });
    for (let i = 1; i <= 5; i++) {
      h.append(ME, BOB, line(i));
    }
    for (let i = 6; i <= 10; i++) {
      h.append(ME, CAROL, line(i));
    }
    await h.flush();
    // A new launch reads the sizes (and the files' times) from the folder and goes on from there.
    clock = Date.now() + 60_000;
    const next = new ChatHistory(dir, { chunkBytes: 200, maxBytes: 1000, now });
    for (let i = 11; i <= 30; i++) {
      next.append(ME, CAROL, line(i));
    }
    const all = async (them: string) => {
      const out: ChatLine[] = [];
      let before: string | null = null;
      do {
        const page = await next.page(ME, them, before, none, 7);
        out.unshift(...page.lines);
        before = page.before;
      } while (before !== null);
      return out;
    };
    // Bob's lines were the oldest: all gone, and the folder with them.
    expect(await all(BOB)).toEqual([]);
    await expect(readdir(path.join(dir, ME, BOB))).rejects.toThrow();
    const carol = texts(await all(CAROL));
    expect(carol[carol.length - 1]).toBe("message 30");
    expect(carol).not.toContain("message 6");
    // What is left is the newest lines, in a row.
    const first = Number(carol[0].split(" ")[1]);
    expect(carol).toEqual(Array.from({ length: 31 - first }, (_, i) => `message ${first + i}`));
    let bytes = 0;
    for (const f of await readdir(path.join(dir, ME, CAROL))) {
      bytes += (await readFile(path.join(dir, ME, CAROL, f))).length;
    }
    expect(bytes).toBeLessThanOrEqual(1000);
  });

  it("keeps at most so many chunk files", async () => {
    const h = new ChatHistory(dir, { maxChunks: 3, now });
    const people = [BOB, CAROL, OTHER_ME, "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"];
    people.forEach((p, i) => h.append(ME, p, line(i)));
    await h.flush();
    expect((await h.page(ME, BOB, null, none)).lines).toEqual([]);
    expect(texts((await h.page(ME, people[3], null, none)).lines)).toEqual(["message 3"]);
  });

  it("skips lines it can't use and refuses odd cursors and uids", async () => {
    const h = new ChatHistory(dir, { now });
    h.append(ME, BOB, line(1));
    await h.flush();
    const file = path.join(dir, ME, BOB, "00000000.jsonl");
    const good = await readFile(file, "utf8");
    await writeFile(
      file,
      `${good}not json\n{"i":"x","t":"\\u202e","a":1}\n{"i":"y","t":5,"a":1}\n{"i":"z","o":1,"t":"ok","a":2}\n{"i":"tr`,
    );
    expect((await h.page(ME, BOB, null, none)).lines).toEqual([
      { id: "id-1", own: false, text: "message 1", at: 1001 },
      { id: "z", own: true, text: "ok", at: 2 },
    ]);
    expect(await h.page(ME, BOB, "../x", none)).toEqual({ lines: [], before: null });
    expect(await h.page(ME, "../etc", null, none)).toEqual({ lines: [], before: null });
    h.append(ME, "nope", line(2));
    h.append(ME, ME, line(3));
    await h.flush();
    expect(await readdir(path.join(dir, ME))).toEqual([BOB]);
  });

  it("parses a stored line through the chat cleaner", () => {
    expect(parseStoredLine('{"i":"a","o":0,"t":"  hi   there ","a":5}')).toEqual({
      id: "a",
      own: false,
      text: "hi there",
      at: 5,
    });
    expect(parseStoredLine('{"i":"","t":"x","a":5}')).toBeUndefined();
    expect(parseStoredLine('{"i":"a","t":"x","a":"5"}')).toBeUndefined();
  });
});
