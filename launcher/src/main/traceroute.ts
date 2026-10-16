import { spawn } from "node:child_process";
import { isIP } from "node:net";
import readline from "node:readline";

/** A route tracing program and how to read a hop from its output. */
export type TraceCommand = {
  command: string;
  args: (address: string) => string[];
  /** The hop number a line of its output reports, or null. */
  hopOf: (line: string) => number | null;
};

const lineNumber = (regex: RegExp) => (line: string) => {
  const m = regex.exec(line);
  return m ? Number(m[1]) : null;
};

/** `traceroute -q 1 -n`: " 1  192.168.1.1  0.512 ms" or " 2  *". */
export const TRACEROUTE: TraceCommand = {
  command: "traceroute",
  args: (address) => ["-q", "1", "-z", "0", "-n", address],
  hopOf: lineNumber(/^\s*(\d+)\s+(?:\S+\s+[0-9.]+\s+ms|\*)/),
};

/** `tracert -d`: "  1    <1 ms    <1 ms    <1 ms  192.168.1.1" or "  2     *        *        *     Request timed out." */
export const TRACERT: TraceCommand = {
  command: "tracert",
  args: (address) => ["-d", address],
  hopOf: lineNumber(/^\s*(\d+)\s+(?:<?\d+\s+ms|\*)/),
};

/** `tracepath -n` (iputils, installed where traceroute is not): " 1:  192.168.1.1  0.512ms", not " 1?: [LOCALHOST]  pmtu 1500". */
export const TRACEPATH: TraceCommand = {
  command: "tracepath",
  args: (address) => ["-n", address],
  hopOf: lineNumber(/^\s*(\d+)\??:\s+(?!\[LOCALHOST\])\S/),
};

/** What to try, in order: the first one that is installed is used. */
export function traceCommands(platform: NodeJS.Platform = process.platform): TraceCommand[] {
  if (platform === "win32") {
    return [TRACERT];
  }
  return [TRACEROUTE, TRACEPATH];
}

class NotInstalled extends Error {}

function runTrace(
  trace: TraceCommand,
  address: string,
  timeoutMs: number,
): Promise<{ hops: number; finished: boolean }> {
  return new Promise((resolve, reject) => {
    const hops = new Set<number>();
    const child = spawn(trace.command, trace.args(address), { stdio: ["ignore", "pipe", "ignore"] });
    let settled = false;
    const settle = (fn: () => void) => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        fn();
      }
    };
    // Without this listener a missing program throws in the main process (spawn ... ENOENT).
    child.on("error", (err: NodeJS.ErrnoException) => {
      settle(() => reject(err.code === "ENOENT" ? new NotInstalled(err.message) : err));
    });
    readline.createInterface({ input: child.stdout, terminal: false }).on("line", (line) => {
      const hop = trace.hopOf(line);
      if (hop !== null && hop > 0) {
        hops.add(hop);
      }
    });
    child.on("close", (code) => {
      settle(() =>
        code === 0
          ? resolve({ hops: hops.size, finished: true })
          : reject(new Error(`${trace.command} exited with ${code}`)),
      );
    });
    const timer = setTimeout(() => {
      child.kill();
      settle(() => resolve({ hops: hops.size, finished: false }));
    }, timeoutMs);
  });
}

/**
 * Traces the route to `address` (an IP address) with the first of `commands` that is installed,
 * and returns how many hops it saw. A trace that runs past `timeoutMs` is stopped and counts what
 * it saw by then. Rejects when none is installed, the trace fails, or it saw no hop.
 */
export async function countHops(
  address: string,
  { commands = traceCommands(), timeoutMs = 9000 }: { commands?: TraceCommand[]; timeoutMs?: number } = {},
): Promise<{ hops: number; finished: boolean }> {
  if (isIP(address) === 0) {
    throw new Error(`Not an IP address: ${address}`);
  }
  for (const trace of commands) {
    try {
      const result = await runTrace(trace, address, timeoutMs);
      if (result.hops === 0) {
        throw new Error(`${trace.command} saw no hop`);
      }
      return result;
    } catch (err) {
      if (!(err instanceof NotInstalled)) {
        throw err;
      }
    }
  }
  throw new Error(`None of ${commands.map((c) => c.command).join(", ")} is installed`);
}
