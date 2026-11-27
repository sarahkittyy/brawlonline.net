import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, describe, expect, it, vi } from "vitest";

const userData = fs.mkdtempSync(path.join(os.tmpdir(), "instance-"));
vi.mock("electron", () => ({ app: { getPath: () => userData } }));

import { PlaybackDolphinInstance } from "./instance";

afterAll(() => fs.rmSync(userData, { recursive: true, force: true }));

/** A stand-in program (this Node) that writes its arguments and BRAWL_ONLINE_REPLAY_COMM next to the comm file. */
const reporter = (out: string) => [
  "-e",
  `require("fs").writeFileSync(${JSON.stringify(out)}, JSON.stringify({ argv: process.argv.slice(1), ` +
    `comm: process.env.BRAWL_ONLINE_REPLAY_COMM ?? null, cwd: process.cwd() }))`,
  // (the rest is the script's, not Node's: Node has an -i of its own)
  "--",
];

async function run(instance: PlaybackDolphinInstance): Promise<void> {
  const closed = new Promise<void>((resolve) => instance.once("close", () => resolve()));
  await instance.play({ mode: "normal", replay: "a.brep", commandId: "c1" });
  await closed;
}

describe("PlaybackDolphinInstance", () => {
  it("gives Dolphin the comm file with -i", async () => {
    const out = path.join(userData, "dolphin.json");
    await run(new PlaybackDolphinInstance(process.execPath, reporter(out)));
    const got = JSON.parse(fs.readFileSync(out, "utf8"));
    expect(got.comm).toBeNull();
    expect(got.argv[got.argv.length - 2]).toBe("-i");
    expect(path.basename(got.argv[got.argv.length - 1])).toMatch(/^playback-comms-.*\.json$/);
  });

  it("gives EmissaryRecomp the comm file in BRAWL_ONLINE_REPLAY_COMM, with its environment and folder", async () => {
    const out = path.join(userData, "recomp.json");
    const instance = new PlaybackDolphinInstance(process.execPath, reporter(out), {
      env: { BRAWL_ONLINE_CORE: "core" },
      cwd: userData,
      commEnv: "BRAWL_ONLINE_REPLAY_COMM",
    });
    await run(instance);
    const got = JSON.parse(fs.readFileSync(out, "utf8"));
    expect(got.argv).not.toContain("-i");
    expect(path.basename(got.comm)).toMatch(/^playback-comms-.*\.json$/);
    expect(path.resolve(got.cwd)).toBe(path.resolve(userData));
  });
});
