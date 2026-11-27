/**
 * Direct message history on disk (docs/social-protocol.md §8), main process. Every line of a
 * conversation, sent or received, is appended to its file; the chat window reads it back a page at a
 * time, newest first, as the player scrolls up.
 *
 * Layout: `<dir>/<own uid>/<their uid>/<n>.jsonl`, one JSON object per line
 * (`{"i": id, "o": 1 when sent, "t": text, "a": unix ms}`), `n` an eight-digit chunk number that
 * grows. A chunk is closed at CHUNK_BYTES, so a page never reads more than a chunk or two whatever
 * the history's length. Every account on the PC shares one budget (MAX_BYTES, MAX_CHUNKS): over it,
 * the chunk whose newest line is the oldest goes, so the oldest messages are cleared first.
 *
 * The files are the launcher's own, but they are still read as untrusted: a line that doesn't parse
 * or whose text doesn't survive the chat cleaner is skipped.
 */

import electronLog from "electron-log";
import { appendFile, mkdir, readdir, readFile, rmdir, stat, unlink } from "fs/promises";
import path from "path";

import { cleanText } from "./chat_text";
import { isUid } from "./shared_files";
import type { ChatLine, HistoryPage } from "./types";

const log = electronLog.scope("chat-history");

/** All accounts' history together, at most. */
export const MAX_BYTES = 32 * 1024 * 1024;
/** Chunk files at most (many one-line conversations would otherwise be many small files). */
export const MAX_CHUNKS = 4096;
/** A chunk is closed once it holds this much. */
export const CHUNK_BYTES = 64 * 1024;
/** Lines per page. */
export const PAGE_LINES = 50;
/** A chunk bigger than this wasn't written by us: it isn't read. */
const MAX_READ_BYTES = 4 * CHUNK_BYTES;
const MAX_ID_CHARS = 64;

const CHUNK_RE = /^(\d{8})\.jsonl$/;
const chunkName = (n: number) => `${String(n).padStart(8, "0")}.jsonl`;

type Chunk = { file: string; bytes: number; /** Newest line's write, ms. */ newest: number };

export type ChatHistoryOptions = {
  maxBytes?: number;
  maxChunks?: number;
  chunkBytes?: number;
  now?: () => number;
};

/** A line as stored, or undefined when it can't be used. */
export function parseStoredLine(raw: string): ChatLine | undefined {
  let v: any;
  try {
    v = JSON.parse(raw);
  } catch {
    return undefined;
  }
  if (!v || typeof v !== "object" || typeof v.i !== "string" || !v.i || v.i.length > MAX_ID_CHARS) {
    return undefined;
  }
  if (typeof v.t !== "string" || typeof v.a !== "number" || !Number.isFinite(v.a)) {
    return undefined;
  }
  const text = cleanText(v.t.slice(0, 1024));
  return text ? { id: v.i, own: v.o === 1, text, at: v.a } : undefined;
}

export class ChatHistory {
  private readonly maxBytes: number;
  private readonly maxChunks: number;
  private readonly chunkBytes: number;
  private readonly now: () => number;
  /** Every chunk on disk, by path; read from the folder on first use. */
  private chunks?: Map<string, Chunk>;
  private total = 0;
  /** The chunk being written per conversation folder: its number. */
  private current = new Map<string, number>();
  /** Writes and reads, one at a time and in order, so a page sees every line appended before it. */
  private queue: Promise<unknown> = Promise.resolve();

  constructor(private readonly dir: string, opts: ChatHistoryOptions = {}) {
    this.maxBytes = opts.maxBytes ?? MAX_BYTES;
    this.maxChunks = opts.maxChunks ?? MAX_CHUNKS;
    this.chunkBytes = opts.chunkBytes ?? CHUNK_BYTES;
    this.now = opts.now ?? Date.now;
  }

  private enqueue<T>(work: () => Promise<T>): Promise<T> {
    const next = this.queue.then(work, work);
    this.queue = next.catch(() => undefined);
    return next;
  }

  /** Waits for the writes so far (tests, and before quitting). */
  flush(): Promise<void> {
    return this.enqueue(async () => undefined);
  }

  private convDir(me: string, them: string): string {
    return path.join(this.dir, me, them);
  }

  /** The chunk numbers in a conversation's folder, oldest first. */
  private async chunkNumbers(dir: string): Promise<number[]> {
    let names: string[];
    try {
      names = await readdir(dir);
    } catch {
      return [];
    }
    const out: number[] = [];
    for (const name of names) {
      const m = CHUNK_RE.exec(name);
      if (m) {
        out.push(Number(m[1]));
      }
    }
    return out.sort((a, b) => a - b);
  }

