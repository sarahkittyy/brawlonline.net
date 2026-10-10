import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { IniFile, splitInterleavedLine } from "./ini_file";

let tmp: string;
let iniPath: string;

beforeEach(async () => {
  tmp = await fs.promises.mkdtemp(path.join(os.tmpdir(), "ini-file-"));
  iniPath = path.join(tmp, "Dolphin.ini");
});

afterEach(async () => {
  await fs.promises.rm(tmp, { recursive: true, force: true });
});

const read = (): Promise<string> => fs.promises.readFile(iniPath, "utf8");
const write = (text: string): Promise<void> => fs.promises.writeFile(iniPath, text);

/** A Dolphin.ini like the launcher's netplay one, with DefaultISO set to `iso`. */
async function dolphinIni(iso: string): Promise<IniFile> {
  const ini = await IniFile.init(iniPath);
  ini.getOrCreateSection("Core").set("DefaultISO", iso);
  const online = ini.getOrCreateSection("Online");
  online.set("ReplayDir", "C:\\Users\\player\\Documents\\Brawl Online");
  online.set("SaveReplays", "True");
  online.set("ReplayMonthlyFolders", "True");
  ini.getOrCreateSection("Analytics").set("ID", "0123456789abcdef0123456789abcdef");
  ini.getOrCreateSection("NetPlay").set("TraversalChoice", "traversal");
  const hints = ini.getOrCreateSection("SDL_Hints");
  for (let i = 0; i < 20; i++) {
    hints.set(`SDL_JOYSTICK_HINT_${i}`, "1");
  }
  return ini;
}

describe("IniFile.save", () => {
  it("leaves one whole file when two saves overlap", async () => {
    // The launcher's isoPath subscription saved Dolphin.ini twice at once, with DefaultISO
    // values one byte apart. On Windows the two writes interleaved line by line.
    for (let round = 0; round < 50; round++) {
      await fs.promises.rm(iniPath, { force: true });
      const a = await dolphinIni("E:\\Roms\\Wii\\SSBB_NTSC.iso");
      const b = await dolphinIni("E:\\Roms\\Wii\\SSBB_NTSC2.iso");
      await Promise.all([a.save(), b.save()]);
      expect([a.toString(), b.toString()]).toContain(await read());
    }
    expect(await fs.promises.readdir(tmp)).toEqual(["Dolphin.ini"]);
  });

  it("creates the folder", async () => {
    iniPath = path.join(tmp, "User", "Config", "Dolphin.ini");
    const ini = await IniFile.init(iniPath);
    ini.getOrCreateSection("Core").set("DefaultISO", "a.iso");
    await ini.save();
    expect(await read()).toBe("[Core]\nDefaultISO = a.iso\n");
  });
});

describe("IniFile.modify", () => {
  it("keeps the changes of overlapping edits", async () => {
    await write("[Core]\nDefaultISO = old.iso\n[General]\nISOPaths = 0\n");
    await Promise.all([
      IniFile.modify(iniPath, async (ini) => {
        ini.getOrCreateSection("General").set("ISOPath0", "E:\\Roms\\Wii");
        await ini.save();
      }),
      IniFile.modify(iniPath, async (ini) => {
        ini.getOrCreateSection("Core").set("DefaultISO", "new.iso");
        await ini.save();
      }),
    ]);
    expect(await read()).toBe("[Core]\nDefaultISO = new.iso\n[General]\nISOPaths = 0\nISOPath0 = E:\\Roms\\Wii\n");
  });

  it("runs the next edit after one that throws", async () => {
    await write("[Core]\nA = 1\n");
    const failed = IniFile.modify(iniPath, async () => {
      throw new Error("boom");
    });
    const next = IniFile.modify(iniPath, async (ini) => ini.getSection("Core")?.get("A", ""));
    await expect(failed).rejects.toThrow("boom");
    await expect(next).resolves.toBe("1");
  });
});

