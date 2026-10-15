import type { ReplayCommunication, ReplayPlaybackStatus } from "@dolphin/types";
import log from "electron-log";
import { mkdir } from "fs/promises";
import path from "path";

import { LiveGameFile } from "./live_game_file";
import type { LiveGameEntry, LiveList, SpectateState, WatchResult } from "./types";

/**
 * Watching a live game (docs/design/spectating.md §3.3, §6.2; docs/spectate-protocol.md §3).
 *
 * The launcher holds the watch stream (server-sent events from accounts) and writes each game of
 * the stream to `<playback User folder>/Spectate/<stream>/<place>.brep` as its chunks come
 * (LiveGameFile). The playback Dolphin follows those files in `mirror` mode through its comm
 * file: the games so far, whether the stream is over, and what to do at its end. The files are
 * the spectator's cache: seeking back reads them, never the server.
 *
 * Joining starts at the game being played (`?game=<place>`); a dropped stream reconnects from
 * the chunk after the newest one. At the stream's end the playback Dolphin holds the last frame
 * and, with "Close Dolphin after a spectated game ends", counts down and asks to be closed. Auto
 * play takes the place of both: it switches to the newest live game at once, or waits for one.
 */

/** No event (not even the server's 15 s ping) for this long: reconnect. */
const IDLE_TIMEOUT_MS = 45_000;
const BACKOFF_MS = [1000, 2000, 5000, 10_000];
/** How often playback's status is read while watching. */
const STATUS_POLL_MS = 500;
/** Auto play with no live game: how often the list is asked again. */
const WAIT_POLL_MS = 3000;
/** A watch event is at most a chunk's base64 (64 KiB → ~88 KiB) and its JSON. */
const MAX_EVENT_BYTES = 256 * 1024;

export type SpectateClientDeps = {
  baseUrl: string;
  userAgent: string;
  getToken: (uid: string) => Promise<string | undefined>;
  /** `<playback User folder>/Spectate`. */
  spectateDir: () => string;
  /** Plays (or updates, under the same commandId) a request in the playback Dolphin. */
  play: (comm: ReplayCommunication) => Promise<void>;
  playbackStatus: () => Promise<ReplayPlaybackStatus | null>;
  isPlaybackOpen: () => boolean;
  closePlayback: () => Promise<void>;
  /** The launcher setting "Close Dolphin after a spectated game ends". */
  closeOnEnd: () => boolean;
  fetch?: typeof fetch;
  sleep?: (ms: number, signal: AbortSignal) => Promise<void>;
};

class StreamEnd extends Error {
  constructor(readonly fatal: boolean, message: string) {
    super(message);
  }
}

function abortableSleep(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    const t = setTimeout(resolve, ms);
    signal.addEventListener(
      "abort",
      () => {
        clearTimeout(t);
        resolve();
      },
      { once: true },
    );
  });
}

/**
 * The live game Auto play would watch: not `skip`, none `uid` plays in, not over. The newest game being played; only
 * if none is, the newest waiting for its set's next game (which may be a while: stage striking, or a set that ended).
 */
export function pickAutoPlay(list: LiveList, skip: string | null, uid: string): LiveGameEntry | null {
  const candidates = list.games.filter(
    (g) => g.id !== skip && g.game.state !== "over" && !g.players.some((p) => p.uid === uid),
  );
  const between = (g: LiveGameEntry) => (g.game.state === "between" ? 1 : 0);
  candidates.sort((a, b) => between(a) - between(b) || Date.parse(b.game.startedAt) - Date.parse(a.game.startedAt));
  return candidates[0] ?? null;
}

/**
 * The game to start watching from: the one being played, or the next one between games. An ended stream is
 * watched from its first game, like a set's replay (the server keeps it for 10 minutes).
 */
export function startPlace(entry: LiveGameEntry): number | null {
  const place = entry.game.place ?? 0;
  if (!place || entry.game.state === "over") {
    return null; // from the stream's start
  }
  return entry.game.state === "between" ? place + 1 : place;
}

