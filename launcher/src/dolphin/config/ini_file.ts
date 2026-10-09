/**
 * rewrite of Inifile.cpp/Inifile.h from dolphin in Typescript
 * https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/IniFile.cpp
 */

import electronLog from "electron-log";
import fs from "fs";
import path from "path";

const log = electronLog.scope("iniFile");

/** The start of a `key = value` line as Dolphin and the launcher write them. */
const KEY_START = /^[A-Za-z_][\w.]* = /;
/** A section header such as `[Online]` or `[SDL_Hints]`. */
const HEADER = /^\[[A-Za-z][\w]*\]/;

function startsLine(text: string): boolean {
  return KEY_START.test(text) || HEADER.test(text);
}

/**
 * Splits a line that two overlapping saves wrote over each other. Each writer wrote the
 * file line by line at its own offset, and when the offsets differed by one byte the
 * second writer's lines landed one byte later, so a run of lines lost their newlines and
 * usually gained a doubled first character:
 *
 *     SSaveReplays = TrueRReplayMonthlyFolders = True[[Analytics]ID = 0123...
 *
 * Returns the lines found in it (`SaveReplays = True`, `ReplayMonthlyFolders = True`,
 * `[Analytics]`, `ID = 0123...`), or undefined when the line is not one of these. A part
 * whose boundary left no doubled character runs into the next part
 * (`HINT_4 = 1SDL_JOYSTICK_HINT_5 = 1`); it cannot be split reliably, so it is dropped.
 */
export function splitInterleavedLine(line: string): string[] | undefined {
  if (line === "" || "#$+*".includes(line[0])) {
    return undefined;
  }

  const doubledAt = (j: number) => line[j] === line[j + 1] && startsLine(line.slice(j + 1));
  // Where the part after a boundary at `at` begins: past the doubled character, taking the
  // last pair in a run of equal characters ("BARRReplay..." is "BAR" + "R" + "Replay...").
  const partStart = (at: number) => {
    if (!doubledAt(at)) {
      return at;
    }
    let pair = at;
    while (doubledAt(pair + 1)) {
      pair++;
    }
    return pair + 1;
  };

  const parts: string[] = [];
  let start = partStart(0);
  let j = start + 1;
  while (j < line.length) {
    const header = line[start] === "[" ? HEADER.exec(line.slice(start)) : null;
    if (header) {
      // A header part ends at its "]".
      parts.push(header[0]);
      start = partStart(start + header[0].length);
      j = start + 1;
    } else if (doubledAt(j) && line.slice(start, j).includes(" = ")) {
      // Only inside a value: "SSAA = 1" is a key, not "S" + "A = 1".
      const next = partStart(j);
      parts.push(line.slice(start, next - 1));
      start = next;
      j = start + 1;
    } else {
      j++;
    }
  }
  if (start < line.length) {
    parts.push(line.slice(start));
  }

  if (parts.length < 2) {
    return undefined;
  }
  return parts.filter((part) => {
    const header = HEADER.exec(part);
    if (header) {
      return header[0] === part;
    }
    return KEY_START.test(part) && !part.slice(part.indexOf(" = ") + 3).includes(" = ");
  });
}

/** Saves to the same file are queued here so their read-modify-write cycles don't overlap. */
const fileQueues = new Map<string, Promise<unknown>>();

function queueKey(filePath: string): string {
  const resolved = path.resolve(filePath);
  return process.platform === "linux" ? resolved : resolved.toLowerCase();
}

let tempCounter = 0;

/**
 * The IniFile Class, contains a Section subclass
 */
export class IniFile {
  private filePath: string;
  private sections: Section[];
  /** Whether loading split interleaved lines (see `splitInterleavedLine`); `save` writes them fixed. */
  repaired = false;

  private constructor(filePath: string) {
    this.filePath = filePath;
    this.sections = [];
  }

  /** Differs from IniFile.cpp via:
   * Instead of editing keyOut and valueOut by reference, return them */
  private static parseLine(line: string): readonly [string, string] | readonly [null, null] {
    let retValueOut = "";
    let keyOut = "";

    if (line === "" || line[0] === "#" || !line.includes("=")) {
      return [null, null] as const;
    }

    const firstEquals = line.indexOf("=");
    if (firstEquals !== -1) {
      keyOut = line.substring(0, firstEquals).trim();
      retValueOut = line
        .substring(firstEquals + 1)
        .trim()
        .replace(/(^"|"$)/g, ""); // remove quotes at the start or end of the string but not inside
    }

    return [keyOut, retValueOut] as const;
  }

