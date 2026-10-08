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

  // Dolphin settings: the executable of our Dolphin build (null = default, see dolphin/install/paths.ts)
  netplayDolphinPath: string | null;
  playbackDolphinPath: string | null;

  // Appearance settings
  enableRankDisplay: boolean;
}

/**
 * Root-level app settings that aren't nested under "settings"
 */
export interface RootSettingsSchema {
  accounts: AccountData;
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
