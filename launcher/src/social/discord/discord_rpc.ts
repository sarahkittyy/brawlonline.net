import electronLog from "electron-log";
import net from "net";
import path from "path";

import type { DiscordActivity } from "./types";

const log = electronLog.scope("discord");

/**
 * The Discord app's local RPC (its IPC socket: a named pipe on Windows, a Unix socket elsewhere):
 * a handshake with the application id, then SET_ACTIVITY frames. Each frame is a little-endian
 * opcode and length, then JSON. Nothing goes over the network: the Discord app on this computer
 * shows the activity on the player's profile. Without a Discord app running, nothing happens; the
 * client tries again every RETRY_MS while there is an activity to show.
 */

const OP_HANDSHAKE = 0;
const OP_FRAME = 1;
const OP_CLOSE = 2;
const OP_PING = 3;
const OP_PONG = 4;

/** Between connection attempts while an activity waits to be shown. */
export const RETRY_MS = 15_000;
/** Discord takes 5 activity updates per 20 s: one every 4 s at most, the newest one after the wait. */
export const MIN_SEND_GAP_MS = 4_000;
const READY_TIMEOUT_MS = 5_000;
const MAX_FRAME = 1024 * 1024;

/** Where the Discord app listens: discord-ipc-0..9 (Windows pipes; Unix sockets, Flatpak and Snap too). */
export function ipcPaths(platform: NodeJS.Platform = process.platform, env: NodeJS.ProcessEnv = process.env): string[] {
  const indexes = Array.from({ length: 10 }, (_, i) => i);
  if (platform === "win32") {
    return indexes.map((i) => `\\\\?\\pipe\\discord-ipc-${i}`);
  }
  const roots = [env.XDG_RUNTIME_DIR, env.TMPDIR, env.TMP, env.TEMP, "/tmp"].filter(
    (r, i, all): r is string => !!r && all.indexOf(r) === i,
  );
  const subs = ["", "app/com.discordapp.Discord", "snap.discord", ".flatpak/dev.vencord.Vesktop/xdg-run"];
  const out: string[] = [];
  for (const i of indexes) {
    for (const root of roots) {
      for (const sub of subs) {
        out.push(path.posix.join(root, sub, `discord-ipc-${i}`));
      }
    }
  }
  return out;
}

/** Discord refuses texts under 2 characters and cuts nothing itself: over 128 it refuses the activity. */
function text(s: string | undefined, max = 128): string | undefined {
  if (s == null) {
    return undefined;
  }
  const chars = Array.from(s.trim());
  if (chars.length < 2) {
    return undefined;
  }
  return chars.length > max ? chars.slice(0, max - 1).join("") + "…" : chars.join("");
}