  /**Differs from IniFile.cpp by:
   * returns section object, not pointer
   */
  getSection(sectionName: string): Section | undefined {
    const section = this.sections.find((section) => section.name === sectionName);
    return section;
  }

  /**Differs from IniFile.cpp by:
   * returns section object, not pointer
   */
  getOrCreateSection(sectionName: string): Section {
    let section = this.getSection(sectionName);
    if (section === undefined) {
      section = new Section(sectionName);
      this.sections.push(section);
    }
    return section;
  }

  deleteSection(sectionName: string): boolean {
    const s = this.getSection(sectionName);
    if (s === undefined) {
      return false;
    }
    this.sections.splice(this.sections.indexOf(s), 1);
    return true;
  }

  hasSection(sectionName: string): boolean {
    return this.getSection(sectionName) != undefined;
  }

  setLines(sectionName: string, lines: string[]): void {
    const section = this.getOrCreateSection(sectionName);
    section.setLines(lines);
  }

  deleteKey(sectionName: string, key: string): boolean {
    const section = this.getSection(sectionName);
    if (section === undefined) {
      return false;
    }
    return section.delete(key);
  }

  /**Differs from IniFile.cpp by:
   * returns keys instead of passing it by reference
   */
  getKeys(sectionName: string): string[] {
    const section = this.getSection(sectionName);
    if (section === undefined) {
      return [];
    }
    return section.keysOrder;
  }

  /**Differs from IniFile.cpp by:
   * returns lines instead of passing it by reference
   */
  getLines(sectionName: string, removeComments = false): string[] {
    const section = this.getSection(sectionName);
    if (section === undefined) {
      return [];
    }

    const lines = section.getLines(removeComments);

    return lines;
  }

  /** Loads `fileName`; a missing file loads as empty. */
  static async init(fileName: string): Promise<IniFile> {
    const iniFile = new IniFile(fileName);
    let text: string;
    try {
      text = await fs.promises.readFile(fileName, "utf8");
    } catch (err) {
      if ((err as NodeJS.ErrnoException).code !== "ENOENT") {
        log.error("failed to read file with error", err);
      }
      return iniFile;
    }
    iniFile.parse(text);
    return iniFile;
  }

  /**
   * Loads `fileName` and runs `edit` on it (which saves it), after any earlier `modify` of
   * the same file has finished, so overlapping edits don't drop each other's changes.
   */
  static async modify<T>(fileName: string, edit: (iniFile: IniFile) => Promise<T>): Promise<T> {
    const key = queueKey(fileName);
    const previous = fileQueues.get(key) ?? Promise.resolve();
    const current = previous.catch(() => undefined).then(async () => edit(await IniFile.init(fileName)));
    fileQueues.set(key, current);
    try {
      return await current;
    } finally {
      if (fileQueues.get(key) === current) {
        fileQueues.delete(key);
      }
    }
  }

  private parse(fileText: string): void {
    // Skips the UTF-8 BOM at the start of files. Notepad likes to add this.
    const text = fileText.charCodeAt(0) === 0xfeff ? fileText.slice(1) : fileText;

    // Keys recovered from interleaved lines, set after loading unless the file has them.
    const recovered: { section: string; key: string; value: string }[] = [];
    let currentSection: Section | undefined = undefined;
    for (const line of text.split(/\r\n|\n|\r/)) {
      // As in IniFile.cpp. `save` ends sections of raw lines with an empty line.
      if (line === "") {
        continue;
      }
      const parts = splitInterleavedLine(line);
      if (parts !== undefined) {
        this.repaired = true;
        let sectionName = currentSection?.name;
        for (const part of parts) {
          if (part[0] === "[") {
            sectionName = part.slice(1, -1);
          } else if (sectionName !== undefined) {
            const [key, value] = IniFile.parseLine(part);
            if (key !== null && value !== null) {
              recovered.push({ section: sectionName, key, value });
            }
          }
        }
        // Readers before this fix took a line starting with "[" as a section header, so the
        // lines after it are in that section.
        if (line[0] === "[" && sectionName !== undefined) {
          currentSection = this.getOrCreateSection(sectionName);
        }
        continue;
      }

      //section line
      if (line[0] === "[") {
        const endpos = line.indexOf("]");
        if (endpos !== -1) {
          //we have a new section
          const sub = line.substr(1, endpos - 1);
          currentSection = this.getOrCreateSection(sub);
        }
      } else {
        if (currentSection !== undefined) {
          const [key, value] = IniFile.parseLine(line);

          // Lines starting with '$', '*' or '+' are kept verbatim.
          // Kind of a hack, but the support for raw lines inside an
          // INI is a hack anyway.
          if (
            (key === null && value === null) ||
            (line.length !== 0 && ["$", "+", "*"].some((val) => line[0] === val))
          ) {
            currentSection.lines.push(line);
          } else if (key !== null && value !== null) {
            currentSection.set(key, value);
          }
        }
      }
    }

    // A key the file also has as a whole line was written after the interleaved save.
    for (const { section, key, value } of recovered) {
      const s = this.getOrCreateSection(section);
      if (!s.exists(key)) {
        s.set(key, value);
      }
    }
    if (this.repaired) {
      log.warn(`repaired interleaved lines in ${this.filePath}`);
    }
  }

