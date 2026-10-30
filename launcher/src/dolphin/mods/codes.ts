import { readFile } from "fs/promises";
import path from "path";

import { fitsRoom, isRunnable, normalizeLine } from "./code_rules";
import DOLPHIN_CODES from "./dolphin_codes.json";
import type { ModBuildCodes, ModCode, ModCodeLists, ModCodeSource, ModOnCode } from "./types";

/**
 * A room's Gecko and Action Replay codes (docs/design/room-codes.md): the lists the Codes dialog
 * shows (the player's own Dolphin's, Dolphin's own, the code database's), what a game starts with,
 * and what its room runs.
 */

/** The ini files Dolphin reads for a game, in its order (ConfigLoaders::GetGameIniFilenames). */
export function gameIniNames(gameId: string, revision?: number | null): string[] {
  const names = gameId.length === 6 ? [gameId.slice(0, 1), gameId.slice(0, 3)] : [];
  names.push(gameId);
  if (revision != null) {
    names.push(`${gameId}r${revision}`);
  }
  return names;
}

type IniCodes = {
  codes: (ModCode & { enabled: boolean })[];
  /** The `$name` lines of [Gecko_Enabled] / [ActionReplay_Enabled], and of the _Disabled ones. */
  enabled: Set<string>;
  disabled: Set<string>;
};

const enabledKey = (kind: ModCode["kind"], name: string) => `${kind}\u0000${name}`;

/** The codes in a game ini's [Gecko] and [ActionReplay] sections, as Dolphin reads them
 * (GeckoCodeConfig.cpp LoadCodes, ActionReplay.cpp LoadCodes): `$Name [author]` (`+$` on), then
 * lines; `*` lines of a Gecko code are its notes. */
export function parseIniCodes(text: string): IniCodes {
  const out: IniCodes = { codes: [], enabled: new Set(), disabled: new Set() };
  let section = "";
  let code: (ModCode & { enabled: boolean }) | null = null;
  const end = () => {
    if (code && code.lines.length > 0) {
      out.codes.push(code);
    }
    code = null;
  };
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    const header = /^\[([^\]]+)\]$/.exec(line);
    if (header) {
      end();
      section = header[1];
      continue;
    }
    if (!line || line.startsWith("#")) {
      continue;
    }
    const kind: ModCode["kind"] | null = section.startsWith("Gecko")
      ? "gecko"
      : section.startsWith("ActionReplay")
      ? "ar"
      : null;
    if (!kind) {
      continue;
    }
    if (section.endsWith("_Enabled") || section.endsWith("_Disabled")) {
      if (line.startsWith("$")) {
        (section.endsWith("_Enabled") ? out.enabled : out.disabled).add(enabledKey(kind, line.slice(1).trim()));
      }
      continue;
    }
    if (section !== "Gecko" && section !== "ActionReplay") {
      continue;
    }
    if (line.startsWith("$") || (kind === "gecko" && line.startsWith("+$"))) {
      end();
      const on = line.startsWith("+");
      const title = line.slice(on ? 2 : 1);
      // Gecko: "Name [author]" (Dolphin stops the name at '['); an AR code's name is all of it.
      const m = kind === "gecko" ? /^([^[]*)(?:\[([^\]]*)\]?)?/.exec(title) : null;
      const name = (m ? m[1] : title).trim();
      const author = m?.[2]?.trim();
      code = { name, kind, lines: [], enabled: on, ...(author ? { author } : {}) };
      continue;
    }
    if (!code) {
      continue;
    }
    const c: ModCode & { enabled: boolean } = code;
    if (kind === "gecko" && line.startsWith("*")) {
      const note = line.slice(1).trim();
      c.notes = c.notes ? `${c.notes}\n${note}` : note;
      continue;
    }
    c.lines.push(normalizeLine(line, kind) ?? line);
  }
  end();
  return out;
}

/** Several inis read as one (Dolphin's game inis are layered: the later's lines after the earlier's). */
function mergeIni(texts: string[]): IniCodes {
  const out: IniCodes = { codes: [], enabled: new Set(), disabled: new Set() };
  for (const text of texts) {
    const one = parseIniCodes(text);
    out.codes.push(...one.codes);
    one.enabled.forEach((k) => out.enabled.add(k));
    one.disabled.forEach((k) => out.disabled.add(k));
  }
  return out;
}

const plain = ({ enabled: _enabled, ...code }: ModCode & { enabled?: boolean }): ModCode => code;

/** Dolphin's own codes for the game (dolphin_codes.json), with the ones Dolphin turns on itself. */
export function dolphinCodes(gameId: string, revision?: number | null): IniCodes {
  const table = DOLPHIN_CODES as Record<string, string>;
  return mergeIni(gameIniNames(gameId, revision).flatMap((n) => (table[n] ? [table[n]] : [])));
}

/** The game's codes in the player's Dolphin User folders (`GameSettings/<id>.ini` and the like),
 * and the files they came from. A folder that is not there is skipped. */
export async function ownDolphinCodes(
  folders: string[],
  gameId: string,
  revision?: number | null,
): Promise<IniCodes & { files: string[] }> {
  const texts: string[] = [];
  const files: string[] = [];
  for (const folder of folders) {
    for (const name of gameIniNames(gameId, revision)) {
      const file = path.join(folder, "GameSettings", `${name}.ini`);
      const text = await readFile(file, "utf8").catch(() => null);
      if (text !== null) {
        const parsed = parseIniCodes(text);
        if (parsed.codes.length > 0 || parsed.enabled.size > 0) {
          texts.push(text);
          files.push(file);
        }
      }
    }
  }
  return { ...mergeIni(texts), files };
}

