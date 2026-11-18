import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image, pattern } from "@brawl_assets/testing/fat_image";
import { installPluginOnSdCard, readFileHashFromImage, sha256 } from "@dolphin/install/sd_card";
import crypto from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import {
  CODESETS,
  gameKey,
  identifyPlusFamilyCard,
  PLUS_FAMILY_BUILDS,
  plusFamilyBuild,
  plusFamilyGameSettings,
  plusFamilyPluginPath,
  plusFamilySdCardFolder,
  replayModName,
  replayPlaysOn,
} from "./plus_family";

let root: string;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "plus-family-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

const hex = (pair: string) => pair.repeat(32);

async function card(name: string, files: Record<string, Buffer>): Promise<string> {
  const image = path.join(root, name);
  await makeEmptyFat32Image(image);
  const vol = await Fat32Volume.open(image, { writable: true });
  for (const [sdPath, data] of Object.entries(files)) {
    await vol.writeFile(sdPath, data);
  }
  await vol.close();
  return image;
}

describe("gameKey", () => {
  it("is Dolphin's key (Core/Online/GameBuild.h, tools/gamecode/game_key.py)", () => {
    // game_key.py make_key("Cobalt", "ab" * 32, ["01" * 32, "", "02" * 32, "03" * 32])
    expect(gameKey("Cobalt", hex("ab"), [hex("01"), null, hex("02"), hex("03")])).toBe(
      "Cobalt:0adfa62002f051c4dae5ebbb4c08298e",
    );
    expect(gameKey("P+Ex", hex("ab"), [hex("01"), "", hex("02"), hex("03")])).toBe(
      "P+Ex:0adfa62002f051c4dae5ebbb4c08298e",
    );
    expect(gameKey("", hex("ab"), [])).toBe("");
    expect(gameKey("Cobalt", hex("ac"), [hex("01"), null, hex("02"), hex("03")])).not.toBe(
      gameKey("Cobalt", hex("ab"), [hex("01"), null, hex("02"), hex("03")]),
    );
    expect(CODESETS).toEqual(["NETPLAY.GCT", "NETBOOST.GCT", "RSBE01.GCT", "BOOST.GCT"]);
  });
});

describe("the supported builds", () => {
  it("are mm's games table without Project+ (server/config/games.json)", () => {
    // (the list is made from the table: this holds what the launcher relies on in it)
    expect(PLUS_FAMILY_BUILDS.map((b) => b.id)).toEqual(["pex-1.8", "cobalt-1.34", "rex-0.5"]);
    for (const b of PLUS_FAMILY_BUILDS) {
      expect(b.codes.length, b.id).toBeGreaterThan(0);
      expect(b.codes.every((c) => /^[0-9a-f]{32}$/.test(c))).toBe(true);
      expect(b.folder).not.toBe("Project+");
      expect(plusFamilyBuild(b.id)).toBe(b);
    }
    // REX boots its own launcher; the P+Ex builds boot P+'s.
    expect(PLUS_FAMILY_BUILDS.filter((b) => b.ownLauncher).map((b) => b.id)).toEqual(["rex-0.5"]);
    // Each takes the plugin built for its kind (game-code/build.sh): the BrawlEx one, REX's.
    expect(PLUS_FAMILY_BUILDS.map((b) => b.plugin)).toEqual(["pex", "pex", "rex"]);
    expect(plusFamilyBuild("pplus")).toBeNull();
  });

  it("have their plugin and their card copy apart from P+'s", () => {
    expect(plusFamilyPluginPath("Cobalt")).toBe("/Cobalt/pf/plugins/PPOnline.rel");
    expect(plusFamilyPluginPath("rex_")).toBe("/rex_/pf/plugins/PPOnline.rel");
    expect(plusFamilySdCardFolder("/n", "cobalt-1.34")).toBe(path.join("/n", "pponline-sd-cobalt-1.34"));
  });
});

