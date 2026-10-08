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
 * The macOS build is only ad-hoc signed (no Apple Developer ID yet). electron-updater installs on
 * macOS through Squirrel.Mac, which refuses an update whose code signature does not match the running
 * app's, and ad-hoc signatures never match, so an in-place update always fails. Until then the
 * launcher only checks the feed (latest-mac.yml) and offers MAC_DOWNLOAD_URL instead
 * (src/common/launcher_update.ts). Flip this to true once the macOS build is signed with a Developer ID.
 */
export const MAC_SELF_UPDATE = false;

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
