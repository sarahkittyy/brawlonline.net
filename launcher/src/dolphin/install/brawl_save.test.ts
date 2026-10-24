import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { BRAWL_BANNER_SIZE, brawlSaveDir, ensureBrawlSave, nandRootFor } from "./brawl_save";

/** A banner.bin of the size the game writes (P+ refuses any other), its text `fill` repeated. */
function banner(fill: string): string {
  return fill.repeat(BRAWL_BANNER_SIZE / fill.length + 1).slice(0, BRAWL_BANNER_SIZE);
}

let tmp: string;

beforeEach(async () => {
  tmp = await fs.promises.mkdtemp(path.join(os.tmpdir(), "brawl-save-"));
});

afterEach(async () => {
  await fs.promises.rm(tmp, { recursive: true, force: true });
});

/** A NAND-shaped template like P+'s Sys/NetplaySave (save files, the TMD, the system menu's setting.txt). */
async function makeTemplate(root: string, files: Record<string, string>): Promise<void> {
  const data = brawlSaveDir(root);
  await fs.promises.mkdir(data, { recursive: true });
  for (const [name, text] of Object.entries(files)) {
    await fs.promises.writeFile(path.join(data, name), text);
  }
  const content = path.join(root, "title", "00010000", "52534245", "content");
  await fs.promises.mkdir(content, { recursive: true });
  await fs.promises.writeFile(path.join(content, "title.tmd"), "tmd");
  const sys = path.join(root, "title", "00000001", "00000002", "data");
  await fs.promises.mkdir(sys, { recursive: true });
  await fs.promises.writeFile(path.join(sys, "setting.txt"), "setting");
}

async function readDir(dir: string): Promise<Record<string, string>> {
  const out: Record<string, string> = {};
  for (const name of (await fs.promises.readdir(dir)).sort()) {
    out[name] = await fs.promises.readFile(path.join(dir, name), "utf8");
  }
  return out;
}

