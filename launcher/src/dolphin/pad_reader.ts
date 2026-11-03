/**
 * Settings > Clips > Manual Clip Hotkey's Configure box: the GameCube buttons the player's
 * controllers hold, read by DolphinTool `pads` (Source/Core/DolphinTool/PadsCommand.cpp) with the
 * netplay User folder's controller settings, so a button is the one Dolphin gives the game (an
 * adapter's as it is, another controller's through its mapping). One reader at a time; it stops
 * when asked, when another starts, or after DolphinTool's own 10 minutes.
 */
import type { ChildProcessWithoutNullStreams } from "child_process";
import { spawn } from "child_process";
import electronLog from "electron-log";

import type { PadReaderEvent, PadReaderPort, PadReaderStatus } from "./types";
import { DolphinEventType } from "./types";

const log = electronLog.scope("dolphin/pad_reader");

/** After stdin closes, DolphinTool has this long to go before it is killed. */
const STOP_GRACE_MS = 2000;

/** A DolphinTool `pads` line, or null for anything else. */
export function parsePadsLine(line: string): { buttons: number } | { status: PadReaderStatus } | null {
  let v: unknown;
  try {
    v = JSON.parse(line);
  } catch {
    return null;
  }
  if (typeof v !== "object" || v === null) {
    return null;
  }
  const o = v as Record<string, unknown>;
  if (typeof o.buttons === "number" && Number.isInteger(o.buttons)) {
    return { buttons: o.buttons };
  }
  if (Array.isArray(o.ports) && typeof o.adapter === "string") {
    const ports: PadReaderPort[] = o.ports.flatMap((p: unknown) => {
      const q = (p ?? {}) as Record<string, unknown>;
      return typeof q.port === "number" && (q.kind === "adapter" || q.kind === "controller")
        ? [{ port: q.port, kind: q.kind, device: String(q.device ?? ""), plugged: q.plugged === true }]
        : [];
    });
    const adapter = ["detected", "not_detected", "error", "unused"].includes(o.adapter)
      ? (o.adapter as PadReaderStatus["adapter"])
      : "error";
    return {
      status: { ports, adapter, ...(typeof o.adapter_error === "string" ? { adapterError: o.adapter_error } : {}) },
    };
  }
  return null;
}

export class PadReader {
  private child: ChildProcessWithoutNullStreams | null = null;
  private readerId: string | null = null;

  constructor(
    private readonly toolPath: () => string,
    private readonly userFolder: () => string,
    private readonly emit: (event: PadReaderEvent) => void,
  ) {}

  start(readerId: string): void {
    this.stop();
    const tool = this.toolPath();
    const child = spawn(tool, ["pads", "-u", this.userFolder()], { windowsHide: true });
    this.child = child;
    this.readerId = readerId;
    log.info(`reading pads (${readerId}) with ${tool}`);

    const send = (event: Omit<PadReaderEvent, "type" | "readerId">) =>
      this.emit({ type: DolphinEventType.PAD_READER, readerId, ...event });
    let pending = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => {
      pending += chunk;
      const lines = pending.split("\n");
      pending = lines.pop() ?? "";
      for (const line of lines) {
        const parsed = parsePadsLine(line.trim());
        if (parsed) {
          send(parsed);
        }
      }
    });
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk: string) => {
      stderr = (stderr + chunk).slice(-2000);
    });
    let ended = false;
    const end = (error: string | null) => {
      if (ended) {
        return;
      }
      ended = true;
      if (this.child === child) {
        this.child = null;
        this.readerId = null;
      }
      if (error) {
        log.warn(`pads (${readerId}): ${error}`);
      }
      send({ ended: { error } });
    };
    child.on("error", (err) => end(err.message));
    child.on("exit", (code, signal) => {
      end(code === 0 || signal !== null ? null : stderr.trim() || `DolphinTool exited with ${code}`);
    });
  }

  /** Stops the reader `readerId` (any reader when not given). */
  stop(readerId?: string): void {
    const child = this.child;
    if (!child || (readerId !== undefined && readerId !== this.readerId)) {
      return;
    }
    this.child = null;
    this.readerId = null;
    child.stdin.end();
    const timer = setTimeout(() => child.kill(), STOP_GRACE_MS);
    child.once("exit", () => clearTimeout(timer));
  }
}
