import { Button } from "@base-ui/react";
import CachedIcon from "@mui/icons-material/Cached";
import CloseIcon from "@mui/icons-material/Close";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { useMutation } from "@tanstack/react-query";

import { useAccount } from "@/lib/hooks/use_account";
import { useServices } from "@/services";
import type { RankedProfile } from "@/services/backend/types";

import { MyRankingMessages as Messages } from "./my_ranking.messages";
import styles from "./ranked_user_profile.module.css";

export const RankedUserProfile = ({ rankedProfile, onHide }: { rankedProfile: RankedProfile; onHide: () => void }) => {
  const { rating, setsPlayed } = rankedProfile;
  // Slippi shows its rank badge and tier here. We have no tiers: the Elo rating is the rank.

  return (
    <div className={styles.container}>
      <div className={styles.content}>
        <div>
          <h3 className={styles.rankNameLabel}>{Messages.rating()}</h3>
          <div style={{ color: "var(--accent-primary)", fontWeight: "bold" }}>{rating.toFixed(1)}</div>
          <div style={{ fontSize: "12px", opacity: 0.7 }}>{Messages.setsPlayed(setsPlayed)}</div>
        </div>
        <RefreshRatingButton />
      </div>
      <Tooltip title={Messages.hide()}>
        <Button className={styles.hideRankButton} onClick={onHide}>
          <CloseIcon color="inherit" fontSize="small" />
        </Button>
      </Tooltip>
    </div>
  );
};

const RefreshRatingButton = () => {
  const updateRanking = useAccount((s) => s.updateRanking);
  const user = useAccount((s) => s.user);

  const { backendService } = useServices();

  const mutation = useMutation({
    mutationFn: async (uid: string) => {
      const profile = await backendService.fetchRankedNetplayProfile(uid);

      // protect against auth changes during request
      if (user?.uid !== uid) {
        return;
      }

      if (profile) {
        updateRanking(profile);
      }

      return profile;
    },
  });

  return (
    <Button
      className={styles.refreshButton}
      disabled={mutation.isPending || !user}
      onClick={() => {
        if (!user) {
          return;
        }

        mutation.mutate(user.uid);
      }}
    >
      {mutation.isPending ? (
        <CircularProgress color="inherit" size={16} />
      ) : (
        <CachedIcon color="inherit" sx={{ fontSize: "16px", color: "var(--surface-3)" }} />
      )}
      <span>{Messages.refresh()}</span>
    </Button>
  );
};
