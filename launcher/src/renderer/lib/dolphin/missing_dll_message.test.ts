import { describe, expect, it } from "vitest";

import { missingDllMessage } from "./missing_dll_message";

const missing = (name: string) => ({ name, importedBy: "Dolphin.exe", problem: "missing" as const });

describe("missingDllMessage", () => {
  it("names the DLL and what installs it", () => {
    expect(missingDllMessage([missing("VCRUNTIME140_1.dll")])).toMatch(/Visual C\+\+ Redistributable/);
    expect(missingDllMessage([missing("MFPlat.DLL")])).toMatch(/Media Feature Pack/);
    expect(missingDllMessage([missing("Qt6Gui.dll")])).toMatch(/antivirus/);
    expect(missingDllMessage([missing("qwave.dll")])).toMatch(/report/);
    expect(missingDllMessage([{ name: "VCRUNTIME140.dll", importedBy: "Dolphin.exe", problem: "not-x64" }])).toMatch(
      /not 64-bit/,
    );
  });

  it("is undefined when no DLL was found missing", () => {
    expect(missingDllMessage([])).toBeUndefined();
  });
});
