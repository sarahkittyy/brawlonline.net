import type { RulesetView } from "@accounts/types";
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";

import { useAccount } from "@/lib/hooks/use_account";
import { useServices } from "@/services";

export const rememberedRulesetKey = (uid: string | undefined) => ["rememberedRuleset", uid] as const;

/**
 * The account's remembered ruleset (docs/rulesets-protocol.md): Home's ruleset box shows it,
 * Create room and `brawlonline://room/create` links start rooms with it, and in-game Create Room
 * reads the same value from the server. `null` is P+ default.
 */
export const useRememberedRuleset = () => {
  const { backendService } = useServices();
  const uid = useAccount((s) => s.user?.uid);
  const queryClient = useQueryClient();

  const query = useQuery({
    queryKey: rememberedRulesetKey(uid),
    queryFn: () => backendService.fetchRememberedRuleset(),
    enabled: uid != null,
    retry: false,
  });

  const mutation = useMutation({
    mutationFn: (code: string | null) => backendService.setRememberedRuleset(code),
    onSuccess: (view: RulesetView | null) => {
      queryClient.setQueryData(rememberedRulesetKey(uid), view);
      void queryClient.invalidateQueries({ queryKey: ["myRulesets", uid] });
    },
  });

  return {
    ruleset: query.data ?? null,
    isLoading: query.isLoading && uid != null,
    error: query.error,
    /** Sets it (null: P+ default); resolves to the new value, throws the server's error. */
    setRemembered: mutation.mutateAsync,
    saving: mutation.isPending,
  };
};

/** "NorCal Weekly v3 by sarah". */
export function rulesetTitle(view: RulesetView): string {
  const r = view.ruleset;
  return `${r.name} v${r.version}${r.by ? ` by ${r.by}` : ""}`;
}
