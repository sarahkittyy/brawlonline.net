import { useQueries } from "@tanstack/react-query";

import { useServices } from "@/services";
import { assetUrl, useGameAssets } from "@/styles/game_theme";

/**
 * Pictures for the accounts in the header (user_menu.tsx, MainPicture): each player's primary main's stock icon, from
 * their profile (`["profile", uid]`, the query the profile and its edit mode use, so a new primary shows at once).
 * `null` for none: no main set, Random (Brawl has no stock icon for it), no disc set up (no stock icons), not fetched
 * (`enabled` off: offline) or the fetch failed. A uid is missing while its profile or the game assets are loading.
 */
export function useMainPictures(uids: readonly string[], enabled: boolean): Partial<Record<string, string | null>> {
  const { backendService } = useServices();
  const assets = useGameAssets((s) => s.state);
  const profiles = useQueries({
    queries: uids.map((uid) => ({
      queryKey: ["profile", uid],
      queryFn: () => backendService.fetchProfile(uid),
      staleTime: 5 * 60_000,
      enabled,
    })),
  });
  const pictures: Partial<Record<string, string | null>> = {};
  uids.forEach((uid, i) => {
    const profile = profiles[i];
    if (!enabled || profile?.isError) {
      pictures[uid] = null;
      return;
    }
    if (!profile?.data || !assets) {
      return;
    }
    const main = profile.data.mains?.[0];
    const file = main ? assets.manifest?.stocks[main]?.file : undefined;
    pictures[uid] = file ? assetUrl(file) : null;
  });
  return pictures;
}
