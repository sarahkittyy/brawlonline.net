import { describe, expect, it } from "vitest";

import { spawnRefusedError } from "./spawn_refused";

const spawnError = (code: string) => Object.assign(new Error(`spawn ${code}`), { code, syscall: "spawn" });
const exe = "C:\\Users\\p\\AppData\\Roaming\\Brawl Online\\netplay\\Dolphin.exe";

describe("spawnRefusedError", () => {
  it("names the antivirus for EPERM on Windows, keeping the code and path", () => {
    const message = spawnRefusedError(spawnError("EPERM"), exe, "win32")?.message;
    expect(message).toMatch(/antivirus/);
    expect(message).toContain("Dolphin.exe");
    expect(message).toContain(exe);
    expect(message).toContain("spawn EPERM");
  });

  it("names the antivirus for UNKNOWN on Windows", () => {
    expect(spawnRefusedError(spawnError("UNKNOWN"), exe, "win32")?.message).toMatch(/antivirus/);
  });

  it("names the administrator setting for EACCES on Windows", () => {
    expect(spawnRefusedError(spawnError("EACCES"), exe, "win32")?.message).toMatch(/administrator/);
  });

  it("says not executable for EACCES elsewhere", () => {
    expect(spawnRefusedError(spawnError("EACCES"), "/opt/dolphin", "linux")?.message).toMatch(/not executable/);
  });

  it("leaves other errors alone", () => {
    expect(spawnRefusedError(spawnError("ENOENT"), exe, "win32")).toBeNull();
    expect(spawnRefusedError(spawnError("EPERM"), "/opt/dolphin", "darwin")).toBeNull();
    expect(spawnRefusedError("text", exe, "win32")).toBeNull();
  });
});