/** One stream being watched. */
export type Watch = {
  uid: string;
  stream: string;
  dir: string;
  commandId: string;
  run: AbortController;
  /** The games' files, by their place in the stream, and the comm file's queue. */
  files: Map<number, LiveGameFile>;
  paths: string[];
  /** Games whose rest won't come (the stream said `bad`, or it ended before them). */
  cut: Set<string>;
  /** The newest chunk received (where a reconnect continues). */
  game: number | null;
  chunk: number;
  over: boolean;
  /** Playback asked to be closed, or was closed. */
  done: boolean;
  /** Why the playback Dolphin could not be opened (the watch then stopped). */
  failed?: string;
  /** The first connection's outcome, for watch(). */
  first?: (r: WatchResult) => void;
  /** Writes to the files and the comm file, one at a time. */
  queue: Promise<void>;
};

export class SpectateClient {
  private state: SpectateState = {
    status: "idle",
    stream: null,
    entry: null,
    spectators: 0,
    autoPlay: false,
    waitingForGame: false,
  };
  private current: Watch | null = null;
  private statusTimer: NodeJS.Timeout | null = null;
  private waitTimer: NodeJS.Timeout | null = null;
  private listeners = new Set<(s: SpectateState) => void>();
  private playbackSeen = false;

  constructor(private readonly deps: SpectateClientDeps) {}

  private get fetch(): typeof fetch {
    return this.deps.fetch ?? fetch;
  }

  getState(): SpectateState {
    return this.state;
  }

  onState(handle: (s: SpectateState) => void): () => void {
    this.listeners.add(handle);
    return () => this.listeners.delete(handle);
  }

  private update(patch: Partial<SpectateState>) {
    this.state = { ...this.state, ...patch };
    for (const l of this.listeners) {
      l(this.state);
    }
  }

  setAutoPlay(on: boolean) {
    this.update({ autoPlay: on });
    const w = this.current;
    if (w) {
      // "Close Dolphin…" applies again (or not) from the stream's next comm update.
      this.enqueue(w, () => this.writeComm(w));
    }
  }

  async list(uid: string): Promise<LiveList> {
    return await this.get<LiveList>(uid, "/v1/live");
  }

  private async get<T>(uid: string, p: string): Promise<T> {
    const token = await this.deps.getToken(uid);
    if (!token) {
      throw new Error("Log in to see live games.");
    }
    const resp = await this.fetch(`${this.deps.baseUrl}${p}`, {
      headers: { Authorization: `Bearer ${token}`, "User-Agent": this.deps.userAgent },
    });
    const body = await resp.json().catch(() => null);
    if (!resp.ok) {
      throw new Error(body?.error?.message ?? `HTTP ${resp.status}`);
    }
    return body as T;
  }

  /** Watches `stream`: switches the playback Dolphin to it (opening it if needed). */
  async watch(uid: string, stream: string): Promise<WatchResult> {
    if (!/^[0-9a-f]{12}$/.test(stream)) {
      return { ok: false, message: "That isn't a live game." };
    }
    let entry: LiveGameEntry | undefined;
    try {
      entry = (await this.list(uid)).games.find((g) => g.id === stream);
    } catch (err) {
      return { ok: false, message: err instanceof Error ? err.message : String(err) };
    }
    if (!entry) {
      return { ok: false, message: "This game has ended." };
    }
    if (entry.players.some((p) => p.uid === uid)) {
      return { ok: false, message: "You're playing in this game." };
    }
    this.stopWatching();
    this.clearWait();
    const dir = path.join(this.deps.spectateDir(), stream);
    await mkdir(dir, { recursive: true });
    const from = startPlace(entry);
    const w: Watch = {
      uid,
      stream,
      dir,
      commandId: `spectate-${stream}-${Date.now().toString(36)}`,
      run: new AbortController(),
      files: new Map(),
      paths: [path.join(dir, `${from ?? 1}.brep`)],
      cut: new Set(),
      game: null,
      chunk: -1,
      over: false,
      done: false,
      queue: Promise.resolve(),
    };
    if (from !== null) {
      w.game = from;
    }
    this.current = w;
    this.playbackSeen = false;
    this.update({
      status: "connecting",
      stream,
      entry,
      spectators: entry.spectators ?? 0,
      message: undefined,
      waitingForGame: false,
    });
    const result = new Promise<WatchResult>((resolve) => {
      w.first = resolve;
    });
    void this.loop(w);
    const r = await result;
    if (!r.ok) {
      return r;
    }
    // The playback Dolphin starts (or switches) now, while the game's first chunks arrive: it
    // waits for them. The click waits for it too, so a Dolphin that can't open is said there
    // (bug report #19: the tab said Watching while every start failed).
    await new Promise<void>((resolve) => this.enqueue(w, () => this.writeComm(w).finally(resolve)));
    if (w.failed !== undefined) {
      return { ok: false, message: w.failed };
    }
    this.watchStatus();
    return r;
  }