describe("IniFile.init", () => {
  it("reads a missing file as empty", async () => {
    const ini = await IniFile.init(iniPath);
    expect(ini.toString()).toBe("");
  });

  it("reads CRLF files, as Dolphin writes them on Windows, and saves them with LF", async () => {
    await write("[Core]\r\nDefaultISO = E:\\a.iso\r\n[Online]\r\nSaveReplays = True\r\n");
    const ini = await IniFile.init(iniPath);
    expect(ini.getSection("Core")?.get("DefaultISO", "")).toBe("E:\\a.iso");
    expect(ini.getSection("Online")?.get("SaveReplays", "")).toBe("True");
    expect(ini.repaired).toBe(false);
    await ini.save();
    expect(await read()).toBe("[Core]\nDefaultISO = E:\\a.iso\n[Online]\nSaveReplays = True\n");
  });

  it("skips a UTF-8 BOM", async () => {
    await write("\uFEFF[Core]\nDefaultISO = a.iso\n");
    const ini = await IniFile.init(iniPath);
    expect(ini.getSection("Core")?.get("DefaultISO", "")).toBe("a.iso");
  });

  it("keeps Gecko code lines verbatim", async () => {
    const text = "[Gecko]\n$Code name [Author]\n04000000 00000000\n*note\n\n[Gecko_Enabled]\n$Code name [Author]\n\n";
    await write(text);
    const ini = await IniFile.init(iniPath);
    expect(ini.repaired).toBe(false);
    expect(ini.toString()).toBe(text);
  });

  for (const [eol, name] of [
    ["\n", "LF"],
    ["\r\n", "CRLF"],
  ] as const) {
    it(`recovers the keys of an interleaved save (${name})`, async () => {
      // What the overlapping saves above wrote before the fix: from ReplayMonthlyFolders on,
      // lines lost their newlines and mostly gained a doubled first character.
      await write(
        [
          "[Core]",
          "DefaultISO = E:\\Roms\\Wii\\SSBB_NTSC.iso",
          "[Online]",
          "ReplayDir = C:\\Users\\player\\Documents\\Brawl Online",
          "SaveReplays = True",
          "ReplayMonthlyFolders = True[[Analytics]IID = 0123456789abcdef0123456789abcdef[[NetPlay]" +
            "TTraversalChoice = traversal[[SDL_Hints]SSDL_JOYSTICK_HINT_0 = 1SSDL_JOYSTICK_HINT_1 = 1" +
            "SSDL_JOYSTICK_HINT_2 = 1SDL_JOYSTICK_HINT_3 = 1",
          "SDL_JOYSTICK_HINT_4 = 1",
          "",
        ].join(eol),
      );

      const ini = await IniFile.init(iniPath);
      expect(ini.repaired).toBe(true);
      // HINT_2 and HINT_3 ran together without a doubled character, so they are dropped
      // (whoever owns them writes them again). HINT_4 stays in the section readers have
      // always put it in.
      expect(ini.toString()).toBe(
        [
          "[Core]",
          "DefaultISO = E:\\Roms\\Wii\\SSBB_NTSC.iso",
          "[Online]",
          "ReplayDir = C:\\Users\\player\\Documents\\Brawl Online",
          "SaveReplays = True",
          "SDL_JOYSTICK_HINT_4 = 1",
          "ReplayMonthlyFolders = True",
          "[Analytics]",
          "ID = 0123456789abcdef0123456789abcdef",
          "[NetPlay]",
          "TraversalChoice = traversal",
          "[SDL_Hints]",
          "SDL_JOYSTICK_HINT_0 = 1",
          "SDL_JOYSTICK_HINT_1 = 1",
          "",
        ].join("\n"),
      );
    });
  }
});

describe("splitInterleavedLine", () => {
  it("leaves whole lines alone", () => {
    for (const line of [
      "",
      "[Core]",
      "DefaultISO = E:\\Roms\\Wii\\SSBB.iso",
      "SSAA = True",
      "ReplayDir = C:\\Users\\Jessica\\Documents\\Brawl Online",
      "IndexServer = https://lobby.example/?aa=1&bb=2",
      "$Code name [Author]",
      "# comment [Core]",
      "Device = SDL/0/Controller [One]",
    ]) {
      expect(splitInterleavedLine(line), line).toBeUndefined();
    }
  });

  it("splits the lines of the user's Dolphin.ini", () => {
    expect(splitInterleavedLine("SSaveReplays = TrueRReplayMonthlyFolders = True[[Analytics]ID = be3ed531")).toEqual([
      "SaveReplays = True",
      "ReplayMonthlyFolders = True",
      "[Analytics]",
      "ID = be3ed531",
    ]);
    expect(splitInterleavedLine("TTraversalChoice = traversal[[SDL_Hints]SSDL_JOYSTICK_DIRECTINPUT = 1")).toEqual([
      "TraversalChoice = traversal",
      "[SDL_Hints]",
      "SDL_JOYSTICK_DIRECTINPUT = 1",
    ]);
  });

  it("splits a line that starts or ends with a doubled header", () => {
    expect(splitInterleavedLine("[[Analytics]IID = abc")).toEqual(["[Analytics]", "ID = abc"]);
    expect(splitInterleavedLine("A = 1[[Analytics]")).toEqual(["A = 1", "[Analytics]"]);
  });

  it("splits after the last of a run of equal characters", () => {
    expect(splitInterleavedLine("Name = BARRReplayDir = x")).toEqual(["Name = BAR", "ReplayDir = x"]);
  });

  it("drops parts that ran together without a doubled character", () => {
    expect(splitInterleavedLine("AA = 1BB = 2C = 3")).toEqual(["A = 1"]);
  });
});
