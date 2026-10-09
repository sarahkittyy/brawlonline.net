import { mkdtemp, readFile, rm } from "node:fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { AccountsManager } from "./accounts_manager";
import { AccountsHttpClient } from "./client";
import { resolveServiceUrls } from "./config";
import type { Cipher } from "./session_store";
import { SessionStore } from "./session_store";
import type { AccountsMe } from "./types";

const UID = "0b6f2a5e-0000-4000-8000-000000000001";

const me = (overrides: Partial<AccountsMe> = {}): AccountsMe => ({
  uid: UID,
  email: "alice@example.test",
  emailVerified: true,
  emailVerificationRequired: true,
  displayName: "alice",
  connectCode: "ALIC#1",
  playKey: "pk",
  latestVersion: "0.1.0",
  role: "user",
  userJson: { uid: UID, playKey: "pk", connectCode: "ALIC#1", displayName: "alice", latestVersion: "0.1.0" },
  ...overrides,
});

type Call = { url: string; method: string; headers: Record<string, string>; body?: any };
type Route = (call: Call) => { status: number; body?: unknown; headers?: Record<string, string> };

function fakeFetch(routes: Record<string, Route>) {
  const calls: Call[] = [];
  const impl = (async (url: string, init: any) => {
    const call: Call = {
      url,
      method: init.method,
      headers: init.headers,
      body: init.body ? JSON.parse(init.body) : undefined,
    };
    calls.push(call);
    const route = routes[`${init.method} ${new URL(url).pathname}`];
    if (!route) {
      return new Response(JSON.stringify({ error: { code: "not_found", message: "Not found" } }), { status: 404 });
    }
    const res = route(call);
    return new Response(res.body === undefined ? null : JSON.stringify(res.body), {
      status: res.status,
      headers: res.headers,
    });
  }) as unknown as typeof fetch;
  return { impl, calls };
}

/** Reversible stand-in for Electron's safeStorage. */
const fakeCipher: Cipher = {
  isEncryptionAvailable: () => true,
  encryptString: (s) => Buffer.from(s.split("").reverse().join(""), "utf8"),
  decryptString: (b) => b.toString("utf8").split("").reverse().join(""),
};

const unauthorized = { status: 401, body: { error: { code: "unauthorized", message: "Not logged in" } } };

