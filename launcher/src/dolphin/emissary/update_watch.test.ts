import { describe, expect, it } from "vitest";

import type { GameStatus } from "../room_handoff";
import type { EmissaryState } from "./types";
import { EmissaryUpdateWatch, readEmissaryUpdate, REQUEST_MAX_AGE_MS, UPDATE_NOW_WAIT_MS } from "./update_watch";

const NOW = Date.parse("2026-11-26T12:00:00Z");

function gameStatus(update?: object): GameStatus {
  return {
    version: 1,
    state: "idle",
    updatedAt: new Date(NOW).toISOString(),
    ...(update ? { emissaryUpdate: update } : {}),
  } as GameStatus;
}

function state(over: Partial<EmissaryState>): EmissaryState {
  return {
    status: "running",
    folder: "",
    supported: true,
    installed: { version: "v0.7.1", programBytes: 1, installedAt: "", playsOnline: true, brawlOnline: null },
    gameFiles: { bytes: 1 },
    gameFilesCurrent: true,
    discReady: true,
    latest: null,
    latestError: null,
    upToDate: true,
    downloadBytes: 0,
    diskBytes: 0,
    staged: null,
    job: null,
    running: true,
    error: null,
    ...over,
  };
}

const latest = (version: string) => ({
  version,
  assetName: "",
  url: "",
  size: 1,
  sha256: null,
  manifestUrl: null,
  manifest: null,
});

function rig(opts: { auto?: boolean; idleAfter?: number; stateAt?: (n: number) => EmissaryState | null } = {}) {
  let t = NOW;
  const calls: string[] = [];
  let status: GameStatus | null = gameStatus();
  let current = state({});
  let running = true;
  let closeTries = 0;
  const watch = new EmissaryUpdateWatch({
    running: () => running,
    status: async () => status,
    state: async (refresh) => {
      calls.push(`state ${refresh}`);
      const n = calls.filter((c) => c.startsWith("state")).length;
      const at = opts.stateAt?.(n);
      if (at) {
        current = at;
      }
      return current;
    },
    install: async () => {
      calls.push("install");
      current = { ...current, staged: current.latest?.version ?? null, upToDate: true };
      return current;
    },
    autoUpdate: () => opts.auto ?? true,
    closeForUpdate: async () => {
      closeTries++;
      calls.push("close");
      if (closeTries > (opts.idleAfter ?? 0)) {
        running = false;
        return true;
      }
      return false;
    },
    relaunch: () => calls.push("relaunch"),
    log: () => undefined,
    now: () => t,
    sleep: async (ms) => {
      t += ms;
    },
  });
  return {
    watch,
    calls,
    setStatus: (s: GameStatus | null) => (status = s),
    setState: (s: EmissaryState) => (current = s),
    setRunning: (r: boolean) => (running = r),
    advance: (ms: number) => (t += ms),
  };
}

describe("readEmissaryUpdate", () => {
  it("takes a version and the time Update Now was pressed", () => {
    expect(readEmissaryUpdate(gameStatus({ version: "v0.7.2", requested: null }))).toEqual({
      version: "v0.7.2",
      requested: null,
    });
    expect(readEmissaryUpdate(gameStatus({ version: "v0.7.2", requested: "2026-11-26T12:00:00Z" }))?.requested).toBe(
      "2026-11-26T12:00:00Z",
    );
    expect(readEmissaryUpdate(gameStatus())).toBeNull();
    expect(readEmissaryUpdate(gameStatus({ version: "0.7.2" }))).toBeNull();
    expect(readEmissaryUpdate(gameStatus({ version: "v0.7.2; rm" }))).toBeNull();
    expect(readEmissaryUpdate(null)).toBeNull();
  });
});

describe("EmissaryUpdateWatch", () => {
  it("looks for a release the game heard of at once, and downloads it when it updates by itself", async () => {
    const r = rig();
    await r.watch.tick();
    expect(r.calls).toEqual([]);
    r.setState(state({ latest: latest("v0.7.2"), upToDate: false }));
    r.setStatus(gameStatus({ version: "v0.7.2", requested: null }));
    await r.watch.tick();
    expect(r.calls).toEqual(["state true", "install"]);
    // Seen once: not again.
    await r.watch.tick();
    expect(r.calls).toEqual(["state true", "install"]);
  });

  it("only looks without Update automatically", async () => {
    const r = rig({ auto: false });
    r.setState(state({ latest: latest("v0.7.2"), upToDate: false }));
    r.setStatus(gameStatus({ version: "v0.7.2", requested: null }));
    await r.watch.tick();
    expect(r.calls).toEqual(["state true"]);
  });

  it("Update Now downloads, closes the idle game and plays again", async () => {
    const r = rig({ auto: false, idleAfter: 2 });
    r.setState(state({ latest: latest("v0.7.2"), upToDate: false }));
    r.setStatus(gameStatus({ version: "v0.7.2", requested: new Date(NOW).toISOString() }));
    await r.watch.tick();
    await r.watch.idle();
    expect(r.calls).toEqual(["state true", "state true", "install", "close", "close", "close", "relaunch"]);
  });

  it("Update Now waits for GitHub to list the release", async () => {
    // GitHub's cache still says v0.7.1 for the first reads after the game's Update Now.
    const r = rig({ stateAt: (n) => (n === 5 ? state({ latest: latest("v0.7.2"), upToDate: false }) : null) });
    r.setStatus(gameStatus({ version: "v0.7.2", requested: new Date(NOW).toISOString() }));
    await r.watch.tick();
    await r.watch.idle();
    expect(r.calls.filter((c) => c === "state true")).toHaveLength(5);
    expect(r.calls.filter((c) => c === "install")).toHaveLength(1);
    expect(r.calls.at(-1)).toBe("relaunch");
  });

  it("gives up on a game that stays busy, and leaves a closed one closed", async () => {
    const r = rig({ idleAfter: 1_000_000 });
    r.setState(state({ staged: "v0.7.2" }));
    r.setStatus(gameStatus({ version: "v0.7.2", requested: new Date(NOW).toISOString() }));
    await r.watch.tick();
    await r.watch.idle();
    expect(r.calls).not.toContain("relaunch");
    expect(r.calls.filter((c) => c === "close").length).toBeGreaterThan(10);

    // Closed by the player while it waited: the staged release applies; nothing starts.
    const closed = rig({ idleAfter: 1_000_000, stateAt: (n) => (n === 3 ? (closed.setRunning(false), null) : null) });
    closed.setState(state({ latest: latest("v0.7.2"), upToDate: false }));
    closed.setStatus(gameStatus({ version: "v0.7.2", requested: new Date(NOW).toISOString() }));
    await closed.watch.tick();
    await closed.watch.idle();
    expect(closed.calls).not.toContain("relaunch");
    expect(UPDATE_NOW_WAIT_MS).toBe(10 * 60 * 1000);
  });

  it("leaves an old Update Now alone when the launcher starts after it", async () => {
    const r = rig();
    r.setState(state({ staged: "v0.7.2" }));
    r.setStatus(gameStatus({ version: "v0.7.2", requested: new Date(NOW - REQUEST_MAX_AGE_MS - 1000).toISOString() }));
    await r.watch.tick();
    await r.watch.idle();
    expect(r.calls).not.toContain("close");
  });
});
