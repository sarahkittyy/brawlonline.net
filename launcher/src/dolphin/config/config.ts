import { DEFAULT_CHAT_KEY } from "@common/chat_key";
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
  /** The game's music (`[Online] Music`; off applies P+'s Music Off code). */
  enableMusic: boolean;
  /** No chat window in the game (`[Online] DisableChat`). */
  disableChat: boolean;
  /** "Let others stream my games" (`[Online] StreamGames`, docs/design/spectating.md). */
  enableStreamGames: boolean;
  /** The Project+ release on the card (`[Online] PPlusRelease`): replays record it. Written only. */
  pplusRelease?: string;
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
  if (options.enableMusic !== undefined) {
    section.set("Music", convertBooleanToIniVal(options.enableMusic));
  }
  if (options.disableChat !== undefined) {
    section.set("DisableChat", convertBooleanToIniVal(options.disableChat));
  }
  if (options.enableStreamGames !== undefined) {
    section.set("StreamGames", convertBooleanToIniVal(options.enableStreamGames));
  }
  if (options.pplusRelease !== undefined) {
    section.set("PPlusRelease", options.pplusRelease);
  }

  await iniFile.save();
}

export async function getOnlineSettings(iniFile: IniFile): Promise<SyncedDolphinSettings> {
  const section = iniFile.getOrCreateSection(DOLPHIN_INI_SECTION);

  const replayPath = section.get("ReplayDir", defaultAppSettings.settings.rootSlpPath);
  const enableNetplayReplays = section.get("SaveReplays", "True") === "True";
  const enableMonthlySubfolders = section.get("ReplayMonthlyFolders", "True") === "True";
  const inputDelay = normalizeInputDelay(section.get("InputDelay", "0"));
  const enableMusic = section.get("Music", "True") === "True";
  const disableChat = section.get("DisableChat", "False") === "True";
  const enableStreamGames = section.get("StreamGames", "True") === "True";

  return {
    replayPath,
    enableNetplayReplays,
    enableMonthlySubfolders,
    inputDelay,
    enableMusic,
    disableChat,
    enableStreamGames,
  };
}

/** Hotkeys.ini's key for the chat hotkey (Dolphin's HotkeyManager: group "General"). */
const CHAT_HOTKEY = "General/Activate NetPlay Chat";

/**
 * The key that opens the chat box (common/chat_key.ts): `[Online] ChatKey` when the launcher set
 * it, else Hotkeys.ini's binding (set in Dolphin's Hotkey Settings), else Dolphin's default.
 */
export function getChatKey(dolphinIni: IniFile, hotkeysIni: IniFile): string {
  const chatKey = dolphinIni.getSection(DOLPHIN_INI_SECTION)?.get("ChatKey", "") ?? "";
  if (chatKey !== "") {
    return chatKey;
  }
  const hotkeys = hotkeysIni.getSection("Hotkeys");
  return hotkeys?.exists(CHAT_HOTKEY) ? hotkeys.get(CHAT_HOTKEY, "") : DEFAULT_CHAT_KEY;
}

export async function setChatKey(dolphinIni: IniFile, expression: string): Promise<void> {
  dolphinIni.getOrCreateSection(DOLPHIN_INI_SECTION).set("ChatKey", expression);
  await dolphinIni.save();
}

function convertBooleanToIniVal(value?: boolean): string {
  return value ? "True" : "False";
}
