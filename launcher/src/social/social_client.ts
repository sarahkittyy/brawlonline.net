/**
 * The launcher's social connection (docs/social-protocol.md), main process: one server-sent event
 * stream from accounts (`GET /v1/social/stream`) for the active account, which is the player's
 * presence and carries everything for them; sending is plain requests.
 *
 * Everything that arrives is hostile until checked: the server's lists and events are validated
 * field by field, a DM must come from a listed player with the session key it was listed with,
 * open with the pair key, carry a sequence number above the last one from that key and a valid
 * signature by the player's identity key, and its text goes through the chat cleaner. The renderer
 * only ever sees cleaned text.
 */

import { randomUUID } from "crypto";
import electronLog from "electron-log";

import { cleanBytes, cleanText, MAX_NAME_CODE_POINTS } from "./chat_text";
import {
  boxAd,
  dmGroup,
  encodePlaintext,
  fromHex,
  identityPublic,
  isValidIdentityKey,
  kxPublic,
  MAX_BOX_BYTES,
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
  verifyKx,
  verifyMessage,
} from "./crypto";
import { hiddenStamp, isUid, loadIdentitySeed, MAX_HIDDEN, readHidden, writeHidden } from "./shared_files";
import type {
  ChatLine,
  Conversation,
  Invite,
  InviteResult,
  SendResult,
  SocialEvent,
  SocialPlayer,
  SocialRoom,
  SocialState,
} from "./types";

const log = electronLog.scope("social");

/** Lines kept per conversation. */
export const MAX_LINES = 200;
/** Players listed at most. */
export const MAX_ONLINE = 5000;
/** The server comments every 15 s; nothing for this long means the stream is dead. */
export const IDLE_TIMEOUT_MS = 45_000;
/** How long an invite is shown (the server lets it be answered for 120 s). */
export const INVITE_SHOWN_MS = 110_000;
/** How often the ignore list file is checked for the game's changes. */
const HIDDEN_POLL_MS = 2000;
const MAX_EVENT_BYTES = 64 * 1024;
const MAX_WELCOME_BYTES = 4 * 1024 * 1024;
const BACKOFF_MS = [1000, 2000, 5000, 10_000, 20_000, 30_000];

type Peer = {
  player: SocialPlayer;
  idKey: Uint8Array;
  kx: Uint8Array;
  kxSig: Uint8Array;
  /** undefined until checked; false: the session key isn't signed by the identity key. */
  verified?: boolean;
  key?: Uint8Array;
};

export type SocialClientDeps = {
  baseUrl: string;
  userAgent: string;
  getToken: (uid: string) => Promise<string | undefined>;
  onlineDir: () => string;
  fetch?: typeof fetch;
  now?: () => number;
  sleep?: (ms: number, signal: AbortSignal) => Promise<void>;
};

