import net from "net";
import os from "os";
import path from "path";
import { afterEach, describe, expect, it } from "vitest";

import { decodeFrames, DiscordRpc, encodeFrame, ipcPaths, wireActivity } from "./discord_rpc";

describe("Discord RPC frames", () => {
  it("round-trips frames, a split one waiting for the rest", () => {
    const a = encodeFrame(1, { cmd: "SET_ACTIVITY", nonce: "1" });
    const b = encodeFrame(3, { x: "é" });
    const both = Buffer.concat([a, b]);
    const first = decodeFrames(both.subarray(0, a.length + 5));
    expect(first.frames).toEqual([{ op: 1, data: { cmd: "SET_ACTIVITY", nonce: "1" } }]);
    expect(first.rest.length).toBe(5);
    const second = decodeFrames(Buffer.concat([first.rest, both.subarray(a.length + 5)]));
    expect(second.frames).toEqual([{ op: 3, data: { x: "é" } }]);
    expect(second.rest.length).toBe(0);
  });

  it("lists Windows pipes and the Unix sockets (Flatpak, Snap)", () => {
    expect(ipcPaths("win32")[0]).toBe("\\\\?\\pipe\\discord-ipc-0");
    expect(ipcPaths("win32")).toHaveLength(10);
    const unix = ipcPaths("linux", { XDG_RUNTIME_DIR: "/run/user/1000" });
    expect(unix).toContain("/run/user/1000/discord-ipc-0");
    expect(unix).toContain("/run/user/1000/app/com.discordapp.Discord/discord-ipc-0");
    expect(unix).toContain("/tmp/snap.discord/discord-ipc-3");
  });
});

describe("wireActivity", () => {
  it("shows the details in the member list and keeps Discord's limits", () => {
    const w = wireActivity({
      details: "Ranked · vs Peachy",
      state: "x",
      largeImage: "https://brawlonline.net/assets/icon-512.png",
      largeText: "B".repeat(200),
      startedAt: 1_700_000_000_500,
      party: { size: 3, max: 4 },
      buttons: [
        { label: "Join their room", url: "https://brawlonline.net/?join=KFQB" },
        { label: "Bad", url: "brawlonline://room/join?code=KFQB" },
      ],
    });
    expect(w.status_display_type).toBe(2);
    expect(w.details).toBe("Ranked · vs Peachy");
    // Under 2 characters: Discord would refuse the whole activity.
    expect(w.state).toBeUndefined();
    expect(Array.from((w.assets as { large_text: string }).large_text)).toHaveLength(128);
    expect(w.timestamps).toEqual({ start: 1_700_000_000 });
    expect(w.party).toEqual({ id: "brawl-online", size: [3, 4] });
    expect(w.buttons).toEqual([{ label: "Join their room", url: "https://brawlonline.net/?join=KFQB" }]);
  });
});

describe("DiscordRpc", () => {
  let server: net.Server | null = null;
  afterEach(() => {
    server?.close();
    server = null;
  });

  /** A Discord app stand-in: answers the handshake with READY and records every frame. */
  const fakeDiscord = async () => {
    const pipe =
      process.platform === "win32"
        ? `\\\\?\\pipe\\ppo-discord-test-${process.pid}-${Date.now()}`
        : path.join(os.tmpdir(), `ppo-discord-test-${process.pid}-${Date.now()}`);
    const frames: { op: number; data: any }[] = [];
    server = net.createServer((sock) => {
      let buf: Buffer = Buffer.alloc(0);
      sock.on("data", (chunk: Buffer) => {
        const out = decodeFrames(Buffer.concat([buf, chunk]));
        buf = out.rest;
        for (const f of out.frames) {
          frames.push(f);
          if (f.op === 0) {
            sock.write(encodeFrame(1, { cmd: "DISPATCH", evt: "READY", data: { v: 1 } }));
          }
        }
      });
    });
    await new Promise<void>((resolve) => server!.listen(pipe, resolve));
    return { pipe, frames };
  };

  const until = async (cond: () => boolean) => {
    for (let i = 0; i < 200 && !cond(); i++) {
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    expect(cond()).toBe(true);
  };

  it("connects, sets the activity, sends a change only once, and clears it", async () => {
    const { pipe, frames } = await fakeDiscord();
    let clock = 0;
    const rpc = new DiscordRpc("123", {
      paths: () => [pipe + "-missing", pipe],
      pid: 42,
      // Every read is 10 s later: no wait between sends.
      now: () => (clock += 10_000),
    });
    const activity = { details: "In the launcher", largeImage: "https://brawlonline.net/assets/icon-512.png" };
    rpc.setActivity(activity);
    await until(() => frames.length >= 2);
    expect(frames[0]).toEqual({ op: 0, data: { v: 1, client_id: "123" } });
    expect(frames[1].data.cmd).toBe("SET_ACTIVITY");
    expect(frames[1].data.args.pid).toBe(42);
    expect(frames[1].data.args.activity.details).toBe("In the launcher");

    rpc.setActivity({ ...activity });
    await new Promise((resolve) => setTimeout(resolve, 50));
    expect(frames).toHaveLength(2);

    rpc.setActivity(null);
    await until(() => frames.length >= 3);
    expect(frames[2].data.args).toEqual({ pid: 42 });
    rpc.close();
  });

  it("does nothing without a Discord app", async () => {
    const rpc = new DiscordRpc("123", { paths: () => [path.join(os.tmpdir(), "no-discord-here")] });
    rpc.setActivity({ largeImage: "https://brawlonline.net/assets/icon-512.png" });
    await new Promise((resolve) => setTimeout(resolve, 50));
    rpc.close();
  });
});
