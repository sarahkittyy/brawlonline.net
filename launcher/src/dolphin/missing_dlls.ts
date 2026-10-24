// Why Dolphin could not start on Windows (exit 0xC0000135, a DLL not found, or 0xC000007B, a DLL
// that is not 64-bit): reads the import tables of Dolphin.exe and of the DLLs beside it and looks
// for each imported DLL where Windows would, so the toast can name the file and what installs it
// (board card 2026-10-24: a player still had "Required DLLs ... are missing" after installing the
// Visual C++ Redistributable, and nothing said which DLL).
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

import type { MissingDll } from "./types";

const IMAGE_FILE_MACHINE_AMD64 = 0x8664;

type PeInfo = { machine: number; imports: string[] };

/** The machine type and the load-time imports (not delay-loaded ones) of a PE file. */
export function readPe(data: Buffer): PeInfo | null {
  if (data.length < 0x40 || data.readUInt16LE(0) !== 0x5a4d) {
    return null;
  }
  const pe = data.readUInt32LE(0x3c);
  if (pe + 24 > data.length || data.readUInt32LE(pe) !== 0x4550) {
    return null;
  }
  const machine = data.readUInt16LE(pe + 4);
  const sectionCount = data.readUInt16LE(pe + 6);
  const optionalSize = data.readUInt16LE(pe + 20);
  const optional = pe + 24;
  const magic = data.readUInt16LE(optional);
  const dataDirs = optional + (magic === 0x20b ? 112 : 96);
  const importRva = data.readUInt32LE(dataDirs + 8);
  const sections: { va: number; size: number; raw: number }[] = [];
  for (let i = 0; i < sectionCount; i++) {
    const s = optional + optionalSize + i * 40;
    const virtualSize = data.readUInt32LE(s + 8);
    const rawSize = data.readUInt32LE(s + 16);
    sections.push({
      va: data.readUInt32LE(s + 12),
      size: Math.max(virtualSize, rawSize),
      raw: data.readUInt32LE(s + 20),
    });
  }
  const offset = (rva: number): number | null => {
    const s = sections.find((sec) => rva >= sec.va && rva < sec.va + sec.size);
    return s ? rva - s.va + s.raw : null;
  };
  const cString = (at: number): string => {
    const end = data.indexOf(0, at);
    return data.toString("latin1", at, end < 0 ? data.length : end);
  };
  const imports: string[] = [];
  let entry = importRva ? offset(importRva) : null;
  while (entry !== null && entry + 20 <= data.length) {
    const nameRva = data.readUInt32LE(entry + 12);
    if (nameRva === 0) {
      break;
    }
    const at = offset(nameRva);
    if (at !== null) {
      imports.push(cString(at));
    }
    entry += 20;
  }
  return { machine, imports };
}

/** API sets resolve inside Windows, not to files. */
const isApiSet = (name: string) => /^(api|ext)-ms-/i.test(name);

/**
 * The DLLs Dolphin (`exePath`) imports that Windows cannot load: looked for beside it (and then
 * the DLLs found there in turn), else in System32 or a PATH folder.
 */
export async function findMissingDlls(
  exePath: string,
  systemDir = path.join(process.env.SystemRoot ?? "C:\\Windows", "System32"),
  pathDirs = (process.env.PATH ?? "").split(path.delimiter).filter(Boolean),
): Promise<MissingDll[]> {
  const appDir = path.dirname(exePath);
  const found: MissingDll[] = [];
  const seen = new Set<string>();
  const exists = (file: string) =>
    fs.promises.stat(file).then(
      (s) => s.isFile(),
      () => false,
    );
  const machineOf = async (file: string) =>
    readPe(await fs.promises.readFile(file).catch(() => Buffer.alloc(0)))?.machine;

  const visit = async (file: string, label: string) => {
    const pe = readPe(await fs.promises.readFile(file));
    if (!pe) {
      return;
    }
    for (const name of pe.imports) {
      const key = name.toLowerCase();
      if (isApiSet(name) || seen.has(key)) {
        continue;
      }
      seen.add(key);
      const local = path.join(appDir, name);
      if (await exists(local)) {
        if ((await machineOf(local)) !== IMAGE_FILE_MACHINE_AMD64) {
          found.push({ name, importedBy: label, problem: "not-x64" });
        } else {
          await visit(local, name);
        }
        continue;
      }
      let where: string | null = null;
      for (const dir of [systemDir, ...pathDirs]) {
        if (await exists(path.join(dir, name))) {
          where = path.join(dir, name);
          break;
        }
      }
      if (!where) {
        found.push({ name, importedBy: label, problem: "missing" });
      } else if ((await machineOf(where)) !== IMAGE_FILE_MACHINE_AMD64) {
        found.push({ name, importedBy: label, problem: "not-x64" });
      }
    }
  };
  await visit(exePath, path.basename(exePath));
  return found;
}