/** The SET_ACTIVITY `activity` for an activity (Discord's field names and limits). */
export function wireActivity(a: DiscordActivity): Record<string, unknown> {
  const assets: Record<string, string> = { large_image: a.largeImage };
  const largeText = text(a.largeText);
  if (largeText) {
    assets.large_text = largeText;
  }
  if (a.smallImage) {
    assets.small_image = a.smallImage;
    const smallText = text(a.smallText);
    if (smallText) {
      assets.small_text = smallText;
    }
  }
  const out: Record<string, unknown> = {
    type: 0,
    // The member list says the details ("Playing Ranked · vs Peachy"), not the app's name (Sarah's B).
    status_display_type: 2,
    assets,
    instance: false,
  };
  const details = text(a.details);
  if (details) {
    out.details = details;
  }
  const state = text(a.state);
  if (state) {
    out.state = state;
  }
  if (a.startedAt != null && Number.isFinite(a.startedAt)) {
    out.timestamps = { start: Math.floor(a.startedAt / 1000) };
  }
  if (a.party && a.party.size > 0 && a.party.max >= a.party.size) {
    out.party = { id: "brawl-online", size: [a.party.size, a.party.max] };
  }
  const buttons = (a.buttons ?? [])
    .map((b) => ({ label: text(b.label, 32), url: b.url }))
    .filter((b): b is { label: string; url: string } => !!b.label && /^https:\/\//.test(b.url) && b.url.length <= 512)
    .slice(0, 2);
  if (buttons.length > 0) {
    out.buttons = buttons;
  }
  return out;
}

export function encodeFrame(op: number, payload: unknown): Buffer {
  const body = Buffer.from(JSON.stringify(payload), "utf8");
  const head = Buffer.alloc(8);
  head.writeUInt32LE(op, 0);
  head.writeUInt32LE(body.length, 4);
  return Buffer.concat([head, body]);
}

/** Splits whole frames off the front of `buf`; returns them and what is left. */
export function decodeFrames(buf: Buffer): { frames: { op: number; data: unknown }[]; rest: Buffer } {
  const frames: { op: number; data: unknown }[] = [];
  let rest = buf;
  while (rest.length >= 8) {
    const op = rest.readUInt32LE(0);
    const len = rest.readUInt32LE(4);
    if (len > MAX_FRAME) {
      throw new Error(`Discord sent a ${len}-byte frame`);
    }
    if (rest.length < 8 + len) {
      break;
    }
    let data: unknown = null;
    try {
      data = JSON.parse(rest.subarray(8, 8 + len).toString("utf8"));
    } catch {
      data = null;
    }
    frames.push({ op, data });
    rest = rest.subarray(8 + len);
  }
  return { frames, rest };
}

type Connect = (path: string) => net.Socket;

export class DiscordRpc {
  private socket: net.Socket | null = null;
  private ready = false;
  private connecting = false;
  /** What should show (null: nothing). */
  private wanted: DiscordActivity | null = null;
  /** The last activity JSON Discord got ("" none yet / cleared). */
  private sent = "";
  private lastSendAt = 0;
  private sendTimer: ReturnType<typeof setTimeout> | null = null;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
  private nonce = 0;
  private closed = false;

  constructor(
    private readonly clientId: string,
    private readonly opts: {
      connect?: Connect;
      paths?: () => string[];
      pid?: number;
      now?: () => number;
    } = {},
  ) {}

  /** Shows `activity` on the player's Discord profile (null: clears it). */
  setActivity(activity: DiscordActivity | null): void {
    if (this.closed) {
      return;
    }
    this.wanted = activity;
    if (this.ready) {
      this.scheduleSend();
    } else if (activity) {
      void this.ensureConnected();
    }
  }

  /** Disconnects for good (the app quits); Discord clears the activity when the connection goes. */
  close(): void {
    this.closed = true;
    this.clearTimers();
    this.socket?.destroy();
    this.socket = null;
    this.ready = false;
  }

  private now(): number {
    return this.opts.now?.() ?? Date.now();
  }

  private clearTimers() {
    if (this.sendTimer) {
      clearTimeout(this.sendTimer);
      this.sendTimer = null;
    }
    if (this.retryTimer) {
      clearTimeout(this.retryTimer);
      this.retryTimer = null;
    }
  }

  private scheduleSend() {
    if (this.sendTimer) {
      return;
    }
    const wait = Math.max(0, this.lastSendAt + MIN_SEND_GAP_MS - this.now());
    this.sendTimer = setTimeout(() => {
      this.sendTimer = null;
      this.sendNow();
    }, wait);
  }

  private sendNow() {
    if (!this.ready || !this.socket) {
      return;
    }
    const activity = this.wanted ? wireActivity(this.wanted) : null;
    const json = activity ? JSON.stringify(activity) : "";
    if (json === this.sent) {
      return;
    }
    const args: Record<string, unknown> = { pid: this.opts.pid ?? process.pid };
    if (activity) {
      args.activity = activity;
    }
    this.socket.write(encodeFrame(OP_FRAME, { cmd: "SET_ACTIVITY", nonce: String(++this.nonce), args }));
    this.sent = json;
    this.lastSendAt = this.now();
  }

  private scheduleRetry() {
    if (this.closed || this.retryTimer || !this.wanted) {
      return;
    }
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      void this.ensureConnected();
    }, RETRY_MS);
  }

  private async ensureConnected(): Promise<void> {
    if (this.closed || this.ready || this.connecting) {
      return;
    }
    this.connecting = true;
    try {
      const paths = this.opts.paths?.() ?? ipcPaths();
      for (const p of paths) {
        if (this.closed) {
          return;
        }
        const socket = await this.tryOpen(p);
        if (!socket) {
          continue;
        }
        if (await this.handshake(socket)) {
          return;
        }
      }
      this.scheduleRetry();
    } finally {
      this.connecting = false;
    }
  }

  private tryOpen(p: string): Promise<net.Socket | null> {
    return new Promise((resolve) => {
      let socket: net.Socket;
      try {
        socket = (this.opts.connect ?? ((q: string) => net.connect(q)))(p);
      } catch {
        resolve(null);
        return;
      }
      const fail = () => {
        socket.destroy();
        resolve(null);
      };
      socket.once("error", fail);
      socket.once("connect", () => {
        socket.off("error", fail);
        resolve(socket);
      });
    });
  }

  private handshake(socket: net.Socket): Promise<boolean> {
    return new Promise((resolve) => {
      let buf: Buffer = Buffer.alloc(0);
      let settled = false;
      const settle = (ok: boolean) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        if (!ok) {
          socket.destroy();
        }
        resolve(ok);
      };
      const timer = setTimeout(() => settle(false), READY_TIMEOUT_MS);
      socket.on("data", (chunk: Buffer) => {
        let frames: { op: number; data: unknown }[];
        try {
          ({ frames, rest: buf } = decodeFrames(Buffer.concat([buf, chunk])));
        } catch (err) {
          log.warn(err);
          socket.destroy();
          return;
        }
        for (const f of frames) {
          this.onFrame(socket, f, settle);
        }
      });
      socket.on("error", (err) => {
        if (settled) {
          log.info(`Discord connection error: ${err.message}`);
        }
      });
      socket.on("close", () => {
        settle(false);
        if (this.socket === socket) {
          this.socket = null;
          this.ready = false;
          this.sent = "";
          if (this.sendTimer) {
            clearTimeout(this.sendTimer);
            this.sendTimer = null;
          }
          log.info("Discord connection closed");
          this.scheduleRetry();
        }
      });
      socket.write(encodeFrame(OP_HANDSHAKE, { v: 1, client_id: this.clientId }));
    });
  }

  private onFrame(socket: net.Socket, f: { op: number; data: unknown }, settle: (ok: boolean) => void) {
    const data = (f.data ?? {}) as { evt?: string; cmd?: string; data?: { message?: string } };
    if (f.op === OP_PING) {
      socket.write(encodeFrame(OP_PONG, f.data));
      return;
    }
    if (f.op === OP_CLOSE) {
      log.info(`Discord closed the connection: ${JSON.stringify(f.data)}`);
      socket.destroy();
      return;
    }
    if (f.op !== OP_FRAME) {
      return;
    }
    if (data.cmd === "DISPATCH" && data.evt === "READY") {
      if (this.closed) {
        settle(false);
        return;
      }
      this.socket = socket;
      this.ready = true;
      this.sent = "";
      log.info("Connected to Discord");
      settle(true);
      this.scheduleSend();
      return;
    }
    if (data.evt === "ERROR") {
      log.warn(`Discord refused ${data.cmd ?? "a command"}: ${data.data?.message ?? JSON.stringify(f.data)}`);
    }
  }
}