describe("accounts", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "ppo-accounts-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("resolves hosts from config with env overrides", () => {
    expect(resolveServiceUrls({}).accountsApi).toBe("https://brawlonline.net");
    expect(resolveServiceUrls({}).matchmakingHost).toBe("mm.brawlonline.net");
    const local = resolveServiceUrls({ PPO_ACCOUNTS_URL: "http://127.0.0.1:8080/" });
    expect(local.accountsApi).toBe("http://127.0.0.1:8080");
    expect(local.website).toBe("http://127.0.0.1:8080");
  });

  it("signs up, stores the session encrypted, and uses it", async () => {
    const { impl, calls } = fakeFetch({
      "POST /v1/auth/signup": () => ({ status: 201, body: { sessionToken: "tok-1", user: me() } }),
      "GET /v1/me": (c) => (c.headers.authorization === "Bearer tok-1" ? { status: 200, body: me() } : unauthorized),
    });
    const sessionsFile = path.join(dir, "sessions.json");
    const manager = new AccountsManager(
      new AccountsHttpClient("http://api", "test", impl),
      new SessionStore(sessionsFile, fakeCipher),
    );

    const res = await manager.signUp({
      email: "alice@example.test",
      password: "a long password",
      displayName: "alice",
    });
    expect(res.ok).toBe(true);
    expect(calls[0].body).toEqual({
      email: "alice@example.test",
      password: "a long password",
      displayName: "alice",
    });
    const file = await readFile(sessionsFile, "utf8");
    expect(file).not.toContain("tok-1");
    expect(file).toContain("enc:");

    // A new process restores the session from disk.
    const fresh = new AccountsManager(
      new AccountsHttpClient("http://api", "test", impl),
      new SessionStore(sessionsFile, fakeCipher),
    );
    expect(await fresh.me(UID)).toEqual({ ok: true, value: me() });
  });

  it("returns the server's error message and forgets a rejected session", async () => {
    const { impl } = fakeFetch({
      "POST /v1/auth/login": () => ({
        status: 401,
        body: { error: { code: "bad_credentials", message: "Wrong email or password" } },
      }),
      "GET /v1/me": () => unauthorized,
    });
    const store = new SessionStore(path.join(dir, "sessions.json"), null);
    const manager = new AccountsManager(new AccountsHttpClient("http://api", "test", impl), store);

    expect(await manager.login("a@b.c", "nope")).toEqual({
      ok: false,
      error: { code: "bad_credentials", message: "Wrong email or password", status: 401 },
    });

    await store.set("uid-1", "old-token");
    expect((await manager.me("uid-1")).ok).toBe(false);
    expect(await store.get("uid-1")).toBeUndefined();
    const again = await manager.me("uid-1");
    expect(again.ok === false && again.error.code).toBe("no_session");
  });

  it("pages the leaderboard and the match history, and passes on a 429's Retry-After", async () => {
    let limited = false;
    const page = { entries: [], next: "Y3Vy", total: 0 };
    const { impl, calls } = fakeFetch({
      "GET /v1/ranked/leaderboard": () =>
        limited
          ? {
              status: 429,
              body: { error: { code: "rate_limited", message: "Too many requests. Try again in 17 s." } },
              headers: { "retry-after": "17" },
            }
          : { status: 200, body: page },
      "GET /v1/me/matches": (c) =>
        c.headers.authorization === "Bearer tok-1" ? { status: 200, body: { matches: [], next: null } } : unauthorized,
    });
    const store = new SessionStore(path.join(dir, "s.json"), null);
    await store.set(UID, "tok-1");
    const manager = new AccountsManager(new AccountsHttpClient("http://api", "test", impl), store);

    expect(await manager.leaderboard({})).toEqual({ ok: true, value: page });
    expect(calls[0].url).toBe("http://api/v1/ranked/leaderboard");
    await manager.leaderboard({ limit: 50, after: "a+b/c" });
    expect(calls[1].url).toBe("http://api/v1/ranked/leaderboard?limit=50&after=a%2Bb%2Fc");
    // Public: no session token.
    expect(calls[1].headers.authorization).toBeUndefined();

    expect((await manager.matchHistory(UID, { mode: "ranked", before: "Yz" })).ok).toBe(true);
    expect(calls[2].url).toBe("http://api/v1/me/matches?mode=ranked&before=Yz");

    limited = true;
    expect(await manager.leaderboard({ after: "x" })).toEqual({
      ok: false,
      error: { code: "rate_limited", message: "Too many requests. Try again in 17 s.", status: 429, retryAfter: 17 },
    });
    // No session: the history asks for a login.
    const none = await manager.matchHistory("someone-else", { mode: "all" });
    expect(none.ok === false && none.error.code).toBe("no_session");
  });

  it("fetches the room list with the session, and passes on the server's 503", async () => {
    let down = false;
    const list = {
      online: 3,
      updatedAt: "2026-10-09T12:00:00.000Z",
      rooms: [
        {
          code: "KFQB",
          host: "alice",
          players: 1,
          openSlots: 2,
          mode: "1v1",
          status: "waiting",
          names: ["alice"],
          joinable: true,
        },
      ],
    };
    const { impl, calls } = fakeFetch({
      "GET /v1/rooms": (c) =>
        c.headers.authorization !== "Bearer tok-1"
          ? unauthorized
          : down
          ? {
              status: 503,
              body: {
                error: { code: "mm_unavailable", message: "The matchmaking server is not answering. Try again later." },
              },
            }
          : { status: 200, body: list },
    });
    const store = new SessionStore(path.join(dir, "s.json"), null);
    await store.set(UID, "tok-1");
    const manager = new AccountsManager(new AccountsHttpClient("http://api", "test", impl), store);

    expect(await manager.rooms(UID)).toEqual({ ok: true, value: list });
    expect(calls[0].url).toBe("http://api/v1/rooms");
    down = true;
    const res = await manager.rooms(UID);
    expect(res.ok === false && res.error).toEqual({
      code: "mm_unavailable",
      message: "The matchmaking server is not answering. Try again later.",
      status: 503,
    });
    // Without a session nothing is sent.
    const none = await manager.rooms("someone-else");
    expect(none.ok === false && none.error.code).toBe("no_session");
    expect(calls.length).toBe(2);
  });

  it("reports an unreachable server as a network error", async () => {
    const failing = (async () => {
      throw new TypeError("fetch failed");
    }) as unknown as typeof fetch;
    const manager = new AccountsManager(
      new AccountsHttpClient("http://api", "test", failing),
      new SessionStore(path.join(dir, "s.json"), null),
    );
    const res = await manager.requestPasswordReset("a@b.c");
    expect(res.ok === false && res.error.code).toBe("network");
  });

  it("logs out locally even when the server is unreachable", async () => {
    const failing = (async () => {
      throw new TypeError("fetch failed");
    }) as unknown as typeof fetch;
    const store = new SessionStore(path.join(dir, "s.json"), null);
    await store.set("uid-1", "tok");
    const manager = new AccountsManager(new AccountsHttpClient("http://api", "test", failing), store);
    expect((await manager.logout("uid-1")).ok).toBe(true);
    expect(await store.get("uid-1")).toBeUndefined();
  });
});
