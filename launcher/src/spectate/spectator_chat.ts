import type { SpectatorChatFile, SpectatorChatOutbox, SpectatorListFile } from "@dolphin/types";
import log from "electron-log";

/**
 * Spectator chat (docs/design/spectating.md §6.4; docs/spectate-protocol.md §3, §4.1): everyone watching a live
 * game chats in the playback Dolphin's chat window. The players of that game never see it (accounts never sends it
 * to a game), and spectators never see the players' chat. It isn't end-to-end encrypted, so the window has no
 * padlock: an eye with how many watch instead, which lists them.
 *
 * The playback Dolphin has no account and no internet connection, so the launcher is the chat's client: the watch
 * stream's `welcome`, `chat` and `spectators` events become `<comm>.chat.json`, which Dolphin follows; what the
 * spectator types, ignores or reports comes back in Dolphin's status (`chat`), and the launcher sends it and says
 * in the file which it took (`done`). A refused line comes back as a notice with the server's message.
 */

/** The lines the file keeps (Dolphin's window keeps as many). */
export const MAX_LINES = 100;
/** The Spectators window's page (the server's largest), and how many names it loads at most. */
export const LIST_PAGE = 200;
export const MAX_LISTED = 20_000;

type ServerLine = { id: number; uid: string; displayName: string; connectCode: string; text: string };
type ServerName = { uid: string; displayName: string; connectCode: string };
type FileLine = SpectatorChatFile["lines"][number] & { serverId?: number };

export type SpectatorChatDeps = {
  baseUrl: string;
  userAgent: string;
  getToken: (uid: string) => Promise<string | undefined>;
  /** Writes the file for the playback Dolphin (null: no spectator chat); false when it isn't open. */
  write: (chat: SpectatorChatFile | null) => Promise<boolean>;
  /** Writes the Spectators window's list (null: none). */
  writeList: (list: SpectatorListFile | null) => Promise<boolean>;
  /** The ignore list shared with the game and the account (`chat-hidden.json` in the netplay User folder). */
  readHidden: () => Promise<string[]>;
  setHidden: (uid: string, hidden: boolean) => Promise<void>;
  /** The launcher's Disable Chat. */
  disabled: () => boolean;
  fetch?: typeof fetch;
};

function isServerLine(l: any): l is ServerLine {
  return (
    l !== null &&
    typeof l === "object" &&
    Number.isInteger(l.id) &&
    typeof l.uid === "string" &&
    typeof l.text === "string" &&
    typeof l.displayName === "string"
  );
}

function names(list: unknown): SpectatorChatFile["spectators"] | null {
  if (!Array.isArray(list)) {
    return null;
  }
  return list
    .filter((n: any): n is ServerName => n && typeof n.uid === "string" && typeof n.displayName === "string")
    .map((n) => ({ uid: n.uid, name: n.displayName, code: typeof n.connectCode === "string" ? n.connectCode : "" }));
}

export class SpectatorChat {
  private account: string | null = null;
  private stream: string | null = null;
  private you = "";
  private over = false;
  private count = 0;
  private spectators: SpectatorChatFile["spectators"] = [];
  private lines: FileLine[] = [];
  /** The launcher's line ids: increasing over every stream (Dolphin numbers them in its own run). */
  private nextId = 1;
  /** The outbox items taken, of Dolphin's run `run`. */
  private done: { run: string; seq: number } | null = null;
  private busy = false;
  /** The Spectators window's list so far, and where its next page starts (null: no more). */
  private listed: SpectatorListFile["names"] = [];
  private listNext: string | null = null;
  private listLoaded = false;
  private writing: Promise<void> = Promise.resolve();
  private dirty = false;

  constructor(private readonly deps: SpectatorChatDeps) {}

  private get fetch(): typeof fetch {
    return this.deps.fetch ?? fetch;
  }

  /** Watching `stream` as `account` (a new stream: its own chat). */
  start(account: string, stream: string, count = 0) {
    if (this.stream === stream && this.account === account) {
      return;
    }
    this.account = account;
    this.stream = stream;
    this.you = account;
    this.over = false;
    this.count = count;
    this.spectators = [];
    this.lines = [];
    this.forgetList();
    this.changed();
  }

