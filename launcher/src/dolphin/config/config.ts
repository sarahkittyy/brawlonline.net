import { normalizeInputDelay } from "@common/input_delay";
import { DOLPHIN_INI_SECTION } from "@common/product";
import { defaultAppSettings } from "@settings/default_settings";

import type { IniFile } from "./ini_file";

/** Settings the launcher and Dolphin keep in sync (Slippi's replay settings, and the input delay). */
export type SyncedDolphinSettings = {
  replayPath: string;
  enableNetplayReplays: boolean;
  enableMonthlySubfolders: boolean;
  /** 0: automatic, else frames (`[Online] InputDelay`). */
  inputDelay: number;
};

export async function addGamePath(iniFile: IniFile, gameDir: string): Promise<void> {
  const generalSection = iniFile.getOrCreateSection("General");
  const numPaths = generalSection.get("ISOPaths", "0");
  generalSection.set("ISOPaths", numPaths !== "0" ? numPaths : "1");
  generalSection.set("ISOPath0", gameDir);
  await iniFile.save();
}

/** P+'s launcher DOL boots Dolphin's default ISO after applying its Gecko codes. */
export async function setDefaultIso(iniFile: IniFile, isoPath: string): Promise<void> {
  const coreSection = iniFile.getOrCreateSection("Core");
  if (coreSection.get("DefaultISO", "") !== isoPath) {
    coreSection.set("DefaultISO", isoPath);
    await iniFile.save();
  }
}

/**
 * Writes the synced settings with Slippi mainline's key names (`ReplayDir`,
 * `SaveReplays`, `ReplayMonthlyFolders`) into our own section of Dolphin.ini.
 */
export async function setOnlineSettings(iniFile: IniFile, options: Partial<SyncedDolphinSettings>): Promise<void> {
  const section = iniFile.getOrCreateSection(DOLPHIN_INI_SECTION);

  if (options.replayPath !== undefined) {
    section.set("ReplayDir", options.replayPath);
  }
  if (options.enableNetplayReplays !== undefined) {
    section.set("SaveReplays", convertBooleanToIniVal(options.enableNetplayReplays));
  }
  if (options.enableMonthlySubfolders !== undefined) {
    section.set("ReplayMonthlyFolders", convertBooleanToIniVal(options.enableMonthlySubfolders));
  }
  if (options.inputDelay !== undefined) {
    section.set("InputDelay", String(normalizeInputDelay(options.inputDelay)));
  }

  await iniFile.save();
}

export async function getOnlineSettings(iniFile: IniFile): Promise<SyncedDolphinSettings> {
  const section = iniFile.getOrCreateSection(DOLPHIN_INI_SECTION);

  const replayPath = section.get("ReplayDir", defaultAppSettings.settings.rootSlpPath);
  const enableNetplayReplays = section.get("SaveReplays", "True") === "True";
  const enableMonthlySubfolders = section.get("ReplayMonthlyFolders", "True") === "True";
  const inputDelay = normalizeInputDelay(section.get("InputDelay", "0"));

  return {
    replayPath,
    enableNetplayReplays,
    enableMonthlySubfolders,
    inputDelay,
  };
}

function convertBooleanToIniVal(value?: boolean): string {
  return value ? "True" : "False";
}
