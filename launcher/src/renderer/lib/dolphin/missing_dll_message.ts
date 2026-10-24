import type { MissingDll } from "@dolphin/types";

import { HandleDolphinExitCodeMessages as Messages } from "./handle_dolphin_exit_code.messages";

/** 0xC0000135 STATUS_DLL_NOT_FOUND and 0xC000007B STATUS_INVALID_IMAGE_FORMAT (a DLL that isn't 64-bit). */
export const DLL_EXIT_CODES: readonly number[] = [0xc0000135, 0xc000007b];

/** The toast for the first DLL that kept Dolphin from starting (findMissingDlls), naming it and what installs it. */
export function missingDllMessage(dlls: MissingDll[]): string | undefined {
  const dll = dlls[0];
  if (!dll) {
    return undefined;
  }
  if (dll.problem === "not-x64") {
    return Messages.dllNotX64(dll.name);
  }
  const n = dll.name.toLowerCase();
  if (/^(msvcp140|vcruntime140|concrt140|vccorlib140)/.test(n)) {
    return Messages.dllMissingVcRedist(dll.name);
  }
  if (/^mf/.test(n)) {
    return Messages.dllMissingMediaFeaturePack(dll.name);
  }
  // Shipped with Dolphin (the launcher installs a lost file again at its next start).
  if (/^qt6/.test(n)) {
    return Messages.dllMissingFromDolphin(dll.name);
  }
  return Messages.dllMissingFromWindows(dll.name);
}
