import { describe, expect, it } from "vitest";

import { fitsRoom, GECKO_LINES_MAX, isRunnable, parsePastedCode } from "./code_rules";
import { cleanCodes, codeLists, gameIniNames, parseDatabaseCodes, parseIniCodes, startingCodes } from "./codes";
import type { ModCode } from "./types";

const gecko = (name: string, lines = 1): ModCode => ({
  name,
  kind: "gecko",
  lines: Array.from({ length: lines }, (_, i) => `04${i.toString(16).padStart(6, "0").toUpperCase()} 00000001`),
});

describe("game inis", () => {
  it("are read in Dolphin's order", () => {
    expect(gameIniNames("GALE01", 2)).toEqual(["G", "GAL", "GALE01", "GALE01r2"]);
    expect(gameIniNames("GALE01")).toEqual(["G", "GAL", "GALE01"]);
  });

  it("give their codes as Dolphin reads them", () => {
    const ini = parseIniCodes(
      [
        "[Gecko]",
        "# a comment",
        "$Netplay Safe Kill Music [Dan Salvato, Summate]",
        "*First note",
        "*Second note",
        "c21c28bc 00000002",
        "+$On Already",
        "04001234 00000001",
        "$Has Options",
        "0412XXXX 00000001",
        "[Gecko_Enabled]",
        "$Netplay Safe Kill Music",
        "[ActionReplay]",
        "$Max Coins [not an author]",
        "0C3B3D2A-8A3C4-1D2E8",
        "[ActionReplay_Disabled]",
        "$Max Coins [not an author]",
        "[Core]",
        "MMU = True",
      ].join("\r\n"),
    );
    expect(ini.codes).toEqual([
      {
        name: "Netplay Safe Kill Music",
        author: "Dan Salvato, Summate",
        kind: "gecko",
        lines: ["C21C28BC 00000002"],
        notes: "First note\nSecond note",
        enabled: false,
      },
      { name: "On Already", kind: "gecko", lines: ["04001234 00000001"], enabled: true },
      { name: "Has Options", kind: "gecko", lines: ["0412XXXX 00000001"], enabled: false },
      // An AR code's whole title is its name.
      { name: "Max Coins [not an author]", kind: "ar", lines: ["0C3B3D2A-8A3C4-1D2E8"], enabled: false },
    ]);
    expect([...ini.enabled]).toEqual(["gecko\u0000Netplay Safe Kill Music"]);
    expect([...ini.disabled]).toEqual(["ar\u0000Max Coins [not an author]"]);
    expect(isRunnable(ini.codes[2])).toBe(false);
  });

  it("start a game with what Dolphin runs: on in the player's ini, or Dolphin's own unless turned off", () => {
    const own = parseIniCodes(
      ["[Gecko]", "$Mine", "04000000 00000001", "[Gecko_Enabled]", "$Mine", "[Gecko_Disabled]", "$SSL Patch"].join(
        "\n",
      ),
    );
    const dolphin = parseIniCodes(
      [
        "[Gecko]",
        "$SSL Patch",
        "04000004 00000001",
        "$Default On",
        "04000008 00000001",
        "[Gecko_Enabled]",
        "$SSL Patch",
        "$Default On",
      ].join("\n"),
    );
    expect(startingCodes(own, dolphin).on.map((c) => [c.name, c.source])).toEqual([
      ["Mine", "own"],
      ["Default On", "dolphin"],
    ]);
  });

  it("show a code once, in the first list that has it", () => {
    const code = "[Gecko]\n$Shared\n04000000 00000001\n$Other\n04000004 00000001\n";
    const lists = codeLists(
      { ...parseIniCodes(code), files: ["own.ini"] },
      parseIniCodes(code + "$Dolphin Only\n04000008 00000001\n"),
      [{ name: "Other", kind: "gecko", lines: ["04000004 00000001"] }],
    );
    expect(lists.own.map((c) => c.name)).toEqual(["Shared"]);
    expect(lists.dolphin.map((c) => c.name)).toEqual(["Dolphin Only"]);
  });
});

