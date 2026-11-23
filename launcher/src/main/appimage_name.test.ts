import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("electron-log", () => ({ default: { info: vi.fn(), warn: vi.fn(), error: vi.fn() } }));

import { keepAppImageName } from "./appimage_name";

// electron-updater's AppImageUpdater.doInstall, as far as the name goes: the running file is
// replaced in place when the downloaded one has its name or its name holds no version, else the
// new file goes next to it under the downloaded file's name.
class FakeUpdater {
  readonly installed: string[] = [];

  constructor(private readonly downloaded: string | null, private readonly appImage: string) {}

  // A getter on the prototype, as BaseUpdater's.
  get installerPath(): string | null {
    return this.downloaded;
  }

  doInstall(_options: unknown): boolean {
    const from = this.installerPath!;
    const name = path.basename(this.appImage);
    const destination =
      path.basename(from) === name || !/\d+\.\d+\.\d+/.test(name)
        ? this.appImage
        : path.join(path.dirname(this.appImage), path.basename(from));
    rmSync(this.appImage);
    writeFileSync(destination, readFileSync(from));
    rmSync(from);
    this.installed.push(destination);
    return true;
  }
}

describe("keepAppImageName", () => {
  let dir: string;
  let apps: string;
  let pending: string;

  beforeEach(() => {
    dir = mkdtempSync(path.join(os.tmpdir(), "appimage-name-"));
    apps = path.join(dir, "Applications");
    pending = path.join(dir, "pending");
    mkdirSync(apps);
    mkdirSync(pending);
  });

  afterEach(() => {
    rmSync(dir, { recursive: true, force: true });
  });

  it("replaces a versioned AppImage under its own name", () => {
    const running = path.join(apps, "Brawl-Online-0.2.3-x86_64.AppImage");
    const downloaded = path.join(pending, "Brawl-Online-0.2.5-x86_64.AppImage");
    writeFileSync(running, "old");
    writeFileSync(downloaded, "new");
    const updater = new FakeUpdater(downloaded, running);
    keepAppImageName(updater, { APPIMAGE: running });

    expect(updater.doInstall({})).toBe(true);

    expect(updater.installed).toEqual([running]);
    expect(readFileSync(running, "utf8")).toBe("new");
    expect(existsSync(path.join(apps, "Brawl-Online-0.2.5-x86_64.AppImage"))).toBe(false);
    // The getter answers again after the install.
    expect(updater.installerPath).toBe(downloaded);
  });

  it("leaves a name without a version as electron-updater does", () => {
    const running = path.join(apps, "BrawlOnline.AppImage");
    const downloaded = path.join(pending, "Brawl-Online-0.2.5-x86_64.AppImage");
    writeFileSync(running, "old");
    writeFileSync(downloaded, "new");
    const updater = new FakeUpdater(downloaded, running);
    keepAppImageName(updater, { APPIMAGE: running });

    updater.doInstall({});

    expect(updater.installed).toEqual([running]);
    expect(readFileSync(running, "utf8")).toBe("new");
  });

  it("installs as before when the download cannot be renamed", () => {
    const running = path.join(apps, "Brawl-Online-0.2.3-x86_64.AppImage");
    const missing = path.join(pending, "Brawl-Online-0.2.5-x86_64.AppImage");
    writeFileSync(running, "old");
    const updater = new FakeUpdater(missing, running);
    const install = vi.spyOn(updater, "doInstall").mockReturnValue(false);
    keepAppImageName(updater, { APPIMAGE: running });

    expect(updater.doInstall({})).toBe(false);
    expect(install).toHaveBeenCalledTimes(1);
    expect(updater.installerPath).toBe(missing);
  });

  it("does nothing outside an AppImage", () => {
    const downloaded = path.join(pending, "Brawl-Online-0.2.5-x86_64.AppImage");
    writeFileSync(downloaded, "new");
    const updater = new FakeUpdater(downloaded, path.join(apps, "x.AppImage"));
    const install = vi.spyOn(updater, "doInstall").mockReturnValue(true);
    keepAppImageName(updater, {});

    updater.doInstall({});

    expect(install).toHaveBeenCalledTimes(1);
    expect(existsSync(downloaded)).toBe(true);
  });
});