describe("replayModName", () => {
  it("names the mod a replay was recorded on", () => {
    expect(replayModName("Cobalt:fe22b89a8cf4cdaac89bc39227111c9f")).toBe("Cobalt 1.34");
    expect(replayModName("P+Ex:812d2df3a26681ce57c335c54e675a8a")).toBe("Project+ Ex 1.8");
    // The offline launcher's boot (the harness's) is the same build; REX by either of its own.
    expect(replayModName("Cobalt:ee8a6422b0cd2077861a51574b09e742")).toBe("Cobalt 1.34");
    expect(replayModName("rex_:747b3fc31200c2aea36293fa91b2c657")).toBe("REX 0.5");
    expect(replayModName("rex_:2ca73eed0dbd9a83044d040a74bede71")).toBe("REX 0.5");
    // Another release: the folder.
    expect(replayModName("Cobalt:00000000000000000000000000000000")).toBe("Cobalt");
    // Project+ itself and older recordings: none.
    expect(replayModName("Project+:bc298983bc1e2e55afc2156e77069341")).toBeNull();
    expect(replayModName(null)).toBeNull();
    expect(replayModName("nonsense")).toBeNull();
  });
});

describe("identifyPlusFamilyCard", () => {
  it("knows a card by its folder, its codesets and the boot file", async () => {
    // A card whose key is made to be the table's: the table's hash can't be met with made-up
    // files, so the build under test is one with this card's own key.
    const boot = path.join(root, "boot.dol");
    await fs.promises.writeFile(boot, pattern(4096, 7));
    const codesets = [pattern(1000, 1), pattern(1200, 2), pattern(900, 3), pattern(800, 4)];
    const files: Record<string, Buffer> = {};
    CODESETS.forEach((name, i) => (files[`/Cobalt/${name}`] = codesets[i]));
    const image = await card("cobalt.raw", files);
    const key = gameKey("Cobalt", sha256(pattern(4096, 7)), codesets.map(sha256));
    const cobalt = PLUS_FAMILY_BUILDS.find((b) => b.folder === "Cobalt")!;
    const real = cobalt.codes;
    (cobalt as { codes: readonly string[] }).codes = [key.split(":")[1]];
    try {
      expect(await identifyPlusFamilyCard({ sdPath: image }, boot)).toEqual({ build: cobalt, key, bootFile: boot });
      // It boots P+'s Netplay Launcher, whatever launcher the game names.
      expect(await identifyPlusFamilyCard({ sdPath: image, launcherPath: "/x.elf" }, boot)).toEqual({
        build: cobalt,
        key,
        bootFile: boot,
      });
      expect(plusFamilyGameSettings({ build: cobalt, key, bootFile: boot })).toBeNull();
      // Another boot file (another P+ release's launcher) is another key.
      const other = path.join(root, "other.dol");
      await fs.promises.writeFile(other, pattern(4096, 8));
      expect(await identifyPlusFamilyCard({ sdPath: image }, other)).toBeNull();
      // A changed codeset (another release of the mod) is not this build.
      const changed = await card("cobalt2.raw", { ...files, "/Cobalt/NETPLAY.GCT": pattern(1000, 9) });
      expect(await identifyPlusFamilyCard({ sdPath: changed }, boot)).toBeNull();
      // The same files in P+'s folder, or no card, or a file that is no card.
      const pplus: Record<string, Buffer> = {};
      CODESETS.forEach((name, i) => (pplus[`/Project+/${name}`] = codesets[i]));
      expect(await identifyPlusFamilyCard({ sdPath: await card("pplus.raw", pplus) }, boot)).toBeNull();
      expect(await identifyPlusFamilyCard({ sdPath: null }, boot)).toBeNull();
      expect(await identifyPlusFamilyCard({ sdPath: boot }, boot)).toBeNull();
      expect(await identifyPlusFamilyCard({ sdPath: image }, path.join(root, "missing.dol"))).toBeNull();
    } finally {
      (cobalt as { codes: readonly string[] }).codes = real;
    }
  });

  it("boots a build with launchers of its own (REX) from the game's, with MMU emulation off", async () => {
    const elf = path.join(root, "REX Netplay Launcher.elf");
    await fs.promises.writeFile(elf, pattern(5000, 11));
    const pplusBoot = path.join(root, "pplus.dol");
    await fs.promises.writeFile(pplusBoot, pattern(4096, 7));
    const codesets = [pattern(1100, 1), pattern(1300, 2), pattern(950, 3), pattern(850, 4)];
    const files: Record<string, Buffer> = {};
    CODESETS.forEach((name, i) => (files[`/rex_/${name}`] = codesets[i]));
    const image = await card("rex.raw", files);
    const key = gameKey("rex_", sha256(pattern(5000, 11)), codesets.map(sha256));
    const rex = PLUS_FAMILY_BUILDS.find((b) => b.id === "rex-0.5")!;
    expect(rex.ownLauncher).toBe(true);
    const real = rex.codes;
    (rex as { codes: readonly string[] }).codes = ["00000000000000000000000000000000", key.split(":")[1]];
    try {
      const found = await identifyPlusFamilyCard({ sdPath: image, launcherPath: elf }, pplusBoot);
      expect(found).toEqual({ build: rex, key, bootFile: elf });
      // Not from P+'s launcher, and not without its own.
      expect(await identifyPlusFamilyCard({ sdPath: image, launcherPath: pplusBoot }, pplusBoot)).toBeNull();
      expect(await identifyPlusFamilyCard({ sdPath: image, launcherPath: null }, pplusBoot)).toBeNull();
      // Dolphin's settings for that boot file ("ID-" and its name, ConfigManager.cpp): MMU off.
      expect(plusFamilyGameSettings(found!)).toEqual({
        file: path.join("GameSettings", "ID-REX Netplay Launcher.ini"),
        text: "[Core]\nCPUThread = True\nMMU = False\n",
      });
    } finally {
      (rex as { codes: readonly string[] }).codes = real;
    }
  });

  it("gets our plugin in the mod's own folder on a copy, the player's card untouched", async () => {
    const source = await card("mod.raw", { "/Cobalt/pf/plugins/AsyncRSP.rel": pattern(3000, 5) });
    const before = crypto
      .createHash("sha256")
      .update(await fs.promises.readFile(source))
      .digest("hex");
    const data = pattern(18_000, 6);
    const sdPath = plusFamilyPluginPath("Cobalt");
    const result = await installPluginOnSdCard({
      sourceImage: source,
      outputDir: plusFamilySdCardFolder(root, "cobalt-1.34"),
      plugin: { data, sha256: sha256(data), source: "PPOnline.rel" },
      sdPath,
    });
    expect(result.action).toBe("copied");
    expect(result.image).toBe(path.join(root, "pponline-sd-cobalt-1.34", "sd.raw"));
    expect(await readFileHashFromImage(result.image, sdPath)).toBe(sha256(data));
    expect(await readFileHashFromImage(result.image, "/Cobalt/pf/plugins/AsyncRSP.rel")).toBe(sha256(pattern(3000, 5)));
    expect(await readFileHashFromImage(result.image, "/Project+/pf/plugins/PPOnline.rel")).toBeNull();
    expect(
      crypto
        .createHash("sha256")
        .update(await fs.promises.readFile(source))
        .digest("hex"),
    ).toBe(before);
  });
});