  private forgetList() {
    const had = this.listLoaded;
    this.listed = [];
    this.listNext = null;
    this.listLoaded = false;
    if (had) {
      void this.deps.writeList(null).catch(() => undefined);
    }
  }

  /** Not watching any more: the window goes. */
  stop() {
    if (this.stream === null) {
      return;
    }
    this.stream = null;
    this.account = null;
    this.lines = [];
    this.spectators = [];
    this.forgetList();
    this.changed();
  }

  /** The stream is over: the lines stay, nothing more can be sent. */
  end() {
    if (this.stream !== null && !this.over) {
      this.over = true;
      this.changed();
    }
  }

  /** A watch event of the stream watched (`welcome`, `chat`, `spectators`; the others are ignored). */
  onEvent(stream: string, ev: any) {
    if (stream !== this.stream) {
      return;
    }
    switch (ev?.type) {
      case "welcome": {
        if (typeof ev.you === "string") {
          this.you = ev.you;
        }
        const count = ev.stream?.spectators;
        if (Number.isInteger(count)) {
          this.count = count;
        }
        const listed = names(ev.spectators);
        if (listed) {
          this.spectators = listed;
        }
        // A reconnect's welcome has lines already shown.
        for (const line of Array.isArray(ev.chat) ? ev.chat : []) {
          this.addLine(line);
        }
        this.changed();
        return;
      }
      case "chat":
        if (this.addLine(ev.line)) {
          this.changed();
        }
        return;
      case "spectators": {
        if (Number.isInteger(ev.count)) {
          this.count = ev.count;
        }
        const listed = names(ev.names);
        if (listed) {
          this.spectators = listed;
        }
        this.changed();
        return;
      }
    }
  }

  private addLine(line: unknown): boolean {
    if (!isServerLine(line) || this.lines.some((l) => l.serverId === line.id)) {
      return false;
    }
    this.lines.push({
      id: this.nextId++,
      serverId: line.id,
      uid: line.uid,
      name: line.displayName,
      code: typeof line.connectCode === "string" ? line.connectCode : "",
      text: line.text,
    });
    this.trim();
    return true;
  }

  private notice(text: string) {
    this.lines.push({ id: this.nextId++, notice: text });
    this.trim();
  }

  private trim() {
    if (this.lines.length > MAX_LINES) {
      this.lines.splice(0, this.lines.length - MAX_LINES);
    }
  }

  /** The file as it is now (exposed for tests). */
  async file(): Promise<SpectatorChatFile | null> {
    if (this.stream === null || this.deps.disabled()) {
      return null;
    }
    const hidden = await this.deps.readHidden().catch(() => []);
    return {
      stream: this.stream,
      you: this.you,
      over: this.over,
      count: Math.max(this.count, this.spectators.length),
      spectators: this.spectators,
      lines: this.lines.map(({ serverId: _serverId, ...l }) => l),
      hidden,
      ...(this.done ? { done: this.done } : {}),
    };
  }

  private changed() {
    this.dirty = true;
    this.writing = this.writing
      .then(() => this.writeNow())
      .catch((err) => log.warn(`spectator chat: ${err instanceof Error ? err.message : err}`));
  }

  private async writeNow() {
    if (!this.dirty) {
      return;
    }
    this.dirty = false;
    if (!(await this.deps.write(await this.file()))) {
      // The playback Dolphin isn't open yet: written again by the next flush().
      this.dirty = true;
    }
  }

  /** Writes the file if it changed since (or couldn't be written yet). */
  flush(): Promise<void> {
    if (this.dirty) {
      this.changed();
    }
    return this.writing;
  }

  /** Written again (Disable Chat changed, or the ignore list). */
  refresh() {
    this.changed();
  }

  /** Dolphin's outbox from its status: does what is new, in order, then says so in the file. */
  async onOutbox(outbox: SpectatorChatOutbox | undefined): Promise<void> {
    if (!outbox || typeof outbox.run !== "string" || !Array.isArray(outbox.out) || this.busy) {
      return;
    }
    const after = this.done?.run === outbox.run ? this.done.seq : 0;
    const todo = outbox.out.filter((i) => Number.isInteger(i?.seq) && i.seq > after).sort((a, b) => a.seq - b.seq);
    if (todo.length === 0) {
      return;
    }
    this.busy = true;
    try {
      for (const item of todo) {
        await this.take(item).catch((err) => log.warn(`spectator chat: ${err instanceof Error ? err.message : err}`));
        this.done = { run: outbox.run, seq: item.seq };
      }
    } finally {
      this.busy = false;
    }
    this.changed();
  }

