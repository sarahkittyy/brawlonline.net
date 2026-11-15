import { useQuery } from "@tanstack/react-query";

import { useAccount } from "@/lib/hooks/use_account";
import { useServices } from "@/services";

/** How many ranked sets the rating's sparkline covers. */
export const RATING_HISTORY_SETS = 24;

/**
 * The logged-in player's rating after each of their last ranked sets, oldest first (the sparkline beside the rating:
 * a number that moves over time shows its recent history). From their match history; sets that didn't change it and
 * placing sets (hidden rating) are skipped.
 */
export const useRatingHistory = (): number[] => {
  const { backendService } = useServices();
  const uid = useAccount((s) => s.user?.uid);
  const rating = useAccount((s) => s.userData?.rankedNetplayProfile?.rating);
  const { data } = useQuery({
    // The rating in the key: a new set refreshes the line along with the number.
    queryKey: ["ratingHistory", uid, rating],
    queryFn: () => backendService.fetchMatchHistory("ranked", undefined, { limit: RATING_HISTORY_SETS }),
    enabled: uid != null,
    staleTime: 60_000,
  });
  if (!data || !uid) {
    return [];
  }
  return data.matches
    .map((m) => m.players.find((p) => p.uid === uid)?.ratingAfter)
    .filter((r): r is number => typeof r === "number")
    .reverse();
};