describe("replayPlaysOn", () => {
  it("plays Project+ replays on their release and never a mod's", () => {
    // The launcher's own release, another one, and a recording that names none.
    expect(replayPlaysOn({ pplusRelease: "v3.2.1" }, "v3.2.1")).toEqual({ release: null, mod: false });
    expect(replayPlaysOn({ pplusRelease: "v3.1.5", gameBuild: null }, "v3.2.1")).toEqual({
      release: "v3.1.5",
      mod: false,
    });
    expect(replayPlaysOn({ pplusRelease: null }, "v3.2.1")).toEqual({ release: null, mod: false });
    // A P+ game names its own folder: still Project+.
    expect(
      replayPlaysOn({ pplusRelease: "v3.2.1", gameBuild: "Project+:bc298983bc1e2e55afc2156e77069341" }, "v3.2.1"),
    ).toEqual({ release: null, mod: false });
    // A mod's replay carries the launcher's Project+ release too (Dolphin writes its setting): it
    // is the mod's all the same, whatever release that says.
    expect(
      replayPlaysOn({ pplusRelease: "v3.2.1", gameBuild: "Cobalt:fe22b89a8cf4cdaac89bc39227111c9f" }, "v3.2.1"),
    ).toEqual({ release: "Cobalt 1.34", mod: true });
    expect(replayPlaysOn({ pplusRelease: null, gameBuild: "REX:0123" }, "v3.2.1")).toEqual({
      release: "REX",
      mod: true,
    });
  });
});