  private async scan(): Promise<Map<string, Chunk>> {
    if (this.chunks) {
      return this.chunks;
    }
    const chunks = new Map<string, Chunk>();
    const list = async (d: string) => readdir(d).catch(() => [] as string[]);
    for (const me of (await list(this.dir)).filter(isUid)) {
      for (const them of (await list(path.join(this.dir, me))).filter(isUid)) {
        const dir = this.convDir(me, them);
        for (const n of await this.chunkNumbers(dir)) {
          const file = path.join(dir, chunkName(n));
          try {
            const st = await stat(file);
            chunks.set(file, { file, bytes: st.size, newest: st.mtimeMs });
          } catch {
            // Gone meanwhile.
          }
        }
      }
    }
    this.chunks = chunks;
    this.total = [...chunks.values()].reduce((sum, c) => sum + c.bytes, 0);
    return chunks;
  }

  /** Adds a line to the conversation between `me` and `them` (written in the background). */
  append(me: string, them: string, line: ChatLine): void {
    if (!isUid(me) || !isUid(them) || me === them) {
      return;
    }
    const data = `${JSON.stringify({ i: line.id, o: line.own ? 1 : 0, t: line.text, a: line.at })}\n`;
    void this.enqueue(async () => {
      try {
        await this.write(me, them, data);
      } catch (err) {
        log.warn("can't save a chat line", err);
      }
    });
  }

  private async write(me: string, them: string, data: string) {
    const chunks = await this.scan();
    const dir = this.convDir(me, them);
    const bytes = Buffer.byteLength(data);
    let n = this.current.get(dir);
    if (n === undefined) {
      n = (await this.chunkNumbers(dir)).pop() ?? 0;
    }
    let file = path.join(dir, chunkName(n));
    const cur = chunks.get(file);
    if (cur && cur.bytes + bytes > this.chunkBytes) {
      n++;
      file = path.join(dir, chunkName(n));
    }
    await mkdir(dir, { recursive: true });
    await appendFile(file, data, "utf8");
    this.current.set(dir, n);
    const chunk = chunks.get(file) ?? { file, bytes: 0, newest: 0 };
    chunk.bytes += bytes;
    chunk.newest = this.now();
    chunks.set(file, chunk);
    this.total += bytes;
    await this.evict(file);
  }

  /** Deletes the chunks with the oldest lines until the history fits (never `keep`, just written). */
  private async evict(keep: string) {
    const chunks = this.chunks!;
    while ((this.total > this.maxBytes || chunks.size > this.maxChunks) && chunks.size > 1) {
      let oldest: Chunk | undefined;
      for (const c of chunks.values()) {
        if (c.file !== keep && (!oldest || c.newest < oldest.newest)) {
          oldest = c;
        }
      }
      if (!oldest) {
        return;
      }
      chunks.delete(oldest.file);
      this.total -= oldest.bytes;
      try {
        await unlink(oldest.file);
      } catch (err: any) {
        if (err?.code !== "ENOENT") {
          log.warn("can't delete old chat history", err);
        }
      }
      const dir = path.dirname(oldest.file);
      if ((await this.chunkNumbers(dir)).length === 0) {
        this.current.delete(dir);
        await rmdir(dir).catch(() => undefined);
      }
    }
  }

  private async readChunk(file: string): Promise<ChatLine[]> {
    try {
      const st = await stat(file);
      if (st.size > MAX_READ_BYTES) {
        return [];
      }
      const lines: ChatLine[] = [];
      for (const raw of (await readFile(file, "utf8")).split("\n")) {
        const line = raw ? parseStoredLine(raw) : undefined;
        if (line) {
          lines.push(line);
        }
      }
      return lines;
    } catch {
      return [];
    }
  }

  /**
   * Up to `limit` lines older than `before` (a page's cursor; null: from the newest), oldest first,
   * skipping the lines in `exclude` (the ones the window already has from this session). The page's
   * `before` reads the next older page; null when the history's start was reached.
   */
  page(
    me: string,
    them: string,
    before: string | null,
    exclude: () => ReadonlySet<string>,
    limit = PAGE_LINES,
  ): Promise<HistoryPage> {
    return this.enqueue(async () => {
      if (!isUid(me) || !isUid(them)) {
        return { lines: [], before: null };
      }
      let chunk = Infinity;
      let index = Infinity;
      if (before !== null) {
        const m = /^(\d{1,8}):(\d{1,6})$/.exec(before);
        if (!m) {
          return { lines: [], before: null };
        }
        chunk = Number(m[1]);
        index = Number(m[2]);
      }
      const skip = exclude();
      const dir = this.convDir(me, them);
      const numbers = (await this.chunkNumbers(dir)).filter((n) => n <= chunk).reverse();
      const out: ChatLine[] = [];
      for (const n of numbers) {
        const lines = await this.readChunk(path.join(dir, chunkName(n)));
        let i = n === chunk ? Math.min(index, lines.length) : lines.length;
        while (i > 0) {
          i--;
          if (!skip.has(lines[i].id)) {
            out.push(lines[i]);
            if (out.length >= limit) {
              out.reverse();
              // Nothing older: no cursor, so the window doesn't ask again.
              const more = i > 0 || numbers.some((m) => m < n);
              return { lines: out, before: more ? `${n}:${i}` : null };
            }
          }
        }
      }
      return { lines: out.reverse(), before: null };
    });
  }
}
