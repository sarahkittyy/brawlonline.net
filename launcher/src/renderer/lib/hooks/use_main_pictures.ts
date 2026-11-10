import { useQueries } from "@tanstack/react-query";

import { useServices } from "@/services";
import { assetUrl, useGameAssets } from "@/styles/game_theme";

/**
 * Profile pictures for the accounts in the header (user_menu.tsx): each player's primary main's stock icon, from their
 * profile (`["profile", uid]`, the query the profile and its edit mode use, so a new primary shows at once). A uid
 * missing from the answer keeps its default picture: no main set, Random (Brawl has no stock icon for it), no disc set
 * up (no stock icons), or the profile not fetched (offline, or still loading).
 */
export function useMainPictures(uids: readonly string[], enabled: boolean): Partial<Record<string, string>> {
  const { backendService } = useServices();
  const stocks = useGameAssets((s) => s.state?.manifest?.stocks);
  const profiles = useQueries({
    queries: uids.map((uid) => ({
      queryKey: ["profile", uid],
      queryFn: () => backendService.fetchProfile(uid),
      staleTime: 5 * 60_000,
      enabled,
    })),
  });
  const pictures: Partial<Record<string, string>> = {};
  uids.forEach((uid, i) => {
    const main = profiles[i]?.data?.mains?.[0];
    const file = main ? stocks?.[main]?.file : undefined;
    if (file) {
      pictures[uid] = assetUrl(file);
    }
  });
  return pictures;
}