describe("the code database", () => {
  it("is read as Dolphin's Download Codes reads it, each version's codes saying their version", () => {
    const text = [
      "GALE01",
      "Super Smash Bros. Melee",
      "",
      "Version 1.0",
      "",
      "Target Test Never Ends [Link Master]",
      "041C3374 38030000",
      "",
      "Unrestricted Pause Camera [Link Master]",
      "04450F94 42000000",
      "04450F98 42000000",
      "Co-author: Jay007",
      "",
      "Version 1.1",
      "",
      "Target Test Never Ends [Link Master]",
      "041C3CD8 38030000",
      "",
    ].join("\r\n");
    expect(parseDatabaseCodes(text)).toEqual([
      {
        name: "Target Test Never Ends",
        author: "Link Master",
        kind: "gecko",
        lines: ["041C3374 38030000"],
        notes: "Version 1.0",
      },
      {
        name: "Unrestricted Pause Camera",
        author: "Link Master",
        kind: "gecko",
        lines: ["04450F94 42000000", "04450F98 42000000"],
        notes: "Version 1.0\nCo-author: Jay007",
      },
      {
        name: "Target Test Never Ends",
        author: "Link Master",
        kind: "gecko",
        lines: ["041C3CD8 38030000"],
        notes: "Version 1.1",
      },
    ]);
  });
});

describe("a pasted code", () => {
  it("is Gecko or Action Replay, as picked, and an encrypted line makes it Action Replay", () => {
    expect(parsePastedCode(" 0426b6a0   0000001e \n\n04001234 00000001", "gecko")).toEqual({
      kind: "gecko",
      lines: ["0426B6A0 0000001E", "04001234 00000001"],
    });
    expect(parsePastedCode("04002F04 CAFEBABE", "ar")?.kind).toBe("ar");
    expect(parsePastedCode("0C3B-3D2A-8A3C4\n1D2E-8F00-3B1C2", "gecko")).toEqual({
      kind: "ar",
      lines: ["0C3B-3D2A-8A3C4", "1D2E-8F00-3B1C2"],
    });
  });

  it("is refused when a line is not a code line", () => {
    expect(parsePastedCode("0412XXXX 00000001", "gecko")).toBeNull();
    expect(parsePastedCode("hello", "gecko")).toBeNull();
    expect(parsePastedCode("0C3B-3D2A-8A3C4", "gecko")?.kind).toBe("ar");
    expect(parsePastedCode("   ", "gecko")).toBeNull();
  });
});

describe("a room's codes", () => {
  it("fit the Gecko code lines the game's RAM holds", () => {
    expect(fitsRoom([gecko("a", GECKO_LINES_MAX)])).toBe(true);
    expect(fitsRoom([gecko("a", GECKO_LINES_MAX), gecko("b")])).toBe(false);
    // Action Replay codes run from Dolphin, not from the game's RAM.
    expect(fitsRoom([gecko("a", GECKO_LINES_MAX), { name: "ar", kind: "ar", lines: ["04002F04 CAFEBABE"] }])).toBe(
      true,
    );
  });

  it("are kept checked: runnable, in order, as many as fit", () => {
    const big = gecko("big", GECKO_LINES_MAX - 1);
    const kept = cleanCodes({
      on: [
        { ...big, source: "dolphin" },
        { ...gecko("too many", 2), source: "dolphin" },
        { ...gecko("fits"), source: "own" },
        { name: "options", kind: "gecko", lines: ["0412XXXX 00000001"], source: "database" },
      ],
      yours: [{ name: " \u0001 ", kind: "gecko", lines: ["04000000 00000001"] }],
    });
    expect(kept.on.map((c) => c.name)).toEqual(["big", "fits"]);
    expect(kept.yours).toEqual([]);
  });
});
