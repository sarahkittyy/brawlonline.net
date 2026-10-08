import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";

import { DolphinLaunchType } from "../types";

// The Dolphin.ini helpers this module imports read Electron's default paths when loaded.
vi.mock("electron", () => ({ app: { getPath: () => os.tmpdir(), getName: () => "test", isPackaged: false } }));
import { LocalDolphinInstallation } from "./local_installation";

let root: string;
let n = 0;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "local-install-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

const bytes = (size: number, seed: number) => Buffer.alloc(size, seed);

describe("LocalDolphinInstallation.ensureUserFolder (development template)", () => {
  let dir: string;
  let template: string;
  let userData: string;
  const make = () =>
    new LocalDolphinInstallation(DolphinLaunchType.NETPLAY, path.join(dir, "Dolphin.exe"), userData, template);
  const files: Record<string, Buffer> = {
    "Launcher/Project+ Netplay Launcher.dol": bytes(3000, 1),
    "Wii/sd.raw": bytes(200_000, 2),
    "Wii/title/data.bin": bytes(500, 3),
    "Config/Dolphin.ini": bytes(100, 4),
    "Logs/dolphin.log": bytes(50, 5),
  };
  const total = 3000 + 200_000 + 500 + 100; // Logs/ is never copied

  beforeEach(async () => {
    dir = path.join(root, `case-${n++}`);
    template = path.join(dir, "template-user");
    userData = path.join(dir, "userData");
    for (const [rel, data] of Object.entries(files)) {
      await fs.promises.mkdir(path.dirname(path.join(template, rel)), { recursive: true });
      await fs.promises.writeFile(path.join(template, rel), data);
    }
  });

  it("copies the template once, with progress, to every caller at the same time", async () => {
    const a = make();
    const b = make();
    const seenA: [number, number][] = [];
    const seenB: [number, number][] = [];
    await Promise.all([
      a.ensureUserFolder((d, t) => seenA.push([d, t])),
      b.ensureUserFolder((d, t) => seenB.push([d, t])),
      a.ensureUserFolder(),
    ]);
    for (const [rel, data] of Object.entries(files)) {
      const copy = path.join(a.userFolder, rel);
      if (rel.startsWith("Logs/")) {
        expect(fs.existsSync(copy)).toBe(false);
      } else {
        expect(await fs.promises.readFile(copy)).toEqual(data);
      }
    }
    for (const seen of [seenA, seenB]) {
      expect(seen.length).toBeGreaterThan(0);
      seen.forEach(([d, t], i) => {
        expect(t).toBe(total);
        expect(d).toBeLessThanOrEqual(t);
        if (i > 0) {
          expect(d).toBeGreaterThanOrEqual(seen[i - 1][0]);
        }
      });
      expect(seen[seen.length - 1]).toEqual([total, total]);
    }
    expect(seenA[0]).toEqual([0, total]);

    // Done: nothing is copied again, no progress.
    const later: number[] = [];
    await make().ensureUserFolder((d) => later.push(d));
    expect(later).toEqual([]);
  });

  it("after an interruption, copies only what is missing and never keeps a truncated file", async () => {
    const inst = make();
    // What a killed seed leaves: the launcher DOL in place, the card only as a `.partial`.
    const dol = path.join(inst.userFolder, "Launcher", "Project+ Netplay Launcher.dol");
    await fs.promises.mkdir(path.dirname(dol), { recursive: true });
    await fs.promises.writeFile(dol, "the user's own DOL");
    await fs.promises.mkdir(path.join(inst.userFolder, "Wii"), { recursive: true });
    await fs.promises.writeFile(path.join(inst.userFolder, "Wii", "sd.raw.partial"), "half");

    const seen: [number, number][] = [];
    await inst.ensureUserFolder((d, t) => seen.push([d, t]));
    expect(await fs.promises.readFile(path.join(inst.userFolder, "Wii", "sd.raw"))).toEqual(files["Wii/sd.raw"]);
    expect(fs.existsSync(path.join(inst.userFolder, "Wii", "sd.raw.partial"))).toBe(false);
    // Existing files are never replaced.
    expect((await fs.promises.readFile(dol)).toString()).toBe("the user's own DOL");
    expect(seen[seen.length - 1]).toEqual([total - 3000, total - 3000]);
    // The template is only read.
    expect(await fs.promises.readFile(path.join(template, "Wii", "sd.raw"))).toEqual(files["Wii/sd.raw"]);
  });
});
