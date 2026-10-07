import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, describe, expect, it } from "vitest";

import { DiscFolderSource, DiscIsoSource, readLayered, resolveDiscRoot, sdNameVariants } from "./sources";

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "brawl-sources-test-"));
afterAll(() => fs.rmSync(tmpRoot, { recursive: true, force: true }));

function write(p: string, data: string) {
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, data);
}

describe("sources", () => {
  it("maps disc names to P+ SD names", () => {
    expect(sdNameVariants("menu2/mu_menumain_en.pac")).toEqual(["menu2/mu_menumain.pac", "menu2/mu_menumain_en.pac"]);
    expect(sdNameVariants("/menu/common/StockFaceTex_en.brres")[0]).toBe("menu/common/StockFaceTex.brres");
    expect(sdNameVariants("system/font/font_latin1.arc")).toEqual(["system/font/font_latin1.arc"]);
  });

  it("finds the files root of an extracted disc", async () => {
    const disc = path.join(tmpRoot, "rev1");
    write(path.join(disc, "DATA", "files", "system", "x.bin"), "x");
    write(path.join(disc, "DATA", "files", "menu2", "a_en.pac"), "disc");
    expect(await resolveDiscRoot(disc)).toBe(path.join(disc, "DATA", "files"));
    expect(await resolveDiscRoot(path.join(disc, "DATA"))).toBe(path.join(disc, "DATA", "files"));
    await expect(resolveDiscRoot(path.join(tmpRoot, "nothing"))).rejects.toThrow();
    const src = await DiscFolderSource.open(disc);
    expect((await src.read("menu2/a_en.pac"))!.toString()).toBe("disc");
    expect(await src.read("menu2/missing.pac")).toBeNull();
  });

  it("prefers the SD copy over the disc copy", async () => {
    const disc = path.join(tmpRoot, "rev1");
    const src = await DiscFolderSource.open(disc);
    const sd = {
      kind: "sd" as const,
      label: "fake",
      read: async (p: string) => (p === "menu2/a.pac" ? Buffer.from("pplus") : null),
      close: async () => undefined,
    };
    const hits = await readLayered(src, sd, "menu2/a_en.pac");
    expect(hits.map((h) => `${h.origin}:${h.path}:${h.data}`)).toEqual([
      "sd:menu2/a.pac:pplus",
      "disc:menu2/a_en.pac:disc",
    ]);
    expect((await readLayered(src, null, "menu2/a_en.pac")).length).toBe(1);
  });

  it("drives DolphinTool to extract single files from an ISO", async () => {
    const iso = path.join(tmpRoot, "game.iso");
    const tool = path.join(tmpRoot, "DolphinTool.exe");
    write(iso, "iso");
    write(tool, "");
    const calls: string[][] = [];
    const exec = async (_file: string, args: string[]) => {
      calls.push(args);
      const out = args[args.indexOf("-o") + 1];
      const single = args[args.indexOf("-s") + 1];
      if (single === "/menu2/missing.pac") {
        throw Object.assign(new Error("exit 1"), {
          stdout: "",
          stderr: "Error: No file/folder was extracted. Maybe you misspelled your specified partition?",
        });
      }
      write(path.join(out, "DATA", "files", ...single.split("/").filter(Boolean)), `content of ${single}`);
      return { stdout: "", stderr: "" };
    };
    const src = await DiscIsoSource.open({ isoPath: iso, dolphinToolPath: tool, exec });
    expect((await src.read("menu2/sc_title_en.pac"))!.toString()).toBe("content of /menu2/sc_title_en.pac");
    // Cached: a second read does not run DolphinTool again.
    await src.read("menu2/sc_title_en.pac");
    expect(await src.read("menu2/missing.pac")).toBeNull();
    expect(calls.length).toBe(2);
    expect(calls[0]).toEqual(
      expect.arrayContaining(["extract", "-i", iso, "-p", "DATA", "-s", "/menu2/sc_title_en.pac", "-q"]),
    );
    await src.close();
  });

  it("reports DolphinTool failures other than a missing file", async () => {
    const iso = path.join(tmpRoot, "game2.iso");
    const tool = path.join(tmpRoot, "DolphinTool2.exe");
    write(iso, "iso");
    write(tool, "");
    const exec = async () => {
      throw Object.assign(new Error("boom"), { stdout: "", stderr: "Error: unable to open disc image" });
    };
    const src = await DiscIsoSource.open({ isoPath: iso, dolphinToolPath: tool, exec });
    await expect(src.read("menu2/x.pac")).rejects.toThrow(/unable to open disc image/);
    await src.close();
  });
});
