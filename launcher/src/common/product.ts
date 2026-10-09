/**
 * Product identity and service hosts.
 *
 * Everything user-visible that names the product reads PRODUCT_NAME, so a rename
 * is a change here plus the matching `productName` fields in package.json,
 * release/app/package.json and electron-builder.json (a unit test checks they
 * agree). Add the old name to LEGACY_PRODUCT_NAMES so existing profiles move over.
 *
 * Hostnames are configuration. They default to BASE_DOMAIN (website, accounts API,
 * update feed) and its "mm" subdomain (matchmaking), and can
 * be overridden at runtime in the main process with environment variables
 * (see `resolveServiceUrls` in src/accounts/config.ts), e.g. for a local
 * accounts service during development.
 */

export const PRODUCT_NAME = "Brawl Online";

/** Lower-case, filesystem- and URL-safe form of the product name ("brawl-online"). */
export const PRODUCT_SLUG = PRODUCT_NAME.toLowerCase().replace(/[^a-z0-9]+/g, "-");

/**
 * Earlier names of the product, newest first. Electron's userData folder and the default replay
 * folder are named after the product, so the launcher looks for these on start (see
 * src/main/legacy_user_data.ts and src/settings/default_settings.ts).
 */
export const LEGACY_PRODUCT_NAMES: readonly string[] = ["PlusOnline"];

/** The game the client is for. Shown in copy such as "Launch Project+". */
export const GAME_NAME = "Project+";

export const BASE_DOMAIN = "brawlonline.net";

/**
 * The app id: electron-builder.json's appId (the macOS bundle id) and, on Windows, the
 * AppUserModelID. The NSIS installer gives the Start Menu shortcut this AppUserModelID, and
 * Windows names notifications after the shortcut carrying the process's id, so main.ts sets it
 * (Electron's default, "electron.app.Electron", shows up as the notification's sender).
 */
export const APP_ID = "net.brawlonline.launcher";

export const defaultServiceUrls = {
  /** HTTP accounts API (server/crates/accounts), served under /v1 on the apex domain. */
  accountsApi: `https://${BASE_DOMAIN}`,
  /** Matchmaking server (ENet/UDP 43113, a DNS-only record); Dolphin uses it, the launcher only displays it. */
  matchmakingHost: `mm.${BASE_DOMAIN}`,
  /**
   * The website, which also serves the pages the account emails link to (email
   * verification, password reset; proxied to the accounts service).
   */
  website: `https://${BASE_DOMAIN}`,
  /** electron-updater "generic" feed with the launcher releases (latest.yml etc.); also the manual download page. */
  launcherUpdates: `https://${BASE_DOMAIN}/updates/launcher`,
};

export type ServiceUrls = typeof defaultServiceUrls;

/**
 * Whether the macOS launcher updates itself in place, as on Windows and Linux.
 *
 * electron-updater installs on macOS through Squirrel.Mac, which refuses an update whose code
 * signature does not match the running app's. Released macOS builds are signed with our Developer
 * ID and notarized (.github/scripts/package-launcher-macos.sh), so they update in place. With
 * false (an ad-hoc signed build, whose signature never matches) the launcher only checks the feed
 * (latest-mac.yml) and offers MAC_DOWNLOAD_URL instead (src/common/launcher_update.ts).
 */
export const MAC_SELF_UPDATE = true;

/** The website's macOS download, offered for updates while MAC_SELF_UPDATE is false. */
export const MAC_DOWNLOAD_URL = `https://${BASE_DOMAIN}/downloads/BrawlOnline.dmg`;

/**
 * Contract with our Dolphin fork (keep in sync with its CommonPaths.h / settings):
 *
 * - `user.json` lives in `<Dolphin User folder>/<DOLPHIN_ONLINE_DIR>/user.json`. Slippi's Dolphin
 *   uses `<User>/Slippi/user.json` (`SLIPPI_DIR`); ours renames only the folder.
 * - Online settings the launcher syncs (replay folder, save replays, monthly folders) live in the
 *   `[<DOLPHIN_INI_SECTION>]` section of `Config/Dolphin.ini`, with Slippi's key names.
 *
 * Deliberately not derived from PRODUCT_NAME, so renaming the product does not move user files.
 */
export const DOLPHIN_ONLINE_DIR = "Online";
export const DOLPHIN_INI_SECTION = "Online";

/** P+ boots through this DOL in the Dolphin user folder, with the Brawl disc as the default ISO. */
export const PPLUS_NETPLAY_LAUNCHER_DOL = "Project+ Netplay Launcher.dol";