  /** The file's text as `save` writes it. */
  toString(): string {
    let text = "";
    this.sections.forEach((section) => {
      // originally section.name was only written if the section was non-empty,
      // but that goes against us wanting to always show the Gecko section
      text += `[${section.name}]\n`;

      if (section.keysOrder.length === 0) {
        section.lines.forEach((line) => {
          text += `${line}\n`;
        });
        text += "\n";
      } else {
        section.keysOrder.forEach((kvit) => {
          const value = section.values.get(kvit);
          text += `${kvit} = ${value}\n`;
        });
      }
    });
    return text;
  }

  /**
   * Writes the whole file to a temporary file next to it and renames that over it, so a
   * reader never sees a half-written file and two saves can't interleave their lines.
   */
  async save(): Promise<void> {
    await fs.promises.mkdir(path.dirname(this.filePath), { recursive: true });
    const tempPath = `${this.filePath}.${process.pid}-${++tempCounter}.tmp`;
    await fs.promises.writeFile(tempPath, this.toString());
    try {
      await renameWithRetry(tempPath, this.filePath);
    } catch (err) {
      await fs.promises.rm(tempPath, { force: true });
      throw err;
    }
  }
}

/** Windows refuses to replace a file another process has open (Dolphin, antivirus) for a moment. */
async function renameWithRetry(from: string, to: string): Promise<void> {
  for (let attempt = 1; ; attempt++) {
    try {
      await fs.promises.rename(from, to);
      return;
    } catch (err) {
      const code = (err as NodeJS.ErrnoException).code;
      if (attempt >= 10 || (code !== "EPERM" && code !== "EACCES" && code !== "EBUSY")) {
        throw err;
      }
      await new Promise((resolve) => setTimeout(resolve, 20 * attempt));
    }
  }
}

/**
 * The Section class
 */
class Section {
  name: string;
  keysOrder: string[];
  lines: string[];
  values: Map<string, string>;

  constructor(name: string) {
    this.name = name;
    this.keysOrder = [];
    this.lines = [];
    this.values = new Map();
  }

  /**Differs from IniFile.cpp by:
   * passes key by value rather than address
   */
  set(key: string, newValue: string): void {
    const newKey = !this.values.has(key);
    if (newKey) {
      this.keysOrder.push(key);
    }
    this.values.set(key, newValue);
  }

  //TODO work around pass by reference
  // no idea what default value is for
  get(key: string, defaultValue: string): string {
    const value = this.values.get(key);

    if (value !== undefined) {
      return value;
    }

    return defaultValue;
  }

  exists(key: string): boolean {
    return this.values.get(key) !== undefined;
  }

  delete(key: string): boolean {
    const success = this.values.delete(key);
    if (success) {
      this.keysOrder.splice(this.keysOrder.indexOf(key), 1);
    }

    return success;
  }

  setLines(lines: string[]): void {
    this.lines = lines;
  }

  /**Differs from IniFile.cpp by:
   * returns lines instead of passing it by reference
   */
  getLines(removeComments: boolean): string[] {
    const lines: string[] = [];
    this.lines.forEach((l) => {
      let line = l.trim();
      if (removeComments) {
        const commentPos = line.indexOf("#");
        if (commentPos === 0) {
          return;
        }
        if (commentPos !== -1) {
          line = line.substring(0, commentPos);
        }
      }
      if (line !== "\n" && line !== "") {
        lines.push(line);
      }
    });
    return lines;
  }
}
