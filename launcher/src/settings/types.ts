import type { ModBuild, ModsController } from "@dolphin/mods/types";

/**
 * Stored account information (for multi-account support)
 */
export interface StoredAccount {
  id: string; // account uid from the accounts service
  email: string;
  displayName: string;
  displayPicture: string;
  lastActive: Date;
}

/**
 * Account data structure
 */
export interface AccountData {
  activeId: string | null;
  list: StoredAccount[];
}

/**
 * Settings Schema
 * This is the single source of truth for all application settings
 */
export interface SettingsSchema {
  // Path settings
  isoPath: string | null;
  rootSlpPath: string;
  extraSlpPaths: string[];

  // Behavior settings
  enableNetplayReplays: boolean;
  useMonthlySubfolders: boolean;
  launchGameOnPlay: boolean;
  autoUpdateLauncher: boolean;

  // "Hardware Acceleration": the launcher draws with the graphics card. Read once at start (main.ts:
  // off is app.disableHardwareAcceleration).
  hardwareAcceleration: boolean;

  // Online: the input delay of the player's own inputs, 0 = automatic (from the ping, per game),
  // else 1-4 frames. Synced to Dolphin.ini `[Online] InputDelay`.
  inputDelay: number;

  // Online: the game's music in matches and menus. Synced to Dolphin.ini `[Online] Music`.
  enableMusic: boolean;

  // Online: "Disable Chat": no chat window in the game at all (rooms, menus, matches). Synced to
  // Dolphin.ini `[Online] DisableChat`.
  disableChat: boolean;

  // Online: "Let others stream my games" (docs/design/spectating.md): this player's game uploads
  // its games' inputs for spectators. Synced to Dolphin.ini `[Online] StreamGames`.
  enableStreamGames: boolean;

  // Spectating: "Close Dolphin after a spectated game ends" (docs/design/spectating.md §6.2): at a stream's end the
  // playback Dolphin holds the last frame for 5 s, then closes. Auto play overrides it.
  closeDolphinAfterSpectating: boolean;

  // Dolphin settings: the executable of our Dolphin build (null = default, see dolphin/install/paths.ts)
  netplayDolphinPath: string | null;
  playbackDolphinPath: string | null;

  // Where the game files are (the netplay folder: Dolphin, its User folder, P+'s files, the patched
  // SD card; null = <userData>/netplay). Only DolphinManager.moveGameFiles changes it, after moving them.
  gameFilesFolder: string | null;

  // Appearance settings
  enableRankDisplay: boolean;

  // Sounds for chat messages and invites (the bell next to the settings gear) and their volume, 0-100.
  notificationSounds: boolean;
  notificationVolume: number;

  // The launcher's interface size in percent (one of common/interface_zoom.ts's levels); Ctrl +/-/0 and Ctrl + wheel change it.
  interfaceZoom: number;
}

/**
 * Root-level app settings that aren't nested under "settings"
 */
export interface RootSettingsSchema {
  accounts: AccountData;
  // The Brawl Mods tab's builds (docs/design/brawl-mods.md §4); absent until one is added.
  modBuilds?: ModBuild[];
  // Other Games: what the player plays with; absent until they pick (then by what is set up).
  modsController?: ModsController;
  previousVersion?: string;
  pendingUpdateVersion?: string;
}

/**
 * Application settings structure
 * Combines nested settings and root-level settings
 */
export type AppSettings = {
  settings: SettingsSchema;
} & RootSettingsSchema;

/**
 * Helper type for all possible setting keys (both nested and root level)
 */
export type SettingKey = keyof SettingsSchema | keyof RootSettingsSchema;

/**
 * Helper type to get the value type for a given setting key
 */
export type SettingValue<K extends SettingKey> = K extends keyof SettingsSchema
  ? SettingsSchema[K]
  : K extends keyof RootSettingsSchema
  ? RootSettingsSchema[K]
  : never;

/**
 * Payload for a single setting update
 */
export interface SettingUpdate<K extends SettingKey = SettingKey> {
  key: K;
  value: SettingValue<K>;
}