  /** Stops watching (the playback Dolphin stays as it is). */
  stop() {
    this.stopWatching();
    this.clearWait();
    this.update({ status: "idle", stream: null, entry: null, spectators: 0, waitingForGame: false });
  }

  private stopWatching() {
    const w = this.current;
    if (!w) {
      return;
    }
    this.current = null;
    w.run.abort();
    w.first?.({ ok: false, message: "Stopped." });
    w.first = undefined;
    this.enqueue(w, async () => {
      for (const f of w.files.values()) {
        await f.close().catch(() => undefined);
      }
    });
  }

  private enqueue(w: Watch, job: () => Promise<void>) {
    w.queue = w.queue.then(job).catch((err) => log.warn(`spectate: ${err instanceof Error ? err.message : err}`));
  }

  private comm(w: Watch): ReplayCommunication {
    const autoPlay = this.state.autoPlay;
    return {
      mode: "mirror",
      commandId: w.commandId,
      queue: w.paths.map((p) => (w.cut.has(p) ? { path: p, cut: true } : { path: p })),
      streamOver: w.over,
      // Auto play overrides "Close Dolphin after a spectated game ends" (the user, 2026-10-13).
      closeOnEnd: this.deps.closeOnEnd() && !autoPlay,
      banner: this.state.waitingForGame ? "Waiting for the next live game..." : undefined,
    };
  }

  private async writeComm(w: Watch) {
    if (w.done || this.current !== w) {
      return; // stopped, or another stream is watched: it must not open or switch playback
    }
    if (this.playbackSeen && !this.deps.isPlaybackOpen()) {
      return; // closed: a comm write would open it again
    }
    try {
      await this.deps.play(this.comm(w));
    } catch (err) {
      // Stop: each later event would only try to open it again.
      const message = err instanceof Error ? err.message : String(err);
      log.warn(`spectate: could not open the playback Dolphin: ${message}`);
      if (this.current === w) {
        // Not for a watch another click already replaced: that click says how its own went.
        w.failed = message;
        this.stopWatching();
        this.update({ status: "error", message });
      }
    }
  }

  // ------------------------------------------------------------- the watch stream

  private async loop(w: Watch) {
    let attempt = 0;
    while (!w.run.signal.aborted && !w.over) {
      try {
        await this.streamOnce(w, () => {
          attempt = 0;
        });
        if (w.run.signal.aborted || w.over) {
          break;
        }
        log.info("spectate: watch stream ended; reconnecting");
      } catch (err) {
        if (w.run.signal.aborted) {
          break;
        }
        const message = err instanceof Error ? err.message : String(err);
        if (err instanceof StreamEnd && err.fatal) {
          log.info(`spectate: ${message}`);
          if (w.first) {
            w.first({ ok: false, message });
            w.first = undefined;
            if (this.current === w) {
              this.current = null;
              this.update({ status: "error", message });
            }
            return;
          }
          // Gone after it was watched: what arrived is all there is.
          this.endStream(w, message);
          return;
        }
        log.info(`spectate: watch stream failed: ${message}`);
      }
      if (this.current === w) {
        this.update({ status: "reconnecting" });
      }
      const wait = BACKOFF_MS[Math.min(attempt, BACKOFF_MS.length - 1)];
      attempt++;
      await (this.deps.sleep ?? abortableSleep)(wait, w.run.signal);
    }
  }