describe("ensureBrawlSave", () => {
  it("seeds an empty NAND with the template's save files only", async () => {
    const template = path.join(tmp, "NetplaySave");
    await makeTemplate(template, { "advsv0.bin": "adv", "autosv0.bin": "auto", "collect.vff": "vff" });
    const nand = path.join(tmp, "User", "Wii");
    const messages: string[] = [];

    const result = await ensureBrawlSave({ nandRoot: nand, templateRoots: [template], log: (m) => messages.push(m) });

    expect(result.action).toBe("seeded");
    expect(await readDir(brawlSaveDir(nand))).toEqual({
      "advsv0.bin": "adv",
      "autosv0.bin": "auto",
      "collect.vff": "vff",
    });
    // Dolphin writes the TMD at the disc boot; the system menu's settings are Dolphin's own.
    expect(fs.existsSync(path.join(nand, "title", "00010000", "52534245", "content"))).toBe(false);
    expect(fs.existsSync(path.join(nand, "title", "00000001"))).toBe(false);
    expect(fs.existsSync(brawlSaveDir(nand) + ".seeding")).toBe(false);
    expect(messages.join("\n")).toContain("Seeded the Brawl save");
  });

  it("never touches a save the game wrote, and a second Play keeps it", async () => {
    const template = path.join(tmp, "NetplaySave");
    await makeTemplate(template, {
      "advsv0.bin": "template",
      "autosv0.bin": "template",
      "banner.bin": banner("template"),
    });
    const nand = path.join(tmp, "Wii");
    expect((await ensureBrawlSave({ nandRoot: nand, templateRoots: [template] })).action).toBe("seeded");

    // The game saves (a new name tag, its records): the files change and one is added.
    await fs.promises.writeFile(path.join(brawlSaveDir(nand), "autosv0.bin"), "written by the game");
    await fs.promises.writeFile(path.join(brawlSaveDir(nand), "net0.bin"), "new");

    const again = await ensureBrawlSave({ nandRoot: nand, templateRoots: [template] });
    expect(again.action).toBe("present");
    expect(await readDir(brawlSaveDir(nand))).toEqual({
      "advsv0.bin": "template",
      "autosv0.bin": "written by the game",
      "banner.bin": banner("template"),
      "net0.bin": "new",
    });
  });

  it("replaces a save without its banner, which Brawl can only throw away", async () => {
    // The user's PC 2026-10-15: what an old machine session left of P+'s save. Brawl deleted its
    // own files, then "There is not enough available space in Wii system memory" on every boot.
    const template = path.join(tmp, "NetplaySave");
    await makeTemplate(template, {
      "banner.bin": banner("banner"),
      "filelist.dat": "template",
      "wc24pubk.mod": "template",
    });
    const nand = path.join(tmp, "Wii");
    await fs.promises.mkdir(brawlSaveDir(nand), { recursive: true });
    await fs.promises.writeFile(path.join(brawlSaveDir(nand), "filelist.dat"), "left");
    await fs.promises.writeFile(path.join(brawlSaveDir(nand), "wc24pubk.mod"), "left");
    const messages: string[] = [];

    const result = await ensureBrawlSave({ nandRoot: nand, templateRoots: [template], log: (m) => messages.push(m) });
    expect(result).toMatchObject({ action: "seeded", removed: ["filelist.dat", "wc24pubk.mod"] });
    expect(await readDir(brawlSaveDir(nand))).toEqual({
      "banner.bin": banner("banner"),
      "filelist.dat": "template",
      "wc24pubk.mod": "template",
    });
    expect(messages.join("\n")).toContain("Removed a Brawl save without banner.bin");

    // With no template, the folder is emptied so that the game can make its own save.
    const other = path.join(tmp, "other");
    await fs.promises.mkdir(brawlSaveDir(other), { recursive: true });
    await fs.promises.writeFile(path.join(brawlSaveDir(other), "filelist.dat"), "left");
    const none = await ensureBrawlSave({ nandRoot: other, templateRoots: [path.join(tmp, "missing")] });
    expect(none).toMatchObject({ action: "no-template", removed: ["filelist.dat"] });
    expect(await readDir(brawlSaveDir(other))).toEqual({});
  });

  it("keeps a partial save as it is (the game decides what to do with it)", async () => {
    const template = path.join(tmp, "NetplaySave");
    await makeTemplate(template, { "advsv0.bin": "template" });
    const nand = path.join(tmp, "Wii");
    await fs.promises.mkdir(brawlSaveDir(nand), { recursive: true });
    await fs.promises.writeFile(path.join(brawlSaveDir(nand), "banner.bin"), banner("mine"));

    expect((await ensureBrawlSave({ nandRoot: nand, templateRoots: [template] })).action).toBe("present");
    expect(await readDir(brawlSaveDir(nand))).toEqual({ "banner.bin": banner("mine") });
  });

  it("moves away a save whose banner P+ refuses and seeds the template (bug report #138)", async () => {
    // A 61600-byte banner (eight icons) in the playback NAND: P+ asked "Create save file for
    // Project+?" at every replay, which the playback Dolphin can't answer.
    const template = path.join(tmp, "NetplaySave");
    await makeTemplate(template, { "advsv0.bin": "template", "banner.bin": banner("template") });
    const user = path.join(tmp, "playback", "User");
    const nand = path.join(user, "Wii");
    await fs.promises.mkdir(brawlSaveDir(nand), { recursive: true });
    await fs.promises.writeFile(path.join(brawlSaveDir(nand), "advsv0.bin"), "theirs");
    await fs.promises.writeFile(path.join(brawlSaveDir(nand), "banner.bin"), "8".repeat(61600));
    const messages: string[] = [];

    const result = await ensureBrawlSave({ nandRoot: nand, templateRoots: [template], log: (m) => messages.push(m) });

    expect(result).toMatchObject({ action: "seeded", removed: [] });
    expect(await readDir(brawlSaveDir(nand))).toEqual({ "advsv0.bin": "template", "banner.bin": banner("template") });
    const backupRoot = path.join(user, "SaveBackups", "RSBE");
    const backups = await fs.promises.readdir(backupRoot);
    expect(backups).toHaveLength(1);
    expect(result.action === "seeded" && result.backup).toBe(path.join(backupRoot, backups[0]));
    expect(await readDir(path.join(backupRoot, backups[0]))).toEqual({
      "advsv0.bin": "theirs",
      "banner.bin": "8".repeat(61600),
    });
    expect(messages.join("\n")).toContain("61600 bytes");

    // The seeded save is kept from then on.
    expect((await ensureBrawlSave({ nandRoot: nand, templateRoots: [template] })).action).toBe("present");
    expect(await fs.promises.readdir(backupRoot)).toHaveLength(1);
  });

  it("never seeds from a template whose banner P+ refuses", async () => {
    const bad = path.join(tmp, "bad");
    await makeTemplate(bad, { "advsv0.bin": "bad", "banner.bin": "short" });
    const good = path.join(tmp, "good");
    await makeTemplate(good, { "advsv0.bin": "good", "banner.bin": banner("good") });
    const nand = path.join(tmp, "Wii");

    const result = await ensureBrawlSave({ nandRoot: nand, templateRoots: [bad, good] });
    expect(result).toMatchObject({ action: "seeded", from: brawlSaveDir(good) });
    expect(await readDir(brawlSaveDir(nand))).toEqual({ "advsv0.bin": "good", "banner.bin": banner("good") });
  });

  it("fills the empty save folder Dolphin creates at a disc boot", async () => {
    const template = path.join(tmp, "NetplaySave");
    await makeTemplate(template, { "advsv0.bin": "adv" });
    const nand = path.join(tmp, "Wii");
    await fs.promises.mkdir(brawlSaveDir(nand), { recursive: true });

    expect((await ensureBrawlSave({ nandRoot: nand, templateRoots: [template] })).action).toBe("seeded");
    expect(await readDir(brawlSaveDir(nand))).toEqual({ "advsv0.bin": "adv" });
  });

  it("redoes a copy that was interrupted (the staging folder is never taken for a save)", async () => {
    const template = path.join(tmp, "NetplaySave");
    await makeTemplate(template, { "advsv0.bin": "adv", "collect.vff": "vff" });
    const nand = path.join(tmp, "Wii");
    await fs.promises.mkdir(brawlSaveDir(nand) + ".seeding", { recursive: true });
    await fs.promises.writeFile(path.join(brawlSaveDir(nand) + ".seeding", "advsv0.bin"), "ad");

    expect((await ensureBrawlSave({ nandRoot: nand, templateRoots: [template] })).action).toBe("seeded");
    expect(await readDir(brawlSaveDir(nand))).toEqual({ "advsv0.bin": "adv", "collect.vff": "vff" });
  });

  it("takes the first template that has a save and reports when there is none", async () => {
    const missing = path.join(tmp, "missing");
    const empty = path.join(tmp, "empty");
    await fs.promises.mkdir(brawlSaveDir(empty), { recursive: true });
    const sys = path.join(tmp, "Sys", "NetplaySave");
    await makeTemplate(sys, { "advsv0.bin": "from sys" });
    const nand = path.join(tmp, "Wii");

    const result = await ensureBrawlSave({ nandRoot: nand, templateRoots: [missing, empty, sys] });
    expect(result).toMatchObject({ action: "seeded", from: brawlSaveDir(sys), files: 1 });

    const none = await ensureBrawlSave({ nandRoot: path.join(tmp, "other"), templateRoots: [missing, empty] });
    expect(none.action).toBe("no-template");
    expect(fs.existsSync(brawlSaveDir(path.join(tmp, "other")))).toBe(false);
  });
});

describe("nandRootFor", () => {
  it("is <User>/Wii unless Dolphin.ini sets NANDRootPath", () => {
    const user = path.join(tmp, "User");
    expect(nandRootFor(user, undefined)).toBe(path.join(user, "Wii"));
    expect(nandRootFor(user, "  ")).toBe(path.join(user, "Wii"));
    const custom = path.join(tmp, "nand");
    expect(nandRootFor(user, custom)).toBe(custom);
  });
});