class StreamEnd extends Error {
  constructor(readonly fatal: boolean, message: string, readonly retryAfterMs?: number) {
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

function emptyState(): SocialState {
  return { status: "off", online: [], room: null, ignored: [], conversations: {}, invites: [] };
}

function parsePlayer(v: any): SocialPlayer | undefined {
  if (!v || typeof v !== "object" || !isUid(v.uid) || typeof v.displayName !== "string") {
    return undefined;
  }
  const displayName = cleanText(v.displayName.slice(0, 256), MAX_NAME_CODE_POINTS);
  if (!displayName) {
    return undefined;
  }
  const code = typeof v.connectCode === "string" && /^[A-Z]{1,4}#[0-9]{1,4}$/.test(v.connectCode) ? v.connectCode : "";
  return { uid: v.uid, displayName, connectCode: code };
}

function parsePeer(v: any): Peer | undefined {
  const player = parsePlayer(v);
  const idKey = fromHex(v?.idKey, 32);
  const kx = fromHex(v?.kx, 32);
  const kxSig = fromHex(v?.kxSig, 64);
  if (!player || !idKey || !kx || !kxSig) {
    return undefined;
  }
  return { player, idKey, kx, kxSig };
}

const ROOM_MODES = new Set(["1v1", "ffa", "teams"]);
const ROOM_STATUSES = new Set(["waiting", "starting", "in-game"]);

function parseRoom(v: any): SocialRoom | null | undefined {
  if (v === null) {
    return null;
  }
  if (
    !v ||
    typeof v !== "object" ||
    typeof v.code !== "string" ||
    !/^[BCDFGHJKLMNPQRSTVWXZ]{4}$/.test(v.code) ||
    !Number.isInteger(v.players) ||
    !Number.isInteger(v.openSlots) ||
    v.players < 0 ||
    v.players > 4 ||
    v.openSlots < 0 ||
    v.openSlots > 4 ||
    !ROOM_MODES.has(v.mode) ||
    !ROOM_STATUSES.has(v.status) ||
    typeof v.joinable !== "boolean"
  ) {
    return undefined;
  }
  return {
    code: v.code,
    players: v.players,
    openSlots: v.openSlots,
    mode: v.mode,
    status: v.status,
    joinable: v.joinable,
  };
}

function byName(a: SocialPlayer, b: SocialPlayer) {
  return a.displayName.localeCompare(b.displayName, undefined, { sensitivity: "base" }) || a.uid.localeCompare(b.uid);
}

export class SocialClient {
  private state: SocialState = emptyState();
  private uid?: string;
  private seed?: Uint8Array;
  private kxSecret?: Uint8Array;
  private peers = new Map<string, Peer>();
  /** Next sequence number per conversation, for this stream's session key. */
  private sendSeq = new Map<string, bigint>();
  /** Highest sequence number accepted per `<uid>:<their kx hex>`. */
  private recvSeq = new Map<string, bigint>();
  private run?: AbortController;
  private stamp = "";
  private timers: NodeJS.Timeout[] = [];
  private stateListeners = new Set<(s: SocialState) => void>();
  private eventListeners = new Set<(e: SocialEvent) => void>();
  private emitQueued = false;

  constructor(private readonly deps: SocialClientDeps) {}

  private get fetch(): typeof fetch {
    return this.deps.fetch ?? fetch;
  }

  private now(): number {
    return this.deps.now?.() ?? Date.now();
  }

  getState(): SocialState {
    return this.state;
  }

  onState(fn: (s: SocialState) => void): () => void {
    this.stateListeners.add(fn);
    return () => this.stateListeners.delete(fn);
  }

  onEvent(fn: (e: SocialEvent) => void): () => void {
    this.eventListeners.add(fn);
    return () => this.eventListeners.delete(fn);
  }

  private update(patch: Partial<SocialState>) {
    this.state = { ...this.state, ...patch };
    if (!this.emitQueued) {
      this.emitQueued = true;
      queueMicrotask(() => {
        this.emitQueued = false;
        for (const fn of this.stateListeners) {
          fn(this.state);
        }
      });
    }
  }

  private emit(e: SocialEvent) {
    for (const fn of this.eventListeners) {
      fn(e);
    }
  }

  // ---------------------------------------------------------------- connection

  async connect(uid: string): Promise<void> {
    if (!isUid(uid)) {
      throw new Error("Not a uid");
    }
    if (this.uid === uid && this.run && !this.run.signal.aborted) {
      return;
    }
    this.disconnect();
    this.uid = uid;
    const run = new AbortController();
    this.run = run;
    const dir = this.deps.onlineDir();
    try {
      this.seed = await loadIdentitySeed(dir);
    } catch (err) {
      log.error("chat identity key", err);
      this.update({ status: "off", message: "Can't read the chat key file." });
      return;
    }
    if (run.signal.aborted) {
      return;
    }
    this.stamp = await hiddenStamp(dir);
    this.update({ ...emptyState(), status: "connecting", ignored: await readHidden(dir) });
    this.timers.push(setInterval(() => void this.pollHidden(), HIDDEN_POLL_MS));
    this.timers.push(setInterval(() => this.expireInvites(), 5000));
    void this.loop(run);
  }

  disconnect() {
    this.run?.abort();
    this.run = undefined;
    this.uid = undefined;
    this.timers.forEach(clearInterval);
    this.timers = [];
    this.peers.clear();
    this.sendSeq.clear();
    this.recvSeq.clear();
    this.update(emptyState());
  }

  private async loop(run: AbortController) {
    let attempt = 0;
    while (!run.signal.aborted) {
      let retryAfter: number | undefined;
      try {
        await this.streamOnce(run, () => {
          attempt = 0;
        });
        if (run.signal.aborted) {
          return;
        }
        log.info("social stream ended; reconnecting");
      } catch (err) {
        if (run.signal.aborted) {
          return;
        }
        if (err instanceof StreamEnd && err.fatal) {
          log.info(`social stream stopped: ${err.message}`);
          this.peers.clear();
          this.update({ status: "off", message: err.message, online: [], room: null });
          this.markAllOffline();
          this.run?.abort();
          return;
        }
        retryAfter = err instanceof StreamEnd ? err.retryAfterMs : undefined;
        log.info(`social stream failed: ${err instanceof Error ? err.message : String(err)}`);
      }
      this.peers.clear();
      this.markAllOffline();
      this.update({ status: "reconnecting", online: [] });
      const wait = retryAfter ?? BACKOFF_MS[Math.min(attempt, BACKOFF_MS.length - 1)] * (0.75 + Math.random() / 2);
      attempt++;
      await (this.deps.sleep ?? abortableSleep)(wait, run.signal);
    }
  }

  private async streamOnce(run: AbortController, onWelcome: () => void) {
    const uid = this.uid!;
    const token = await this.deps.getToken(uid);
    if (!token) {
      throw new StreamEnd(true, "Log in to see who's online.");
    }
    const seed = this.seed!;
    this.kxSecret = randomSecret();
    this.sendSeq.clear();
    const kx = kxPublic(this.kxSecret);
    const ctl = new AbortController();
    const abort = () => ctl.abort();
    run.signal.addEventListener("abort", abort, { once: true });
    let idle: NodeJS.Timeout | undefined;
    const resetIdle = () => {
      clearTimeout(idle);
      idle = setTimeout(() => ctl.abort(), IDLE_TIMEOUT_MS);
    };
    try {
      resetIdle();
      const resp = await this.fetch(`${this.deps.baseUrl}/v1/social/stream`, {
        headers: {
          Authorization: `Bearer ${token}`,
          Accept: "text/event-stream",
          "User-Agent": this.deps.userAgent,
          "X-Chat-Key": toHex(identityPublic(seed)),
          "X-Chat-Kx": toHex(kx),
          "X-Chat-Kx-Sig": toHex(signKx(seed, sessionGroup(uid), kx)),
        },
        signal: ctl.signal,
      });
      if (!resp.ok) {
        const body = await resp.json().catch(() => null);
        const message: string = body?.error?.message ?? `HTTP ${resp.status}`;
        if (resp.status === 401) {
          throw new StreamEnd(true, "Log in again to see who's online.");
        }
        if (resp.status === 403 || resp.status === 400) {
          throw new StreamEnd(true, message);
        }
        const retry = Number(resp.headers.get("retry-after"));
        throw new StreamEnd(false, message, Number.isFinite(retry) && retry > 0 ? retry * 1000 : undefined);
      }
      if (!resp.body) {
        throw new StreamEnd(false, "No stream");
      }
      const reader = resp.body.getReader();
      const decoder = new TextDecoder();
      let buf = "";
      let welcomed = false;
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
          if (!welcomed) {
            if (ev?.type !== "welcome") {
              continue;
            }
            welcomed = true;
            onWelcome();
          }
          this.handle(ev);
        }
        if (buf.length > (welcomed ? MAX_EVENT_BYTES : MAX_WELCOME_BYTES)) {
          throw new StreamEnd(false, "Event too large");
        }
      }
    } finally {
      clearTimeout(idle);
      run.signal.removeEventListener("abort", abort);
      ctl.abort();
    }
  }

