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

describe("LocalDolphinInstallation Dolphin.ini", () => {
  let dir: string;
  let inst: LocalDolphinInstallation;
  let iniPath: string;

  beforeEach(async () => {
    dir = path.join(root, `ini-${n++}`);
    inst = new LocalDolphinInstallation(
      DolphinLaunchType.NETPLAY,
      path.join(dir, "Dolphin.exe"),
      path.join(dir, "data"),
    );
    iniPath = path.join(inst.userFolder, "Config", "Dolphin.ini");
    await fs.promises.mkdir(path.dirname(iniPath), { recursive: true });
  });

  const readIni = () => fs.promises.readFile(iniPath, "utf8");

  for (const [eol, name] of [
    ["\n", "LF"],
    ["\r\n", "CRLF"],
  ] as const) {
    it(`Play repairs a Dolphin.ini that overlapping saves garbled (${name})`, async () => {
      // A user's netplay Dolphin.ini (IDs and paths replaced). Dolphin and the launcher have
      // saved it many times since, keeping the garbled lines as keys and adding back the keys
      // the lines swallowed, so [Analytics] and [SDL_Hints] show up twice.
      await fs.promises.writeFile(
        iniPath,
        [
          "[Core]",
          "DefaultISO = E:\\Roms\\Wii\\SSBB_NTSC2.iso",
          "[Online]",
          "ReplayDir = C:\\Users\\player\\Documents\\Brawl Online",
          "SSaveReplays = TrueRReplayMonthlyFolders = True[[Analytics]ID = 11111111111111111111111111111111",
          "SaveReplays = True",
          "ReplayMonthlyFolders = True",
          "[General]",
          "WirelessMac = 00:17:ab:00:00:01",
          "ISOPaths = 1",
          "ISOPath0 = E:\\Roms\\Wii",
          "[NetPlay]",
          "TTraversalChoice = traversal[[SDL_Hints]SSDL_JOYSTICK_DIRECTINPUT = 1",
          "SDL_JOYSTICK_WGI = 0",
          "TraversalChoice = traversal",
          "[DSP]",
          "DSPThread = True",
          "[Analytics]",
          "ID = 22222222222222222222222222222222",
          "[SDL_Hints]",
          "SDL_JOYSTICK_DIRECTINPUT = 1",
          "SDL_JOYSTICK_WGI = 0",
          "",
        ].join(eol),
      );

      await inst.updateSettings({
        replayPath: "C:\\Users\\player\\Documents\\Brawl Online",
        enableNetplayReplays: false,
        enableMonthlySubfolders: true,
      });

      // The garbled keys are gone; keys saved whole after the garbling win over the ones
      // recovered from it (the second analytics ID).
      expect(await readIni()).toBe(
        [
          "[Core]",
          "DefaultISO = E:\\Roms\\Wii\\SSBB_NTSC2.iso",
          "[Online]",
          "ReplayDir = C:\\Users\\player\\Documents\\Brawl Online",
          "SaveReplays = False",
          "ReplayMonthlyFolders = True",
          "[General]",
          "WirelessMac = 00:17:ab:00:00:01",
          "ISOPaths = 1",
          "ISOPath0 = E:\\Roms\\Wii",
          "[NetPlay]",
          "SDL_JOYSTICK_WGI = 0",
          "TraversalChoice = traversal",
          "[DSP]",
          "DSPThread = True",
          "[Analytics]",
          "ID = 22222222222222222222222222222222",
          "[SDL_Hints]",
          "SDL_JOYSTICK_DIRECTINPUT = 1",
          "SDL_JOYSTICK_WGI = 0",
          "",
        ].join("\n"),
      );
    });
  }

  it("keeps both changes when the ISO setting saves the game path and default ISO at once", async () => {
    await fs.promises.writeFile(iniPath, "[Core]\r\nDefaultISO = E:\\Roms\\Wii\\SSBB_NTSC.iso\r\n");
    for (let round = 0; round < 20; round++) {
      const iso = path.join(dir, `Roms${round}`, `SSBB_NTSC${round % 2 ? "2" : ""}.iso`);
      // What the isoPath subscription in settings/setup.ts runs.
      await Promise.all([inst.addGamePath(path.dirname(iso)), inst.setDefaultIso(iso)]);
      expect(await readIni()).toBe(
        `[Core]\nDefaultISO = ${iso}\n[General]\nISOPaths = 1\nISOPath0 = ${path.dirname(iso)}\n`,
      );
    }
  });
});
