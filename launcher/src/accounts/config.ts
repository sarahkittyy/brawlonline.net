import type { ServiceUrls } from "@common/product";
import { defaultServiceUrls } from "@common/product";

/**
 * Runtime overrides for the service hosts (main process only):
 *
 * - `PPO_ACCOUNTS_URL`  accounts API base URL, e.g. `http://127.0.0.1:8080` for a local server
 * - `PPO_WEBSITE_URL`   website base URL (defaults to the accounts URL)
 * - `PPO_MM_HOST`       matchmaking host
 * - `PPO_UPDATES_URL`   launcher update feed
 *
 * Read through a plain object lookup so webpack's DefinePlugin does not inline
 * them at build time.
 */
export function resolveServiceUrls(env: Record<string, string | undefined> = process.env): ServiceUrls {
  const read = (name: string): string | undefined => {
    const value = env[name];
    return value && value.trim() !== "" ? value.trim().replace(/\/+$/, "") : undefined;
  };
  const accountsApi = read("PPO_ACCOUNTS_URL") ?? defaultServiceUrls.accountsApi;
  return {
    accountsApi,
    website: read("PPO_WEBSITE_URL") ?? (read("PPO_ACCOUNTS_URL") ? accountsApi : defaultServiceUrls.website),
    matchmakingHost: read("PPO_MM_HOST") ?? defaultServiceUrls.matchmakingHost,
    launcherUpdates: read("PPO_UPDATES_URL") ?? defaultServiceUrls.launcherUpdates,
  };
}
