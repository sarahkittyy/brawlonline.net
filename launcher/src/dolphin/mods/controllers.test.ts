import { mkdir, mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { defaultControllerKind, deviceLabel, readDolphinControllers } from "./controllers";

describe("deviceLabel", () => {
  it("names devices the way players call them", () => {
    expect(deviceLabel("XInput/0/Gamepad")).toBe("Xbox controller");
    expect(deviceLabel("XInput/1/Gamepad")).toBe("Xbox controller 2");
    expect(deviceLabel("DInput/0/Keyboard Mouse")).toBe("Keyboard");
    expect(deviceLabel("SDL/0/PS5 Controller")).toBe("PS5 Controller");
    expect(deviceLabel("Bluetooth/0/Wii Remote")).toBe("Wii Remote");
    expect(deviceLabel("")).toBe("No device");
  });
});

describe("readDolphinControllers", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "controllers-"));
    await mkdir(path.join(dir, "Config"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("is the adapter on every port and no Wii Remote with no settings", async () => {
    const c = await readDolphinControllers(dir);
    expect(c.gc.map((p) => p.label)).toEqual(Array(4).fill("GameCube adapter"));
    expect(c.wiimote).toEqual([]);
    expect(defaultControllerKind(c)).toBe("gc");
  });

  it("folds the set-up ports and Wii Remotes in order", async () => {
    await writeFile(
      path.join(dir, "Config", "Dolphin.ini"),
      "[Core]\r\nSIDevice0 = 6\r\nSIDevice1 = 0\r\nSIDevice2 = 0\r\nSIDevice3 = 12\r\n",
    );
    await writeFile(path.join(dir, "Config", "GCPadNew.ini"), "[GCPad1]\nDevice = XInput/0/Gamepad\n");
    await writeFile(
      path.join(dir, "Config", "WiimoteNew.ini"),
      "[Wiimote1]\nSource = 2\n[Wiimote2]\nSource = 1\nDevice = DInput/0/Keyboard Mouse\nExtension = Nunchuk\n[Wiimote3]\nSource = 0\n",
    );
    const c = await readDolphinControllers(dir);
    expect(c.gc).toEqual([
      { index: 1, device: "XInput/0/Gamepad", label: "Xbox controller" },
      { index: 4, device: "GameCube adapter", label: "GameCube adapter" },
    ]);
    expect(c.wiimote).toEqual([
      { index: 1, device: "", label: "Real Wii Remote", real: true },
      { index: 2, device: "DInput/0/Keyboard Mouse", label: "Keyboard + Nunchuk" },
    ]);
  });

  it("plays a Wii Remote by default only when that is all that is set up", () => {
    const wii = { index: 1, device: "", label: "Real Wii Remote", real: true };
    expect(defaultControllerKind({ gc: [], wiimote: [wii] })).toBe("wiimote");
    expect(defaultControllerKind({ gc: [{ index: 1, device: "", label: "" }], wiimote: [wii] })).toBe("gc");
  });
});