  private async take(item: SpectatorChatOutbox["out"][number]) {
    const stream = this.stream;
    const account = this.account;
    // Typed for a game no longer watched: dropped.
    if (!stream || !account || item.stream !== stream) {
      return;
    }
    if ("send" in item && typeof item.send === "string") {
      const r = await this.post(account, `/v1/live/${stream}/chat`, { text: item.send });
      if (this.stream !== stream) {
        return;
      }
      if (r.ok) {
        this.addLine(r.body?.line);
      } else {
        this.notice(r.message);
      }
      return;
    }
    if ("hide" in item && typeof item.hide === "string") {
      await this.deps.setHidden(item.hide, item.hidden === true);
      return;
    }
    if ("report" in item && Number.isInteger(item.report)) {
      const line = this.lines.find((l) => l.id === item.report);
      if (!line || line.serverId === undefined || !("uid" in line)) {
        return;
      }
      const r = await this.post(account, `/v1/live/${stream}/report`, { line: line.serverId, reason: "" });
      if (this.stream === stream) {
        // The game's own words for it (Online/Chat.cpp).
        this.notice(r.ok ? `Reported ${line.name}.` : r.message);
      }
      return;
    }
    if ("list" in item && (item.list === "first" || item.list === "next")) {
      await this.loadList(account, stream, item.list === "first");
    }
  }

  /** The Spectators window's first page (again), or its next one: written for Dolphin before `done` says so. */
  private async loadList(account: string, stream: string, first: boolean) {
    if (first) {
      this.listed = [];
      this.listNext = null;
    } else if (!this.listLoaded || this.listNext === null || this.listed.length >= MAX_LISTED) {
      return;
    }
    const after = first ? "" : `&after=${encodeURIComponent(this.listNext ?? "")}`;
    const r = await this.get(account, `/v1/live/${stream}/spectators?limit=${LIST_PAGE}${after}`);
    if (this.stream !== stream) {
      return;
    }
    if (!r.ok) {
      this.notice(r.message);
      return;
    }
    const page = names(r.body?.spectators) ?? [];
    const known = new Set(this.listed.map((n) => n.uid));
    this.listed.push(...page.filter((n) => !known.has(n.uid)));
    this.listNext = typeof r.body?.next === "string" ? r.body.next : null;
    if (Number.isInteger(r.body?.count)) {
      this.count = r.body.count;
    }
    this.listLoaded = true;
    await this.deps.writeList({
      stream,
      count: Math.max(this.count, this.listed.length),
      names: this.listed,
      more: this.listNext !== null && this.listed.length < MAX_LISTED,
    });
  }

  private async get(account: string, p: string): Promise<{ ok: true; body: any } | { ok: false; message: string }> {
    return await this.request(account, "GET", p);
  }

  private async post(account: string, p: string, body: unknown) {
    return await this.request(account, "POST", p, body);
  }

  private async request(
    account: string,
    method: "GET" | "POST",
    p: string,
    body?: unknown,
  ): Promise<{ ok: true; body: any } | { ok: false; message: string }> {
    try {
      const token = await this.deps.getToken(account);
      if (!token) {
        return { ok: false, message: "Log in to watch live games." };
      }
      const headers: Record<string, string> = { Authorization: `Bearer ${token}`, "User-Agent": this.deps.userAgent };
      if (body !== undefined) {
        headers["Content-Type"] = "application/json";
      }
      const resp = await this.fetch(`${this.deps.baseUrl}${p}`, {
        method,
        headers,
        body: body === undefined ? undefined : JSON.stringify(body),
      });
      const json = await resp.json().catch(() => null);
      if (!resp.ok) {
        return { ok: false, message: json?.error?.message ?? `HTTP ${resp.status}` };
      }
      return { ok: true, body: json };
    } catch (err) {
      return { ok: false, message: err instanceof Error ? err.message : String(err) };
    }
  }
}
