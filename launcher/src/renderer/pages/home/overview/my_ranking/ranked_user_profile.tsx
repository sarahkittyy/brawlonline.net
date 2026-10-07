import { Button } from "@base-ui/react";
import CachedIcon from "@mui/icons-material/Cached";
import CloseIcon from "@mui/icons-material/Close";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { useMutation } from "@tanstack/react-query";
import React from "react";

import { useAccount } from "@/lib/hooks/use_account";
import { useServices } from "@/services";
import type { Rank, RankedProfile } from "@/services/backend/types";

import { getRankDetails } from "./get_rank_details";
import { MyRankingMessages as Messages } from "./my_ranking.messages";
import styles from "./ranked_user_profile.module.css";

export const RankedUserProfile = ({ rankedProfile, onHide }: { rankedProfile: RankedProfile; onHide: () => void }) => {
  const { rating, rank } = rankedProfile;
  // Slippi shows its rank badge art and a tier colour here; we show the tier as text only.
  const { name } = getRankDetails(rank);
  const color = "var(--accent-primary)";
  const isUnrankedRank = isUnranked(rank);

  const rankNameLabel = React.useMemo(() => {
    if (rank === "pending") {
      return Messages.rankPending();
    }
    if (isUnrankedRank) {
      return Messages.noRanking();
    }
    return name;
  }, [rank, isUnrankedRank, name]);

  return (
    <div className={styles.container}>
      <div className={styles.content}>
        <div>
          <h3 className={styles.rankNameLabel}>{rankNameLabel}</h3>
          {!isUnrankedRank && <div style={{ color, fontWeight: "bold" }}>{rating.toFixed(1)}</div>}
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

function isUnranked(rank: Rank) {
  return rank === "none" || rank === "banned" || rank === "pending";
}

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