  // ---------------------------------------------------------------- events

  /** Exposed for tests. */
  handle(ev: any) {
    switch (ev?.type) {
      case "welcome":
        return this.onWelcome(ev);
      case "online": {
        const peer = parsePeer(ev.member);
        if (peer && peer.player.uid !== this.uid) {
          this.peers.set(peer.player.uid, peer);
          this.syncOnline();
        }
        return;
      }
      case "offline":
        if (isUid(ev.uid) && this.peers.delete(ev.uid)) {
          this.syncOnline();
        }
        return;
      case "dm":
        return this.onDm(ev);
      case "invite":
        return this.onInvite(ev);
      case "invite-answer":
        return this.onInviteAnswer(ev);
      case "room": {
        const room = parseRoom(ev.room);
        if (room !== undefined) {
          this.update({ room });
        }
        return;
      }
      case "replaced":
        this.emit({ type: "replaced" });
        throw new StreamEnd(true, "You're signed in on another launcher.");
    }
  }

  private onWelcome(ev: any) {
    if (ev.you !== this.uid) {
      throw new StreamEnd(false, "Welcome for another account");
    }
    this.peers.clear();
    const list = Array.isArray(ev.online) ? ev.online.slice(0, MAX_ONLINE) : [];
    for (const m of list) {
      const peer = parsePeer(m);
      if (peer && peer.player.uid !== this.uid) {
        this.peers.set(peer.player.uid, peer);
      }
    }
    const room = parseRoom(ev.room);
    this.update({ status: "online", message: undefined, you: this.uid, room: room ?? null });
    this.syncOnline();
  }

