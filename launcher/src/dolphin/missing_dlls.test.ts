import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { findMissingDlls, readPe } from "./missing_dlls";

const AMD64 = 0x8664;
const I386 = 0x14c;

/** A minimal PE file: one section holding the import directory and the DLL names. */
function makePe(machine: number, imports: string[]): Buffer {
  const peAt = 0x40;
  const optionalSize = 240; // PE32+: 112 bytes of fields, then 16 data directories
  const sectionTable = peAt + 24 + optionalSize;
  const raw = 0x200;
  const va = 0x1000;
  const descriptors = (imports.length + 1) * 20;
  const names = imports.map((n) => Buffer.from(n + "\0", "latin1"));
  const sectionSize = descriptors + names.reduce((a, b) => a + b.length, 0);
  const buf = Buffer.alloc(raw + sectionSize);
  buf.writeUInt16LE(0x5a4d, 0);
  buf.writeUInt32LE(peAt, 0x3c);
  buf.writeUInt32LE(0x4550, peAt);
  buf.writeUInt16LE(machine, peAt + 4);
  buf.writeUInt16LE(1, peAt + 6);
  buf.writeUInt16LE(optionalSize, peAt + 20);
  const optional = peAt + 24;
  buf.writeUInt16LE(0x20b, optional);
  buf.writeUInt32LE(va, optional + 112 + 8); // import directory
  buf.writeUInt32LE(descriptors, optional + 112 + 12);
  buf.writeUInt32LE(sectionSize, sectionTable + 8);
  buf.writeUInt32LE(va, sectionTable + 12);
  buf.writeUInt32LE(sectionSize, sectionTable + 16);
  buf.writeUInt32LE(raw, sectionTable + 20);
  let nameAt = descriptors;
  names.forEach((name, i) => {
    buf.writeUInt32LE(va + nameAt, raw + i * 20 + 12);
    name.copy(buf, raw + nameAt);
    nameAt += name.length;
  });
  return buf;
}

describe("readPe", () => {
  it("reads the machine and the imported DLLs", () => {
    expect(readPe(makePe(AMD64, ["KERNEL32.dll", "Qt6Core.dll"]))).toEqual({
      machine: AMD64,
      imports: ["KERNEL32.dll", "Qt6Core.dll"],
    });
  });

  it("is null for a file that isn't a PE", () => {
    expect(readPe(Buffer.from("not a program"))).toBeNull();
  });
});

describe("findMissingDlls", () => {
  let root: string;
  let app: string;
  let system: string;
  beforeEach(async () => {
    root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "missing-dlls-"));
    app = path.join(root, "app");
    system = path.join(root, "System32");
    await fs.promises.mkdir(app);
    await fs.promises.mkdir(system);
  });
  afterEach(async () => {
    await fs.promises.rm(root, { recursive: true, force: true });
  });
  const put = (dir: string, name: string, machine: number, imports: string[] = []) =>
    fs.promises.writeFile(path.join(dir, name), makePe(machine, imports));

  it("names the DLLs found nowhere, also those a DLL beside Dolphin imports", async () => {
    await put(app, "Dolphin.exe", AMD64, [
      "KERNEL32.dll",
      "Qt6Core.dll",
      "MFPlat.DLL",
      "api-ms-win-crt-heap-l1-1-0.dll",
    ]);
    await put(app, "Qt6Core.dll", AMD64, ["KERNEL32.dll", "VCRUNTIME140_1.dll"]);
    await put(system, "KERNEL32.dll", AMD64);
    expect(await findMissingDlls(path.join(app, "Dolphin.exe"), system, [])).toEqual([
      { name: "VCRUNTIME140_1.dll", importedBy: "Qt6Core.dll", problem: "missing" },
      { name: "MFPlat.DLL", importedBy: "Dolphin.exe", problem: "missing" },
    ]);
  });

  it("finds a DLL in a PATH folder, and reports a 32-bit one", async () => {
    const other = path.join(root, "other");
    await fs.promises.mkdir(other);
    await put(app, "Dolphin.exe", AMD64, ["MSVCP140.dll", "VCRUNTIME140.dll"]);
    await put(other, "MSVCP140.dll", AMD64);
    await put(system, "VCRUNTIME140.dll", I386);
    expect(await findMissingDlls(path.join(app, "Dolphin.exe"), system, [other])).toEqual([
      { name: "VCRUNTIME140.dll", importedBy: "Dolphin.exe", problem: "not-x64" },
    ]);
  });

  it("is empty when everything is there", async () => {
    await put(app, "Dolphin.exe", AMD64, ["Qt6Core.dll", "USER32.dll"]);
    await put(app, "Qt6Core.dll", AMD64, ["USER32.dll"]);
    await put(system, "USER32.dll", AMD64);
    expect(await findMissingDlls(path.join(app, "Dolphin.exe"), system, [])).toEqual([]);
  });
});
