import { Button } from "@base-ui/react";
import CachedIcon from "@mui/icons-material/Cached";
import CloseIcon from "@mui/icons-material/Close";
import HistoryIcon from "@mui/icons-material/History";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { useMutation } from "@tanstack/react-query";
import { useNavigate } from "react-router-dom";

import { RankBadge } from "@/components/rank_badge/rank_badge";
import { useAccount } from "@/lib/hooks/use_account";
import { formatPosition, formatProfileRating } from "@/lib/ranked_format";
import { useServices } from "@/services";
import type { RankedProfile } from "@/services/backend/types";

import { MyRankingMessages as Messages } from "./my_ranking.messages";
import styles from "./ranked_user_profile.module.css";

export const RankedUserProfile = ({ rankedProfile, onHide }: { rankedProfile: RankedProfile; onHide: () => void }) => {
  const { setsPlayed, rank, setsToPlace } = rankedProfile;
  const position = formatPosition(rankedProfile.position, rankedProfile.rankedPlayers);
  const navigate = useNavigate();
  // Slippi shows its rank badge and tier here; so do we, with the rating beside it (docs/design/ranked-tiers.md),
  // `????` while placing.

  return (
    <div className={styles.container}>
      <div className={styles.content}>
        <div className={styles.top}>
          <div className={styles.info}>
            <h3 className={styles.rankNameLabel}>{Messages.rating()}</h3>
            <div style={{ color: "var(--accent-primary)", fontWeight: "bold" }}>
              {formatProfileRating(rankedProfile)}
            </div>
            <div style={{ fontSize: "12px", opacity: 0.7 }}>
              {Messages.setsPlayed(setsPlayed)}
              {position && ` · ${position}`}
            </div>
          </div>
          <div className={styles.rankBadge}>
            <RankBadge rank={rank} setsToPlace={setsToPlace} size="large" />
          </div>
        </div>
        <div style={{ display: "flex", gap: 16 }}>
          <RefreshRatingButton />
          <Button className={styles.refreshButton} onClick={() => navigate("/main/profile")}>
            <HistoryIcon color="inherit" sx={{ fontSize: "16px", color: "var(--surface-3)" }} />
            <span>{Messages.matchHistory()}</span>
          </Button>
        </div>
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
