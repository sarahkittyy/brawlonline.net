import { describe, expect, it } from "vitest";

import type { TraceCommand } from "./traceroute";
import { countHops, traceCommands, TRACEPATH, TRACEROUTE, TRACERT } from "./traceroute";

const hopsIn = (trace: TraceCommand, output: string) =>
  output
    .split("\n")
    .map((l) => trace.hopOf(l))
    .filter((h) => h !== null);

/** A stand-in program: node printing `output` (and then waiting `waitMs`), exiting with `code`. */
const fake = (hopOf: TraceCommand["hopOf"], output: string, code = 0, waitMs = 0): TraceCommand => ({
  command: process.execPath,
  args: () => [
    "-e",
    `process.stdout.write(${JSON.stringify(output)}); setTimeout(() => process.exit(${code}), ${waitMs});`,
  ],
  hopOf,
});

const missing: TraceCommand = { ...TRACEROUTE, command: "pponline-no-such-traceroute" };

describe("hop parsing", () => {
  it("reads traceroute -n", () => {
    const out = [
      "traceroute to 203.0.113.7 (203.0.113.7), 30 hops max, 60 byte packets",
      " 1  192.168.1.1  0.512 ms",
      " 2  *",
      " 3  203.0.113.7  9.871 ms",
    ].join("\n");
    expect(hopsIn(TRACEROUTE, out)).toEqual([1, 2, 3]);
  });

  it("reads tracert -d", () => {
    const out = [
      "Tracing route to 203.0.113.7 over a maximum of 30 hops",
      "",
      "  1    <1 ms    <1 ms    <1 ms  192.168.1.1",
      "  2     *        *        *     Request timed out.",
      "  3    10 ms     9 ms     9 ms  203.0.113.7",
      "",
      "Trace complete.",
    ].join("\n");
    expect(hopsIn(TRACERT, out)).toEqual([1, 2, 3]);
  });

  it("reads tracepath -n, without its LOCALHOST line", () => {
    const out = [
      " 1?: [LOCALHOST]                      pmtu 1500",
      " 1:  192.168.1.1                                           0.517ms",
      " 1:  192.168.1.1                                           0.402ms",
      " 2:  no reply",
      " 3:  203.0.113.7                                           9.112ms reached",
      "     Resume: pmtu 1500 hops 3 back 3",
    ].join("\n");
    expect(hopsIn(TRACEPATH, out)).toEqual([1, 1, 2, 3]);
  });

  it("tries traceroute, then tracepath, except on Windows", () => {
    expect(traceCommands("linux")).toEqual([TRACEROUTE, TRACEPATH]);
    expect(traceCommands("darwin")).toEqual([TRACEROUTE, TRACEPATH]);
    expect(traceCommands("win32")).toEqual([TRACERT]);
  });
});

describe("countHops", () => {
  it("counts each hop once", async () => {
    const out = " 1?: [LOCALHOST] pmtu 1500\n 1:  192.168.1.1  0.5ms\n 1:  192.168.1.1  0.4ms\n 2:  203.0.113.7  9ms\n";
    expect(await countHops("203.0.113.7", { commands: [fake(TRACEPATH.hopOf, out)] })).toEqual({
      hops: 2,
      finished: true,
    });
  });

  it("goes on to the next program when one is not installed", async () => {
    const tracepath = fake(TRACEPATH.hopOf, " 1:  203.0.113.7  0.5ms\n");
    expect(await countHops("203.0.113.7", { commands: [missing, tracepath] })).toEqual({ hops: 1, finished: true });
  });

  it("rejects, without throwing in the process, when none is installed", async () => {
    await expect(
      countHops("203.0.113.7", { commands: [missing, { ...TRACEPATH, command: "pponline-no-tracepath" }] }),
    ).rejects.toThrow("None of pponline-no-such-traceroute, pponline-no-tracepath is installed");
  });

  it("stops a trace that runs too long and counts what it saw", async () => {
    const slow = fake(TRACEROUTE.hopOf, " 1  192.168.1.1  0.5 ms\n 2  100.64.0.1  3.1 ms\n", 0, 30_000);
    const started = Date.now();
    expect(await countHops("203.0.113.7", { commands: [slow], timeoutMs: 1500 })).toEqual({
      hops: 2,
      finished: false,
    });
    expect(Date.now() - started).toBeLessThan(10_000);
  });

  it("rejects a failed trace, one that saw no hop, and anything but an IP address", async () => {
    await expect(countHops("203.0.113.7", { commands: [fake(TRACEROUTE.hopOf, " 1  *\n", 1)] })).rejects.toThrow(
      /exited with 1/,
    );
    await expect(countHops("203.0.113.7", { commands: [fake(TRACEROUTE.hopOf, "nothing\n")] })).rejects.toThrow(
      /saw no hop/,
    );
    await expect(countHops("-f; rm", { commands: [missing] })).rejects.toThrow(/Not an IP address/);
  });
});