  private syncOnline() {
    const online = [...this.peers.values()].map((p) => p.player).sort(byName);
    const conversations = { ...this.state.conversations };
    for (const [uid, c] of Object.entries(conversations)) {
      const peer = this.peers.get(uid);
      const player = peer?.player ?? c.player;
      if (c.online !== !!peer || player !== c.player) {
        conversations[uid] = { ...c, online: !!peer, player };
      }
    }
    // Invites from players who left can't be answered.
    const invites = this.state.invites.filter((i) => this.peers.has(i.from.uid));
    this.update({ online, conversations, invites });
  }

  private markAllOffline() {
    const conversations: Record<string, Conversation> = {};
    for (const [uid, c] of Object.entries(this.state.conversations)) {
      conversations[uid] = c.online ? { ...c, online: false } : c;
    }
    this.update({ conversations, invites: [] });
  }

  private peerKey(peer: Peer): Uint8Array | undefined {
    if (peer.verified === undefined) {
      peer.verified =
        isValidIdentityKey(peer.idKey) && verifyKx(peer.idKey, sessionGroup(peer.player.uid), peer.kx, peer.kxSig);
    }
    if (!peer.verified || !this.kxSecret || !this.uid) {
      return undefined;
    }
    peer.key ??= pairKey(dmGroup(this.uid, peer.player.uid), this.kxSecret, peer.kx);
    return peer.key;
  }

  private onDm(ev: any) {
    const me = this.uid;
    const peer = isUid(ev.from) ? this.peers.get(ev.from) : undefined;
    const kx = fromHex(ev.kx, 32);
    const sealed = fromHex(ev.box);
    if (!me || !peer || !kx || !sealed || sealed.length > MAX_BOX_BYTES || toHex(kx) !== toHex(peer.kx)) {
      return;
    }
    const key = this.peerKey(peer);
    if (!key) {
      return;
    }
    const from = peer.player.uid;
    const group = dmGroup(me, from);
    const plain = open(key, boxAd(group, from, me), sealed);
    const parsed = plain && parsePlaintext(plain);
    if (!parsed) {
      return;
    }
    const seqKey = `${from}:${toHex(peer.kx)}`;
    if (parsed.seq <= (this.recvSeq.get(seqKey) ?? 0n)) {
      return;
    }
    if (!verifyMessage(peer.idKey, group, from, parsed.seq, parsed.text, parsed.sig)) {
      return;
    }
    this.recvSeq.set(seqKey, parsed.seq);
    const text = cleanBytes(parsed.text);
    if (!text) {
      return;
    }
    const ignored = this.state.ignored.includes(from);
    this.addLine(peer.player, { id: randomUUID(), own: false, text, at: this.now() }, !ignored);
    if (!ignored) {
      this.emit({ type: "message", uid: from });
    }
  }

  private addLine(player: SocialPlayer, line: ChatLine, unread: boolean) {
    const prev = this.state.conversations[player.uid];
    const lines = [...(prev?.lines ?? []), line].slice(-MAX_LINES);
    const conv: Conversation = {
      player,
      lines,
      unread: (prev?.unread ?? 0) + (unread ? 1 : 0),
      online: this.peers.has(player.uid),
    };
    this.update({ conversations: { ...this.state.conversations, [player.uid]: conv } });
  }

  private onInvite(ev: any) {
    const peer = isUid(ev.from) ? this.peers.get(ev.from) : undefined;
    if (!peer || this.state.ignored.includes(peer.player.uid)) {
      return;
    }
    let invite: Invite;
    if (ev.kind === "room") {
      const room = parseRoom(ev.room);
      if (!room) {
        return;
      }
      invite = { id: randomUUID(), from: peer.player, kind: "room", room, receivedAt: this.now() };
    } else if (ev.kind === "direct") {
      if (ev.connectCode !== peer.player.connectCode || !peer.player.connectCode) {
        return;
      }
      invite = { id: randomUUID(), from: peer.player, kind: "direct", receivedAt: this.now() };
    } else {
      return;
    }
    const invites = [...this.state.invites.filter((i) => i.from.uid !== invite.from.uid), invite];
    this.update({ invites });
    this.emit({ type: "invite", invite });
  }

  private onInviteAnswer(ev: any) {
    const peer = isUid(ev.by) ? this.peers.get(ev.by) : undefined;
    if (!peer || (ev.kind !== "room" && ev.kind !== "direct") || typeof ev.accept !== "boolean") {
      return;
    }
    if (ev.connectCode !== peer.player.connectCode) {
      return;
    }
    this.emit({ type: "invite-answer", player: peer.player, kind: ev.kind, accept: ev.accept });
  }

  private expireInvites() {
    const now = this.now();
    const invites = this.state.invites.filter((i) => now - i.receivedAt < INVITE_SHOWN_MS);
    if (invites.length !== this.state.invites.length) {
      this.update({ invites });
    }
  }

