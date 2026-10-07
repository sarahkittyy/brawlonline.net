/**
 * Product identity and service hosts.
 *
 * The product name is not decided yet. Everything user-visible that names the
 * product reads PRODUCT_NAME, so a rename is a change here plus the matching
 * `productName` fields in package.json, release/app/package.json and
 * electron-builder.json (a unit test checks they agree).
 *
 * Hostnames are configuration. They default to subdomains of BASE_DOMAIN and can
 * be overridden at runtime in the main process with environment variables
 * (see `resolveServiceUrls` in src/accounts/config.ts), e.g. for a local
 * accounts service during development.
 */

export const PRODUCT_NAME = "PlusOnline";

/** Lower-case, filesystem- and URL-safe form of the product name. */
export const PRODUCT_SLUG = PRODUCT_NAME.toLowerCase();

/** The game the client is for. Shown in copy such as "Launch Project+". */
export const GAME_NAME = "Project+";

export const BASE_DOMAIN = "fluffycat.gay";

export const defaultServiceUrls = {
  /** HTTP accounts API (server/crates/accounts). */
  accountsApi: `https://accounts.${BASE_DOMAIN}`,
  /** Matchmaking server (ENet/UDP); Dolphin uses it, the launcher only displays it. */
  matchmakingHost: `mm.${BASE_DOMAIN}`,
  /**
   * Web pages for an account (email verification, password reset). Served by the
   * accounts service until a website exists.
   */
  website: `https://accounts.${BASE_DOMAIN}`,
  /** electron-updater "generic" feed with the launcher releases (latest.yml etc.); also the manual download page. */
  launcherUpdates: `https://updates.${BASE_DOMAIN}/launcher`,
};

export type ServiceUrls = typeof defaultServiceUrls;

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