  private async streamOnce(w: Watch, onWelcome: () => void) {
    const token = await this.deps.getToken(w.uid);
    if (!token) {
      throw new StreamEnd(true, "Log in to watch live games.");
    }
    const query = w.game === null ? "" : `?game=${w.game}&chunk=${w.chunk + 1}`;
    const ctl = new AbortController();
    const abort = () => ctl.abort();
    w.run.signal.addEventListener("abort", abort, { once: true });
    let idle: NodeJS.Timeout | undefined;
    const resetIdle = () => {
      clearTimeout(idle);
      idle = setTimeout(() => ctl.abort(), IDLE_TIMEOUT_MS);
    };
    try {
      resetIdle();
      const resp = await this.fetch(`${this.deps.baseUrl}/v1/live/${w.stream}/watch${query}`, {
        headers: { Authorization: `Bearer ${token}`, Accept: "text/event-stream", "User-Agent": this.deps.userAgent },
        signal: ctl.signal,
      });
      if (!resp.ok) {
        const body = await resp.json().catch(() => null);
        const message: string = body?.error?.message ?? `HTTP ${resp.status}`;
        throw new StreamEnd([401, 403, 404].includes(resp.status), message);
      }
      if (!resp.body) {
        throw new StreamEnd(false, "No stream");
      }
      const reader = resp.body.getReader();
      const decoder = new TextDecoder();
      let buf = "";
      for (;;) {
        const { done, value } = await reader.read();
        if (done) {
          return;
        }
        resetIdle();
        buf += decoder.decode(value, { stream: true });
        buf = buf.replace(/\r\n?/g, "\n");
        let end: number;
        while ((end = buf.indexOf("\n\n")) >= 0) {
          const block = buf.slice(0, end);
          buf = buf.slice(end + 2);
          const data = block
            .split("\n")
            .filter((l) => l.startsWith("data:"))
            .map((l) => l.slice(5).replace(/^ /, ""))
            .join("\n");
          if (!data) {
            continue;
          }
          let ev: any;
          try {
            ev = JSON.parse(data);
          } catch {
            continue;
          }
          if (ev?.type === "welcome") {
            onWelcome();
          }
          this.handle(w, ev);
          if (w.over) {
            return;
          }
        }
        if (buf.length > MAX_EVENT_BYTES) {
          throw new StreamEnd(false, "Event too large");
        }
      }
    } finally {
      clearTimeout(idle);
      w.run.signal.removeEventListener("abort", abort);
      ctl.abort();
    }
  }