  // ---------------------------------------------------------------- ignore list

  private async pollHidden() {
    const dir = this.deps.onlineDir();
    const stamp = await hiddenStamp(dir);
    if (stamp === this.stamp) {
      return;
    }
    this.stamp = stamp;
    this.applyIgnored(await readHidden(dir));
  }

  private applyIgnored(ignored: string[]) {
    const invites = this.state.invites.filter((i) => !ignored.includes(i.from.uid));
    const conversations = { ...this.state.conversations };
    for (const uid of ignored) {
      if (conversations[uid]?.unread) {
        conversations[uid] = { ...conversations[uid], unread: 0 };
      }
    }
    this.update({ ignored, invites, conversations });
  }

  async setIgnored(uid: string, ignored: boolean): Promise<void> {
    if (!isUid(uid) || uid === this.uid) {
      return;
    }
    const dir = this.deps.onlineDir();
    // The game may have changed the file since the last look.
    const current = await readHidden(dir);
    const next = ignored
      ? [...current.filter((u) => u !== uid), uid].slice(-MAX_HIDDEN)
      : current.filter((u) => u !== uid);
    await writeHidden(dir, next);
    this.stamp = await hiddenStamp(dir);
    this.applyIgnored(next);
  }

  markRead(uid: string) {
    const c = this.state.conversations[uid];
    if (c?.unread) {
      this.update({ conversations: { ...this.state.conversations, [uid]: { ...c, unread: 0 } } });
    }
  }

  // ---------------------------------------------------------------- sending

  private async post(path: string, body: unknown): Promise<{ ok: true; json: any } | { ok: false; message: string }> {
    const uid = this.uid;
    const token = uid && (await this.deps.getToken(uid));
    if (!token) {
      return { ok: false, message: "Log in again." };
    }
    try {
      const resp = await this.fetch(`${this.deps.baseUrl}${path}`, {
        method: "POST",
        headers: {
          Authorization: `Bearer ${token}`,
          "Content-Type": "application/json",
          "User-Agent": this.deps.userAgent,
        },
        body: JSON.stringify(body),
        signal: AbortSignal.timeout(10_000),
      });
      const json = await resp.json().catch(() => null);
      if (!resp.ok) {
        return { ok: false, message: json?.error?.message ?? `Something went wrong (HTTP ${resp.status}).` };
      }
      return { ok: true, json };
    } catch {
      return { ok: false, message: "Can't reach the server." };
    }
  }

  async sendMessage(to: string, typed: string): Promise<SendResult> {
    const me = this.uid;
    const peer = isUid(to) ? this.peers.get(to) : undefined;
    if (!me || this.state.status !== "online") {
      return { ok: false, message: "You're not connected." };
    }
    if (!peer) {
      return { ok: false, message: "That player is offline." };
    }
    const text = cleanText(typeof typed === "string" ? typed : "");
    if (!text) {
      return { ok: false, message: "Nothing to send." };
    }
    const key = this.peerKey(peer);
    if (!key) {
      return { ok: false, message: "Can't message that player." };
    }
    const group = dmGroup(me, to);
    const seq = (this.sendSeq.get(to) ?? 0n) + 1n;
    this.sendSeq.set(to, seq);
    const bytes = new TextEncoder().encode(text);
    const sig = signMessage(this.seed!, group, me, seq, bytes);
    const sealed = seal(key, randomNonce(), boxAd(group, me, to), encodePlaintext(seq, sig, bytes));
    const res = await this.post("/v1/social/dm", { to, box: toHex(sealed) });
    if (!res.ok) {
      return res;
    }
    this.addLine(peer.player, { id: randomUUID(), own: true, text, at: this.now() }, false);
    return { ok: true };
  }

  async invite(to: string): Promise<InviteResult> {
    if (!isUid(to) || !this.peers.has(to)) {
      return { ok: false, message: "That player is offline." };
    }
    const res = await this.post("/v1/social/invite", { to });
    if (!res.ok) {
      return res;
    }
    return { ok: true, kind: res.json?.kind === "room" ? "room" : "direct" };
  }

  async answerInvite(id: string, accept: boolean): Promise<SendResult> {
    const invite = this.state.invites.find((i) => i.id === id);
    if (!invite) {
      return { ok: false, message: "That invite has expired." };
    }
    this.update({ invites: this.state.invites.filter((i) => i.id !== id) });
    const res = await this.post("/v1/social/invite/answer", { from: invite.from.uid, accept });
    return res.ok ? { ok: true } : res;
  }

  dismissInvite(id: string) {
    this.update({ invites: this.state.invites.filter((i) => i.id !== id) });
  }
}
