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

  // Online: the input delay of the player's own inputs, 0 = automatic (from the ping, per game),
  // else 1-4 frames. Synced to Dolphin.ini `[Online] InputDelay`.
  inputDelay: number;

  // Online: the game's music in matches and menus. Synced to Dolphin.ini `[Online] Music`.
  enableMusic: boolean;

  // Dolphin settings: the executable of our Dolphin build (null = default, see dolphin/install/paths.ts)
  netplayDolphinPath: string | null;
  playbackDolphinPath: string | null;

  // Appearance settings
  enableRankDisplay: boolean;

  // Sounds for chat messages and invites (the bell next to the settings gear) and their volume, 0-100.
  notificationSounds: boolean;
  notificationVolume: number;
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