  /** One watch event (exposed for tests). */
  handle(w: Watch, ev: any) {
    const live = this.current === w;
    switch (ev?.type) {
      case "welcome": {
        w.first?.({ ok: true });
        w.first = undefined;
        if (live) {
          const entry = ev.stream && typeof ev.stream === "object" ? (ev.stream as LiveGameEntry) : this.state.entry;
          this.update({ status: "watching", entry, spectators: entry?.spectators ?? this.state.spectators });
        }
        return;
      }
      case "spectators":
        if (live && Number.isInteger(ev.count)) {
          this.update({ spectators: ev.count });
        }
        return;
      case "game": {
        const place = ev.game;
        if (!Number.isInteger(place) || place < 1 || typeof ev.uid !== "string") {
          return;
        }
        w.game = place;
        w.chunk = -1;
        const file = path.join(w.dir, `${place}.brep`);
        this.enqueue(w, async () => {
          // A game before it that never ended was cut short (its uploader left).
          for (const [n, f] of w.files) {
            if (n < place && !f.ended) {
              await f.close();
              w.cut.add(f.path);
            }
          }
          w.files.set(place, await LiveGameFile.create(file, ev.uid));
          if (!w.paths.includes(file)) {
            if (w.files.size === 1 && w.paths.length === 1) {
              // The first game isn't the one expected (the list was a second old): start over
              // with it under a new request.
              w.paths = [file];
              w.commandId = `${w.commandId}-r`;
            } else {
              w.paths.push(file);
            }
            await this.writeComm(w);
          }
        });
        return;
      }
      case "chunk": {
        if (!Number.isInteger(ev.game) || !Number.isInteger(ev.i) || typeof ev.b !== "string") {
          return;
        }
        w.game = ev.game;
        w.chunk = ev.i;
        const bytes = Buffer.from(ev.b, "base64");
        this.enqueue(w, async () => {
          await w.files.get(ev.game)?.append(bytes);
        });
        return;
      }
      case "end":
        this.enqueue(w, async () => {
          await w.files.get(ev.game)?.end();
        });
        return;
      case "bad":
        // The rest of the game won't come: playback plays what there is, then the next game.
        log.warn(`spectate: game ${ev.game} can't be shown further: ${ev.error}`);
        this.enqueue(w, async () => {
          const f = w.files.get(ev.game);
          if (f && !f.ended) {
            await f.close();
            w.cut.add(f.path);
            await this.writeComm(w);
          }
        });
        return;
      case "over":
        this.endStream(w, undefined);
        return;
    }
  }

  private endStream(w: Watch, message: string | undefined) {
    w.over = true;
    w.run.abort();
    this.enqueue(w, async () => {
      for (const f of w.files.values()) {
        if (!f.ended) {
          await f.close();
          w.cut.add(f.path);
        }
      }
      await this.writeComm(w);
    });
    if (this.current === w) {
      this.update({ status: "over", message });
    }
  }

  // ------------------------------------------------------------- playback

  private watchStatus() {
    if (this.statusTimer) {
      return;
    }
    this.statusTimer = setInterval(() => void this.checkPlayback(), STATUS_POLL_MS);
  }

  private stopStatus() {
    if (this.statusTimer) {
      clearInterval(this.statusTimer);
      this.statusTimer = null;
    }
  }

  private async checkPlayback() {
    const w = this.current;
    if (!w) {
      this.stopStatus();
      return;
    }
    const open = this.deps.isPlaybackOpen();
    if (open) {
      this.playbackSeen = true;
    } else if (this.playbackSeen) {
      // The spectator closed the playback Dolphin: nothing more to download.
      log.info("spectate: playback closed; stopped watching");
      this.stop();
      return;
    }
    const st = await this.deps.playbackStatus();
    if (!st || st.commandId !== w.commandId) {
      return;
    }
    if (st.state === "close") {
      w.done = true;
      this.stop();
      await this.deps.closePlayback();
      return;
    }
    if (w.over && st.state === "ended" && this.state.autoPlay && !this.waitTimer) {
      await this.autoPlay(w);
    }
  }

  /** Auto play at the stream's end: the newest live game at once, or wait for one. */
  private async autoPlay(w: Watch) {
    let next: LiveGameEntry | null = null;
    try {
      next = pickAutoPlay(await this.list(w.uid), w.stream, w.uid);
    } catch (err) {
      log.warn(`spectate: auto play: ${err instanceof Error ? err.message : err}`);
    }
    if (next) {
      log.info(`spectate: auto play: ${next.id}`);
      const r = await this.watch(w.uid, next.id);
      if (r.ok) {
        return;
      }
    }
    if (!this.state.waitingForGame) {
      this.update({ waitingForGame: true });
      this.enqueue(w, () => this.writeComm(w));
    }
    this.waitTimer = setTimeout(() => {
      this.waitTimer = null;
      if (this.current === w && this.state.autoPlay) {
        void this.checkPlayback();
      } else if (this.state.waitingForGame) {
        this.update({ waitingForGame: false });
      }
    }, WAIT_POLL_MS);
  }

  private clearWait() {
    if (this.waitTimer) {
      clearTimeout(this.waitTimer);
      this.waitTimer = null;
    }
  }
}
