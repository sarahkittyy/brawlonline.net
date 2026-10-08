import { mkdir } from "node:fs/promises";
import path from "path";

import type { GeckoCode } from "./config/gecko_code";
import { loadGeckoCodes, setCodes } from "./config/gecko_code";
import { IniFile } from "./config/ini_file";
import type { DolphinInstallation } from "./types";

/**
 * Game ID whose Gecko codes the launcher's code manager edits. Slippi edits
 * Melee's `GALE01`; we edit Brawl's `RSBE01` (Sys `RSBE01.ini` for the built-in
 * list, User `GameSettings/RSBE01.ini` for the user's own codes). P+'s own
 * codeset is the GCT on the SD card and is not touched here.
 */
export const GECKO_GAME_ID = "RSBE01";

export async function fetchGeckoCodes(installation: DolphinInstallation) {
  const { userFolder, sysFolder } = installation;

  // Never create folders inside the Dolphin build (sysFolder); IniFile reads a missing file as empty.
  await mkdir(userFolder, { recursive: true });
  const globalIniPath = path.join(sysFolder, "GameSettings", `${GECKO_GAME_ID}.ini`);
  const localIniPath = path.join(userFolder, "GameSettings", `${GECKO_GAME_ID}.ini`);
  const globalIni = await IniFile.init(globalIniPath);
  const localIni = await IniFile.init(localIniPath);

  return loadGeckoCodes(globalIni, localIni);
}

export async function saveGeckoCodes(installation: DolphinInstallation, geckoCodes: GeckoCode[]) {
  const { userFolder } = installation;

  await mkdir(path.join(userFolder, "GameSettings"), { recursive: true });
  const localIniPath = path.join(userFolder, "GameSettings", `${GECKO_GAME_ID}.ini`);
  const localIni = await IniFile.init(localIniPath);

  const localCodes = geckoCodes;
  setCodes(localIni, localCodes);
  return await localIni.save();
}