/** The code database's text for a game (codes.rc24.xyz/txt.php, geckocodes.org's format), as
 * Dolphin reads it (GeckoCodeConfig.cpp DownloadCodes): a header of 3 lines, then codes apart by a
 * blank line: "Name [author]", the lines, then notes. All are Gecko codes. */
export function parseDatabaseCodes(text: string): ModCode[] {
  const lines = text.split(/\r?\n/).slice(3);
  const codes: ModCode[] = [];
  let code: ModCode | null = null;
  let state = 0;
  // A game with codes for several of its versions (Melee 1.0, 1.1, 1.02) has a "Version 1.1" line
  // alone before each version's codes: their notes say it.
  let version: string | null = null;
  const end = () => {
    const c: ModCode | null = code;
    if (c && c.lines.length > 0) {
      codes.push(version ? { ...c, notes: c.notes ? `${version}\n${c.notes}` : version } : c);
    } else if (c && state === 1 && /^version\b/i.test(c.name)) {
      version = c.name;
    }
    code = null;
    state = 0;
  };
  for (const raw of lines) {
    const line = raw.trim();
    if (!line) {
      end();
      continue;
    }
    if (state === 0) {
      const m = /^([^[]*)(?:\[([^\]]*)\]?)?/.exec(line)!;
      code = { name: m[1].trim() || line, kind: "gecko", lines: [], ...(m[2]?.trim() ? { author: m[2].trim() } : {}) };
      state = 1;
      continue;
    }
    const c: ModCode = code!;
    const pieces = line.split(/\s+/);
    if (state === 1 && pieces.length >= 2 && pieces[0].length === 8 && pieces[1].length === 8) {
      c.lines.push(normalizeLine(`${pieces[0]} ${pieces[1]}`, "gecko") ?? `${pieces[0]} ${pieces[1]}`);
      continue;
    }
    state = 2;
    c.notes = c.notes ? `${c.notes}\n${line}` : line;
  }
  end();
  return codes;
}

const sameCode = (a: ModCode, b: ModCode) =>
  a.kind === b.kind && a.name === b.name && a.lines.join("\n") === b.lines.join("\n");

/** The dialog's lists: a code shows once, in the first list that has it (Yours, Your Dolphin,
 * Dolphin, the database). */
export function codeLists(own: IniCodes & { files: string[] }, dolphin: IniCodes, yours: ModCode[]): ModCodeLists {
  const ownCodes: ModCode[] = [];
  for (const c of own.codes.map(plain)) {
    if (!yours.some((y) => sameCode(y, c)) && !ownCodes.some((o) => sameCode(o, c))) {
      ownCodes.push(c);
    }
  }
  const dolphinCodesList: ModCode[] = [];
  for (const c of dolphin.codes.map(plain)) {
    const shown = [...yours, ...ownCodes, ...dolphinCodesList];
    if (!shown.some((o) => sameCode(o, c))) {
      dolphinCodesList.push(c);
    }
  }
  return { own: ownCodes, ownFiles: own.files, dolphin: dolphinCodesList };
}

/** The codes a game starts with: the ones on in the player's own Dolphin (Dolphin's rule: a code
 * is on when its ini says `+$`, its name is in an _Enabled list, or Dolphin turns it on itself and
 * no _Disabled list names it). */
export function startingCodes(own: IniCodes, dolphin: IniCodes): ModBuildCodes {
  const on: ModOnCode[] = [];
  const isOn = (c: ModCode & { enabled: boolean }, defaultOn: boolean) => {
    const key = enabledKey(c.kind, c.name);
    return c.enabled || own.enabled.has(key) || (defaultOn && !own.disabled.has(key));
  };
  const add = (c: ModCode & { enabled: boolean }, source: ModCodeSource) => {
    const code = plain(c);
    if (isRunnable(code) && !on.some((o) => sameCode(o, code))) {
      on.push({ ...code, source });
    }
  };
  for (const c of own.codes) {
    if (isOn(c, false)) {
      add(c, "own");
    }
  }
  for (const c of dolphin.codes) {
    // The player's own copy of one of Dolphin's codes (by its name) stands for it.
    const own = on.some((o) => o.kind === c.kind && o.name === c.name);
    if (!own && isOn(c, dolphin.enabled.has(enabledKey(c.kind, c.name)))) {
      add(c, "dolphin");
    }
  }
  return { on: fitRoom(on), yours: [] };
}

/** The first of these codes that fit a room, in order. */
function fitRoom<T extends ModCode>(codes: T[]): T[] {
  const out: T[] = [];
  for (const c of codes) {
    if (fitsRoom([...out, c])) {
      out.push(c);
    }
  }
  return out;
}

/** A game's codes as the player changed them, checked: the on ones runnable, within the room's
 * limits, the player's own with clean names. */
export function cleanCodes(codes: ModBuildCodes): ModBuildCodes {
  const clean = (c: ModCode): ModCode => ({
    name: Array.from(c.name)
      .filter((ch) => ch >= " " && ch !== "\u007f")
      .join("")
      .trim()
      .slice(0, 128),
    kind: c.kind === "ar" ? "ar" : "gecko",
    lines: c.lines.map((l) => normalizeLine(l, c.kind) ?? l.trim()),
    ...(c.author ? { author: c.author } : {}),
    ...(c.notes ? { notes: c.notes } : {}),
  });
  const yours = codes.yours.map(clean).filter((c) => c.name && isRunnable(c));
  const on = fitRoom(codes.on.map((c) => ({ ...clean(c), source: c.source })).filter((c) => c.name && isRunnable(c)));
  return { on, yours };
}
