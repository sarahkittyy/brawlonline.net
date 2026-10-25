import type { ModBuild, ModsController } from "@dolphin/mods/types";
import type { ManualClip } from "@replays/highlights/types";

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

  // Online: "Matchmaking Ping Limit": Unranked and Ranked pair the player with no one whose
  // estimated ping is above it, in ms (100, 150 or 200). Synced to Dolphin.ini `[Online]
  // MatchmakingPingLimit`, sent in the game's tickets.
  pingLimit: number;

  // Online: the game's music in matches and menus. Synced to Dolphin.ini `[Online] Music`.
  enableMusic: boolean;

  // Online: "Disable Chat": no chat window in the game at all (rooms, menus, matches). Synced to
  // Dolphin.ini `[Online] DisableChat`.
  disableChat: boolean;

  // Online: "Show my games on the Spectate tab" (docs/design/spectating.md): this player's game
  // uploads its games' inputs for spectators (Ranked's always, listed only when a player allows
  // it). Synced to Dolphin.ini `[Online] StreamGames`.
  enableStreamGames: boolean;

  // Online: "Network for online games": "" = automatic (the way to the internet), else one of the
  // PC's addresses as "a.b.c.d/prefix", a VPN adapter's such as ZeroTier's, which friends in
  // Direct and rooms on the same network reach this PC by. Synced to Dolphin.ini `[Online]
  // ForceLanIP` and `LanIP` (Config::GetChosenNetwork).
  netplayNetwork: string;

  // Spectating: "Close Dolphin after a spectated game ends" (docs/design/spectating.md §6.2): at a stream's end the
  // playback Dolphin holds the last frame for 5 s, then closes. Auto play overrides it.
  closeDolphinAfterSpectating: boolean;

  // Clips (DESIGN.md, the user 2026-10-20): "L+R+Z to clip" (Project Clippi's button-input
  // highlights, found after the game in the replay's recorded pads); "Disable automatic highlight
  // detection"; where rendered videos go (null = <rootSlpPath>/Highlights).
  clipOnLrz: boolean;
  disableHighlightDetection: boolean;
  clipsFolder: string | null;
  /** The rendered videos' height: 480, 720 (the default), 1080 or 1440 (@common/clip_resolution). */
  clipResolution: number;
  /** Replays > Highlights' starred rows, by favoriteKey (src/renderer/.../highlights/favorites.ts): replay path, kind, start frame. */
  favoriteHighlights: string[];
  /** Replays > Highlights > Manual Clips: the seek bar's clips (replays/highlights/manual_clips.ts), oldest first. */
  manualClips: ManualClip[];

  // Dolphin settings: the executable of our Dolphin build (null = default, see dolphin/install/paths.ts)
  netplayDolphinPath: string | null;
  playbackDolphinPath: string | null;

  // Where the game files are (the netplay folder: Dolphin, its User folder, P+'s files, the patched
  // SD card; null = <userData>/netplay). Only DolphinManager.moveGameFiles changes it, after moving them.
  gameFilesFolder: string | null;

  // The Brawl Online folder (dolphin/install/main_folder.ts): the folder the player chose to keep
  // the game files and the replays in, null until they choose one (everything where it was). Only
  // DolphinManager.moveMainFolder changes it, after moving them.
  mainFolder: string | null;

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
  // The build hash version (build_hash.ts BUILD_HASH_VERSION) whose one pass over the games hashed
  // by an older launcher has run (ModsManager.rehashChanged); absent before the first.
  modsHashVersion?: string;
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
